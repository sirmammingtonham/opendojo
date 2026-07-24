#pragma once

#include <cstddef>
#include <string>

// Runtime store of per-slot display names. Populated when a drill is loaded
// (each recording's name maps to the slot it was installed into) and read by
// practice_rename to relabel the practice-menu "CPU Opponent Action N" rows.
//
// Names are UTF-8. An empty name means "no custom label" — practice_rename
// leaves that row's original game text untouched.

namespace opendojo::slot_labels {

inline constexpr std::size_t COUNT = 8;

// Set / clear the name for slot `idx` (0-based). Out-of-range is ignored.
void set(std::size_t idx, std::string name);
void clear_all();

// Copy of slot `idx`'s name, or "" if unset / out of range. Thread-safe.
std::string get(std::size_t idx);

}  // namespace opendojo::slot_labels
