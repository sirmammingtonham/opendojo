// Unit tests for the submit_drill validator. Pure-logic: no network,
// no DB. Run with:
//
//     cd supabase/functions/submit_drill
//     deno test
//
// (The Supabase CLI ships Deno; if you have it on PATH, `deno test`
// from this dir Just Works. Otherwise: `supabase functions deno-test`.)
//
// Each case either expects ok or asserts the error message contains
// a specific substring - never the full string, since wording may
// shift. Add new cases when the validator gains rules.

import {
    assertEquals,
    assertStringIncludes,
} from "https://deno.land/std@0.220.0/assert/mod.ts";

import {
    drillShapeError,
    hasForbiddenChar,
    MAX_CONTENT_LINE_LEN,
    MAX_CONTENT_LINES,
    MAX_RECORDING_EVENTS,
    normalizeLineEndings,
    type SubmitBody,
    validate,
} from "./validate.ts";

// ---- helpers ---------------------------------------------------------------

// Build a minimal-valid drill body. Overrides merge in shallowly.
function body(overrides: Partial<SubmitBody> = {}): SubmitBody {
    const content =
        "# OpenDojo drill\n" +
        "name: test\n" +
        "character: jin\n" +
        "recordings: 1\n" +
        "\n" +
        "--- recording 1\n" +
        "events: 0\n";
    return {
        name: "test drill",
        character: "jin",
        recordings_count: 1,
        content,
        ...overrides,
    };
}

function expectOk(b: SubmitBody): void {
    const r = validate(b);
    if ("err" in r) throw new Error("expected ok, got err: " + r.err);
}

function expectErr(b: SubmitBody, includes: string): void {
    const r = validate(b);
    if ("ok" in r) throw new Error("expected err containing '" + includes + "', got ok");
    assertStringIncludes(r.err, includes);
}

// Build content with a given recording count, numbered sequentially.
function makeContent(n: number): string {
    let out = "# OpenDojo drill\nrecordings: " + n + "\n\n";
    for (let i = 1; i <= n; ++i) out += "--- recording " + i + "\nevents: 0\n";
    return out;
}

// ---- happy path ------------------------------------------------------------

Deno.test("validate: minimal-valid drill", () => {
    expectOk(body());
});

Deno.test("validate: full-featured drill", () => {
    expectOk(body({
        description: "block string mixups\nframe data:\n- d/f+1: -3",
        author_handle: "Komugi",
        cpu_side: "p2",
        categories: ["reaction", "punishment"],
        difficulty: "intermediate",
        game_version: "3.00.02",
        recordings_count: 3,
        content: makeContent(3),
    }));
});

Deno.test("validate: 8 recordings is fine", () => {
    expectOk(body({ recordings_count: 8, content: makeContent(8) }));
});

// ---- name ------------------------------------------------------------------

Deno.test("name: missing", () => {
    expectErr(body({ name: undefined }), "name");
});

Deno.test("name: empty string", () => {
    expectErr(body({ name: "" }), "name");
});

Deno.test("name: whitespace only", () => {
    expectErr(body({ name: "   " }), "name");
});

Deno.test("name: too long", () => {
    expectErr(body({ name: "x".repeat(97) }), "name");
});

Deno.test("name: 96 chars is the boundary", () => {
    expectOk(body({ name: "x".repeat(96) }));
});

Deno.test("name: control char (BEL)", () => {
    expectErr(body({ name: "jin\x07hack" }), "forbidden");
});

Deno.test("name: newline rejected", () => {
    expectErr(body({ name: "two\nlines" }), "forbidden");
});

Deno.test("name: RTL override (U+202E)", () => {
    expectErr(body({ name: "jin‮gnij" }), "forbidden");
});

Deno.test("name: zero-width space (U+200B)", () => {
    expectErr(body({ name: "j​in" }), "forbidden");
});

// ---- character -------------------------------------------------------------

Deno.test("character: missing", () => {
    expectErr(body({ character: undefined }), "character");
});

Deno.test("character: uppercase rejected", () => {
    expectErr(body({ character: "Jin" }), "character");
});

Deno.test("character: leading digit rejected", () => {
    expectErr(body({ character: "1jin" }), "character");
});

Deno.test("character: special chars rejected", () => {
    expectErr(body({ character: "jin-x" }), "character");
});

Deno.test("character: snake_case allowed", () => {
    expectOk(body({ character: "devil_jin" }));
});

// ---- description -----------------------------------------------------------

Deno.test("description: optional", () => {
    expectOk(body({ description: undefined }));
});

Deno.test("description: multi-line allowed", () => {
    expectOk(body({ description: "line 1\nline 2\nline 3" }));
});

Deno.test("description: CRLF normalized to LF", () => {
    const r = validate(body({ description: "a\r\nb\rc" }));
    if ("err" in r) throw new Error(r.err);
    assertEquals(r.ok.description, "a\nb\nc");
});

Deno.test("description: 1000 chars boundary", () => {
    expectOk(body({ description: "x".repeat(1000) }));
});

Deno.test("description: over 1000 chars rejected", () => {
    expectErr(body({ description: "x".repeat(1001) }), "description");
});

Deno.test("description: null byte rejected", () => {
    expectErr(body({ description: "hi\x00there" }), "forbidden");
});

Deno.test("description: bidi override rejected", () => {
    expectErr(body({ description: "ok‮text" }), "forbidden");
});

// ---- author_handle ---------------------------------------------------------

Deno.test("author_handle: optional", () => {
    expectOk(body({ author_handle: undefined }));
});

Deno.test("author_handle: control char rejected", () => {
    expectErr(body({ author_handle: "Komugi\x07" }), "forbidden");
});

Deno.test("author_handle: too long", () => {
    expectErr(body({ author_handle: "x".repeat(33) }), "author_handle");
});

// ---- cpu_side --------------------------------------------------------------

Deno.test("cpu_side: missing treated as empty", () => {
    expectOk(body({ cpu_side: undefined }));
});

Deno.test("cpu_side: 'p1' ok", () => {
    expectOk(body({ cpu_side: "p1" }));
});

Deno.test("cpu_side: 'p3' rejected", () => {
    expectErr(body({ cpu_side: "p3" }), "cpu_side");
});

// ---- recordings_count ------------------------------------------------------

Deno.test("recordings_count: 0 rejected", () => {
    expectErr(body({ recordings_count: 0 }), "recordings_count");
});

Deno.test("recordings_count: 9 rejected", () => {
    expectErr(body({ recordings_count: 9, content: makeContent(9) }), "recordings_count");
});

Deno.test("recordings_count: float rejected", () => {
    expectErr(body({ recordings_count: 1.5 }), "recordings_count");
});

// ---- content body ----------------------------------------------------------

Deno.test("content: missing header", () => {
    expectErr(body({ content: "recordings: 1\n--- recording 1\n" }), "OpenDojo header");
});

Deno.test("content: null byte rejected", () => {
    expectErr(body({ content: "# OpenDojo drill\nrecordings: 1\nx\x00y\n--- recording 1\n" }),
              "null byte");
});

Deno.test("content: declared count mismatch", () => {
    // Declared 2 in JSON, but content header + markers say 1.
    expectErr(body({ recordings_count: 2 }), "recordings");
});

Deno.test("content: marker count mismatch", () => {
    // Header says 2 but only one marker present.
    const c = "# OpenDojo drill\nrecordings: 2\n\n--- recording 1\n";
    expectErr(body({ recordings_count: 2, content: c }), "markers");
});

Deno.test("content: markers out of order", () => {
    const c = "# OpenDojo drill\nrecordings: 2\n\n--- recording 2\n--- recording 1\n";
    expectErr(body({ recordings_count: 2, content: c }), "order");
});

Deno.test("content: 'recordings: N' in a later comment doesn't fool the count check", () => {
    // The legitimate header says 1, but a comment after the first --- says 5.
    // Validator must read the count from the header section only.
    const c =
        "# OpenDojo drill\nrecordings: 1\n\n" +
        "--- recording 1\n" +
        "# recordings: 5\n";
    expectOk(body({ recordings_count: 1, content: c }));
});

Deno.test("content: line longer than cap rejected", () => {
    const long = "x".repeat(MAX_CONTENT_LINE_LEN + 1);
    const c = "# OpenDojo drill\nrecordings: 1\n" + long + "\n--- recording 1\n";
    expectErr(body({ content: c }), "longer than");
});

Deno.test("content: too many lines rejected", () => {
    // Build a content with > MAX_CONTENT_LINES newlines but small bytes.
    // We deliberately exceed the line cap; we don't care about other
    // shape errors triggering - the line cap fires first because we
    // walk the whole content once before the regex matches.
    const c = "# OpenDojo drill\nrecordings: 1\n" +
              "\n".repeat(MAX_CONTENT_LINES + 1) +
              "--- recording 1\n";
    expectErr(body({ content: c }), "more than");
});

// ---- per-recording event cap -----------------------------------------------

// One recording carrying `k` event lines. Each is a short, well-formed event
// ("dir buttons frames"); the server only counts them, it doesn't grammar-check.
function drillWithEvents(k: number, recordings = 1): string {
    let c = "# OpenDojo drill\nrecordings: " + recordings + "\n\n";
    for (let r = 1; r <= recordings; ++r) {
        c += "--- recording " + r + "\n";
        for (let i = 0; i < k; ++i) c += "f 1 10\n";
    }
    return c;
}

Deno.test("content: recording at the event cap is accepted", () => {
    const c = drillWithEvents(MAX_RECORDING_EVENTS);
    expectOk(body({ recordings_count: 1, content: c }));
});

Deno.test("content: recording over the event cap rejected", () => {
    const c = drillWithEvents(MAX_RECORDING_EVENTS + 1);
    expectErr(body({ recordings_count: 1, content: c }), "more than");
});

Deno.test("content: event cap is per-recording, not total", () => {
    // Two recordings each at the cap: total events exceed the cap but no
    // single recording does, so this must pass.
    const c = drillWithEvents(MAX_RECORDING_EVENTS, 2);
    expectOk(body({ recordings_count: 2, content: c }));
});

Deno.test("content: comment + blank lines don't count toward the event cap", () => {
    // At the cap, then padded with comments/blanks that classify as non-events.
    let c = drillWithEvents(MAX_RECORDING_EVENTS);
    c += "# a comment\n\n# another\n";
    expectOk(body({ recordings_count: 1, content: c }));
});

// ---- categories ------------------------------------------------------------

Deno.test("categories: empty array ok", () => {
    expectOk(body({ categories: [] }));
});

Deno.test("categories: unknown rejected", () => {
    expectErr(body({ categories: ["mystery_meat"] }), "unknown category");
});

Deno.test("categories: too many", () => {
    expectErr(body({
        categories: ["reaction", "option_select", "fuzzy_guard",
                     "punishment", "throw_break", "reaction"],
    }), "at most");
});

Deno.test("categories: duplicates deduped", () => {
    const r = validate(body({ categories: ["reaction", "reaction", "punishment"] }));
    if ("err" in r) throw new Error(r.err);
    assertEquals(r.ok.categories.sort(), ["punishment", "reaction"]);
});

// ---- difficulty ------------------------------------------------------------

Deno.test("difficulty: unknown rejected", () => {
    expectErr(body({ difficulty: "godlike" }), "unknown difficulty");
});

Deno.test("difficulty: valid", () => {
    expectOk(body({ difficulty: "advanced" }));
});

// ---- game_version ----------------------------------------------------------

Deno.test("game_version: dot-separated ok", () => {
    expectOk(body({ game_version: "3.00.02" }));
});

Deno.test("game_version: spaces rejected", () => {
    expectErr(body({ game_version: "3 00 02" }), "game_version");
});

Deno.test("game_version: too long", () => {
    expectErr(body({ game_version: "x".repeat(25) }), "game_version");
});

// ---- low-level helper tests -----------------------------------------------

Deno.test("hasForbiddenChar: clean ASCII passes both modes", () => {
    assertEquals(hasForbiddenChar("hello world", false), false);
    assertEquals(hasForbiddenChar("hello world", true),  false);
});

Deno.test("hasForbiddenChar: tab/lf are mode-dependent", () => {
    assertEquals(hasForbiddenChar("a\tb", false), true);
    assertEquals(hasForbiddenChar("a\tb", true),  false);
    assertEquals(hasForbiddenChar("a\nb", false), true);
    assertEquals(hasForbiddenChar("a\nb", true),  false);
});

Deno.test("hasForbiddenChar: BOM blocked in both modes", () => {
    assertEquals(hasForbiddenChar("﻿text", false), true);
    assertEquals(hasForbiddenChar("﻿text", true),  true);
});

Deno.test("normalizeLineEndings: every flavor maps to LF", () => {
    assertEquals(normalizeLineEndings("a\r\nb\rc\nd"), "a\nb\nc\nd");
});

Deno.test("drillShapeError: returns null on a clean drill", () => {
    const c = makeContent(2);
    assertEquals(drillShapeError(c, 2), null);
});

Deno.test("drillShapeError: line length is measured in chars, not bytes", () => {
    // A single line of MAX_CONTENT_LINE_LEN multibyte chars decodes
    // to more than the byte cap, but the line-len check counts
    // UTF-16 chars (string.length). Either way, this should pass
    // when invoked directly (the byte cap is checked elsewhere).
    const line = "ä".repeat(MAX_CONTENT_LINE_LEN);  // U+00E4 = ä
    const c = "# OpenDojo drill\nrecordings: 1\n" + line + "\n--- recording 1\n";
    assertEquals(drillShapeError(c, 1), null);
});
