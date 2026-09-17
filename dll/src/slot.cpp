#include "slot.hpp"

#include <cstring>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include "write_batch.hpp"

#include "log.hpp"
#include "memory.hpp"
#include "subsystems.hpp"
#include "signatures.hpp"
#include "game_thread.hpp"
#include "slot_labels.hpp"

namespace opendojo::slot {

namespace {
struct Snapshot {
    std::array<CapturedSlot, USER_SLOTS> slots;
    std::uint64_t epoch;
    std::string character;
    std::chrono::steady_clock::time_point captured;
};
std::atomic<std::shared_ptr<const Snapshot>> published;
std::shared_ptr<const Snapshot> snapshot() {
    const auto value = published.load(std::memory_order_acquire);
    return value && value->epoch == game_thread::epoch() &&
                   std::chrono::steady_clock::now() - value->captured < std::chrono::seconds(2)
               ? value
               : nullptr;
}
}  // namespace

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
        case WriteStatus::InvalidRecording: return "invalid or empty recording payload";
        case WriteStatus::NoFreeSlots: return "not enough empty slots - use Replace instead";
        case WriteStatus::StateChanged:
            return "practice state changed or layout unavailable; nothing written";
        case WriteStatus::PartialWrite:
            return "practice memory changed during import; slots may be partially updated";
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
    if (!game_thread::is_current()) {
        const auto value = snapshot();
        return value && slot_idx < USER_SLOTS ? value->slots[slot_idx].kind : Kind::Empty;
    }
    const auto flags_base = signatures::slot_flag_base();
    if (!flags_base) return Kind::Empty;
    if (slot_idx >= USER_SLOTS) return Kind::Empty;
    auto gameplay = subsystems::lookup(subsystems::KEY_GAMEPLAY);
    if (!gameplay) return Kind::Empty;

    // Side 0 only — side handling is a TODO for P2-side users.
    auto flag = memory::read_u32(gameplay + flags_base + slot_idx * 8);
    switch (flag) {
        case FLAG_LIVE: return Kind::Live;
        case FLAG_MOVELIST: return Kind::MoveList;
        default: return Kind::Empty;
    }
}

std::uint16_t event_count(std::size_t slot_idx) {
    if (!game_thread::is_current()) {
        const auto value = snapshot();
        std::uint16_t count = 0;
        if (value && slot_idx < USER_SLOTS && value->slots[slot_idx].bytes.size() >= 2)
            std::memcpy(&count, value->slots[slot_idx].bytes.data(), 2);
        return count;
    }
    if (kind(slot_idx) == Kind::Empty) return 0;
    auto p1 = subsystems::pool1();
    return p1 ? memory::read_u16(p1 + slot_idx * SLOT_PITCH) : std::uint16_t{0};
}

bool is_populated(std::size_t slot_idx) {
    return kind(slot_idx) != Kind::Empty;
}

bool read(std::size_t slot_idx, std::uint8_t* out) {
    if (!game_thread::is_current()) {
        const auto value = snapshot();
        if (!value || !out || slot_idx >= USER_SLOTS ||
            value->slots[slot_idx].bytes.size() != SLOT_PITCH)
            return false;
        std::memcpy(out, value->slots[slot_idx].bytes.data(), SLOT_PITCH);
        return true;
    }
    if (!out || slot_idx >= USER_SLOTS) return false;
    if (kind(slot_idx) == Kind::Empty) return false;
    auto p1 = subsystems::pool1();
    if (!p1) return false;
    memory::read_bytes(p1 + slot_idx * SLOT_PITCH, out, SLOT_PITCH);
    return true;
}

WriteStatus set_recorded_flag(std::size_t slot_idx, bool recorded) {
    if (!game_thread::is_current()) return WriteStatus::StateChanged;
    const auto flags_base = signatures::slot_flag_base();
    if (!flags_base) return WriteStatus::StateChanged;
    if (slot_idx >= USER_SLOTS) return WriteStatus::InvalidSlot;

    // Re-resolve every time — subsystem pointers change at scene transitions
    // (see project_opendojo_subsystem_lifecycle memory). Caching breaks
    // silently after the first scene change.
    auto gameplay = subsystems::lookup(subsystems::KEY_GAMEPLAY);
    auto singleton = subsystems::lookup(subsystems::KEY_SINGLETON);
    auto subB = subsystems::lookup(subsystems::KEY_SUBB);
    auto subC = subsystems::lookup(subsystems::KEY_SUBC);
    if (!gameplay || !singleton || !subB || !subC) {
        return WriteStatus::NotInPracticeMode;
    }

    auto flag_addr = gameplay + flags_base + slot_idx * 8;

    const auto state = signatures::recording_state_layout();
    const auto session = signatures::session_layout();
    if (!state.counter || !session.active_mask) return WriteStatus::StateChanged;
    std::uint32_t word = 0;
    if (!memory::try_read_u32(singleton, &word)) return WriteStatus::StateChanged;
    WriteBatch batch;
    batch.expect(singleton, word);
    batch.add(flag_addr, recorded ? FLAG_LIVE : FLAG_EMPTY);
    batch.add(singleton, recorded ? word | session.active_mask : word & ~session.active_mask);
    batch.add(singleton + state.counter, recorded ? 1u : 0u);
    if (!recorded) batch.add(subB + state.pause, std::uint8_t{1});
    batch.add(subC + state.side_record, recorded ? 1u : 0xFFFFFFFFu);
    const auto result = batch.commit();
    return result == WriteBatch::Result::Ok             ? WriteStatus::Ok
           : result == WriteBatch::Result::PartialWrite ? WriteStatus::PartialWrite
                                                        : WriteStatus::StateChanged;
}

WriteStatus write(std::size_t slot_idx, const std::uint8_t* data) {
    if (!game_thread::is_current()) return WriteStatus::StateChanged;
    const auto flags_base = signatures::slot_flag_base();
    if (!flags_base) return WriteStatus::StateChanged;
    if (slot_idx >= USER_SLOTS) return WriteStatus::InvalidSlot;
    if (!data) return WriteStatus::InvalidSlot;

    auto p1 = subsystems::pool1();
    if (!p1) return WriteStatus::PoolNotAllocated;

    if (!signatures::recording_state_layout().counter || !signatures::session_layout().active_mask)
        return WriteStatus::StateChanged;

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
    const auto layout = signatures::movelist_layout();
    if (!layout.element_stride) return 0;
    auto recordpool = subsystems::lookup(subsystems::KEY_RECORDPOOL);
    if (!gameplay || !recordpool) return 0;
    std::uint64_t begin = 0, end = 0;
    std::uint8_t human_side = 0;
    if (!memory::try_read_u64(recordpool, &begin) ||
        !memory::try_read_u64(recordpool + layout.vector_end, &end) ||
        !memory::try_read_u8(gameplay + layout.human_side, &human_side) || human_side > 1)
        return 0;
    if (!begin || end <= begin || (end - begin) % layout.element_stride != 0) return 0;
    const auto cpu_side = static_cast<std::uint8_t>(human_side ^ 1u);
    const std::size_t n_elem = (end - begin) / layout.element_stride;
    if (cpu_side >= n_elem) return 0;
    auto obj = static_cast<std::uintptr_t>(begin + cpu_side * layout.element_stride);
    return obj + layout.move_ids + slot_idx * 4;
}

std::uint32_t movelist_move_id(std::size_t slot_idx) {
    if (!game_thread::is_current()) {
        const auto value = snapshot();
        return value && slot_idx < USER_SLOTS ? value->slots[slot_idx].move_id : MOVE_ID_NONE;
    }
    auto addr = movelist_addr(slot_idx);
    if (!addr) return MOVE_ID_NONE;
    return memory::read_u32(addr);
}

WriteStatus set_movelist(std::size_t slot_idx, std::uint32_t move_id) {
    if (!game_thread::is_current()) return WriteStatus::StateChanged;
    const auto flags_base = signatures::slot_flag_base();
    if (!flags_base) return WriteStatus::StateChanged;
    if (slot_idx >= USER_SLOTS) return WriteStatus::InvalidSlot;
    auto addr = movelist_addr(slot_idx);
    if (!addr) return WriteStatus::NotInPracticeMode;
    auto gameplay = subsystems::lookup(subsystems::KEY_GAMEPLAY);
    if (!gameplay) return WriteStatus::NotInPracticeMode;
    // Write move ID first, then set the flag so the playback engine never
    // sees flag=1 before move_id is in place.
    memory::write_u32(addr, move_id);
    memory::write_u32(gameplay + flags_base + slot_idx * 8, FLAG_MOVELIST);
    return WriteStatus::Ok;
}

bool capture(std::array<CapturedSlot, USER_SLOTS>& out) {
    if (!game_thread::is_current()) {
        out = {};
        const auto value = snapshot();
        if (!value) return false;
        out = value->slots;
        return true;
    }
    const auto flags_base = signatures::slot_flag_base();
    out = {};
    if (!flags_base) return false;
    const auto gameplay = subsystems::lookup(subsystems::KEY_GAMEPLAY);
    if (!gameplay) return false;
    std::array<CapturedSlot, USER_SLOTS> captured;
    std::array<std::uint32_t, USER_SLOTS> flags{};
    bool need_live = false, need_moves = false;
    for (std::size_t i = 0; i < USER_SLOTS; ++i) {
        if (!memory::try_read_u32(gameplay + flags_base + i * 8, &flags[i]) || flags[i] > FLAG_LIVE)
            return false;
        need_live |= flags[i] == FLAG_LIVE;
        need_moves |= flags[i] == FLAG_MOVELIST;
    }
    const auto pool = need_live ? subsystems::pool1() : 0;
    if (need_live && (!pool || pool > UINTPTR_MAX - USER_SLOTS * SLOT_PITCH)) return false;
    const auto layout = signatures::movelist_layout();
    if (need_moves && !layout.element_stride) return false;
    const auto recordpool = need_moves ? subsystems::lookup(subsystems::KEY_RECORDPOOL) : 0;
    std::uint64_t begin = 0, end = 0;
    std::uint8_t human = 0;
    if (need_moves && (!recordpool || !memory::try_read_u64(recordpool, &begin) ||
                       !memory::try_read_u64(recordpool + layout.vector_end, &end) ||
                       !memory::try_read_u8(gameplay + layout.human_side, &human) || human > 1 ||
                       !begin || end <= begin || (end - begin) % layout.element_stride != 0 ||
                       (end - begin) / layout.element_stride <= (human ^ 1u)))
        return false;
    for (std::size_t i = 0; i < USER_SLOTS; ++i) {
        auto& slot = captured[i];
        if (flags[i] == FLAG_LIVE) {
            slot.kind = Kind::Live;
            slot.bytes.resize(SLOT_PITCH);
            if (!memory::try_read_bytes(pool + i * SLOT_PITCH, slot.bytes.data(), SLOT_PITCH))
                return false;
            std::uint16_t count = 0;
            std::memcpy(&count, slot.bytes.data(), sizeof(count));
            if (count > (SLOT_PITCH - 2) / 4) return false;
        } else if (flags[i] == FLAG_MOVELIST) {
            slot.kind = Kind::MoveList;
            if (!memory::try_read_u32(begin + (human ^ 1u) * layout.element_stride +
                                          layout.move_ids + i * 4,
                                      &slot.move_id) ||
                slot.move_id == MOVE_ID_NONE)
                return false;
        }
    }
    // Revalidate source identities and flags; never publish a knowingly partial
    // capture that could overwrite a good autosave with fewer recordings.
    if (gameplay != subsystems::lookup(subsystems::KEY_GAMEPLAY) ||
        (need_live && pool != subsystems::pool1()))
        return false;
    if (need_moves) {
        std::uint64_t now_begin = 0, now_end = 0;
        std::uint8_t now_human = 0;
        if (recordpool != subsystems::lookup(subsystems::KEY_RECORDPOOL) ||
            !memory::try_read_u64(recordpool, &now_begin) || now_begin != begin ||
            !memory::try_read_u64(recordpool + layout.vector_end, &now_end) || now_end != end ||
            !memory::try_read_u8(gameplay + layout.human_side, &now_human) || now_human != human)
            return false;
    }
    for (std::size_t i = 0; i < USER_SLOTS; ++i) {
        std::uint32_t now = 0;
        if (!memory::try_read_u32(gameplay + flags_base + i * 8, &now) || now != flags[i])
            return false;
    }
    for (std::size_t i = 0; i < USER_SLOTS; ++i)
        captured[i].label = slot_labels::get(i);
    out = std::move(captured);
    return true;
}

WriteStatus import_recordings(const std::vector<drill::Recording>& recordings, bool replace,
                              std::vector<std::size_t>& targets) {
    const auto flags_base = signatures::slot_flag_base();
    targets.clear();
    if (!game_thread::is_current()) return WriteStatus::StateChanged;
    if (!flags_base) return WriteStatus::StateChanged;
    const auto layout = signatures::movelist_layout();
    if (recordings.empty() || recordings.size() > USER_SLOTS) return WriteStatus::InvalidRecording;
    bool live = false, movelist = false;
    for (const auto& rec : recordings) {
        if (rec.kind == drill::Kind::Live) {
            if (rec.slot_bytes.size() != SLOT_PITCH) return WriteStatus::InvalidRecording;
            std::uint16_t count = 0;
            std::memcpy(&count, rec.slot_bytes.data(), sizeof(count));
            if (count > (SLOT_PITCH - 2) / 4) return WriteStatus::InvalidRecording;
            live = true;
        } else if (rec.kind == drill::Kind::MoveList && rec.move_id != MOVE_ID_NONE) {
            movelist = true;
        } else
            return WriteStatus::InvalidRecording;
    }
    if (live && !signatures::live_recordings_supported()) return WriteStatus::StateChanged;
    if (!subsystems::in_practice()) return WriteStatus::NotInPracticeMode;
    const auto gameplay = subsystems::lookup(subsystems::KEY_GAMEPLAY);
    const auto singleton = subsystems::lookup(subsystems::KEY_SINGLETON);
    const auto subB = subsystems::lookup(subsystems::KEY_SUBB);
    const auto subC = subsystems::lookup(subsystems::KEY_SUBC);
    if (!gameplay || !singleton || !subB || !subC) return WriteStatus::NotInPracticeMode;

    const auto state = signatures::recording_state_layout();
    const auto session = signatures::session_layout();
    if ((replace || live) && (!state.counter || !session.active_mask))
        return WriteStatus::StateChanged;
    std::uint32_t session_word = 0;
    if ((replace || live) && !memory::try_read_u32(singleton, &session_word))
        return WriteStatus::StateChanged;
    WriteBatch batch;
    if (replace || live) batch.expect(singleton, session_word);
    std::vector<std::size_t> selected;
    for (std::size_t i = 0; i < USER_SLOTS; ++i) {
        const auto address = gameplay + flags_base + i * 8;
        std::uint32_t flag = 0;
        if (!memory::try_read_u32(address, &flag) || flag > FLAG_LIVE)
            return WriteStatus::StateChanged;
        batch.expect(address, flag);
        if ((replace || flag == FLAG_EMPTY) && selected.size() < recordings.size())
            selected.push_back(i);
    }
    if (selected.size() != recordings.size()) return WriteStatus::NoFreeSlots;

    std::uintptr_t moves = 0, recordpool = 0;
    if (movelist) {
        if (!layout.element_stride) return WriteStatus::StateChanged;
        recordpool = subsystems::lookup(subsystems::KEY_RECORDPOOL);
        std::uint64_t begin = 0, end = 0;
        std::uint8_t human = 0;
        if (!recordpool || !memory::try_read_u64(recordpool, &begin) ||
            !memory::try_read_u64(recordpool + layout.vector_end, &end) ||
            !memory::try_read_u8(gameplay + layout.human_side, &human) || human > 1 || !begin ||
            end <= begin || (end - begin) % layout.element_stride != 0 ||
            (end - begin) / layout.element_stride <= (human ^ 1u))
            return WriteStatus::StateChanged;
        moves = begin + (human ^ 1u) * layout.element_stride + layout.move_ids;
        batch.expect(recordpool, begin);
        batch.expect(recordpool + layout.vector_end, end);
        batch.expect(gameplay + layout.human_side, human);
    }
    // All input/layout checks precede forced pool allocation.
    if (live && !subsystems::pool1()) subsystems::ensure_pool_allocated();
    const auto pool = live ? subsystems::pool1() : 0;
    if (live && !pool) return WriteStatus::PoolNotAllocated;
    if (live && pool > UINTPTR_MAX - USER_SLOTS * SLOT_PITCH) return WriteStatus::StateChanged;

    // Build the entire mutation sequence without modifying game memory.
    if (replace) {
        for (std::size_t i = 0; i < USER_SLOTS; ++i)
            batch.add(gameplay + flags_base + i * 8, FLAG_EMPTY);
        session_word &= ~session.active_mask;
        batch.add(singleton, session_word);
        batch.add(singleton + state.counter, std::uint32_t{0});
        batch.add(subB + state.pause, std::uint8_t{1});
        batch.add(subC + state.side_record, std::uint32_t{0xFFFFFFFF});
    }
    for (std::size_t i = 0; i < recordings.size(); ++i) {
        const auto slot = selected[i];
        const auto& rec = recordings[i];
        if (rec.kind == drill::Kind::Live) {
            batch.add(pool + slot * SLOT_PITCH, rec.slot_bytes.data(), rec.slot_bytes.size());
            batch.add(gameplay + flags_base + slot * 8, FLAG_LIVE);
            session_word |= session.active_mask;
            batch.add(singleton, session_word);
            batch.add(singleton + state.counter, std::uint32_t{1});
            batch.add(subC + state.side_record, std::uint32_t{1});
        } else {
            batch.add(moves + slot * 4, rec.move_id);
            batch.add(gameplay + flags_base + slot * 8, FLAG_MOVELIST);
        }
    }
    // Detect a scene change during preparation, including forced allocation.
    if (!subsystems::in_practice() || gameplay != subsystems::lookup(subsystems::KEY_GAMEPLAY) ||
        singleton != subsystems::lookup(subsystems::KEY_SINGLETON) ||
        subB != subsystems::lookup(subsystems::KEY_SUBB) ||
        subC != subsystems::lookup(subsystems::KEY_SUBC) || (live && pool != subsystems::pool1()) ||
        (movelist && recordpool != subsystems::lookup(subsystems::KEY_RECORDPOOL)))
        return WriteStatus::StateChanged;
    switch (batch.commit()) {
        case WriteBatch::Result::Rejected: return WriteStatus::StateChanged;
        case WriteBatch::Result::PartialWrite: return WriteStatus::PartialWrite;
        case WriteBatch::Result::Ok: targets = std::move(selected); return WriteStatus::Ok;
    }
    return WriteStatus::StateChanged;
}

bool capture_snapshot(std::array<CapturedSlot, USER_SLOTS>& out, std::string_view character) {
    out = {};
    const auto value = published.load(std::memory_order_acquire);
    if (!value || value->character != character ||
        std::chrono::steady_clock::now() - value->captured >= std::chrono::seconds(2))
        return false;
    out = value->slots;
    return true;
}

void publish_snapshot(bool force) {
    if (!game_thread::is_current()) return;
    const auto previous = snapshot();
    if (!force && previous &&
        std::chrono::steady_clock::now() - previous->captured < std::chrono::milliseconds(100))
        return;
    auto value = std::make_shared<Snapshot>();
    value->epoch = game_thread::epoch();
    value->character = game_thread::current_cpu().character_name;
    value->captured = std::chrono::steady_clock::now();
    if (subsystems::in_practice() && capture(value->slots))
        published.store(std::move(value), std::memory_order_release);
    else
        published.store(nullptr, std::memory_order_release);
}

}  // namespace opendojo::slot
