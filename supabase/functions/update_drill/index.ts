// update_drill - author edit of an existing drill's name + description.
//
// This is the SECOND privileged write path into `drills` (alongside
// submit_drill) and exists so edits get the SAME server-side validation +
// profanity screen as uploads. It deliberately touches ONLY name and
// description - never content, content_hash, author_handle, status, or any
// counter - so an edit can't re-hash content, un-hide a flagged drill,
// launder reports, or reassign ownership.
//
// Lifecycle:
//   1. Verify the caller's token via getUser(). config.toml sets
//      verify_jwt = false (publishable keys aren't JWTs, so the gateway can't
//      verify), so the function authorizes the caller itself.
//   2. Validate id / name / description (validate.ts - pure + unit-tested).
//   3. Reject profanity / hate speech in name + description.
//   4. Refuse if the user is banned (a ban must also stop edits, or it
//      wouldn't stop a banned user rewriting their live drills' visible text).
//   5. UPDATE ... WHERE id = ? AND uploader_id = <caller> - ownership is
//      enforced in the WHERE clause, so a non-owner edits zero rows.
//
// Errors return JSON { error: string } with a 4xx status; retryable failures
// return 5xx. Postgres details never leak - they go only to the function log.

import { createClient } from "https://esm.sh/@supabase/supabase-js@2.45.0";
import { type EditBody, MAX_EDIT_BODY_BYTES, validateEdit } from "./validate.ts";
// Shared with submit_drill: reusing the exact module guarantees the two write
// paths screen content by identical rules and can never drift apart.
import { containsBannedLanguage } from "../submit_drill/profanity.ts";

const SUPABASE_URL        = Deno.env.get("SUPABASE_URL");
const OPENDOJO_SECRET_KEY = Deno.env.get("OPENDOJO_SECRET_KEY");
if (!SUPABASE_URL || !OPENDOJO_SECRET_KEY) {
    throw new Error("update_drill: SUPABASE_URL and OPENDOJO_SECRET_KEY must be set");
}

function json(status: number, body: unknown): Response {
    return new Response(JSON.stringify(body), {
        status,
        headers: { "content-type": "application/json" },
    });
}

Deno.serve(async (req) => {
    if (req.method !== "POST") {
        return json(405, { error: "method not allowed" });
    }

    // Reject early if the client advertises a body bigger than an edit could be.
    const lenHdr = req.headers.get("content-length");
    if (lenHdr) {
        const len = parseInt(lenHdr, 10);
        if (Number.isFinite(len) && len > MAX_EDIT_BODY_BYTES) {
            return json(413, { error: "request body too large" });
        }
    }

    // verify_jwt is off, so the function is its own auth check. One secret-key
    // client does both jobs: getUser() validates the caller's token, and the
    // same client does the privileged UPDATE (service_role bypasses RLS).
    const authz = req.headers.get("Authorization");
    if (!authz) return json(401, { error: "missing Authorization" });
    const token = authz.replace(/^[Bb]earer\s+/, "");

    const admin = createClient(SUPABASE_URL, OPENDOJO_SECRET_KEY, {
        auth: { persistSession: false },
    });

    const { data: userData, error: userErr } = await admin.auth.getUser(token);
    if (userErr || !userData.user) {
        return json(401, { error: "invalid token" });
    }
    const userId = userData.user.id;

    let body: EditBody;
    try {
        body = await req.json();
    } catch {
        return json(400, { error: "invalid JSON" });
    }

    const v = validateEdit(body);
    if ("err" in v) return json(400, { error: v.err });
    const edit = v.ok;

    // Same moderation screen as upload, on the same fields.
    if (containsBannedLanguage(edit.name) || containsBannedLanguage(edit.description)) {
        return json(400, { error: "That name or description isn't allowed.." });
    }

    // Banned users can't edit either.
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

    // Ownership is enforced here: the row must be the caller's. A non-owner or
    // a missing id matches nothing -> updated:false, with no error and no
    // signal of whether the drill exists. search_tsv is a generated column,
    // so it re-indexes automatically.
    const { data: rows, error: updErr } = await admin
        .from("drills")
        .update({ name: edit.name, description: edit.description })
        .eq("id", edit.id)
        .eq("uploader_id", userId)
        .select("id");
    if (updErr) {
        console.error("update failed", updErr);
        return json(500, { error: "internal error" });
    }

    return json(200, { updated: (rows?.length ?? 0) > 0 });
});
