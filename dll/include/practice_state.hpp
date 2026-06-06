#pragma once

// Practice-mode lifecycle tracking.
//
// Entry: is_active() polls the practice-controller singleton slot
// (signatures::practice_slot_addr()). The slot is non-null only in
// Practice/Training/Replay, so it also gates the menu.
//
// Exit: one MinHook detour on the controller's scalar-deleting dtor
// (signatures::practice_dtor()). It runs while gameplay subsystems are
// still live, so we flush autosave there before the slot data is gone.
// Not the battle/round dtor FUN_145C8C2F0 — that misses practice→
// main-menu and fires on in-practice character switch (where flushing
// crashes mid-teardown). See practice_state.cpp.
//
// See docs/RE_NOTES.md for the RE walk that justified these targets.

namespace opendojo::practice_state {

// True while in practice. Polls the practice slot each call and fires a
// side effect on each transition: 0→nonzero notifies autosave (new
// session), nonzero→0 invalidates the player_hook cache. Cheap per frame.
bool is_active();

// Install the practice-dtor MinHook detour (exit-flush; entry is
// poll-based, no hook). Idempotent. Call once at DLL init after the
// polaris base is known. MinHook is initialized internally.
void install_hooks();

}  // namespace opendojo::practice_state
