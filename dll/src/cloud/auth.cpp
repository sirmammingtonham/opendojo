#include "auth.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>

#include "../log.hpp"
#include "cloud.hpp"
#include "http.hpp"

namespace opendojo::cloud::auth {

namespace {

using clock = std::chrono::system_clock;

struct Token {
    std::string access;
    std::string refresh;
    std::string user_id;
    clock::time_point expires_at{};
    bool valid() const { return !access.empty() && expires_at > clock::now(); }
    bool near_expiry() const { return expires_at < clock::now() + std::chrono::seconds(60); }
};

// All token mutations go through g_mutex; reads of the current
// access_token / user_id snapshot it once under the mutex.
std::mutex g_mutex;
Token g_token;

nlohmann::json read_file_json(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return nullptr;
    try {
        return nlohmann::json::parse(f, /*cb*/ nullptr, /*allow_exceptions*/ false);
    } catch (...) { return nullptr; }
}

void persist(const Token& t) {
    nlohmann::json j;
    j["access_token"] = t.access;
    j["refresh_token"] = t.refresh;
    j["user_id"] = t.user_id;
    j["expires_at"] =
        std::chrono::duration_cast<std::chrono::seconds>(t.expires_at.time_since_epoch()).count();

    auto path = opendojo::cloud::token_store_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        OPENDOJO_LOG("cloud/auth: failed to write %ls", path.c_str());
        return;
    }
    f << j.dump();
}

bool load_from_disk(Token& out) {
    auto path = opendojo::cloud::token_store_path();
    auto j = read_file_json(path);
    if (j.is_null() || !j.is_object()) return false;
    try {
        out.access = j.value("access_token", "");
        out.refresh = j.value("refresh_token", "");
        out.user_id = j.value("user_id", "");
        auto secs = j.value("expires_at", static_cast<long long>(0));
        out.expires_at = clock::time_point{std::chrono::seconds(secs)};
        return !out.access.empty() && !out.refresh.empty();
    } catch (...) { return false; }
}

// Parse the Supabase auth response shape:
//   { "access_token": "...", "refresh_token": "...", "expires_in": 3600,
//     "user": { "id": "..." } }
bool apply_auth_response(const std::string& body, Token& out) {
    auto j = nlohmann::json::parse(body, nullptr, false);
    if (!j.is_object()) return false;

    auto access = j.value("access_token", std::string{});
    auto refresh = j.value("refresh_token", std::string{});
    auto expires = j.value("expires_in", 3600);
    if (access.empty() || refresh.empty()) return false;

    std::string uid;
    if (j.contains("user") && j["user"].is_object()) { uid = j["user"].value("id", std::string{}); }
    // Refresh tokens carry the user id back too, but only on refresh.
    if (uid.empty()) { uid = j.value("user_id", std::string{}); }

    out.access = std::move(access);
    out.refresh = std::move(refresh);
    out.expires_at = clock::now() + std::chrono::seconds(expires);
    if (!uid.empty()) out.user_id = std::move(uid);
    return true;
}

bool sign_up_anonymous(Token& out) {
    auto url = opendojo::cloud::auth_url() + "/signup";
    std::vector<opendojo::cloud::http::Header> headers = {
        {"apikey", opendojo::cloud::anon_key()},
        {"Content-Type", "application/json"},
    };
    // Empty object body — Supabase treats POST /signup with no email
    // as an anonymous sign-up when the project has anon-auth enabled.
    auto res = opendojo::cloud::http::post(url, headers, "{}");
    if (res.transport_error) {
        OPENDOJO_LOG("cloud/auth: signup transport error: %s", res.error_message.c_str());
        return false;
    }
    if (res.status < 200 || res.status >= 300) {
        OPENDOJO_LOG("cloud/auth: signup HTTP %ld body=%s", res.status, res.body.c_str());
        return false;
    }
    return apply_auth_response(res.body, out);
}

bool refresh_token(Token& out) {
    auto url = opendojo::cloud::auth_url() + "/token?grant_type=refresh_token";
    nlohmann::json body;
    body["refresh_token"] = out.refresh;
    std::vector<opendojo::cloud::http::Header> headers = {
        {"apikey", opendojo::cloud::anon_key()},
        {"Content-Type", "application/json"},
    };
    auto res = opendojo::cloud::http::post(url, headers, body.dump());
    if (res.transport_error) {
        OPENDOJO_LOG("cloud/auth: refresh transport error: %s", res.error_message.c_str());
        return false;
    }
    if (res.status < 200 || res.status >= 300) {
        // 4xx on refresh means the refresh token was revoked or the
        // user was deleted; surrender and force a fresh anon signup.
        OPENDOJO_LOG("cloud/auth: refresh HTTP %ld — wiping local token", res.status);
        out = {};
        return false;
    }
    // The refresh response carries forward the same user id, but
    // older Supabase builds omit it; preserve what we had.
    auto previous_uid = out.user_id;
    if (!apply_auth_response(res.body, out)) return false;
    if (out.user_id.empty()) out.user_id = previous_uid;
    return true;
}

}  // namespace

bool ensure_valid() {
    if (!opendojo::cloud::configured()) return false;

    std::lock_guard lk(g_mutex);

    if (g_token.access.empty()) {
        // First call this session — try the disk cache.
        load_from_disk(g_token);
    }

    if (g_token.valid() && !g_token.near_expiry()) return true;

    // Refresh path: only if we have a refresh token. After a wipe
    // (forget() / failed refresh) this falls through to anonymous
    // signup below.
    if (!g_token.refresh.empty()) {
        if (refresh_token(g_token)) {
            persist(g_token);
            return true;
        }
        // fall through to sign-up
    }

    Token fresh;
    if (!sign_up_anonymous(fresh)) {
        OPENDOJO_LOG("cloud/auth: ensure_valid: anonymous signup failed");
        return false;
    }
    g_token = std::move(fresh);
    persist(g_token);
    OPENDOJO_LOG("cloud/auth: anonymous identity issued (user_id=%s)", g_token.user_id.c_str());
    return true;
}

std::string access_token() {
    std::lock_guard lk(g_mutex);
    return g_token.access;
}

std::string user_id() {
    std::lock_guard lk(g_mutex);
    return g_token.user_id;
}

void forget() {
    std::lock_guard lk(g_mutex);
    g_token = {};
    std::error_code ec;
    std::filesystem::remove(opendojo::cloud::token_store_path(), ec);
}

}  // namespace opendojo::cloud::auth
