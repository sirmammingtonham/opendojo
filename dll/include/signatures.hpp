#pragma once

#include <cstdint>

// Byte-pattern signatures for things we hook in Polaris-Win64-Shipping.exe.
//
// Why: Tekken patches every few months. Hardcoded RVAs break on every patch
// because relink shifts every offset. AOB scans (find a function by its
// distinctive byte pattern instead of its address) survive patches that
// don't touch the function body itself — usually most of them.
//
// How: resolve_all() walks the .text section once at DLL init and caches
// each function's address. Patterns include wildcards (??) for any
// RIP-relative immediates inside, since those shift if data layout
// changes even when the surrounding instructions don't.
//
// Failure mode: if a signature doesn't match (or matches >1 time, which
// suggests the pattern isn't unique enough), the getter returns 0 and
// the corresponding feature silently no-ops. Each scan failure is logged
// loudly so the patch breakage is obvious in the log.

namespace opendojo::signatures {

// Scan Polaris's .text for all known patterns. Call once at DLL init,
// before any hook installs. Returns true iff every required signature
// resolved to a unique match. Logs detail either way.
bool resolve_all();

// Resolved runtime addresses. 0 if scan failed for that target.
// Always safe to call; never blocks.

// Code addresses (in .text).
std::uintptr_t practice_dtor();
std::uintptr_t player_refresh();
std::uintptr_t pool_init();

// Data slot addresses (in .data). Resolved by decoding RIP-relative
// instructions inside the corresponding code site, so a Tekken patch
// that shifts data layout doesn't break us as long as the code that
// touches the slot keeps the same shape.
//
// Returns ABSOLUTE addresses (already polaris_base + offset). 0 if
// unresolved.
std::uintptr_t practice_slot_addr();
std::uintptr_t pool1_ptr_addr();
std::uintptr_t pool2_ptr_addr();
std::uintptr_t ctx_ptr_addr();

}  // namespace opendojo::signatures
