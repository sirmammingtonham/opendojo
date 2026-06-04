// Content moderation for submit_drill — profanity / hate-speech screening
// of the human-visible fields (name, description, author handle).
//
// Uses `obscenity`: a transformation-aware English matcher that resists the
// usual evasions (leetspeak, character substitution, inserted spaces /
// punctuation, repeated letters) while using word boundaries + a whitelist
// to avoid the "Scunthorpe problem" (banning innocent words that merely
// contain a bad substring). The version is pinned for reproducible deploys
// and the matcher is built once per isolate at module load, so the
// per-request cost is just a regex scan of three short strings.
//
// This is ONE layer, not the whole moderation story: user reports plus the
// report-driven auto-hide threshold (see migrations) are the backstop for
// whatever slips through, and the admin panel is the final arbiter.

import {
    DataSet,
    englishDataset,
    englishRecommendedTransformers,
    pattern,
    RegExpMatcher,
} from "https://esm.sh/obscenity@0.4.6";

// Extra terms to ban beyond obscenity's built-in English set (which already
// covers common profanity + the major slurs). Each entry uses obscenity's
// `pattern` template, so the shared transformers still handle case /
// leetspeak / spacing for them. Example:  pattern`somebadterm`
const EXTRA_PATTERNS: ReturnType<typeof pattern>[] = [
    // pattern`...`,
];

// Words obscenity would otherwise flag but that are legitimate here (e.g. a
// character/move name that collides with a banned substring). These win
// over the blacklist — add to this list to clear a false positive.
const ALLOWLIST: string[] = [
    // "cockpit", "sussex", ...
];

function buildMatcher(): RegExpMatcher {
    const dataset = new DataSet<{ originalWord: string }>().addAll(englishDataset);
    EXTRA_PATTERNS.forEach((p, i) =>
        dataset.addPhrase((phrase) =>
            phrase.setMetadata({ originalWord: `extra_${i}` }).addPattern(p)
        )
    );
    const built = dataset.build();
    return new RegExpMatcher({
        ...built,
        whitelistedTerms: [...(built.whitelistedTerms ?? []), ...ALLOWLIST],
        ...englishRecommendedTransformers,
    });
}

const matcher = buildMatcher();

// obscenity's recommended transformers handle leetspeak / confusables /
// case / repeats, but deliberately NOT separator-insertion ("f u c k",
// "f.u.c.k"). Globally stripping separators would cause the classic
// "pen is" -> "penis" false positive, so we only collapse runs of *single*
// letters each followed by a separator — i.e. genuine letter-by-letter
// spacing. Multi-letter words like "pen is" are left untouched.
const SEPARATED_RUN = /(?:[A-Za-z][^A-Za-z0-9]+){2,}[A-Za-z]/g;

function collapseSeparated(text: string): string {
    return text.replace(SEPARATED_RUN, (run) => run.replace(/[^A-Za-z0-9]+/g, ""));
}

// True if the text contains banned language. Null / empty is always clean
// (description + author handle are optional fields). We scan the text as
// written and, if different, its de-separated form.
export function containsBannedLanguage(text: string | null | undefined): boolean {
    if (!text) return false;
    if (matcher.hasMatch(text)) return true;
    const collapsed = collapseSeparated(text);
    return collapsed !== text && matcher.hasMatch(collapsed);
}
