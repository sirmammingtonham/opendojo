#include "practice_state.hpp"

#include <windows.h>

#include <atomic>

#include "MinHook.h"

#include "autosave.hpp"
#include "log.hpp"
#include "memory.hpp"
#include "hooks/player_hook.hpp"
#include "signatures.hpp"

namespace opendojo::practice_state {

namespace {

// PRACTICE-specific controller slot — historically at module-base +
// 0x9B792A8, now resolved via signatures::practice_slot_addr() (xref
// from the dtor's slot-clear instruction). Created by FUN_145CA38A0
// (alloc + ctor at FUN_145C8AFF0) only on Practice/Training/Replay
// entry — identified by xref to the string `TEXT_000_UI_PRACTICE_193`.
// This is what we gate the menu on so the menu only opens in practice
// and NOT in vs / online / story.
//
// Previously this constant pointed at +0x9B79290, but that slot turned
// out to be the generic *battle/round* controller (created for any
// match: vs, story, practice, …) — which explained why the menu still
// opened in vs/story matches. The two slots live 8 bytes apart in the
// same data cluster, easy to confuse.

// Practice-controller scalar-deleting destructor — historically at RVA
// 0x05C8C880 in v3.00.02, now resolved via AOB scan (see signatures.cpp).
// Identified via vtable slot 0 of PTR_LAB_148550eb0 (the practice
// controller's vtable, written by FUN_145C8AFF0). The slot write to
// 0x149B792A8 lives ~0xB9 bytes into this function body, exactly the
// MSVC scalar-deleting-dtor pattern.
//
// We hook this (instead of the battle/round dtor at FUN_145C8C2F0)
// because:
//   1) The battle dtor doesn't fire on practice→main-menu transitions
//      (battle controller lingers), so autosave never ran on exit.
//   2) The battle dtor DOES fire on character switch within practice
//      (which tears down and rebuilds the battle controller). Running
//      flush_now there crashed the game — gameplay subsystems were
//      mid-teardown when save_for tried to read slot data.

std::atomic<bool> g_installed{false};
// Last observed slot state — drives the 0→nonzero transition that
// fires autosave::on_practice_entered. is_active() updates it.
std::atomic<bool> g_prev_active{false};

using DtorFn = void* (*)(void*, unsigned);
DtorFn g_dtor_orig = nullptr;

void* dtor_hook(void* this_ptr, unsigned flags) {
    // The dtor function symbol is shared by all instances of this
    // class. We only care about the singleton — confirm by reading
    // the practice slot and matching `this`.
    auto slot = signatures::practice_slot_addr();
    if (slot && this_ptr) {
        auto singleton = opendojo::memory::read_u64(slot);
        if (singleton == reinterpret_cast<std::uintptr_t>(this_ptr)) {
            OPENDOJO_LOG("practice_state: dtor (this=0x%p) — flushing autosave", this_ptr);
            // CRITICAL: save BEFORE forwarding to the original dtor.
            // After this call returns, gameplay subsystems are gone
            // and save_for can't read slot state.
            opendojo::autosave::flush_now();
        }
    }
    return g_dtor_orig(this_ptr, flags);
}

bool install_one(void* target, void* shim, void** orig, const char* label) {
    auto s = MH_CreateHook(target, shim, orig);
    if (s != MH_OK) {
        OPENDOJO_LOG("practice_state: MH_CreateHook(%s) failed: %d", label, static_cast<int>(s));
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        OPENDOJO_LOG("practice_state: MH_EnableHook(%s) failed", label);
        return false;
    }
    return true;
}

}  // namespace

bool is_active() {
    auto slot = signatures::practice_slot_addr();
    if (!slot) return false;
    bool active = opendojo::memory::read_u64(slot) != 0;

    // Detect 0→nonzero transition and notify autosave so it resets its
    // prev-character state for the new session. Using compare_exchange
    // so only ONE caller (whichever wins the race on the render thread)
    // fires the side effect.
    bool prev = g_prev_active.load(std::memory_order_acquire);
    if (active != prev) {
        if (g_prev_active.compare_exchange_strong(prev, active, std::memory_order_acq_rel)) {
            if (active) {
                OPENDOJO_LOG("practice_state: entered (slot transition 0→nonzero)");
                opendojo::autosave::on_practice_entered();
            } else {
                OPENDOJO_LOG("practice_state: cleared (slot transition nonzero→0)");
                // Invalidate the player_hook cache so the next
                // practice entry's ensure_fresh re-walks the chain
                // instead of returning stale detected=true.
                opendojo::player_hook::invalidate();
            }
        }
    }
    return active;
}

void install_hooks() {
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true)) return;

    // signatures::practice_dtor() returns 0 if either polaris isn't loaded
    // or the AOB scan didn't find the function — both surface as "no
    // address" here, so we don't need a separate polaris_base check.
    auto dtor_addr = signatures::practice_dtor();
    if (!dtor_addr) {
        OPENDOJO_LOG("practice_state: practice_dtor signature unresolved — autosave-on-exit OFF");
        return;
    }

    // MinHook init is idempotent; render_hook + proxy_dinput8 may have
    // already done it. ALREADY_INITIALIZED is OK.
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        OPENDOJO_LOG("practice_state: MH_Initialize failed: %d", static_cast<int>(s));
        return;
    }

    auto dtor = reinterpret_cast<void*>(dtor_addr);
    if (!install_one(dtor, reinterpret_cast<void*>(&dtor_hook),
                     reinterpret_cast<void**>(&g_dtor_orig), "dtor")) {
        return;
    }

    OPENDOJO_LOG("practice_state: dtor hook installed (dtor=0x%p)", dtor);
}

}  // namespace opendojo::practice_state
