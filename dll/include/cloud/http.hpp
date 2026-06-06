#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Tiny sync HTTPS client built on WinHTTP. The rest of the cloud
// module assumes:
//   * TLS 1.2+ only (WinHTTP default on Win10+).
//   * No redirect-following — every Supabase endpoint we hit serves
//     terminal responses; following 3xx would let a misconfigured
//     project leak our auth header to a third party.
//   * Body capped at 256 KB. A drill list page tops out around ~10 KB
//     and a single drill at ~64 KB; cap leaves headroom and stops a
//     compromised server from making us eat unbounded memory.
//   * 15-second total timeout. Async cancellation isn't worth the
//     code — the worker thread either gets a response or we toast
//     a network error.
//
// All calls are blocking. Callers should run them on the cloud
// worker thread, not the render thread.

namespace opendojo::cloud::http {

struct Header {
    std::string name;
    std::string value;
};

struct Response {
    // 0 if we never got past TLS / DNS. Otherwise the HTTP status code.
    long status = 0;
    std::string body;

    // True when the request never reached the server (DNS, TLS,
    // timeout). UI shows a generic "couldn't reach OpenDojo Cloud"
    // toast for these; for non-zero `status`, callers inspect the
    // body to surface server messages.
    bool transport_error = false;
    std::string error_message;  // human-readable, for logs
};

// `url` must be https://... — http:// is rejected. `headers` is
// applied verbatim; do not include Content-Length (WinHTTP sets it
// from `body`'s size). Authorization, apikey, etc. are caller-owned.
Response get(const std::string& url, const std::vector<Header>& headers);
Response post(const std::string& url, const std::vector<Header>& headers, std::string_view body);

}  // namespace opendojo::cloud::http
