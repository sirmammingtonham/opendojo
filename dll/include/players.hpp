#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Live read of P1/P2 character ids and human-vs-CPU side, used at drill
// export time to auto-fill the drill's character/cpu_side header.
//
// Implementation reaches the live Player structs via two AOB patterns in
// Polaris's .text section, then dereferences a stable two-level pointer
// chain (the GlobalPlayerHolder) to read character_id at Player+0x168 and
// main_player_info.player_id (which slot the human is controlling).
//
// Patterns and offsets verified live on Tekken 8 v3.00.02. Originally
// reverse-engineered by Irony (github.com/tomislav-ivankovic/Irony).
//
// Detection works *only inside a practice/match scene* — outside a match
// the holder is null. detect_cpu() returns detected=false in that case.

namespace opendojo::players {

enum class Side : std::uint8_t { p1 = 0, p2 = 1 };

struct CpuInfo {
    bool detected = false;           // false => not in a match / pattern miss
    std::uint32_t character_id = 0;  // raw u32 from the Player struct
    std::string character_name;      // "jin" or "unknown_<id>"
    Side cpu_side = Side::p2;        // which game slot the CPU occupies
};

// One-shot resolution: re-walks the pointer chain on every call (the
// holder and Player addresses are not stable across scenes). Cheap — just
// a handful of memory reads after the first-call pattern scan caches the
// holder-pointer slot address.
CpuInfo detect_cpu();

// Stringify / parse Side for drill file headers.
const char* side_to_string(Side s);
bool parse_side(std::string_view s, Side& out);

// Character id -> lowercase name (e.g. 6 -> "jin"). Returns nullptr for
// ids outside the known roster; callers should format "unknown_<id>".
const char* character_name(std::uint32_t id);

// Sorted list of every playable character name. The cloud Browse
// tab's character-filter combo iterates this so the dropdown stays
// in sync with character_name() when a DLC pass adds new ids — one
// place to update instead of two.
//
// Built from the same id -> name table; NPC-only ids (the 116+
// range) are skipped by virtue of the playable-id ceiling.
std::vector<std::string> character_roster();

// True iff a round is actively in progress (not the intro / round-start
// animation). Reads Player1[0x15C0] = frames_since_round_start (Irony's
// offset for T8). Returns 0 / false when out of a match or during the
// intro phase when input is still locked. Used as the deterministic
// gate for all autoload writes — writing recording-flag state during
// the intro freezes character input until the user manually re-evaluates
// state (pause menu open, Select+A reset).
bool round_active();

// Address of the CPU/opponent's Player struct, or 0 if no match.
// Reaches the same struct as the natural game code via the
// GlobalPlayerHolder chain (more reliable than the service-locator
// players_sub subsystem, which isn't always registered when we'd want
// to write opponent state).
std::uintptr_t cpu_player_address();

// Heap address of the GlobalPlayerHolder, or 0 if the holder slot
// hasn't been populated yet (out of match, or pre-load). Holder+0x30
// and +0x38 are the P1 / P2 player pointer slots. Used by diag_hook
// to install a one-shot hardware write-watch.
std::uintptr_t holder_address();

// Local player display name as Tekken sees it. In practice that's the
// Steam persona on whichever Steam account launched the game — Tekken
// pulls it through steam_api64.dll and uses it for the HUD nameplate.
//
// We resolve the persona by GetProcAddress'ing two exports from the
// already-loaded steam_api64.dll:
//   * SteamFriends                                 -> opaque self ptr
//   * SteamAPI_ISteamFriends_GetPersonaName(self)  -> UTF-8 string
// Using the flat shim keeps us ABI-stable against Steam SDK version
// drift (the shim knows its vtable, we don't).
//
// Returns "" if steam_api64.dll isn't loaded, the symbols aren't
// exported by this game's bundled version, or Steam isn't logged in.
// Callers (the cloud upload path) treat empty as "unknown handle"
// and fall back to the user's manual override from config.
std::string local_username();

}  // namespace opendojo::players
