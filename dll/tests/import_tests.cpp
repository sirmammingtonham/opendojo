#include "slot.hpp"
#include "game_thread.hpp"
#include "signatures.hpp"
#include "subsystems.hpp"
#include "memory.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
std::array<std::uint8_t, 2048> gameplay{}, singleton{}, subB{}, subC{};
std::array<std::uint8_t, opendojo::slot::USER_SLOTS * opendojo::slot::SLOT_PITCH> pool{};
std::array<std::uint8_t, 3 * 0x180> moves{};
std::array<std::uint64_t, 3> recordpool{};
opendojo::signatures::MovelistLayout discovered{0x47C, 0x148, 0x44, 8};
opendojo::signatures::RecordingStateLayout recording_fields{8, 0x65, 0x25C, 0x28};
opendojo::signatures::SessionLayout session_fields{0x88, 0x3A50, 0x99, 0x400000};
std::uint32_t flags_base = 0x484;
bool format_supported = true;
bool owner_context = true;
std::uint64_t current_epoch = 1;
bool practice = true;
bool available = true;
int allocations = 0;
template <typename T>
std::uintptr_t address(T& value) {
    return reinterpret_cast<std::uintptr_t>(value.data());
}
void check(bool value) {
    if (!value) throw std::runtime_error("import test failed");
}
void reset() {
    gameplay.fill(0);
    singleton.fill(0);
    subB.fill(0);
    subC.fill(0);
    pool.fill(0);
    moves.fill(0);
    practice = true;
    available = true;
    allocations = 0;
    recording_fields = {8, 0x65, 0x25C, 0x28};
    session_fields = {0x88, 0x3A50, 0x99, 0x400000};
    flags_base = 0x484;
    format_supported = true;
    discovered = {0x47C, 0x148, 0x44, 8};
    recordpool = {address(moves), address(moves) + 3 * discovered.element_stride, 0};
}
std::uint32_t flag(std::size_t slot) {
    return opendojo::memory::read_u32(address(gameplay) + flags_base + slot * 8);
}
}  // namespace

namespace opendojo::signatures {
bool live_recordings_supported() {
    return format_supported;
}
RecordingStateLayout recording_state_layout() {
    return recording_fields;
}
SessionLayout session_layout() {
    return session_fields;
}
std::uint32_t slot_flag_base() {
    return flags_base;
}
MovelistLayout movelist_layout() {
    return discovered;
}
}  // namespace opendojo::signatures
namespace opendojo::log {
void format(const char*, ...) {}
}  // namespace opendojo::log
namespace opendojo::subsystems {
bool in_practice() {
    return practice;
}
std::uintptr_t pool1() {
    return available ? address(pool) : 0;
}
void ensure_pool_allocated() {
    ++allocations;
}
std::uintptr_t lookup(std::uint32_t key) {
    switch (key) {
        case KEY_GAMEPLAY: return address(gameplay);
        case KEY_SINGLETON: return address(singleton);
        case KEY_SUBB: return address(subB);
        case KEY_SUBC: return address(subC);
        case KEY_RECORDPOOL: return address(recordpool);
    }
    return 0;
}
}  // namespace opendojo::subsystems

int main() {
    using namespace opendojo;
    drill::Recording live;
    live.slot_bytes.resize(slot::SLOT_PITCH);
    live.slot_bytes[0] = 1;
    live.slot_bytes[2] = 42;
    drill::Recording move;
    move.kind = drill::Kind::MoveList;
    move.move_id = 123;
    std::vector<std::size_t> targets;
    reset();
    check(slot::import_recordings({live, move}, true, targets) == slot::WriteStatus::Ok);
    check(slot::clear_all() == slot::WriteStatus::Ok);
    for (std::size_t i = 0; i < slot::USER_SLOTS; ++i) check(flag(i) == slot::FLAG_EMPTY);
    check((memory::read_u32(address(singleton)) & session_fields.active_mask) == 0);
    check(memory::read_u32(address(singleton) + recording_fields.counter) == 0);
    check(slot::clear_all() == slot::WriteStatus::Ok);  // already empty
    practice = false;
    check(slot::clear_all() == slot::WriteStatus::NotInPracticeMode);
    reset();
    check(slot::import_recordings({live, move}, true, targets) == slot::WriteStatus::Ok);
    owner_context = false;
    check(slot::clear_all() == slot::WriteStatus::StateChanged);
    check(flag(0) == slot::FLAG_LIVE && flag(1) == slot::FLAG_MOVELIST);
    owner_context = true;
    reset();
    check(slot::import_recordings({live, move}, true, targets) == slot::WriteStatus::Ok);
    check(targets == std::vector<std::size_t>({0, 1}));
    check(flag(0) == slot::FLAG_LIVE && flag(1) == slot::FLAG_MOVELIST && flag(2) == 0);
    check(pool[2] == 42);
    check(memory::read_u32(address(moves) + 0x148 + 0x44 + 4) == 123);
    check(slot::import_recordings({live}, false, targets) == slot::WriteStatus::Ok);
    check(targets == std::vector<std::size_t>({2}));

    std::array<slot::CapturedSlot, slot::USER_SLOTS> captured;
    check(slot::capture(captured));
    check(captured[0].kind == slot::Kind::Live && captured[0].bytes[2] == 42);
    check(captured[1].kind == slot::Kind::MoveList && captured[1].move_id == 123);
    const auto before_gameplay = gameplay;
    const auto before_pool = pool;
    const auto before_singleton = singleton;
    --recordpool[1];  // Simulate a patch changing the stride.
    check(!slot::capture(captured));
    check(captured[0].kind == slot::Kind::Empty && captured[0].bytes.empty());
    check(slot::import_recordings({live, move}, true, targets) == slot::WriteStatus::StateChanged);
    check(targets.empty() && gameplay == before_gameplay && pool == before_pool &&
          singleton == before_singleton);
    auto malformed = live;
    malformed.slot_bytes.pop_back();
    check(slot::import_recordings({malformed}, true, targets) ==
          slot::WriteStatus::InvalidRecording);
    check(gameplay == before_gameplay && pool == before_pool);
    check(slot::import_recordings({}, true, targets) == slot::WriteStatus::InvalidRecording);
    check(slot::import_recordings(std::vector<drill::Recording>(9, live), true, targets) ==
          slot::WriteStatus::InvalidRecording);

    reset();
    check(slot::import_recordings(std::vector<drill::Recording>(8, live), true, targets) ==
          slot::WriteStatus::Ok);
    check(slot::import_recordings({live}, false, targets) == slot::WriteStatus::NoFreeSlots);
    practice = false;
    check(slot::import_recordings({live}, true, targets) == slot::WriteStatus::NotInPracticeMode);
    reset();
    available = false;
    check(slot::import_recordings({move}, true, targets) == slot::WriteStatus::Ok &&
          allocations == 0);
    check(slot::import_recordings({live}, true, targets) == slot::WriteStatus::PoolNotAllocated &&
          allocations == 1);
    check(flag(0) == slot::FLAG_MOVELIST);  // failed live import did not clear it
    reset();
    discovered = {0x490, 0x180, 0x50, 16};
    recordpool[1] = 0;
    recordpool[2] = address(moves) + moves.size();
    check(slot::import_recordings({move}, true, targets) == slot::WriteStatus::Ok);
    check(memory::read_u32(address(moves) + 0x180 + 0x50) == move.move_id);
    check(slot::movelist_move_id(0) == move.move_id);
    check(slot::capture(captured) && captured[0].move_id == move.move_id);
    const auto safe_moves = moves;
    const auto safe_gameplay = gameplay;
    discovered = {};
    check(slot::import_recordings({move}, true, targets) == slot::WriteStatus::StateChanged);
    check(moves == safe_moves && gameplay == safe_gameplay && !slot::capture(captured));
    reset();
    flags_base = 0x504;
    recording_fields = {12, 0x69, 0x294, 0x34};
    session_fields = {0xA0, 0x3B00, 0xB1, 0x800000};
    constexpr std::uint32_t unrelated_flags = 0x100480;
    memory::write_u32(address(singleton), unrelated_flags);
    check(slot::import_recordings({live}, true, targets) == slot::WriteStatus::Ok);
    check(flag(0) == slot::FLAG_LIVE && memory::read_u32(address(gameplay) + 0x484) == 0);
    check(memory::read_u32(address(singleton)) == (unrelated_flags | session_fields.active_mask));
    check(memory::read_u32(address(singleton) + 12) == 1 &&
          memory::read_u32(address(singleton) + 8) == 0);
    check(memory::read_u32(address(subC) + 0x294) == 1 &&
          memory::read_u32(address(subC) + 0x25C) == 0);
    check(slot::set_recorded_flag(0, false) == slot::WriteStatus::Ok);
    check(memory::read_u32(address(singleton)) == unrelated_flags && flag(0) == slot::FLAG_EMPTY);
    check(subB[0x69] == 1 && subB[0x65] == 0 &&
          memory::read_u32(address(subC) + 0x294) == 0xFFFFFFFFu);
    const auto old_gameplay = gameplay, old_singleton = singleton, old_subB = subB, old_subC = subC;
    const auto old_pool = pool;
    format_supported = false;
    check(slot::import_recordings({live}, true, targets) == slot::WriteStatus::StateChanged &&
          allocations == 0);
    format_supported = true;
    recording_fields = {};
    check(slot::import_recordings({live}, true, targets) == slot::WriteStatus::StateChanged);
    check(slot::set_recorded_flag(0, true) == slot::WriteStatus::StateChanged);
    check(slot::write(0, live.slot_bytes.data()) == slot::WriteStatus::StateChanged);
    flags_base = 0;
    check(slot::import_recordings({move}, true, targets) == slot::WriteStatus::StateChanged);
    check(gameplay == old_gameplay && singleton == old_singleton && subB == old_subB &&
          subC == old_subC && pool == old_pool);
    reset();
    check(slot::import_recordings({live, move}, true, targets) == slot::WriteStatus::Ok);
    slot::publish_snapshot(true);
    owner_context = false;
    pool[2] = 99; // Game changes are invisible until the next owner-published snapshot.
    check(slot::capture(captured) && captured[0].bytes[2] == 42 && captured[1].move_id == 123);
    check(slot::kind(0) == slot::Kind::Live && slot::event_count(0) == 1);
    const auto off_thread_pool = pool;
    const auto off_thread_gameplay = gameplay;
    check(slot::import_recordings({live}, true, targets) == slot::WriteStatus::StateChanged && targets.empty());
    check(slot::set_recorded_flag(0, false) == slot::WriteStatus::StateChanged);
    check(slot::write(0, live.slot_bytes.data()) == slot::WriteStatus::StateChanged);
    check(slot::set_movelist(0, 456) == slot::WriteStatus::StateChanged);
    check(pool == off_thread_pool && gameplay == off_thread_gameplay);
    ++current_epoch;
    check(!slot::capture(captured) && slot::kind(0) == slot::Kind::Empty);
    check(slot::capture_snapshot(captured, "jin") && captured[0].bytes[2] == 42);
    check(!slot::capture_snapshot(captured, "alisa"));
    owner_context = true;
    std::cout << "Import preflight and mixed live/movelist tests passed\n";
}

namespace opendojo::game_thread {
bool is_current() { return owner_context; }
std::uint64_t epoch() { return current_epoch; }
opendojo::players::CpuInfo current_cpu() { return {true, 6, "jin", opendojo::players::Side::p2}; }
}
namespace opendojo::slot_labels { std::string get(std::size_t) { return {}; } }
