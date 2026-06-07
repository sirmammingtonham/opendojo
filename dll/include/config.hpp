#pragma once

#include <cstdint>
#include <string>
#include <vector>

// This module owns all persistent state, split across two files:
//   * Settings — opendojo/config.json, beside the game executable:
//     bind preferences and the author handle.
//   * Upload identity — %LOCALAPPDATA%\OpenDojo\identity.json: the
//     anonymous auth bundle (access/refresh tokens + user id). It lives
//     in AppData so it survives a game update or reinstall, which wipes
//     the game-dir folder; losing it would orphan a user's uploads.
// Other modules (handle.cpp, auth.cpp) delegate persistence to the
// accessors below.
//
// On first load() we migrate older layouts forward: the auth bundle from
// an embedded config.json `cloud.auth` object or the even-older
// opendojo/cloud.json moves to identity.json; opendojo/handle.txt moves
// to the author handle. Stale copies are deleted.

namespace opendojo::config {

// Load config.json from disk, run one-time migrations, hold the
// parsed doc in memory. Called once at DLL init.
void load();

// Force-flush in-memory state to disk. Most setters call this
// automatically; explicit save() is only needed for batch updates.
void save();

// ---- Menu open binds (existing) -------------------------------------------

std::uint32_t toggle_vk();
void set_toggle_vk(std::uint32_t vk);

void start_capture();
void cancel_capture();
bool is_capturing();
std::uint32_t consume_captured_vk();  // 0 if nothing pending
void notify_captured_vk(std::uint32_t vk);

std::uint16_t toggle_pad_btn();
void set_toggle_pad_btn(std::uint16_t mask);

void start_pad_capture();
void cancel_pad_capture();
bool is_pad_capturing();
std::uint16_t consume_captured_pad_btn();  // 0 if nothing pending
void notify_captured_pad_btn(std::uint16_t mask);

// ---- Cloud author handle --------------------------------------------------

// True iff the handle field has ever been written to disk. Used by
// handle.cpp to distinguish "first ever launch" (seed from Steam)
// from "user explicitly cleared their handle" (leave empty).
bool author_handle_exists();
std::string author_handle();                   // "" if missing
void set_author_handle(const std::string& v);  // also marks exists=true

// ---- Cloud anonymous-auth tokens ------------------------------------------

struct AuthTokens {
    std::string access_token;
    std::string refresh_token;
    std::string user_id;
    std::int64_t expires_at_sec = 0;  // unix seconds
};

// Returns all-empty tokens if not signed in.
AuthTokens auth_tokens();
void set_auth_tokens(const AuthTokens& tokens);

// ---- Pinned drills --------------------------------------------------------
// Identified by file name (with extension) within opendojo/, so renames
// or directory moves cleanly un-pin the entry. Pinned drills sort above
// non-pinned ones in the Drills tab.

std::vector<std::string> pinned_drills();
bool is_drill_pinned(const std::string& filename);
void set_drill_pinned(const std::string& filename, bool pinned);

}  // namespace opendojo::config
