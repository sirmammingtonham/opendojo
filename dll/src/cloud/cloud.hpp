#pragma once

#include <filesystem>
#include <string>

// Top-level cloud module helpers. The Supabase URL + public anon key
// are baked into the DLL at compile time via CMake cache variables
// (OPENDOJO_SUPABASE_URL / OPENDOJO_SUPABASE_ANON_KEY). Builds without
// values produce a DLL that still loads — the menu just renders a
// "cloud features not configured" state.

namespace opendojo::cloud {

// True when both URL and anon key are non-empty. The menu uses this
// to gate the Browse tab + Upload button.
bool configured();

// Always non-null. Empty string if not configured.
const std::string& base_url();
const std::string& anon_key();

// PostgREST endpoint: <base>/rest/v1
std::string rest_url();
// Auth endpoint:      <base>/auth/v1
std::string auth_url();
// Edge Functions:     <base>/functions/v1
std::string functions_url();

// Path to opendojo/cloud.json (sibling of the existing opendojo/
// drills directory). Persistent home for the anon-auth token bundle.
std::filesystem::path token_store_path();

}  // namespace opendojo::cloud
