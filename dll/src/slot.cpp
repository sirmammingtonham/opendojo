#include "slot.hpp"

#include <cstring>

#include "log.hpp"
#include "memory.hpp"
#include "subsystems.hpp"

namespace opendojo::slot {

const char* kind_name(Kind k) {
    switch (k) {
        case Kind::Empty: return "empty";
        case Kind::Live: return "live";
        case Kind::MoveList: return "movelist";
    }
    return "unknown";
}

const char* describe(WriteStatus s) {
    switch (s) {
        case WriteStatus::Ok: return "ok";
        case WriteStatus::InvalidSlot: return "invalid slot index";
        case WriteStatus::PoolNotAllocated:
            return "pool1 not allocated — record once in practice mode first";
        case WriteStatus::NotInPracticeMode: return "not in practice mode (subsystem unresolved)";
    }
    return "unknown";
}

std::uintptr_t address(std::size_t slot_idx) {
    if (slot_idx >= USER_SLOTS) return 0;
    auto p1 = subsystems::pool1();
    if (!p1) return 0;
    return p1 + slot_idx * SLOT_PITCH;
}

Kind kind(std::size_t slot_idx) {
    if (slot_idx >= USER_SLOTS) return Kind::Empty;
    auto gameplay = subsystems::lookup(subsystems::KEY_GAMEPLAY);
    if (!gameplay) return Kind::Empty;

    // Side 0 only — side handling is a TODO for P2-side users.
    auto flag = memory::read_u32(gameplay + GAMEPLAY_SLOT_BASE + slot_idx * GAMEPLAY_SLOT_STRIDE +
                                 GAMEPLAY_SLOT_FLAG);
    switch (flag) {
        case FLAG_LIVE: return Kind::Live;
        case FLAG_MOVELIST: return Kind::MoveList;
        default: return Kind::Empty;
    }
}

std::uint16_t event_count(std::size_t slot_idx) {
    if (kind(slot_idx) == Kind::Empty) return 0;
    auto p1 = subsystems::pool1();
    return p1 ? memory::read_u16(p1 + slot_idx * SLOT_PITCH) : std::uint16_t{0};
}

bool is_populated(std::size_t slot_idx) {
    return kind(slot_idx) != Kind::Empty;
}

bool read(std::size_t slot_idx, std::uint8_t* out) {
    if (!out || slot_idx >= USER_SLOTS) return false;
    if (kind(slot_idx) == Kind::Empty) return false;
    auto p1 = subsystems::pool1();
    if (!p1) return false;
    memory::read_bytes(p1 + slot_idx * SLOT_PITCH, out, SLOT_PITCH);
    return true;
}

WriteStatus set_recorded_flag(std::size_t slot_idx, bool recorded) {
    if (slot_idx >= USER_SLOTS) return WriteStatus::InvalidSlot;

    // Re-resolve every time — subsystem pointers change at scene transitions
    // (see project_opendojo_subsystem_lifecycle memory). Caching breaks
    // silently after the first scene change.
    auto gameplay = subsystems::lookup(subsystems::KEY_GAMEPLAY);
    auto singleton = subsystems::lookup(subsystems::KEY_SINGLETON);
    auto subB = subsystems::lookup(subsystems::KEY_SUBB);
    auto subC = subsystems::lookup(subsystems::KEY_SUBC);
    if (!gameplay || !singleton || !subB || !subC) { return WriteStatus::NotInPracticeMode; }

    auto flag_addr = gameplay + GAMEPLAY_SLOT_BASE + slot_idx * GAMEPLAY_SLOT_STRIDE +
                     GAMEPLAY_SLOT_FLAG;

    if (recorded) {
        memory::write_u32(flag_addr, 2u);  // per-slot "has recording" sentinel
        memory::write_u8(singleton + 0x002,
                         0x40u);  // playback side-gate (P1=0x40); without it P2-side drills mirror
        memory::write_u8(singleton + 0x008,
                         0x01u);  // recording-state flag — best guess "session present"
        // subB[0x065] = 0 is omitted on purpose: writing 0 mid-intro
        // locks character input. Best guess: "playback session armed";
        // bisected as the sole single write that triggers the freeze.
        memory::write_u32(subC + 0x25C, 1u);  // global "≥1 slot is recorded" counter
    } else {
        memory::write_u32(flag_addr, 0u);            // clear per-slot sentinel
        memory::write_u8(singleton + 0x002, 0x00u);  // clear side-gate
        memory::write_u8(singleton + 0x008, 0x00u);  // clear session-present
        memory::write_u8(subB + 0x065, 0x01u);       // baseline "no playback" — safe during intro
        memory::write_u32(subC + 0x25C,
                          0xFFFFFFFFu);  // -1 == "no recordings" (game's baseline-pass value)
    }
    return WriteStatus::Ok;
}

WriteStatus write(std::size_t slot_idx, const std::uint8_t* data) {
    if (slot_idx >= USER_SLOTS) return WriteStatus::InvalidSlot;
    if (!data) return WriteStatus::InvalidSlot;

    auto p1 = subsystems::pool1();
    if (!p1) return WriteStatus::PoolNotAllocated;

    // Write the slot bytes first; the in-game tick handler reads the per-slot
    // flag, so we want the data to be in place before the flag flips.
    memory::write_bytes(p1 + slot_idx * SLOT_PITCH, data, SLOT_PITCH);
    return set_recorded_flag(slot_idx, true);
}

// Resolve the address where slot N's movelist move ID lives.
// Returns 0 if any link in the chain is unresolved.
static std::uintptr_t movelist_addr(std::size_t slot_idx) {
    if (slot_idx >= USER_SLOTS) return 0;
    auto gameplay = subsystems::lookup(subsystems::KEY_GAMEPLAY);
    auto recordpool = subsystems::lookup(subsystems::KEY_RECORDPOOL);
    if (!gameplay || !recordpool) return 0;
    auto begin = memory::read_u64(recordpool);
    auto end = memory::read_u64(recordpool + 8);
    if (!begin || end <= begin) return 0;
    auto cpu_side = static_cast<std::uint8_t>(memory::read_u8(gameplay + 0x47C) ^ 1u);
    std::size_t n_elem = (end - begin) / RECORDPOOL_OBJ_STRIDE;
    if (cpu_side >= n_elem) return 0;
    auto obj = static_cast<std::uintptr_t>(begin + cpu_side * RECORDPOOL_OBJ_STRIDE);
    return obj + RECORDPOOL_MOVE_ID_BASE + slot_idx * 4;
}

std::uint32_t movelist_move_id(std::size_t slot_idx) {
    auto addr = movelist_addr(slot_idx);
    if (!addr) return MOVE_ID_NONE;
    return memory::read_u32(addr);
}

WriteStatus set_movelist(std::size_t slot_idx, std::uint32_t move_id) {
    if (slot_idx >= USER_SLOTS) return WriteStatus::InvalidSlot;
    auto addr = movelist_addr(slot_idx);
    if (!addr) return WriteStatus::NotInPracticeMode;
    auto gameplay = subsystems::lookup(subsystems::KEY_GAMEPLAY);
    if (!gameplay) return WriteStatus::NotInPracticeMode;
    // Write move ID first, then set the flag so the playback engine never
    // sees flag=1 before move_id is in place.
    memory::write_u32(addr, move_id);
    memory::write_u32(gameplay + GAMEPLAY_SLOT_BASE + slot_idx * GAMEPLAY_SLOT_STRIDE +
                          GAMEPLAY_SLOT_FLAG,
                      FLAG_MOVELIST);
    return WriteStatus::Ok;
}

}  // namespace opendojo::slot
