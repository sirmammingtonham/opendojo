#include "subsystems.hpp"
#include "signatures.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
std::array<std::uint8_t, 0x200> singleton{}, recording{}, map{};
std::array<std::uint8_t, 0x4000> player{};
std::array<std::uint64_t, 4> ctx{};
std::array<std::uint64_t, 6> first{}, last{};
std::array<std::uint64_t, 4> buckets{};
std::uint64_t ctx_slot = 0;
opendojo::signatures::SubsystemLayout table{0x10, 0x128, 0x100, 0x110, 16, 8, 16, 8, 24};
opendojo::signatures::SessionLayout layout{0x88, 0x3A50, 0x99, 0x400000};
template <typename T>
std::uintptr_t addr(T& value) {
    return reinterpret_cast<std::uintptr_t>(value.data());
}
template <typename T>
void put(std::uint8_t* at, T value) {
    std::memcpy(at, &value, sizeof(value));
}
void check(bool ok) {
    if (!ok) throw std::runtime_error("session write regression");
}
}  // namespace
namespace opendojo::log {
void format(const char*, ...) {}
}  // namespace opendojo::log
namespace opendojo::players {
std::uintptr_t cpu_player_address() {
    return addr(player);
}
}  // namespace opendojo::players
namespace opendojo::practice_state {
bool is_active() {
    return true;
}
}  // namespace opendojo::practice_state
namespace opendojo::signatures {
bool live_recordings_supported() {
    return true;
}
RecordingStateLayout recording_state_layout() {
    return {8, 0x65, 0x25C, 0x28};
}
SubsystemLayout subsystem_layout() {
    return table;
}
SessionLayout session_layout() {
    return layout;
}
std::uintptr_t ctx_ptr_addr() {
    return reinterpret_cast<std::uintptr_t>(&ctx_slot);
}
std::uintptr_t pool1_ptr_addr() {
    return 0;
}
std::uintptr_t pool2_ptr_addr() {
    return 0;
}
std::uintptr_t pool_init() {
    return 0;
}
}  // namespace opendojo::signatures
namespace {
bool owner_context = true;
}
int main() {
    using namespace opendojo;
    singleton.fill(0xA5);
    recording.fill(0xA5);
    player.fill(0xA5);
    ctx[2] = addr(map);
    ctx_slot = addr(ctx);
    first[2] = subsystems::KEY_SINGLETON;
    first[3] = addr(singleton);
    last[1] = addr(first);
    last[2] = subsystems::KEY_RECORDING;
    last[3] = addr(recording);
    buckets = {addr(first), addr(last)};
    put(map.data() + 0x100, std::uint64_t{1});
    put(map.data() + 0x110, static_cast<std::uint64_t>(addr(buckets)));
    put(map.data() + 0x128, std::uint64_t{0});
    auto expected_singleton = singleton;
    auto expected_recording = recording;
    auto expected_player = player;
    put(expected_singleton.data(), std::uint32_t{0xA5A5A5A5 | 0x400000});
    put(expected_singleton.data() + 0x88, std::uint32_t{0});
    expected_singleton[0x99] = 0;
    put(expected_recording.data() + 0x28, std::uint32_t{0});
    put(expected_player.data() + 0x3A50, std::uint32_t{1});
    owner_context = false;
    const auto untouched_singleton = singleton;
    const auto untouched_recording = recording;
    const auto untouched_player = player;
    check(!subsystems::mark_session_loaded(true));
    subsystems::ensure_pool_allocated();
    check(singleton == untouched_singleton && recording == untouched_recording &&
          player == untouched_player);
    owner_context = true;
    check(subsystems::mark_session_loaded(true));
    // Compare whole objects to catch writes to old offsets and adjacent fields.
    check(singleton == expected_singleton && recording == expected_recording &&
          player == expected_player);
    layout = {};
    check(!subsystems::mark_session_loaded(true));
    check(singleton == expected_singleton && recording == expected_recording &&
          player == expected_player);
    // Move every table field and the bucket stride, then exercise a collision.
    table = {24, 0x150, 0x120, 0x130, 32, 16, 24, 16, 32};
    ctx.fill(0);
    map.fill(0);
    first.fill(0);
    last.fill(0);
    buckets.fill(0);
    ctx[3] = addr(map);
    first[3] = subsystems::KEY_SINGLETON;
    first[4] = addr(singleton);
    last[2] = addr(first);
    last[3] = subsystems::KEY_RECORDING;
    last[4] = addr(recording);
    buckets[0] = addr(first);
    buckets[2] = addr(last);
    put(map.data() + 0x120, std::uint64_t{1});
    put(map.data() + 0x130, static_cast<std::uint64_t>(addr(buckets)));
    check(subsystems::lookup(subsystems::KEY_SINGLETON) == addr(singleton));
    check(subsystems::lookup(subsystems::KEY_RECORDING) == addr(recording));
    check(!subsystems::lookup(123));
    put(map.data() + 0x150, std::uint64_t{2});  // invalid bucket mask
    check(!subsystems::lookup(subsystems::KEY_SINGLETON));
    put(map.data() + 0x150, std::uint64_t{0});
    last[2] = addr(last);  // corrupt cyclic collision chain must terminate
    check(!subsystems::lookup(subsystems::KEY_SINGLETON));
    table = {};
    check(!subsystems::lookup(subsystems::KEY_RECORDING));
    std::cout << "Session layout writes and unresolved-layout rejection passed\n";
}

namespace opendojo::game_thread {
bool is_current() {
    return owner_context;
}
}  // namespace opendojo::game_thread
