#pragma once

#include <cstdint>

// Byte-pattern signatures for things we hook in Polaris-Win64-Shipping.exe.
//
// Why: Tekken patches every few months. Hardcoded RVAs break on every patch
// because relink shifts every offset. AOB scans (find a function by its
// distinctive byte pattern instead of its address) survive patches that
// don't touch the function body itself — usually most of them.
//
// How: resolve_all() scans the .text section at DLL init and caches
// each function's address. Patterns include wildcards (??) for any
// RIP-relative immediates inside, since those shift if data layout
// changes even when the surrounding instructions don't.
//
// Failure mode: if a signature doesn't match (or matches >1 time, which
// suggests the pattern isn't unique enough), the getter returns 0 and
// the corresponding feature silently no-ops. Each scan failure is logged
// loudly so the patch breakage is obvious in the log.

namespace opendojo::signatures {

// Native boundaries and UObject helpers; zero when their instruction contracts fail.
struct RuntimeLayout {
    std::uintptr_t update = 0, game_thread_id = 0, scheduler_slot = 0;
    std::uintptr_t engine_free = 0, weak_assign = 0, weak_valid = 0, weak_get = 0;
    std::uintptr_t scheduler = 0;
};
RuntimeLayout runtime_layout();

// Fields decoded from the complete native hash-table lookup leaf function.
struct SubsystemLayout {
    std::uint32_t map = 0, mask = 0, sentinel = 0, buckets = 0;
    std::uint32_t bucket_stride = 0, bucket_last = 0;
    std::uint32_t key = 0, previous = 0, value = 0;
};
SubsystemLayout subsystem_layout();

// Native getter also validates eight slots with eight-byte entries.
std::uint32_t slot_flag_base();

struct MovelistLayout {
    std::uint32_t human_side = 0, element_stride = 0, move_ids = 0, vector_end = 0;
};
MovelistLayout movelist_layout();

// Native fields used by the existing recording-session mutations.
struct RecordingStateLayout {
    std::uint32_t counter = 0, pause = 0, side_record = 0, recording_state = 0;
};
RecordingStateLayout recording_state_layout();

struct PlayerLayout {
    std::uint32_t p1 = 0, p2 = 0, character = 0, native_bias = 0;
};
PlayerLayout player_layout();

struct ReflectionLayout {
    std::uint32_t object_class = 0, object_name = 0, object_flags = 0;
    std::uint32_t children = 0, child_properties = 0, field_next = 0;
    std::uint32_t ffield_name = 0, ffield_next = 0, property_offset = 0;
    std::uint32_t native_function = 0, super_getter_slot = 0, name_blocks = 0, name_text = 0;
    std::uintptr_t process_event = 0, name_pool = 0;
    std::uint32_t function_parms_size = 0;
};
ReflectionLayout reflection_layout();
// Validate the supported FString ABI through the reflected SetRawText native target.
bool native_text_abi_supported(std::uintptr_t raw_text);
bool native_object_array_abi_supported(std::uintptr_t enumerate);

struct SessionLayout {
    std::uint32_t pending = 0;
    std::uint32_t player_flag = 0;  // Relative to the holder's Player pointer.
    std::uint32_t finished = 0;
    std::uint32_t active_mask = 0;
};
// Decoded from the native finalizer. All zero if its code is unavailable.
SessionLayout session_layout();

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
bool live_recordings_supported();

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
