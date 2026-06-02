// submit_drill — the ONLY write path into the drills table.
//
// Lifecycle:
//   1. Verify the caller's JWT (handled by Supabase's gateway because
//      config.toml sets verify_jwt = true). The JWT must come from an
//      anonymous sign-in — these are the only identities we issue.
//   2. Decode + validate the JSON payload. Hard caps on every field.
//   3. Sanity-check the drill text shape so obviously-broken uploads
//      get a 400 before they reach the table.
//   4. Hash the content (SHA-256, hex). If a row with this hash
//      already exists, return that id and tell the client it was a
//      duplicate — don't double-count rate limits.
//   5. Bump the user's daily upload counter via
//      `try_record_submission`. If they're over 5/day, abort.
//   6. Insert. The unique index on content_hash is the final
//      backstop against concurrent duplicate inserts.
//
// Errors return JSON { error: string } with a 4xx status. Anything
// the client should retry returns 5xx. Don't leak Postgres details.

import { createClient } from "https://esm.sh/@supabase/supabase-js@2.45.0";

// ---- Hard caps. These match the column constraints in the
// migration; the function rejects early so a malformed payload never
// even touches the database.
const MAX_CONTENT_BYTES = 64 * 1024;      // matches drills.content cap
const MAX_BODY_BYTES    = 80 * 1024;      // request envelope overhead
const MAX_RECORDINGS    = 8;              // matches in-game slot count
const MAX_UPLOADS_PER_DAY = 50;
const MAX_CATEGORIES    = 5;              // matches drills.categories cap

// Allowed taxonomy values. These mirror the seed data in
// drill_categories / drill_difficulties — we duplicate them here so
// the function can reject early without a round trip to the DB.
// If you ever add a value, add it in both places; the column-level
// FK on `difficulty` is the safety net if they drift.
const ALLOWED_CATEGORIES = new Set([
    "reaction", "option_select", "fuzzy_guard", "punishment", "throw_break",
]);
const ALLOWED_DIFFICULTIES = new Set([
    "beginner", "intermediate", "advanced",
]);

const SUPABASE_URL              = Deno.env.get("SUPABASE_URL")!;
const SUPABASE_ANON_KEY         = Deno.env.get("SUPABASE_ANON_KEY")!;
const SUPABASE_SERVICE_ROLE_KEY = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!;

function json(status: number, body: unknown): Response {
    return new Response(JSON.stringify(body), {
        status,
        headers: { "content-type": "application/json" },
    });
}

interface SubmitBody {
    name?: unknown;
    description?: unknown;
    character?: unknown;
    cpu_side?: unknown;
    recordings_count?: unknown;
    content?: unknown;
    author_handle?: unknown;
    categories?: unknown;
    difficulty?: unknown;
    game_version?: unknown;
}

type Validated = {
    name: string;
    description: string | null;
    character: string;
    cpu_side: "p1" | "p2" | "";
    recordings_count: number;
    content: string;
    author_handle: string | null;
    categories: string[];
    difficulty: string | null;
    game_version: string | null;
};

function trimStr(v: unknown, max: number): string | null {
    if (typeof v !== "string") return null;
    const s = v.trim();
    if (s.length === 0 || s.length > max) return null;
    return s;
}

function validate(body: SubmitBody): { ok: Validated } | { err: string } {
    const name = trimStr(body.name, 96);
    if (!name) return { err: "name is required (1-96 chars)" };

    const character = trimStr(body.character, 32);
    if (!character) return { err: "character is required (1-32 chars)" };

    const description = body.description == null
        ? null
        : (typeof body.description === "string" && body.description.length <= 1000
            ? body.description
            : null);
    if (body.description != null && description == null) {
        return { err: "description must be a string up to 1000 chars" };
    }

    const author_handle = body.author_handle == null
        ? null
        : trimStr(body.author_handle, 32);
    if (body.author_handle != null && author_handle == null) {
        return { err: "author_handle must be a string up to 32 chars" };
    }

    const cpu_side_raw = typeof body.cpu_side === "string" ? body.cpu_side : "";
    if (cpu_side_raw !== "p1" && cpu_side_raw !== "p2" && cpu_side_raw !== "") {
        return { err: "cpu_side must be 'p1', 'p2', or empty" };
    }
    const cpu_side = cpu_side_raw as "p1" | "p2" | "";

    const recordings_count = typeof body.recordings_count === "number"
        ? body.recordings_count : NaN;
    if (!Number.isInteger(recordings_count)
        || recordings_count < 1
        || recordings_count > MAX_RECORDINGS) {
        return { err: `recordings_count must be 1..${MAX_RECORDINGS}` };
    }

    if (typeof body.content !== "string" || body.content.length === 0) {
        return { err: "content is required" };
    }
    const content = body.content;
    // octet length, not char length — matches the column constraint.
    if (new TextEncoder().encode(content).byteLength > MAX_CONTENT_BYTES) {
        return { err: `content exceeds ${MAX_CONTENT_BYTES} bytes` };
    }
    if (!looksLikeDrill(content, recordings_count)) {
        return { err: "content does not look like an OpenDojo drill" };
    }

    // ---- Taxonomy fields -----------------------------------------------
    // Empty array is allowed (drill without category tags). Reject any
    // unknown id rather than silently dropping it, so the mod surfaces
    // taxonomy drift instead of uploading mis-tagged data.
    let categories: string[] = [];
    if (body.categories != null) {
        if (!Array.isArray(body.categories)) {
            return { err: "categories must be an array of strings" };
        }
        if (body.categories.length > MAX_CATEGORIES) {
            return { err: `at most ${MAX_CATEGORIES} categories` };
        }
        const seen = new Set<string>();
        for (const c of body.categories) {
            if (typeof c !== "string" || !ALLOWED_CATEGORIES.has(c)) {
                return { err: `unknown category: ${String(c)}` };
            }
            seen.add(c);
        }
        categories = [...seen];  // dedupe while preserving validity
    }

    let difficulty: string | null = null;
    if (body.difficulty != null) {
        if (typeof body.difficulty !== "string" ||
            !ALLOWED_DIFFICULTIES.has(body.difficulty)) {
            return { err: `unknown difficulty: ${String(body.difficulty)}` };
        }
        difficulty = body.difficulty;
    }

    let game_version: string | null = null;
    if (body.game_version != null) {
        if (typeof body.game_version !== "string"
            || body.game_version.length === 0
            || body.game_version.length > 24) {
            return { err: "game_version must be a string up to 24 chars" };
        }
        game_version = body.game_version;
    }

    return {
        ok: {
            name,
            description,
            character,
            cpu_side,
            recordings_count,
            content,
            author_handle,
            categories,
            difficulty,
            game_version,
        },
    };
}

// Loose server-side sniff: the file is allowed to drift (users edit
// drills by hand), but it should at minimum:
//   - start with the OpenDojo header marker
//   - declare a `recordings:` count that matches `recordings_count`
//   - contain at least one `--- recording` separator
function looksLikeDrill(content: string, declared: number): boolean {
    const head = content.slice(0, 256);
    if (!head.startsWith("# OpenDojo drill")) return false;

    const recMatch = content.match(/^\s*recordings:\s*(\d+)/m);
    if (!recMatch) return false;
    const parsed = parseInt(recMatch[1], 10);
    if (parsed !== declared) return false;

    const sepCount = (content.match(/^---\s*recording\s+\d+/gm) ?? []).length;
    if (sepCount === 0 || sepCount > MAX_RECORDINGS) return false;

    return true;
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

    // Body size guard — reject early if the client advertises a body
    // bigger than we'd ever accept. (Not load-bearing — we re-check
    // the content size after decoding — but it short-circuits the
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
    // We still need to *decode* it to learn who the user is — re-use a
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

    const contentHash = await sha256Hex(drill.content);
    const sizeBytes   = new TextEncoder().encode(drill.content).byteLength;

    // Service-role client for the actual write path. It bypasses RLS,
    // which is exactly what we want — RLS denies all writes for the
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
