#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include "recording_name.hpp"

// Runtime store of per-slot display names. Populated when a drill is loaded
// (each recording's name maps to the slot it was installed into) and read by
// practice_rename to relabel the practice-menu "CPU Opponent Action N" rows.
//
// Names are UTF-8. An empty name means "no custom label" — practice_rename
// leaves that row's original game text untouched.

namespace opendojo::slot_labels {

inline constexpr std::size_t COUNT = 8;
inline constexpr std::size_t MAX_NAME_CHARS = recording_name::MAX_CHARS;
inline constexpr std::size_t NAME_BUFFER_SIZE = recording_name::BUFFER_SIZE;

// Byte length of a safe prefix of at most 32 UTF-8 code points.
std::size_t name_prefix_size(std::string_view name);

// Set / clear the name for slot `idx` (0-based), capped at 32 code points.
// Out-of-range is ignored. The cap also applies to names from imported drills.
void set(std::size_t idx, std::string_view name);
void clear_all();

// Copy of slot `idx`'s name, or "" if unset / out of range. Thread-safe.
std::string get(std::size_t idx);

}  // namespace opendojo::slot_labels
