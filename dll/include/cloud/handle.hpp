#pragma once

#include <string>

// Author handle the mod stamps onto uploaded drills.
//
// Lifecycle:
//   * First ever call (file opendojo/handle.txt does not exist) seeds
//     the persisted value once with the live Steam persona. After that
//     the file is the source of truth — Steam name changes are not
//     re-pulled, so a user who picks "Komugi" keeps "Komugi" even if
//     they later rename their Steam account.
//   * An empty-but-present file means the user explicitly cleared the
//     handle (upload anonymously). We do NOT re-seed in that state.
//   * The user can edit / reset via the Settings tab.

namespace opendojo::cloud::handle {

// Effective handle to send on an upload. Returns "" if the user
// has explicitly cleared their handle, or if Steam wasn't available
// at first-seed time and no manual value was ever entered.
std::string current();

// Replace the persisted handle. Trims whitespace. Empty `value`
// writes an empty file — the next current() call returns "" rather
// than re-seeding from Steam.
void set(const std::string& value);

// Force-pull the current Steam persona into the persisted file,
// overwriting whatever was there. Used by the Settings tab's
// "Reset to Steam name" button.
void reset_to_steam();

}  // namespace opendojo::cloud::handle
