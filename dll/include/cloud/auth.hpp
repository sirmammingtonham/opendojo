#pragma once

#include <string>

// Anonymous-auth identity for OpenDojo Cloud.
//
// First call to ensure_valid() either:
//   * reads a persisted token bundle from opendojo/cloud.json and
//     refreshes if it's near expiry, or
//   * if no bundle exists, signs the install in anonymously via
//     Supabase auth and persists the result.
//
// Anonymous sign-in needs to be enabled on the Supabase project
// (Authentication → Providers → Anonymous Sign-Ins). Without it the
// signup endpoint returns 422 and ensure_valid() returns false.
//
// Everything in here is called from the cloud worker thread only;
// callers should never block the render thread on these.

namespace opendojo::cloud::auth {

// Returns true if we have a non-expired access token (refreshing or
// signing up as needed). On failure logs the reason and returns
// false — callers should toast a generic "couldn't reach OpenDojo
// Cloud" rather than expose details.
bool ensure_valid();

// The current access token (bearer), or empty if ensure_valid()
// hasn't succeeded yet. Cheap to call — read of an atomic-protected
// std::string under a short mutex.
std::string access_token();

// Stable per-install UUID, useful for "my uploads" filters and log
// correlation. Empty until we've signed in once.
std::string user_id();

// Wipe local credentials. Mainly for diagnostics — there's no UI
// path that triggers it. Next ensure_valid() will sign up fresh.
void forget();

}  // namespace opendojo::cloud::auth
