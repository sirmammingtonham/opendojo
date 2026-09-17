#include "hooks/player_hook.hpp"

#include <atomic>
#include <chrono>

#include "MinHook.h"

#include "log.hpp"
#include "memory.hpp"
#include "players.hpp"
#include "signatures.hpp"

namespace opendojo::player_hook {

namespace {

// Refresh-players function — historically at RVA 0x5E70CD0 in v3.00.02,
// now resolved via AOB scan (see signatures.cpp). This is the function
// Tekken calls on practice entry and on CPU character change.
// Identified via DR0 watch on holder+0x30 (the write instruction lives
// ~0x8A bytes into this fn).
//
// Note: Ghidra has an analyzed function at 0x5E70B40 that *looks*
// like this one (same service-locator + same write to RBX+0x30),
// but it's a separate sibling. Hooking that function gets you
// nothing — it doesn't fire in the practice flow we care about.
// The unanalyzed function here (Ghidra never created the symbol)
// is the live one.

std::atomic<bool> g_installed{false};
// Publish validity and ID together; separate atomics can mix two characters.
constexpr std::uint64_t DETECTED = std::uint64_t{1} << 32;
std::atomic<std::uint64_t> g_cpu{0};
std::atomic<bool> g_hook_enabled{false};

using RefreshFn = bool (*)(std::uintptr_t holder);
RefreshFn g_orig = nullptr;

bool refresh_detour(std::uintptr_t holder) {
    // Run the original first — it does the actual service-locator
    // lookup and writes holder+0x30 / +0x38. After it returns, the
    // pointer slots reflect the NEW state.
    bool result = g_orig(holder);

    // Re-walk the player chain to update the cache. detect_cpu reads
    // from the same globals the game just updated, so this gives us
    // the post-refresh CPU character. All reads are SEH-guarded
    // inside detect_cpu — if the chain is partially valid we'll
    // get detected=false.
    auto cpu = players::detect_cpu();
    const auto old = g_cpu.exchange(cpu.detected ? DETECTED | cpu.character_id : 0);
    const auto old_id = static_cast<std::uint32_t>(old);
    const bool old_detected = (old & DETECTED) != 0;

    if (cpu.detected != old_detected || cpu.character_id != old_id) {
        OPENDOJO_LOG(
            "player_hook: refresh -> detected=%d id=%u name=%s "
            "(was: detected=%d id=%u) holder=0x%llX",
            cpu.detected ? 1 : 0, cpu.character_id, cpu.character_name.c_str(),
            old_detected ? 1 : 0, old_id, static_cast<unsigned long long>(holder));
    }
    return result;
}

}  // anonymous namespace

Cached current_cpu() {
    const auto cpu = g_cpu.load(std::memory_order_acquire);
    return {(cpu & DETECTED) != 0, static_cast<std::uint32_t>(cpu)};
}

void invalidate() {
    g_cpu.store(0, std::memory_order_release);
}

void ensure_fresh() {
    const auto before = g_cpu.load(std::memory_order_acquire);
    if ((before & DETECTED) && g_hook_enabled.load(std::memory_order_acquire)) return;
    // When a patch breaks the refresh hook, continue detecting character
    // changes at 4 Hz. Also bound retries during scene transitions.
    using Clock = std::chrono::steady_clock;
    static thread_local Clock::time_point next_poll{};
    const auto now = Clock::now();
    if (now < next_poll) return;
    next_poll = now + std::chrono::milliseconds(250);
    const auto cpu = players::detect_cpu();
    const auto value = cpu.detected ? DETECTED | cpu.character_id : 0;
    auto expected = before;
    // Do not overwrite a newer update from the game-thread refresh hook.
    g_cpu.compare_exchange_strong(expected, value, std::memory_order_acq_rel);
}

void install() {
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true)) return;

    auto refresh_addr = signatures::player_refresh();
    if (!refresh_addr) {
        OPENDOJO_LOG(
            "player_hook: player_refresh signature unresolved — character-switch detect OFF");
        g_installed.store(false);
        return;
    }

    // MinHook may already have been initialized by render_hook /
    // practice_state — ALREADY_INITIALIZED is OK.
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        OPENDOJO_LOG("player_hook: MH_Initialize failed: %d", static_cast<int>(s));
        g_installed.store(false);
        return;
    }

    auto target = reinterpret_cast<void*>(refresh_addr);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&refresh_detour),
                      reinterpret_cast<void**>(&g_orig)) != MH_OK) {
        OPENDOJO_LOG("player_hook: MH_CreateHook failed");
        g_installed.store(false);
        return;
    }
    if (MH_EnableHook(target) != MH_OK) {
        OPENDOJO_LOG("player_hook: MH_EnableHook failed");
        MH_RemoveHook(target);
        g_installed.store(false);
        return;
    }
    g_hook_enabled.store(true, std::memory_order_release);
    OPENDOJO_LOG("player_hook: installed at 0x%p", target);
}

}  // namespace opendojo::player_hook
