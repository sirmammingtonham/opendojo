// Pure validation logic for submit_drill — split out of index.ts so
// it can be unit-tested without standing up Deno.serve / the
// Supabase client.
//
// Everything exported here is side-effect free: given a body, return
// either { ok: Validated } or { err: string }. The HTTP wrapper in
// index.ts handles the request lifecycle, ban check, dedupe, rate
// limit, and DB write.

// ---- Hard caps. These match the column constraints in the
// migration; we reject early so a malformed payload never even
// touches the database.
export const MAX_CONTENT_BYTES   = 64 * 1024;
export const MAX_BODY_BYTES      = 80 * 1024;
export const MAX_RECORDINGS      = 8;
export const MAX_CATEGORIES      = 5;

// Allowed taxonomy values - mirrors the seed data in
// drill_categories / drill_difficulties. Add new ids in both places;
// the column-level FK on `difficulty` is the safety net if they drift.
export const ALLOWED_CATEGORIES = new Set([
    "reaction", "option_select", "fuzzy_guard", "punishment", "throw_break",
]);
export const ALLOWED_DIFFICULTIES = new Set([
    "beginner", "intermediate", "advanced",
]);

// Tight grammars for machine-format fields. character matches the
// [a-z][a-z0-9_]* shape that players::character_name() produces;
// dll_version is the same alphabet we'd expect from a build tag.
const RE_CHARACTER    = /^[a-z][a-z0-9_]{0,31}$/;
const RE_DLL_VERSION  = /^[a-zA-Z0-9._-]{1,24}$/;

// Code-point classifier for "dangerous chars in user-visible text".
// Inlined char-code switch instead of a regex literal so the source
// file stays pure ASCII (literal control / zero-width chars in a
// regex body don't survive every editor + shell round-trip cleanly).
//
// Blocks:
//   C0 controls (U+0000..U+001F)
//   DEL (U+007F)
//   C1 controls (U+0080..U+009F)
//   Zero-width + LRM/RLM (U+200B..U+200F)
//   Bidi direction overrides (U+202A..U+202E, U+2066..U+2069)
//   BOM (U+FEFF)
// allowTabLf=true permits TAB (U+0009) + LF (U+000A) so descriptions
// can wrap; everything else stays blocked.
export function hasForbiddenChar(s: string, allowTabLf: boolean): boolean {
    for (let i = 0; i < s.length; ++i) {
        const c = s.charCodeAt(i);
        if (c <= 0x1f) {
            if (allowTabLf && (c === 0x09 || c === 0x0a)) continue;
            return true;
        }
        if (c >= 0x7f   && c <= 0x9f)   return true;
        if (c >= 0x200b && c <= 0x200f) return true;
        if (c >= 0x202a && c <= 0x202e) return true;
        if (c >= 0x2066 && c <= 0x2069) return true;
        if (c === 0xfeff)               return true;
    }
    return false;
}

// Drill-body line caps. The byte cap (MAX_CONTENT_BYTES) is the floor;
// these two prevent newline-bombing (small compressed payload that
// expands to millions of empty lines) and absurdly long lines that
// bog down regex scans.
export const MAX_CONTENT_LINES    = 8192;
export const MAX_CONTENT_LINE_LEN = 512;

// Per-recording event-line cap. On the client each recording's event lines
// are packed into one fixed slot of SLOT_PITCH (0x1C22 = 7202) bytes: a 2-byte
// count header + 4 bytes per event, so the slot holds at most
// (7202 - 2) / 4 = 1800 events. A recording carrying more would overflow that
// fixed buffer on every client that loads the drill, so reject it here — the
// MAX_CONTENT_LINES cap alone doesn't stop it (8192 lines is enough to put
// well over 1800 event lines in a single recording).
export const MAX_RECORDING_EVENTS = 1800;

// Classify one drill line the same way the client decoder does, so the
// server's event-line count matches what the client will actually pack.
// Comments (from the first '#') are stripped and surrounding whitespace
// trimmed before classifying.
function classifyLine(raw: string): "blank" | "marker" | "header" | "event" {
    const hash = raw.indexOf("#");
    const line = (hash >= 0 ? raw.slice(0, hash) : raw).trim();
    if (line.length === 0)      return "blank";
    if (line.startsWith("---")) return "marker";
    const colon = line.indexOf(":");
    if (colon > 0 && /^[A-Za-z0-9_]+$/.test(line.slice(0, colon).trim())) {
        return "header";
    }
    return "event";
}

export interface SubmitBody {
    name?: unknown;
    description?: unknown;
    character?: unknown;
    cpu_side?: unknown;
    recordings_count?: unknown;
    content?: unknown;
    author_handle?: unknown;
    categories?: unknown;
    difficulty?: unknown;
    dll_version?: unknown;
}

export type Validated = {
    name: string;
    description: string | null;
    character: string;
    cpu_side: "p1" | "p2" | "";
    recordings_count: number;
    content: string;
    author_handle: string;
    categories: string[];
    difficulty: string | null;
    dll_version: string | null;
};

// Trim + cap-length; reject if blank or too long. Returns null on
// failure so callers can distinguish "missing" from "invalid".
function trimStr(v: unknown, max: number): string | null {
    if (typeof v !== "string") return null;
    const s = v.trim();
    if (s.length === 0 || s.length > max) return null;
    return s;
}

// Normalize CRLF/CR -> LF so Windows pastes don't fail the multi-line
// check on stray \r bytes.
export function normalizeLineEndings(s: string): string {
    return s.replace(/\r\n?/g, "\n");
}

// Null-byte scan. Postgres TEXT columns can't store U+0000.
export function containsNullByte(s: string): boolean {
    for (let i = 0; i < s.length; ++i) {
        if (s.charCodeAt(i) === 0) return true;
    }
    return false;
}

// Returns null if content shape is OK, otherwise a user-visible
// reason. Cheapest checks first so a malformed upload short-circuits
// without touching the full string twice.
export function drillShapeError(content: string, declared: number): string | null {
    if (containsNullByte(content)) return "content contains null bytes";

    // Header marker must be the very start (no leading whitespace).
    if (!content.startsWith("# OpenDojo drill")) {
        return "content is missing the OpenDojo header";
    }

    // Cap line count + per-line length. Walk once.
    let lines = 0;
    let lineStart = 0;
    for (let i = 0; i < content.length; ++i) {
        if (content.charCodeAt(i) === 0x0a /* \n */) {
            ++lines;
            if (i - lineStart > MAX_CONTENT_LINE_LEN) {
                return `content has a line longer than ${MAX_CONTENT_LINE_LEN} chars`;
            }
            lineStart = i + 1;
            if (lines > MAX_CONTENT_LINES) {
                return `content has more than ${MAX_CONTENT_LINES} lines`;
            }
        }
    }

    // Bound the recordings header search to the header section so an
    // attacker can't put a misleading "recordings: 1" in a later
    // comment to bypass the count check.
    const headerEnd = content.indexOf("\n---");
    const header    = headerEnd >= 0 ? content.slice(0, headerEnd) : content;
    const recMatch  = header.match(/^\s*recordings:\s*(\d+)\s*$/m);
    if (!recMatch) return "content is missing a 'recordings:' header";
    const parsed = parseInt(recMatch[1], 10);
    if (parsed !== declared) {
        return `content's recordings: ${parsed} doesn't match declared ${declared}`;
    }

    // Exactly `declared` markers, numbered 1..declared, in order.
    // The mod's encoder emits them sequentially; anything else is
    // either tampered or corrupted.
    const markers = content.match(/^---\s*recording\s+(\d+)/gm) ?? [];
    if (markers.length !== declared) {
        return `content has ${markers.length} recording markers, expected ${declared}`;
    }
    for (let i = 0; i < markers.length; ++i) {
        const m = markers[i].match(/(\d+)/)!;
        if (parseInt(m[1], 10) !== i + 1) {
            return "recording markers must be numbered 1..N in order";
        }
    }

    // Per-recording event-line cap. Walk the body, resetting the count at each
    // recording marker, and reject the moment a single recording exceeds what
    // the client's fixed slot can hold. Event lines only exist inside a
    // recording section (before the first marker is the drill header).
    let inRecording = false;
    let recIdx = 0;
    let eventsInRec = 0;
    for (const rawLine of content.split("\n")) {
        const kind = classifyLine(rawLine);
        if (kind === "marker") {
            inRecording = true;
            ++recIdx;
            eventsInRec = 0;
        } else if (kind === "event" && inRecording) {
            if (++eventsInRec > MAX_RECORDING_EVENTS) {
                return `recording ${recIdx} has more than ${MAX_RECORDING_EVENTS} events`;
            }
        }
    }
    return null;
}

export function validate(body: SubmitBody): { ok: Validated } | { err: string } {
    // ---- name (single-line, no controls) -------------------------------
    const name = trimStr(body.name, 96);
    if (!name)                            return { err: "name is required (1-96 chars)" };
    if (hasForbiddenChar(name, false))    return { err: "name contains forbidden characters" };

    // ---- character (lowercase id) --------------------------------------
    const character_raw = trimStr(body.character, 32);
    if (!character_raw)                     return { err: "character is required (1-32 chars)" };
    if (!RE_CHARACTER.test(character_raw))  return { err: "character must match [a-z][a-z0-9_]*" };
    const character = character_raw;

    // ---- description (multi-line, allow tab + lf) ----------------------
    let description: string | null = null;
    if (body.description != null) {
        if (typeof body.description !== "string") {
            return { err: "description must be a string" };
        }
        const d = normalizeLineEndings(body.description);
        if (d.length > 1000)               return { err: "description exceeds 1000 chars" };
        if (hasForbiddenChar(d, true))     return { err: "description contains forbidden characters" };
        description = d;
    }

    // ---- author_handle (required, single-line) -------------------------
    // Anonymous uploads aren't allowed. The DLL refuses to send a blank
    // handle; the server enforces it again so a hand-rolled request
    // can't bypass attribution.
    const ah = trimStr(body.author_handle, 32);
    if (!ah)                              return { err: "author_handle is required (1-32 chars)" };
    if (hasForbiddenChar(ah, false))      return { err: "author_handle contains forbidden characters" };
    const author_handle: string = ah;

    // ---- cpu_side (enum) -----------------------------------------------
    const cpu_side_raw = typeof body.cpu_side === "string" ? body.cpu_side : "";
    if (cpu_side_raw !== "p1" && cpu_side_raw !== "p2" && cpu_side_raw !== "") {
        return { err: "cpu_side must be 'p1', 'p2', or empty" };
    }
    const cpu_side = cpu_side_raw as "p1" | "p2" | "";

    // ---- recordings_count (integer 1..8) -------------------------------
    const recordings_count = typeof body.recordings_count === "number"
        ? body.recordings_count : NaN;
    if (!Number.isInteger(recordings_count)
        || recordings_count < 1
        || recordings_count > MAX_RECORDINGS) {
        return { err: `recordings_count must be 1..${MAX_RECORDINGS}` };
    }

    // ---- content (byte cap + shape) ------------------------------------
    if (typeof body.content !== "string" || body.content.length === 0) {
        return { err: "content is required" };
    }
    const content = body.content;
    if (new TextEncoder().encode(content).byteLength > MAX_CONTENT_BYTES) {
        return { err: `content exceeds ${MAX_CONTENT_BYTES} bytes` };
    }
    const shapeErr = drillShapeError(content, recordings_count);
    if (shapeErr) return { err: shapeErr };

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
        categories = [...seen];
    }

    let difficulty: string | null = null;
    if (body.difficulty != null) {
        if (typeof body.difficulty !== "string" ||
            !ALLOWED_DIFFICULTIES.has(body.difficulty)) {
            return { err: `unknown difficulty: ${String(body.difficulty)}` };
        }
        difficulty = body.difficulty;
    }

    let dll_version: string | null = null;
    if (body.dll_version != null) {
        if (typeof body.dll_version !== "string"
            || !RE_DLL_VERSION.test(body.dll_version)) {
            return { err: "dll_version must match [A-Za-z0-9._-]{1,24}" };
        }
        dll_version = body.dll_version;
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
            dll_version,
        },
    };
}
