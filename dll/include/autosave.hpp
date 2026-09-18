#pragma once

// Per-character autosave / autoload of practice-mode recordings.
//
// User-toggleable via Settings (persisted in opendojo/_autosave_enabled —
// a marker file whose mere existence means "on"). When enabled, the
// contents of pool1 + the move-list slots are persisted to a
// per-character "scratch" drill file every time:
//   - the user leaves practice (detected -> not detected), or
//   - the CPU character changes mid-session.
// Saves replace the file atomically from a dedicated worker. Empty or invalid
// captures preserve the previous file. Pending writes may be lost on process exit.
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
// Autoload waits for discovered readiness and allocates the recording pool only
// at the owning update boundary. Unsupported native contracts disable the load.

namespace opendojo::autosave {

bool is_enabled();
void set_enabled(bool on);

// Call from the native scheduler completion boundary. Cheap when disabled.
void tick();

// Queue the current character's owned snapshot for autosave, ignoring the
// normal "on transition" trigger. Called from the WndProc subclass on
// WM_CLOSE so the user's slot state survives a direct quit from
// practice mode (where neither the leave-practice nor character-change
// trigger would fire). Does not wait for disk I/O. Skips a busy state lock
// on the game thread to avoid blocking teardown. No-op if autosave is disabled, if
// we're not in a match, or if no character is known.
void flush_now();

// Called from practice_state::poll() on the practice-slot 0→nonzero
// transition (practice entry). Clears stale prev-tick state so the
// upcoming character is detected as a fresh entry and the normal
// autoload flow (round-active wait → load_drill) kicks off.
void on_practice_entered();
// A manual slot operation supersedes pending autoload/recovery for this session.
void on_manual_action();

}  // namespace opendojo::autosave
