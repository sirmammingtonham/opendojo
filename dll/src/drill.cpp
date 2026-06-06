#include "drill.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

namespace opendojo::drill {

namespace {

// Max events that fit in one fixed-size slot. The slot is SLOT_PITCH bytes:
// a 2-byte event-count header followed by 4 bytes per event. A recording with
// more events than this would overflow the fixed slot buffer, so the decoder
// rejects it (and pack_events clamps to it defensively).
constexpr std::size_t MAX_EVENTS = (SLOT_PITCH - 2) / 4;

// ---- Event-level encoding --------------------------------------------------

constexpr std::uint8_t DEFAULT_MARK = 0x2;
constexpr std::uint8_t DEFAULT_AUX = 0xA0;

struct DirRow {
    std::uint8_t nibble;
    const char* text;
};
constexpr std::array<DirRow, 9> DIR_TABLE = {{
    {0, "n"},
    {1, "u"},
    {2, "d"},
    {4, "f"},
    {5, "uf"},
    {6, "df"},
    {8, "b"},
    {9, "ub"},
    {10, "db"},
}};

const char* dir_to_text(std::uint8_t nibble) {
    for (const auto& row : DIR_TABLE)
        if (row.nibble == nibble) return row.text;
    return nullptr;
}

int text_to_dir(std::string_view tok) {
    for (const auto& row : DIR_TABLE)
        if (tok == row.text) return row.nibble;
    return -1;
}

struct ButtonEncode {
    std::string text;
    std::uint8_t unknown;
};

ButtonEncode encode_buttons(std::uint8_t byte1) {
    ButtonEncode out;
    auto add = [&](char c, std::uint8_t bit) {
        if (byte1 & bit) {
            if (!out.text.empty()) out.text += '+';
            out.text += c;
        }
    };
    add('1', 0x40);
    add('2', 0x80);
    add('3', 0x10);
    add('4', 0x20);
    if (out.text.empty()) out.text = ".";
    out.unknown = byte1 & 0x0F;
    return out;
}

int parse_buttons(std::string_view tok) {
    if (tok == ".") return 0;
    int mask = 0;
    std::size_t pos = 0;
    while (pos < tok.size()) {
        auto end = tok.find('+', pos);
        if (end == std::string_view::npos) end = tok.size();
        auto sub = tok.substr(pos, end - pos);
        if (sub == "1")
            mask |= 0x40;
        else if (sub == "2")
            mask |= 0x80;
        else if (sub == "3")
            mask |= 0x10;
        else if (sub == "4")
            mask |= 0x20;
        else
            return -1;
        pos = (end < tok.size()) ? end + 1 : end;
    }
    return mask;
}

// ---- Generic line helpers --------------------------------------------------

std::string_view trim(std::string_view s) {
    auto is_ws = [](char c) { return c == ' ' || c == '\t'; };
    while (!s.empty() && is_ws(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && is_ws(s.back()))
        s.remove_suffix(1);
    return s;
}

std::string_view strip_comment(std::string_view s) {
    auto pos = s.find('#');
    return (pos == std::string_view::npos) ? s : s.substr(0, pos);
}

long long parse_hex(std::string_view s) {
    if (s.empty()) return -1;
    long long n = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F')
            d = 10 + (c - 'A');
        else
            return -1;
        n = (n << 4) | d;
    }
    return n;
}

long long parse_dec(std::string_view s) {
    if (s.empty()) return -1;
    long long n = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return -1;
        n = n * 10 + (c - '0');
    }
    return n;
}

std::vector<std::string_view> split_ws(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
            ++i;
        std::size_t start = i;
        while (i < s.size() && !(s[i] == ' ' || s[i] == '\t'))
            ++i;
        if (start < i) out.push_back(s.substr(start, i - start));
    }
    return out;
}

bool is_section_marker(std::string_view line) {
    return line.size() >= 3 && line.substr(0, 3) == "---";
}

// `key: value` style header line. Used to distinguish metadata from events
// within a section.
bool parse_kv(std::string_view line, std::string_view& key, std::string_view& val) {
    auto colon = line.find(':');
    if (colon == std::string_view::npos) return false;
    key = trim(line.substr(0, colon));
    val = trim(line.substr(colon + 1));
    if (key.empty()) return false;
    for (char c : key) {
        bool ident = (c == '_') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9');
        if (!ident) return false;
    }
    return true;
}

// ---- Event-line encode / decode -------------------------------------------

std::string encode_event_line(std::uint8_t b0, std::uint8_t b1, std::uint8_t b2, std::uint8_t b3) {
    std::uint8_t mark = (b0 >> 4) & 0x0F;
    std::uint8_t dir = b0 & 0x0F;

    const char* dir_text = dir_to_text(dir);
    std::string dir_raw_annot;
    if (!dir_text) {
        dir_text = "n";
        char buf[16];
        std::snprintf(buf, sizeof(buf), "dir_raw=%X", dir);
        dir_raw_annot = buf;
    }

    auto btn = encode_buttons(b1);

    char line[128];
    std::snprintf(line, sizeof(line), "  %-2s   %-5s  %4u", dir_text, btn.text.c_str(),
                  static_cast<unsigned>(b3));
    std::string result = line;

    const bool need_meta = (mark != DEFAULT_MARK) || (btn.unknown != 0) || (b2 != DEFAULT_AUX);
    if (!dir_raw_annot.empty() || need_meta) {
        result += "   ";
        if (!dir_raw_annot.empty()) {
            result += dir_raw_annot;
            if (need_meta) result += ' ';
        }
        if (need_meta) {
            char meta[16];
            std::snprintf(meta, sizeof(meta), "meta=%X%X%02X", mark, btn.unknown, b2);
            result += meta;
        }
    }
    return result;
}

bool decode_event_line(std::string_view line, std::array<std::uint8_t, 4>& ev, std::string& err) {
    auto toks = split_ws(line);
    if (toks.size() < 3) {
        err = "expected at least 3 tokens (dir buttons frames), got: ";
        err.append(line.data(), line.size());
        return false;
    }

    const int dir = text_to_dir(toks[0]);
    if (dir < 0) {
        err = "unknown direction: ";
        err.append(toks[0].data(), toks[0].size());
        return false;
    }

    const int btn_mask = parse_buttons(toks[1]);
    if (btn_mask < 0) {
        err = "bad buttons: ";
        err.append(toks[1].data(), toks[1].size());
        return false;
    }

    const long long frames = parse_dec(toks[2]);
    if (frames < 0 || frames > 255) {
        err = "bad frame count: ";
        err.append(toks[2].data(), toks[2].size());
        return false;
    }

    int mark = DEFAULT_MARK;
    int btn_raw = 0;
    int aux = DEFAULT_AUX;
    int dir_raw = -1;

    for (std::size_t i = 3; i < toks.size(); ++i) {
        auto eq = toks[i].find('=');
        if (eq == std::string_view::npos) {
            err = "expected key=value, got: ";
            err.append(toks[i].data(), toks[i].size());
            return false;
        }
        auto key = toks[i].substr(0, eq);
        auto val = toks[i].substr(eq + 1);
        long long n = parse_hex(val);
        if (n < 0) {
            err = "bad hex value in annotation: ";
            err.append(toks[i].data(), toks[i].size());
            return false;
        }

        if (key == "meta") {
            if (n > 0xFFFF) {
                err = "meta out of range: ";
                err.append(val.data(), val.size());
                return false;
            }
            const auto v = static_cast<std::uint16_t>(n);
            mark = (v >> 12) & 0xF;
            btn_raw = (v >> 8) & 0xF;
            aux = v & 0xFF;
        } else if (key == "mark") {
            mark = static_cast<int>(n);
        } else if (key == "btn_raw") {
            btn_raw = static_cast<int>(n);
        } else if (key == "aux") {
            aux = static_cast<int>(n);
        } else if (key == "dir_raw") {
            dir_raw = static_cast<int>(n);
        } else {
            err = "unknown annotation key: ";
            err.append(key.data(), key.size());
            return false;
        }
    }

    const std::uint8_t b0_dir = (dir_raw >= 0) ? static_cast<std::uint8_t>(dir_raw & 0xF)
                                               : static_cast<std::uint8_t>(dir & 0xF);
    ev = {
        static_cast<std::uint8_t>(((mark & 0xF) << 4) | b0_dir),
        static_cast<std::uint8_t>((btn_mask & 0xF0) | (btn_raw & 0x0F)),
        static_cast<std::uint8_t>(aux),
        static_cast<std::uint8_t>(frames),
    };
    return true;
}

// ---- Slot-payload helpers --------------------------------------------------

void slot_count_and_frames(const std::uint8_t* slot, std::uint16_t& events, std::uint32_t& frames) {
    events = static_cast<std::uint16_t>(slot[0]) | (static_cast<std::uint16_t>(slot[1]) << 8);
    frames = 0;
    for (std::uint16_t i = 0; i < events; ++i) {
        std::size_t off = 2 + std::size_t{i} * 4;
        if (off + 3 >= SLOT_PITCH) break;
        frames += slot[off + 3];
    }
}

void encode_recording(std::string& out, const Recording& r, std::size_t idx_one_based) {
    char hdr[128];
    std::snprintf(hdr, sizeof(hdr), "--- recording %zu\n", idx_one_based);
    out += hdr;
    std::snprintf(hdr, sizeof(hdr), "name:         %s\n", r.name.c_str());
    out += hdr;
    // Only emit `kind:` for non-live; old files implicitly mean live.
    if (r.kind != Kind::Live) {
        out += "kind:         movelist\n";
    }

    if (r.kind == Kind::MoveList) {
        // Movelist entries are just a move ID — no per-frame events.
        std::snprintf(hdr, sizeof(hdr), "move_id:      %u\n", static_cast<unsigned>(r.move_id));
        out += hdr;
        return;
    }

    std::snprintf(hdr, sizeof(hdr), "events:       %u\n", static_cast<unsigned>(r.event_count));
    out += hdr;
    std::snprintf(hdr, sizeof(hdr), "total_frames: %u\n", static_cast<unsigned>(r.total_frames));
    out += hdr;
    out += "#\n";

    for (std::uint16_t i = 0; i < r.event_count; ++i) {
        std::size_t off = 2 + std::size_t{i} * 4;
        if (off + 3 >= SLOT_PITCH || off + 3 >= r.slot_bytes.size()) break;
        out += encode_event_line(r.slot_bytes[off], r.slot_bytes[off + 1], r.slot_bytes[off + 2],
                                 r.slot_bytes[off + 3]);
        out += '\n';
    }
}

// Build the SLOT_PITCH-byte payload from an in-memory event list.
std::vector<std::uint8_t> pack_events(const std::vector<std::array<std::uint8_t, 4>>& events) {
    std::vector<std::uint8_t> out(SLOT_PITCH, 0);
    // Never write more than the slot can hold. decode_text rejects oversize
    // recordings before reaching here, but clamp regardless so a future caller
    // can't turn an oversize event list into an out-of-bounds heap write.
    const std::size_t n = std::min(events.size(), MAX_EVENTS);
    const auto cnt = static_cast<std::uint16_t>(n);
    out[0] = static_cast<std::uint8_t>(cnt & 0xFF);
    out[1] = static_cast<std::uint8_t>((cnt >> 8) & 0xFF);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t off = 2 + i * 4;
        out[off] = events[i][0];
        out[off + 1] = events[i][1];
        out[off + 2] = events[i][2];
        out[off + 3] = events[i][3];
    }
    return out;
}

}  // anonymous namespace

// ---------------------------------------------------------------------------

std::string slugify(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    bool last_us = false;
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out += c;
            last_us = false;
        } else if (c >= 'A' && c <= 'Z') {
            out += static_cast<char>(c - 'A' + 'a');
            last_us = false;
        } else {
            if (!last_us && !out.empty()) {
                out += '_';
                last_us = true;
            }
        }
    }
    while (!out.empty() && out.back() == '_')
        out.pop_back();
    if (out.empty()) out = "drill";
    return out;
}

Recording make_live_recording(std::string name, const std::uint8_t* slot_bytes) {
    Recording r;
    r.name = std::move(name);
    r.kind = Kind::Live;
    r.slot_bytes.assign(slot_bytes, slot_bytes + SLOT_PITCH);
    slot_count_and_frames(r.slot_bytes.data(), r.event_count, r.total_frames);
    return r;
}

Recording make_movelist_recording(std::string name, std::uint32_t move_id) {
    Recording r;
    r.name = std::move(name);
    r.kind = Kind::MoveList;
    r.move_id = move_id;
    return r;
}

std::string encode_text(const Drill& d) {
    std::string out;
    out.reserve(1024 + d.recordings.size() * 256);

    char buf[256];
    out += "# OpenDojo drill\n";
    std::snprintf(buf, sizeof(buf), "name:         %s\n", d.name.c_str());
    out += buf;
    std::snprintf(buf, sizeof(buf), "description:  %s\n", d.description.c_str());
    out += buf;
    std::snprintf(buf, sizeof(buf), "character:    %s\n",
                  d.character.empty() ? "unknown" : d.character.c_str());
    out += buf;
    if (!d.cpu_side.empty()) {
        std::snprintf(buf, sizeof(buf), "cpu_side:     %s\n", d.cpu_side.c_str());
        out += buf;
    }
    std::snprintf(buf, sizeof(buf), "recordings:   %zu\n", d.recordings.size());
    out += buf;
    out +=
        "#\n"
        "# === Editing this drill ===\n"
        "# Each recording below is one slot's worth of inputs. The event line\n"
        "# format is:  <dir>  <buttons>  <frames>  [meta=NNNN]\n"
        "#\n"
        "# Editable fields:\n"
        "#   dir       n f b u d, uf df ub db\n"
        "#   buttons   1 2 3 4 (LP RP LK RK), combined with + (e.g. 1+2), or .\n"
        "#   frames    duration in frames (60fps)\n"
        "#\n"
        "# meta=NNNN  recorder metadata — DO NOT EDIT. Encodes engine state\n"
        "#            (input gate, raw input flags, per-frame sync hash).\n"
        "#            Default (meta=20A0) is omitted.\n"
        "#\n";

    for (std::size_t i = 0; i < d.recordings.size(); ++i) {
        out += '\n';
        encode_recording(out, d.recordings[i], i + 1);
    }
    return out;
}

TextResult decode_text(std::string_view text) {
    TextResult result;

    enum class Section { Drill, Recording };
    Section section = Section::Drill;
    Recording current;
    std::vector<std::array<std::uint8_t, 4>> events;

    auto flush_recording = [&]() {
        if (section == Section::Recording) {
            // Movelist recordings carry only a move_id; no event lines.
            if (current.kind != Kind::MoveList) {
                current.slot_bytes = pack_events(events);
                current.event_count = static_cast<std::uint16_t>(events.size());
                current.total_frames = 0;
                for (auto& e : events)
                    current.total_frames += e[3];
            }
            result.drill.recordings.push_back(std::move(current));
            current = Recording{};
            events.clear();
        }
    };

    std::size_t pos = 0;
    while (pos <= text.size()) {
        auto eol = text.find_first_of("\r\n", pos);
        if (eol == std::string_view::npos) eol = text.size();
        auto raw_line = text.substr(pos, eol - pos);
        pos = eol;
        if (pos < text.size() && text[pos] == '\r') ++pos;
        if (pos < text.size() && text[pos] == '\n') ++pos;
        if (raw_line.empty() && pos >= text.size()) break;

        auto line = trim(strip_comment(raw_line));
        if (line.empty()) continue;

        if (is_section_marker(line)) {
            flush_recording();
            section = Section::Recording;
            // Allow `--- recording N` or `--- recording N: name` styles; we
            // recover `name` from the per-recording `name:` field, so the
            // marker line itself is purely a section break.
            continue;
        }

        std::string_view key, val;
        const bool is_header = parse_kv(line, key, val);

        if (section == Section::Drill) {
            if (!is_header) {
                result.error = "expected key:value in drill header, got: ";
                result.error.append(line.data(), line.size());
                return result;
            }
            if (key == "name")
                result.drill.name = std::string(val);
            else if (key == "description")
                result.drill.description = std::string(val);
            else if (key == "character")
                result.drill.character = std::string(val);
            else if (key == "cpu_side")
                result.drill.cpu_side = std::string(val);
            else if (key == "recordings") { /* informational only — count comes from data */
            } else {
                // Unknown drill-level keys are skipped so the format can
                // gain new optional fields without breaking older readers.
            }
        } else {  // Section::Recording
            if (is_header) {
                if (key == "name")
                    current.name = std::string(val);
                else if (key == "kind") {
                    if (val == "movelist")
                        current.kind = Kind::MoveList;
                    else if (val == "live")
                        current.kind = Kind::Live;
                    else {
                        result.error = "unknown recording kind: ";
                        result.error.append(val.data(), val.size());
                        return result;
                    }
                } else if (key == "move_id") {
                    try {
                        current.move_id = static_cast<std::uint32_t>(std::stoul(std::string(val)));
                    } catch (...) {
                        result.error = "bad move_id: ";
                        result.error.append(val.data(), val.size());
                        return result;
                    }
                } else if (key == "events") {       /* informational */
                } else if (key == "total_frames") { /* informational */
                } else {
                    // Unknown per-recording keys are skipped — same forward-
                    // compatibility rationale as drill-level keys above.
                }
            } else {
                std::array<std::uint8_t, 4> ev{};
                if (!decode_event_line(line, ev, result.error)) return result;
                events.push_back(ev);
                // Bound the event list as it grows. flush_recording() packs it
                // into a fixed SLOT_PITCH buffer; catching the overflow here —
                // before the next flush — keeps a malicious drill from writing
                // past that buffer. (The post-parse cap check below is now a
                // backstop for the same invariant.)
                if (events.size() > MAX_EVENTS) {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf),
                                  "recording %zu has more than %zu events",
                                  result.drill.recordings.size() + 1, MAX_EVENTS);
                    result.error = buf;
                    result.drill.recordings.clear();
                    return result;
                }
            }
        }
    }
    flush_recording();

    if (result.drill.recordings.empty()) {
        result.error = "drill contains no recordings";
        return result;
    }

    for (std::size_t i = 0; i < result.drill.recordings.size(); ++i) {
        if (result.drill.recordings[i].event_count > MAX_EVENTS) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "recording %zu has %u events (max %zu)", i + 1,
                          static_cast<unsigned>(result.drill.recordings[i].event_count),
                          MAX_EVENTS);
            result.error = buf;
            result.drill.recordings.clear();
            return result;
        }
    }

    if (result.drill.character.empty()) result.drill.character = "unknown";
    return result;
}

}  // namespace opendojo::drill
