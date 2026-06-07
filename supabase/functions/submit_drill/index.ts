// submit_drill - the ONLY write path into the drills table.
//
// Lifecycle:
//   1. Verify the caller. config.toml sets verify_jwt = false (publishable
//      keys aren't JWTs, so gateway JWT verification isn't available), so the
//      function validates the user's token itself via getUser() below. The
//      token must come from an anonymous sign-in - the only identities we issue.
//   2. Decode + validate the JSON payload via `validate()` in
//      validate.ts. That module is pure-logic and unit-tested.
//   3. Hash the content (SHA-256, hex). If a row with this hash
//      already exists, return that id and tell the client it was a
//      duplicate - don't re-insert.
//   4. Insert. The unique index on content_hash is the final
//      backstop against concurrent duplicate inserts.
//
// Errors return JSON { error: string } with a 4xx status. Anything
// the client should retry returns 5xx. Don't leak Postgres details.

import { createClient } from "https://esm.sh/@supabase/supabase-js@2.45.0";
import {
    MAX_BODY_BYTES,
    type SubmitBody,
    validate,
} from "./validate.ts";
import { containsBannedLanguage } from "./profanity.ts";

// The function uses one key: the project's SECRET key. It both validates the
// caller's token (getUser) and does the privileged writes - the drills /
// user_bans tables deny the public roles via RLS, so service_role access is
// required. Set it explicitly as a custom function secret (the SUPABASE_*
// prefix is reserved, so it can't live there); no fallbacks - fail fast if
// it's missing.
const SUPABASE_URL        = Deno.env.get("SUPABASE_URL");
const OPENDOJO_SECRET_KEY = Deno.env.get("OPENDOJO_SECRET_KEY");
if (!SUPABASE_URL || !OPENDOJO_SECRET_KEY) {
    throw new Error("submit_drill: SUPABASE_URL and OPENDOJO_SECRET_KEY must be set");
}

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

    // verify_jwt is off (the gateway apikey isn't a JWT), so the function is
    // its own auth check. One secret-key client does both jobs: getUser()
    // validates the caller's token, and the same client does the privileged
    // writes (the secret key uses service_role, which bypasses RLS).
    const auth = req.headers.get("Authorization");
    if (!auth) return json(401, { error: "missing Authorization" });
    const token = auth.replace(/^[Bb]earer\s+/, "");

    const admin = createClient(SUPABASE_URL, OPENDOJO_SECRET_KEY, {
        auth: { persistSession: false },
    });

    const { data: userData, error: userErr } = await admin.auth.getUser(token);
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
                "That name or description isn't allowed..",
        });
    }

    const contentHash = await sha256Hex(drill.content);
    const sizeBytes   = new TextEncoder().encode(drill.content).byteLength;

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
            dll_version:      drill.dll_version,
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
