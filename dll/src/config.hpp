#pragma once

#include <cstdint>
#include <string>

// All persistent state lives in opendojo/config.json. This module
// owns the single source of truth — bind preferences, cloud auth
// tokens, and the author handle all flow through here. Other modules
// (handle.cpp, auth.cpp) delegate their persistence to the accessors
// below.
//
// On first load() we also migrate state from the older split files
// (opendojo/cloud.json, opendojo/handle.txt) into config.json and
// delete the originals.

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

}  // namespace opendojo::config
