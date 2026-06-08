// Pure validation for update_drill - split from index.ts so it can be
// unit-tested without Deno.serve / a Supabase client. Mirrors the name +
// description rules in submit_drill's validate(), reusing the SAME character
// classifier + line-ending normalizer so the two write paths can't drift.

import { hasForbiddenChar, normalizeLineEndings } from "../submit_drill/validate.ts";

// Edits are tiny (a name + a short description). Cap the body well below the
// upload cap so a junk payload is rejected before we read it.
export const MAX_EDIT_BODY_BYTES = 8 * 1024;

export interface EditBody {
    id?: unknown;
    name?: unknown;
    description?: unknown;
}

export type ValidatedEdit = {
    id: string;
    name: string;
    description: string;
};

const RE_UUID =
    /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;

// Returns { ok } with the cleaned fields, or { err } with a user-facing
// reason. Length bounds mirror the drills.name / drills.description CHECK
// constraints so the DB write can't fail on something we should have caught.
export function validateEdit(body: EditBody): { ok: ValidatedEdit } | { err: string } {
    if (typeof body.id !== "string" || !RE_UUID.test(body.id)) {
        return { err: "invalid drill id" };
    }

    // name: required, single-line, 1..96 chars, no control/zero-width/bidi.
    if (typeof body.name !== "string")          return { err: "name is required (1-96 chars)" };
    const name = body.name.trim();
    if (name.length === 0 || name.length > 96)  return { err: "name is required (1-96 chars)" };
    if (hasForbiddenChar(name, false))          return { err: "name contains forbidden characters" };

    // description: optional. Absent/null -> "" (clears it), matching the
    // column default and the previous RPC's coalesce. Multi-line allowed.
    let description = "";
    if (body.description != null) {
        if (typeof body.description !== "string") return { err: "description must be a string" };
        description = normalizeLineEndings(body.description);
        if (description.length > 1000)            return { err: "description exceeds 1000 chars" };
        if (hasForbiddenChar(description, true))  return { err: "description contains forbidden characters" };
    }

    return { ok: { id: body.id, name, description } };
}
