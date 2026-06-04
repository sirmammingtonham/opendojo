// Tests for the profanity / hate-speech screen. Unlike validate.test.ts,
// these DO pull a network dependency (obscenity, via esm.sh), so they need
// network access the first time. Run with:
//
//     cd supabase/functions/submit_drill
//     deno test --allow-net
//
// The assertions cover three things: clean text (incl. Tekken jargon and
// known false-positive substrings) passes; obvious profanity is caught; and
// common evasions (inserted separators, leetspeak) are caught. We avoid
// hard-coding slurs in this file — coverage of those comes from obscenity's
// built-in English dataset.

import { assert, assertEquals } from "https://deno.land/std@0.220.0/assert/mod.ts";

import { containsBannedLanguage } from "./profanity.ts";

Deno.test("null / empty / optional fields are clean", () => {
    assertEquals(containsBannedLanguage(null), false);
    assertEquals(containsBannedLanguage(undefined), false);
    assertEquals(containsBannedLanguage(""), false);
    assertEquals(containsBannedLanguage("   "), false);
});

Deno.test("legitimate drill text passes", () => {
    for (
        const s of [
            "jin string defense",
            "EWGF just-frame practice",
            "low parry into launch punish",
            "Asuka oki mixup",
            "block string / throw break reactions",
        ]
    ) {
        assertEquals(containsBannedLanguage(s), false, `should be clean: ${s}`);
    }
});

Deno.test("innocent words containing banned substrings pass (Scunthorpe problem)", () => {
    for (const s of ["class of punishes", "grass", "Scunthorpe", "passage", "analysis"]) {
        assertEquals(containsBannedLanguage(s), false, `false positive: ${s}`);
    }
});

Deno.test("ordinary spacing is not treated as evasion", () => {
    // The de-separation normalizer must only collapse single-letter runs,
    // so multi-letter words and letter lists stay clean.
    for (const s of ["pen is mightier than the sword", "a, b, and c", "W T F", "g g e z"]) {
        assertEquals(containsBannedLanguage(s), false, `false positive: ${s}`);
    }
});

Deno.test("obvious profanity is caught", () => {
    assert(containsBannedLanguage("this drill is shit"));
    assert(containsBannedLanguage("fuck this matchup"));
});

Deno.test("common evasions are caught", () => {
    assert(containsBannedLanguage("sh1t"), "leetspeak");
    assert(containsBannedLanguage("f u c k"), "space-separated");
    assert(containsBannedLanguage("f.u.c.k"), "dot-separated");
    assert(containsBannedLanguage("f-u-c-k"), "dash-separated");
    assert(containsBannedLanguage("f*u*c*k"), "star-separated");
    assert(containsBannedLanguage("a s s hole punish"), "spaced slur fragment");
});
