#pragma once

// Per-character autosave / autoload of practice-mode recordings.
//
// User-toggleable via Settings (persisted in opendojo/_autosave_enabled —
// a marker file whose mere existence means "on"). When enabled, the
// contents of pool1 + the move-list slots are persisted to a
// per-character "scratch" drill file every time:
//   - the user leaves practice (detected -> not detected), or
//   - the CPU character changes mid-session.
// The file is overwritten on every save, and deleted when no slots are
// populated (so the autosave always reflects current state).
//
// On entering practice (or on a character change), if a scratch drill
// exists for the new character, it's loaded back via
// commands::load_drill(..., ReplaceAll).
//
// Scratch drills live at opendojo/_autosave_<character>.drill.txt. The
// leading underscore is a label convention so the menu can pin them at
// the top of the Drills list with distinct styling. Promote one to a
// permanent drill via the "Save as drill" button in the menu.
//
// Limitations:
//   - pool1 is allocated lazily by the game on the FIRST practice
//     recording per process launch. Until that happens, autoload
//     retries each frame and reports nothing. After a single
//     user-initiated recording, pool1 is alive for the rest of the
//     session.

namespace opendojo::autosave {

bool is_enabled();
void set_enabled(bool on);

// Call from the render hook once per frame. Cheap when disabled (early
// return).
void tick();

// Immediately write the current character's autosave, ignoring the
// normal "on transition" trigger. Called from the WndProc subclass on
// WM_CLOSE so the user's slot state survives a direct quit from
// practice mode (where neither the leave-practice nor character-change
// trigger would fire). Idempotent; no-op if autosave is disabled, if
// we're not in a match, or if no character is known.
void flush_now();

// Called from practice_state::is_active() on the practice-slot 0→nonzero
// transition (practice entry). Clears stale prev-tick state so the
// upcoming character is detected as a fresh entry and the normal
// autoload flow (round-active wait → load_drill) kicks off.
void on_practice_entered();

}  // namespace opendojo::autosave
