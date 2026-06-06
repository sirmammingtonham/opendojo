#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Drill file format. One file = one drill = N recordings (N >= 1). A
// "single recording" is the N=1 case — same format, same parser.
// Multi-recording drills enable shareable scenarios like "Jin string
// defense" with several related recordings grouped together.
//
// Encoder produces text. Decoder reads text. Each recording carries a
// 7202-byte slot payload alongside text-decoded event lines (dir /
// buttons / frames / meta=NNNN).

namespace opendojo::drill {

inline constexpr std::size_t SLOT_PITCH = 0x1C22;  // mirrors opendojo::slot::SLOT_PITCH

// Source pool for a recording. Default Live (pool1) matches every drill
// produced before this field existed, so older files without a `kind:`
// header decode correctly.
enum class Kind {
    Live,      // pool1, 7202-byte slot
    MoveList,  // pool2, 1202-byte slot
};

struct Recording {
    std::string name;  // human-readable; "" => unnamed
    Kind kind = Kind::Live;
    // Live-only: 7202 bytes of pool1 slot data + derived counters.
    std::uint16_t event_count = 0;
    std::uint32_t total_frames = 0;
    std::vector<std::uint8_t> slot_bytes;  // exactly SLOT_PITCH bytes (live)
    // MoveList-only: the move ID the in-game "Select from Move List → send
    // to slot" UI stored. Reproduced verbatim on import. Same game version
    // is required for the ID to resolve to the right move.
    std::uint32_t move_id = 0;
};

struct Drill {
    std::string name;         // human-readable drill name
    std::string description;  // single-line free-form
    std::string character;    // lowercase id; "unknown" if unset
    std::string cpu_side;     // "p1" / "p2" / "" (unset)
    std::vector<Recording> recordings;
};

// Encode a drill to text. Always emits the full structure even if N==1.
std::string encode_text(const Drill& d);

struct TextResult {
    Drill drill;
    std::string error;  // empty on success
};

// Decode drill text. On error, `drill.recordings` is empty and `error`
// describes why.
TextResult decode_text(std::string_view text);

// Build a Recording from raw 7202-byte slot payload (the same bytes returned
// by opendojo::slot::read). Populates event_count and total_frames from the
// payload's leading uint16 + per-event frame field. `name` is stored as-is.
Recording make_live_recording(std::string name, const std::uint8_t* slot_bytes);

// Build a movelist Recording. The move_id is whatever the in-game
// "Select from Move List → send to slot" UI stored (one uint32).
Recording make_movelist_recording(std::string name, std::uint32_t move_id);

// Convert a drill / recording name to a filesystem-safe lowercase slug.
// Non-alphanumeric runs collapse to a single underscore; leading/trailing
// underscores are trimmed; empty input returns "drill".
//   "Jin String Defense"  -> "jin_string_defense"
//   "ff+3   sweep!"       -> "ff_3_sweep"
//   ""                    -> "drill"
std::string slugify(std::string_view name);

}  // namespace opendojo::drill
