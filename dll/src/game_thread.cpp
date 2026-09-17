#include "game_thread.hpp"

#include <windows.h>
#include <atomic>
#include <exception>
#include "MinHook.h"
#include "autosave.hpp"
#include "hooks/player_hook.hpp"
#include "log.hpp"
#include "memory.hpp"
#include "players.hpp"
#include "practice_rename.hpp"
#include "practice_state.hpp"
#include "session_queue.hpp"
#include "signatures.hpp"
#include "slot_labels.hpp"
#include "slot.hpp"

namespace opendojo::game_thread {
namespace {
SessionQueue queue([] {
    OPENDOJO_LOG("game_thread: queued request threw; remaining requests preserved");
});
std::atomic<bool> installed{false}, flush_requested{false};
std::atomic<std::uint64_t> generation{1};
thread_local bool executing = false;
std::mutex cpu_mutex;
players::CpuInfo cached_cpu;
std::uint64_t cpu_epoch = 0;
using UpdateFn = std::uint64_t (*)(void*, float);
UpdateFn original = nullptr;
using SchedulerFn = void (*)(void*, float, int);
SchedulerFn scheduler_original = nullptr;
thread_local std::uint64_t scheduler_completions = 0;

void scheduler_hook(void* self, float delta, int phase) {
    scheduler_original(self, delta, phase);
    if (phase == 3 && is_game_thread() &&
        reinterpret_cast<std::uintptr_t>(self) ==
            memory::read_u64(signatures::runtime_layout().scheduler_slot))
        ++scheduler_completions;
}

void pump() {
    if (!is_game_thread() || executing) return;
    struct Scope {
        Scope() { executing = true; }
        ~Scope() { executing = false; }
    } scope;
    static std::uintptr_t previous_controller = 0, previous_player = 0;
    static std::uint32_t previous_character = 0, previous_round = 0;
    static bool had_round = false;
    const auto controller = memory::read_u64(signatures::practice_slot_addr());
    const auto player = controller ? players::cpu_player_address() : 0;
    const auto cpu = controller ? players::detect_cpu() : players::CpuInfo{};
    std::uint32_t round = 0;
    const bool has_round = controller && players::try_round_counter(round);
    if (has_round && had_round && round < previous_round) {
        OPENDOJO_LOG("game_thread: round counter decreased (%u -> %u); cancelling queued loads",
                     previous_round, round);
        invalidate();
    }
    previous_round = round;
    had_round = has_round;
    const bool changed = controller != previous_controller || player != previous_player ||
                         (cpu.detected && cpu.character_id != previous_character);
    if (changed) {
        OPENDOJO_LOG("game_thread: controller/player identity changed; cancelling queued loads");
        invalidate();
        practice_rename::on_practice_exit();
        if (controller) practice_rename::on_practice_reentry();
        if (previous_player && player != previous_player) slot_labels::clear_all();
        previous_controller = controller;
        previous_player = player;
        previous_character = cpu.character_id;
    }
    {
        std::lock_guard lock(cpu_mutex);
        cached_cpu = cpu;
        cpu_epoch = epoch();
    }
    practice_state::poll();
    if (controller) player_hook::ensure_fresh();
    // Auto-load and manual requests share the same native boundary. Manual requests
    // run last so a user choice is never immediately overwritten by an auto-load.
    autosave::tick();
    queue.drain([] { return generation.load(std::memory_order_acquire); });
    if (flush_requested.exchange(false)) autosave::flush_now();
    if (controller) practice_rename::tick();
    slot::publish_snapshot();
}

std::uint64_t update_hook(void* self, float delta) {
    const auto before = scheduler_completions;
    const auto result = original(self, delta);
    if (before == scheduler_completions) return result;
    static std::atomic<bool> announced{false};
    if (!announced.exchange(true))
        OPENDOJO_LOG("game_thread: native completion active on game thread %lu",
                     GetCurrentThreadId());
    try {
        pump();
    } catch (const std::exception& e) {
        OPENDOJO_LOG("game_thread: update failed: %s", e.what());
    } catch (...) {
        OPENDOJO_LOG("game_thread: update failed");
    }
    return result;
}
}  // namespace

players::CpuInfo current_cpu() {
    if (is_current()) return players::detect_cpu();
    std::lock_guard lock(cpu_mutex);
    return cpu_epoch == epoch() ? cached_cpu : players::CpuInfo{};
}
bool is_game_thread() {
    const auto address = signatures::runtime_layout().game_thread_id;
    std::uint32_t id = 0;
    return address && memory::try_read_u32(address, &id) && id && id == GetCurrentThreadId();
}
bool is_current() {
    return executing && is_game_thread();
}
bool available() {
    return installed.load(std::memory_order_acquire);
}
std::uint64_t epoch() {
    return generation.load(std::memory_order_acquire);
}
void invalidate() {
    generation.fetch_add(1, std::memory_order_acq_rel);
    queue.cancel();
}
void request_flush() {
    flush_requested.store(true);
}
bool enqueue(std::function<void(bool)> job) {
    const auto requested_epoch = epoch();
    if (!available() || !practice_state::is_active()) return false;
    if (!job) return false;
    const auto submitted = SessionQueue::Clock::now();
    return queue.push(
        requested_epoch,
        [requested_epoch, submitted, job = std::move(job)](bool eligible) {
            if (!eligible) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         SessionQueue::Clock::now() - submitted)
                                         .count();
                OPENDOJO_LOG(
                    "game_thread: load cancelled (submitted epoch=%llu current=%llu age=%lldms)",
                    static_cast<unsigned long long>(requested_epoch),
                    static_cast<unsigned long long>(epoch()), static_cast<long long>(elapsed));
            }
            job(eligible);
        },
        submitted);
}

void install() {
    const auto layout = signatures::runtime_layout();
    const auto target = reinterpret_cast<void*>(layout.update);
    const auto scheduler = reinterpret_cast<void*>(layout.scheduler);
    if (!target || !scheduler) return;
    const auto init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return;
    if (MH_CreateHook(scheduler, reinterpret_cast<void*>(&scheduler_hook),
                      reinterpret_cast<void**>(&scheduler_original)) != MH_OK)
        return;
    if (MH_CreateHook(target, reinterpret_cast<void*>(&update_hook),
                      reinterpret_cast<void**>(&original)) != MH_OK) {
        MH_RemoveHook(scheduler);
        return;
    }
    if (MH_EnableHook(scheduler) != MH_OK || MH_EnableHook(target) != MH_OK) {
        MH_DisableHook(target);
        MH_DisableHook(scheduler);
        MH_RemoveHook(target);
        MH_RemoveHook(scheduler);
        return;
    }
    installed.store(true, std::memory_order_release);
    OPENDOJO_LOG("game_thread: scheduler completion hook installed at %p", target);
}
}  // namespace opendojo::game_thread
