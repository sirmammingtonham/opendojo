#include <windows.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include "session_queue.hpp"
#include "../src/hooks/player_hook.cpp"

namespace {
opendojo::SessionQueue pending;
std::uint64_t generation = 1;
std::array<std::uint64_t, 4> holder{0, 100, 200, 0};
auto next_holder = holder;
opendojo::players::CpuInfo cpu{true, 6, "jin", opendojo::players::Side::p2};
bool readable = true;
void check(bool ok) {
    if (!ok) throw std::runtime_error("player refresh regression");
}
bool native_refresh(std::uintptr_t) {
    holder = next_holder;
    return true;
}
}  // namespace
namespace opendojo::log {
void format(const char*, ...) {}
}  // namespace opendojo::log
namespace opendojo::memory {
bool try_read_u64(std::uintptr_t address, std::uint64_t* out) {
    if (!readable) {
        *out = 0;
        return false;
    }
    std::memcpy(out, reinterpret_cast<void*>(address), 8);
    return true;
}
}  // namespace opendojo::memory
namespace opendojo::players {
CpuInfo detect_cpu() {
    return cpu;
}
}  // namespace opendojo::players
namespace opendojo::signatures {
PlayerLayout player_layout() {
    return {8, 16, 0, 0};
}
std::uintptr_t player_refresh() {
    return 0;
}
}  // namespace opendojo::signatures
namespace opendojo::game_thread {
void invalidate() {
    ++generation;
    pending.cancel();
}
}  // namespace opendojo::game_thread
MH_STATUS WINAPI MH_Initialize() {
    return MH_OK;
}
MH_STATUS WINAPI MH_CreateHook(LPVOID, LPVOID, LPVOID*) {
    return MH_OK;
}
MH_STATUS WINAPI MH_EnableHook(LPVOID) {
    return MH_OK;
}
MH_STATUS WINAPI MH_RemoveHook(LPVOID) {
    return MH_OK;
}

int main() {
    using namespace opendojo;
    player_hook::g_orig = native_refresh;
    const auto address = reinterpret_cast<std::uintptr_t>(holder.data());
    check(player_hook::refresh_detour(address));  // Initial identity publication.
    const auto stable_generation = generation;
    int completed = 0;
    check(pending.push(generation, [&](bool eligible) {
        check(eligible);
        ++completed;
    }));
    for (int i = 0; i < 120; ++i)
        check(player_hook::refresh_detour(address));
    check(generation == stable_generation && completed == 0);
    pending.drain([] { return generation; });
    check(completed == 1);
    auto expect_cancel = [&] {
        int cancelled = 0;
        check(pending.push(generation, [&](bool eligible) {
            check(!eligible);
            ++cancelled;
        }));
        const auto before = generation;
        check(player_hook::refresh_detour(address));
        check(generation == before + 1 && cancelled == 1);
        pending.drain([] { return generation; });
        check(cancelled == 1);
    };
    next_holder[2] = 300;
    expect_cancel();  // Replacement CPU, same character.
    next_holder[1] = 400;
    expect_cancel();  // Replacement P1 also invalidates.
    cpu.character_id = 7;
    expect_cancel();  // Character changes at the same addresses.
    cpu.detected = false;
    next_holder[1] = next_holder[2] = 0;
    expect_cancel();
    readable = false;
    expect_cancel();
    std::cout << "Unchanged player refresh preserves manual loads; replacements cancel them\n";
}
