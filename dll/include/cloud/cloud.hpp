#pragma once

#include <string>

// Top-level cloud module helpers. The cloud URL + access key are baked into
// the DLL at compile time via CMake cache variables (OPENDOJO_CLOUD_URL /
// OPENDOJO_CLOUD_PROXY_KEY). Builds without values produce a DLL that still
// loads — the menu just renders a "cloud features not configured" state.

namespace opendojo::cloud {

// True when both the proxy URL and proxy key are non-empty. The menu uses
// this to gate the Browse tab + Upload button.
bool configured();

// Always non-null. Empty string if not configured.
const std::string& base_url();
// Access key sent with each request.
const std::string& proxy_key();

// Tekken patch this DLL was *confirmed working on* (e.g. "3.01.01").
const std::string& confirmed_tekken_version();

// This DLL's own build version (CMake PROJECT_VERSION, e.g. "0.1.0").
// Recorded on every uploaded drill so the server can tell which
// client revision produced a given recording.
const std::string& dll_version();

// PostgREST endpoint: <base>/rest/v1
std::string rest_url();
// Auth endpoint:      <base>/auth/v1
std::string auth_url();
// Edge Functions:     <base>/functions/v1
std::string functions_url();

}  // namespace opendojo::cloud
