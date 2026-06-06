#pragma once

#include <cstddef>
#include <cstdint>

// Practice-mode slot read/write.
//
// Both live recordings AND move-list "send to slot" entries live in
// pool1 (`+0x986AC70`), 7202 bytes per slot, max ~1800 events. The
// event format is 4 bytes — `+0..1` dir/mark+buttons word, `+2` aux,
// `+3` frames — preceded by a uint16 event_count at slot offset 0.
//
// What distinguishes the two is the per-slot flag uint32 at
// `gameplay + 0x484 + (slot + side*8)*8`:
//
//   0 = empty
//   1 = move-list (programmatic move sequence)
//   2 = live (user-recorded inputs)
//
// (Empirically confirmed via dump_flag_state() — see RE_NOTES.)
//
// pool2 (`+0x986AC78`, 1202 B/slot) and the +0x504/+0x544 flag arrays
// stay at zero during both live recording and move-list select; they
// belong to a separate feature (Action Interval Recording is the
// likely owner). OpenDojo currently ignores them.

namespace opendojo::slot {

inline constexpr std::size_t SLOT_PITCH = 0x1C22;  // pool1 entry, 7202 bytes
inline constexpr std::size_t USER_SLOTS = 8;

// Per-slot flag array within the gameplay subsystem. Side 0 only — see
// the file-header note for the side caveat. Flag values:
//   0 = empty
//   1 = move-list (programmatic move sequence)
//   2 = live (user-recorded)
inline constexpr std::uintptr_t GAMEPLAY_SLOT_BASE = 0x480;
inline constexpr std::uintptr_t GAMEPLAY_SLOT_STRIDE = 0x08;
inline constexpr std::uintptr_t GAMEPLAY_SLOT_FLAG = 0x04;  // uint32 within each 8B entry
inline constexpr std::uint32_t FLAG_EMPTY = 0u;
inline constexpr std::uint32_t FLAG_MOVELIST = 1u;
inline constexpr std::uint32_t FLAG_LIVE = 2u;

// Move-list payload storage. All 8 movelist slots share a single
// uint32[8] array stored at:
//   recordpool[cpu_side].obj + 0x44 + slot*4
// where recordpool = lookup(KEY_RECORDPOOL), each element is 0x140 bytes,
// cpu_side = gameplay[+0x47C] XOR 1. The dispatcher FUN_145f24060 writes
// the move ID via FUN_14191f220 (verified via flag dump). A move ID of
// 0xFFFFFFFF means "no movelist set" (the cleared sentinel).
inline constexpr std::uintptr_t RECORDPOOL_OBJ_STRIDE = 0x140;
inline constexpr std::uintptr_t RECORDPOOL_MOVE_ID_BASE = 0x44;  // obj + 0x44 + slot*4
inline constexpr std::uint32_t MOVE_ID_NONE = 0xFFFFFFFFu;

// What's currently in this slot, if anything.
enum class Kind {
    Empty,
    Live,      // user-recorded inputs (flag == 2)
    MoveList,  // move-list "send to slot" entry (flag == 1)
};

const char* kind_name(Kind k);

// Detect which pool holds slot N's data, if any.
Kind kind(std::size_t slot_idx);

// Result codes for any operation that touches subsystems. Read-only ops
// don't return this — they just return 0 / false since they only need
// the relevant pool to be allocated.
enum class WriteStatus {
    Ok,
    InvalidSlot,        // slot_idx out of range
    PoolNotAllocated,   // pool ptr still 0 — record once in practice first
    NotInPracticeMode,  // a subsystem lookup returned 0 — user left the scene
};

const char* describe(WriteStatus s);

// Absolute address of slot N's pool1 entry. Returns 0 if pool1 isn't
// allocated yet or slot_idx is out of range. Used by the import path,
// which currently only writes pool1.
std::uintptr_t address(std::size_t slot_idx);

// uint16 event count for slot N, read from whichever pool currently
// holds it (per kind()). 0 if empty or pool not allocated.
std::uint16_t event_count(std::size_t slot_idx);

// True iff kind(slot_idx) != Empty.
bool is_populated(std::size_t slot_idx);

// Copy slot N's data into `out` (which must be at least SLOT_PITCH
// bytes). For move-list slots the actual pool2 bytes (POOL2_PITCH) are
// copied and the trailing tail is zero-padded so downstream code that
// assumes SLOT_PITCH-sized buffers doesn't read stale memory.
// Returns false if the slot is empty or the relevant pool isn't allocated.
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
