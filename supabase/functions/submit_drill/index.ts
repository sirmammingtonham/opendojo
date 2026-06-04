// submit_drill - the ONLY write path into the drills table.
//
// Lifecycle:
//   1. Verify the caller's JWT (handled by Supabase's gateway because
//      config.toml sets verify_jwt = true). The JWT must come from an
//      anonymous sign-in - these are the only identities we issue.
//   2. Decode + validate the JSON payload via `validate()` in
//      validate.ts. That module is pure-logic and unit-tested.
//   3. Hash the content (SHA-256, hex). If a row with this hash
//      already exists, return that id and tell the client it was a
//      duplicate - don't double-count rate limits.
//   4. Bump the user's daily upload counter via
//      `try_record_submission`. If they're over the cap, abort.
//   5. Insert. The unique index on content_hash is the final
//      backstop against concurrent duplicate inserts.
//
// Errors return JSON { error: string } with a 4xx status. Anything
// the client should retry returns 5xx. Don't leak Postgres details.

import { createClient } from "https://esm.sh/@supabase/supabase-js@2.45.0";
import {
    MAX_BODY_BYTES,
    MAX_UPLOADS_PER_DAY,
    type SubmitBody,
    validate,
} from "./validate.ts";
import { containsBannedLanguage } from "./profanity.ts";

const SUPABASE_URL              = Deno.env.get("SUPABASE_URL")!;
const SUPABASE_ANON_KEY         = Deno.env.get("SUPABASE_ANON_KEY")!;
const SUPABASE_SERVICE_ROLE_KEY = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!;

function json(status: number, body: unknown): Response {
    return new Response(JSON.stringify(body), {
        status,
        headers: { "content-type": "application/json" },
    });
}

async function sha256Hex(s: string): Promise<string> {
    const buf = new TextEncoder().encode(s);
    const hash = await crypto.subtle.digest("SHA-256", buf);
    return Array.from(new Uint8Array(hash))
        .map((b) => b.toString(16).padStart(2, "0"))
        .join("");
}

Deno.serve(async (req) => {
    if (req.method !== "POST") {
        return json(405, { error: "method not allowed" });
    }

    // Body size guard - reject early if the client advertises a body
    // bigger than we'd ever accept. (Not load-bearing - we re-check
    // the content size after decoding - but it short-circuits the
    // 100 MB upload case.)
    const lenHdr = req.headers.get("content-length");
    if (lenHdr) {
        const len = parseInt(lenHdr, 10);
        if (Number.isFinite(len) && len > MAX_BODY_BYTES) {
            return json(413, { error: "request body too large" });
        }
    }

    // The gateway already verified the JWT (config.toml: verify_jwt = true),
    // so by the time we're here the Authorization header has a valid token.
    // We still need to *decode* it to learn who the user is - re-use a
    // user-scoped client so .auth.getUser() does that and re-validates.
    const auth = req.headers.get("Authorization");
    if (!auth) return json(401, { error: "missing Authorization" });

    const userClient = createClient(SUPABASE_URL, SUPABASE_ANON_KEY, {
        global: { headers: { Authorization: auth } },
        auth:   { persistSession: false },
    });
    const { data: userData, error: userErr } = await userClient.auth.getUser();
    if (userErr || !userData.user) {
        return json(401, { error: "invalid token" });
    }
    const userId = userData.user.id;

    let body: SubmitBody;
    try {
        body = await req.json();
    } catch {
        return json(400, { error: "invalid JSON" });
    }

    const v = validate(body);
    if ("err" in v) return json(400, { error: v.err });
    const drill = v.ok;

    // Content moderation. Reject profanity / hate speech in the
    // human-visible fields before anything touches the database. The drill
    // body itself is machine data (recording event lines) and isn't screened
    // here; user reports + auto-hide cover anything that gets through.
    if (
        containsBannedLanguage(drill.name) ||
        containsBannedLanguage(drill.description) ||
        containsBannedLanguage(drill.author_handle)
    ) {
        return json(400, {
            error:
                "Name, description, and author handle must not contain profanity or hate speech.",
        });
    }

    const contentHash = await sha256Hex(drill.content);
    const sizeBytes   = new TextEncoder().encode(drill.content).byteLength;

    // Service-role client for the actual write path. It bypasses RLS,
    // which is exactly what we want - RLS denies all writes for the
    // public roles and we are the trusted gate.
    const admin = createClient(SUPABASE_URL, SUPABASE_SERVICE_ROLE_KEY, {
        auth: { persistSession: false },
    });

    // Ban check. A row in user_bans for this uid means the admin
    // panel has flagged them; refuse uploads. We do this before
    // dedupe so a banned user can't keep "re-uploading" to discover
    // hashes of content others have already submitted.
    {
        const { data: ban, error } = await admin
            .from("user_bans")
            .select("user_id")
            .eq("user_id", userId)
            .maybeSingle();
        if (error) {
            console.error("ban lookup failed", error);
            return json(500, { error: "internal error" });
        }
        if (ban) {
            return json(403, { error: "This account has been banned from uploading." });
        }
    }

    // Dedupe first. If this exact content has been uploaded before,
    // return that row's id without touching rate limits or inserting
    // a new row. This means re-uploading the same drill is free
    // (good for the user, neutral for us).
    {
        const { data: existing, error } = await admin
            .from("drills")
            .select("id")
            .eq("content_hash", contentHash)
            .maybeSingle();
        if (error) {
            console.error("dedupe lookup failed", error);
            return json(500, { error: "internal error" });
        }
        if (existing) {
            return json(200, { id: existing.id, deduped: true });
        }
    }

    // Rate limit: atomically increment today's counter for this user.
    {
        const { data: uploads, error } = await admin
            .rpc("try_record_submission", { p_user_id: userId });
        if (error) {
            console.error("try_record_submission failed", error);
            return json(500, { error: "internal error" });
        }
        if (typeof uploads === "number" && uploads > MAX_UPLOADS_PER_DAY) {
            return json(429, {
                error: `upload rate limit exceeded (${MAX_UPLOADS_PER_DAY}/day)`,
            });
        }
    }

    // Insert. The unique index on content_hash is the final guard
    // against a race where two concurrent submits of the same content
    // both pass the dedupe lookup above. ON CONFLICT returns the
    // pre-existing id rather than failing.
    const { data: inserted, error: insErr } = await admin
        .from("drills")
        .upsert({
            name:             drill.name,
            description:      drill.description,
            character:        drill.character,
            cpu_side:         drill.cpu_side,
            recordings_count: drill.recordings_count,
            content:          drill.content,
            content_hash:     contentHash,
            size_bytes:       sizeBytes,
            author_handle:    drill.author_handle,
            categories:       drill.categories,
            difficulty:       drill.difficulty,
            game_version:     drill.game_version,
            uploader_id:      userId,
        }, { onConflict: "content_hash", ignoreDuplicates: false })
        .select("id")
        .single();

    if (insErr || !inserted) {
        console.error("insert failed", insErr);
        return json(500, { error: "internal error" });
    }

    return json(200, { id: inserted.id, deduped: false });
});
