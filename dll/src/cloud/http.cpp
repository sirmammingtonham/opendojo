#include "cloud/http.hpp"

#include <windows.h>
#include <winhttp.h>

#include <cstring>
#include <string>

#include "log.hpp"

namespace opendojo::cloud::http {

namespace {

constexpr DWORD kTimeoutMs = 15'000;
constexpr DWORD kMaxBodyBytes = 256 * 1024;

// Simple RAII for the WinHTTP HINTERNET handles. Order matters at
// destruction: the session owns connections owns requests, but
// WinHttpCloseHandle is safe in any order.
struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
    operator HINTERNET() const { return h; }
};

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

Response fail(const char* what) {
    Response r;
    r.transport_error = true;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s (winhttp err=0x%08lX)", what, GetLastError());
    r.error_message = buf;
    OPENDOJO_LOG("cloud/http: %s", r.error_message.c_str());
    return r;
}

// Build the "Name: Value\r\n..." block WinHttpSendRequest expects.
// Empty input returns an empty string; caller passes NULL/0 in that
// case (WinHTTP requires NULL, not L"").
std::wstring build_headers(const std::vector<Header>& headers) {
    std::string utf8;
    utf8.reserve(headers.size() * 64);
    for (const auto& h : headers) {
        utf8 += h.name;
        utf8 += ": ";
        utf8 += h.value;
        utf8 += "\r\n";
    }
    return widen(utf8);
}

Response do_request(const std::string& url, const std::wstring& verb,
                    const std::vector<Header>& headers, std::string_view body) {
    // Crack the URL into its components.
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;

    auto wurl = widen(url);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        return fail("WinHttpCrackUrl");
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) {
        Response r;
        r.transport_error = true;
        r.error_message = "refusing non-https URL";
        OPENDOJO_LOG("cloud/http: refusing non-https URL");
        return r;
    }

    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength > 0) {
        path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    }

    Handle session{WinHttpOpen(L"OpenDojo/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session) return fail("WinHttpOpen");

    WinHttpSetTimeouts(session, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
    // Force TLS 1.2+ even on older Win10 builds where AUTOMATIC may
    // negotiate down. Modern values:
    DWORD secure_protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    secure_protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &secure_protocols,
                     sizeof(secure_protocols));

    Handle connection{WinHttpConnect(session, host.c_str(),
                                     uc.nPort ? uc.nPort : INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (!connection) return fail("WinHttpConnect");

    Handle request{WinHttpOpenRequest(connection, verb.c_str(), path.c_str(), nullptr,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      WINHTTP_FLAG_SECURE)};
    if (!request) return fail("WinHttpOpenRequest");

    // Explicitly disable redirect-following so we never leak the auth
    // header to a non-Supabase host.
    DWORD disable_redirects = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable_redirects,
                     sizeof(disable_redirects));

    // Transparent gzip/deflate. WinHTTP injects Accept-Encoding into
    // the request and unwraps Content-Encoding from the response, so
    // ReadData returns plain bytes. Drill JSON compresses to roughly
    // 15-25% of raw, which directly trims our Supabase bandwidth
    // budget on every list + download.
    //
    // Added in Win8.1; guarded so older SDK headers still compile.
#ifdef WINHTTP_OPTION_DECOMPRESSION
    {
        DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
        // Failure here is non-fatal — the request will just go out
        // without Accept-Encoding and we'll receive uncompressed bytes.
        WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));
    }
#endif

    auto hdrs = build_headers(headers);
    const wchar_t* hdr_ptr = hdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdrs.c_str();
    DWORD hdr_len = hdrs.empty() ? 0 : (DWORD)-1;

    void* body_ptr = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
    DWORD body_len = static_cast<DWORD>(body.size());

    if (!WinHttpSendRequest(request, hdr_ptr, hdr_len, body_ptr, body_len, body_len, 0)) {
        return fail("WinHttpSendRequest");
    }
    if (!WinHttpReceiveResponse(request, nullptr)) {
        return fail("WinHttpReceiveResponse");
    }

    Response out;

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                             WINHTTP_NO_HEADER_INDEX)) {
        return fail("WinHttpQueryHeaders(status)");
    }
    out.status = static_cast<long>(status);

    // Drain the body. Cap at kMaxBodyBytes — anything over that is
    // either a misbehaving server or an attack.
    out.body.reserve(4096);
    while (true) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail)) {
            return fail("WinHttpQueryDataAvailable");
        }
        if (avail == 0) break;
        if (out.body.size() + avail > kMaxBodyBytes) {
            out.transport_error = true;
            out.error_message = "response body exceeded cap";
            OPENDOJO_LOG("cloud/http: response > %u bytes, aborting", kMaxBodyBytes);
            return out;
        }
        std::size_t old_size = out.body.size();
        out.body.resize(old_size + avail);
        DWORD read = 0;
        if (!WinHttpReadData(request, out.body.data() + old_size, avail, &read)) {
            return fail("WinHttpReadData");
        }
        out.body.resize(old_size + read);
        if (read == 0) break;
    }

    return out;
}

}  // namespace

Response get(const std::string& url, const std::vector<Header>& headers) {
    return do_request(url, L"GET", headers, {});
}

Response post(const std::string& url, const std::vector<Header>& headers, std::string_view body) {
    return do_request(url, L"POST", headers, body);
}

}  // namespace opendojo::cloud::http
