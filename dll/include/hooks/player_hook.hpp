#pragma once

// MinHook detour on Polaris's "refresh player pointers" function
// (FUN_145E70B40 / RVA 0x5E70B40). The game calls this whenever the
// GlobalPlayerHolder's P1/P2 slots need to be (re)populated:
//   - practice scene load
//   - CPU character change via Practice Pause → Change CPU Character
//   - practice exit (writes 0 to the slots)
//
// The hook maintains an atomic cache of the current CPU character so
// autosave::tick can replace its per-frame players::detect_cpu chain
// walk with a single atomic load. The hook also fires before/after
// callbacks into autosave so it can save-for-old-character at the
// exact moment Tekken is rotating the player pointer.
//
// Hook target identified at runtime via a one-shot DR0 watch on
// holder+0x30 (see diag_hook.cpp, since removed). The watch caught
// the writing RIP, and Ghidra-decompile of the surrounding function
// confirmed the role (service-locator P1/P2 lookup + writes).

#include <cstdint>

namespace opendojo::player_hook {

struct Cached {
    bool detected;                   // true => both P1 and P2 are valid
    std::uint32_t cpu_character_id;  // 0 when !detected
};

// Atomic load — safe to call from any thread, every frame. Free.
Cached current_cpu();

// Fallback: when the cache says undetected, do a single chain walk
// to populate it. Cheap atomic load fast-path once detected. Needed
// because FUN_145E70B40 only fires on subsequent player refreshes
// (e.g., character switch), NOT on the initial practice-load
// population — that path goes through a different function we
// haven't pinned. Call from autosave::tick (gated on in_practice).
void ensure_fresh();

// Clear the cache. Call when leaving practice so that the next
// practice entry starts undetected and ensure_fresh re-walks the
// chain rather than skipping on stale-true.
void invalidate();

// Install the detour. Idempotent. Safe to call before MinHook is
// initialized (it'll initialize MinHook if needed).
void install();

}  // namespace opendojo::player_hook
