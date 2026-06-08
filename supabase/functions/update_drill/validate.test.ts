// Unit tests for the update_drill validator. Pure-logic: no network, no DB.
// Run with:
//
//     cd supabase/functions/update_drill
//     deno test
//
// The forbidden-char / line-ending behavior is exercised more thoroughly in
// submit_drill's suite (same shared primitives); these cases pin the edit
// path's own rules: id shape, name required/length, optional description.

import {
    assertEquals,
    assertStringIncludes,
} from "https://deno.land/std@0.220.0/assert/mod.ts";

import { type EditBody, validateEdit } from "./validate.ts";

const ID = "00000000-0000-4000-8000-000000000000";

// Build "dangerous" strings programmatically so no literal control /
// zero-width characters live in this source file.
const BEL  = String.fromCharCode(0x07);    // C0 control
const ZWSP = String.fromCharCode(0x200b);  // zero-width space

function ok(body: unknown) {
    const r = validateEdit(body as EditBody);
    if ("err" in r) throw new Error(`expected ok, got err: ${r.err}`);
    return r.ok;
}
function err(body: unknown): string {
    const r = validateEdit(body as EditBody);
    if (!("err" in r)) throw new Error("expected err, got ok");
    return r.err;
}

Deno.test("accepts a valid edit", () => {
    const e = ok({ id: ID, name: "My Drill", description: "Some notes" });
    assertEquals(e.id, ID);
    assertEquals(e.name, "My Drill");
    assertEquals(e.description, "Some notes");
});

Deno.test("keeps interior spaces, trims the ends of the name", () => {
    assertEquals(ok({ id: ID, name: "  Electric Wind God Fist  " }).name,
        "Electric Wind God Fist");
});

Deno.test("description is optional and defaults to empty", () => {
    assertEquals(ok({ id: ID, name: "n" }).description, "");
    assertEquals(ok({ id: ID, name: "n", description: null }).description, "");
});

Deno.test("normalizes CRLF in description", () => {
    assertEquals(ok({ id: ID, name: "n", description: "a\r\nb" }).description, "a\nb");
});

Deno.test("rejects a bad / missing id", () => {
    assertStringIncludes(err({ id: "not-a-uuid", name: "n" }), "drill id");
    assertStringIncludes(err({ name: "n" }), "drill id");
});

Deno.test("rejects a missing / blank / oversized name", () => {
    assertStringIncludes(err({ id: ID }), "name is required");
    assertStringIncludes(err({ id: ID, name: "   " }), "name is required");
    assertStringIncludes(err({ id: ID, name: "x".repeat(97) }), "name is required");
});

Deno.test("rejects control chars in name (spaces are fine, controls are not)", () => {
    assertStringIncludes(err({ id: ID, name: "bad" + BEL + "name" }), "forbidden");
});

Deno.test("rejects an oversized description", () => {
    assertStringIncludes(
        err({ id: ID, name: "n", description: "x".repeat(1001) }),
        "1000",
    );
});

Deno.test("rejects zero-width chars in description", () => {
    assertStringIncludes(
        err({ id: ID, name: "n", description: "a" + ZWSP + "b" }),
        "forbidden",
    );
});
