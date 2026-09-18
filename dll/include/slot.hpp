#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>
#include <string_view>
#include "drill.hpp"

// Practice slots: live input recordings use the supported count + four-byte event
// file format. Native allocation, copy and event-consumer code must confirm that
// format before pool access. Movelist entries use a separate native vector.
// Gameplay flags and movelist fields are decoded by signatures at startup.
// The existing first flag-bank behavior is retained; bank selection is not the
// same as human-side selection. Pool2 belongs to a separate, unused feature.

namespace opendojo::slot {

inline constexpr std::size_t SLOT_PITCH = 0x1C22;  // pool1 entry, 7202 bytes
inline constexpr std::size_t USER_SLOTS = 8;

inline constexpr std::uint32_t FLAG_EMPTY = 0u;
inline constexpr std::uint32_t FLAG_MOVELIST = 1u;
inline constexpr std::uint32_t FLAG_LIVE = 2u;

inline constexpr std::uint32_t MOVE_ID_NONE = 0xFFFFFFFFu;

// What's currently in this slot, if anything.
enum class Kind {
    Empty,
    Live,      // user-recorded inputs (flag == 2)
    MoveList,  // move-list "send to slot" entry (flag == 1)
};

const char* kind_name(Kind k);

struct CapturedSlot {
    Kind kind = Kind::Empty;
    std::vector<std::uint8_t> bytes;
    std::uint32_t move_id = MOVE_ID_NONE;
    std::string label;
};
// Operation-local capture. False discards the whole capture on invalid data.
bool capture(std::array<CapturedSlot, USER_SLOTS>& out);
// Publish an owned UI/export snapshot from the native update boundary.
void publish_snapshot(bool force = false);
// Autosave may use the last owned snapshot of the departing character.
bool capture_snapshot(std::array<CapturedSlot, USER_SLOTS>& out, std::string_view character);

// Detect which pool holds slot N's data, if any.
Kind kind(std::size_t slot_idx);

// Result codes for any operation that touches subsystems. Read-only ops
// don't return this — they just return 0 / false since they only need
// the relevant pool to be allocated.
enum class WriteStatus {
    Ok,
    InvalidRecording,
    NoFreeSlots,
    StateChanged,
    PartialWrite,
    InvalidSlot,        // slot_idx out of range
    PoolNotAllocated,   // pool ptr still 0 — record once in practice first
    NotInPracticeMode,  // a subsystem lookup returned 0 — user left the scene
};

const char* describe(WriteStatus s);

// Resolve an operation-local snapshot, validate all payloads/destinations, then
// write. targets is populated only on success. No game-thread atomicity implied.
WriteStatus import_recordings(const std::vector<drill::Recording>& recordings, bool replace,
                              std::vector<std::size_t>& targets);
// Empty every slot and stop the recording session at the game-update boundary.
WriteStatus clear_all();

// Absolute address of slot N's pool1 entry. Returns 0 if pool1 isn't
// allocated yet or slot_idx is out of range. Used by the import path,
// which currently only writes pool1.
std::uintptr_t address(std::size_t slot_idx);

// uint16 event count for slot N, read from whichever pool currently
// holds it (per kind()). 0 if empty or pool not allocated.
std::uint16_t event_count(std::size_t slot_idx);

// True iff kind(slot_idx) != Empty.
bool is_populated(std::size_t slot_idx);

// Copy a live-format pool1 slot into `out` (at least SLOT_PITCH bytes).
// Use capture() for mixed live/movelist contents.
bool read(std::size_t slot_idx, std::uint8_t* out);

// Write 7202 bytes into pool1 slot N and set the per-slot "recorded"
// flag. Pool1-only; the import path doesn't write pool2 yet.
WriteStatus write(std::size_t slot_idx, const std::uint8_t* data);

// Flip just the recorded flag for a pool1 slot (4 writes across
// gameplay / singleton / subB / subC). Exposed for diagnostics & tests.
WriteStatus set_recorded_flag(std::size_t slot_idx, bool recorded);

// Read the move-list move ID for slot N. Returns 0xFFFFFFFF (MOVE_ID_NONE)
// if the slot is not a movelist slot, or the recordpool subsystem isn't
// resolved.
std::uint32_t movelist_move_id(std::size_t slot_idx);

// Write the move-list move ID for slot N + set the movelist flag.
// The move ID is what the in-game "Select from Move List → send to
// slot" UI would have stored. Returns Ok on success.
WriteStatus set_movelist(std::size_t slot_idx, std::uint32_t move_id);

}  // namespace opendojo::slot
