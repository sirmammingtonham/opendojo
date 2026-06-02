#include "config.hpp"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>

#include "commands.hpp"
#include "log.hpp"

namespace opendojo::config {

namespace {

using json = nlohmann::json;

std::mutex g_mtx;
json g_doc = json::object();  // in-memory copy of opendojo/config.json

// Cached fast-paths for hot accessors that hooks/render code call
// every frame. These shadow the json doc; writers update both.
std::atomic<std::uint32_t> g_toggle_vk{VK_F12};
std::atomic<bool> g_capturing{false};
std::atomic<std::uint32_t> g_captured_vk{0};

std::atomic<std::uint16_t> g_toggle_pad_btn{XINPUT_GAMEPAD_Y};
std::atomic<bool> g_pad_capturing{false};
std::atomic<std::uint16_t> g_captured_pad_btn{0};

std::filesystem::path config_path() {
    return opendojo::commands::drills_dir() / L"config.json";
}

std::filesystem::path legacy_cloud_path() {
    return opendojo::commands::drills_dir() / L"cloud.json";
}

std::filesystem::path legacy_handle_path() {
    return opendojo::commands::drills_dir() / L"handle.txt";
}

std::string trim_str(std::string s) {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

void write_locked() {
    auto path = config_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        OPENDOJO_LOG("config: failed to open %ls for write", path.c_str());
        return;
    }
    // Pretty-print so a curious user opening the file can read it.
    f << g_doc.dump(2);
}

// ---- migration: fold the older split files into the unified doc ----
//
// Old layout (pre-consolidation):
//   opendojo/config.json   { toggle_vk, toggle_pad_btn }
//   opendojo/cloud.json    { access_token, refresh_token, user_id, expires_at }
//   opendojo/handle.txt    plain text, single line, the handle
//
// We read whichever of those exist, splat the values into the new
// doc under their canonical keys, and delete the old files. Idempotent:
// missing inputs are silently skipped, so a fresh install does nothing.
void migrate_legacy_locked() {
    bool changed = false;

    // cloud.json -> cloud.auth.*
    auto cloud_path = legacy_cloud_path();
    std::error_code ec;
    if (std::filesystem::exists(cloud_path, ec)) {
        std::ifstream f(cloud_path, std::ios::binary);
        if (f) {
            auto j = json::parse(f, nullptr, false);
            if (j.is_object()) {
                if (!g_doc.contains("cloud")) g_doc["cloud"] = json::object();
                if (!g_doc["cloud"].contains("auth")) g_doc["cloud"]["auth"] = json::object();
                auto& dst = g_doc["cloud"]["auth"];
                if (j.contains("access_token")) dst["access_token"] = j["access_token"];
                if (j.contains("refresh_token")) dst["refresh_token"] = j["refresh_token"];
                if (j.contains("user_id")) dst["user_id"] = j["user_id"];
                if (j.contains("expires_at")) dst["expires_at"] = j["expires_at"];
                changed = true;
                OPENDOJO_LOG("config: migrated cloud.json into config.json");
            }
        }
        std::filesystem::remove(cloud_path, ec);
    }

    // handle.txt -> cloud.author_handle
    auto handle_path = legacy_handle_path();
    if (std::filesystem::exists(handle_path, ec)) {
        std::ifstream f(handle_path, std::ios::binary);
        if (f) {
            std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            auto v = trim_str(std::move(s));
            if (!g_doc.contains("cloud")) g_doc["cloud"] = json::object();
            // Only set if the new field isn't already present — the
            // user might have written through the new API by now.
            if (!g_doc["cloud"].contains("author_handle")) {
                g_doc["cloud"]["author_handle"] = v;
                changed = true;
                OPENDOJO_LOG("config: migrated handle.txt -> author_handle '%s'", v.c_str());
            }
        }
        std::filesystem::remove(handle_path, ec);
    }

    if (changed) write_locked();
}

void hydrate_caches_locked() {
    if (g_doc.contains("toggle_vk") && g_doc["toggle_vk"].is_number_integer()) {
        auto v = g_doc["toggle_vk"].get<std::int64_t>();
        if (v > 0) g_toggle_vk.store(static_cast<std::uint32_t>(v));
    }
    if (g_doc.contains("toggle_pad_btn") && g_doc["toggle_pad_btn"].is_number_integer()) {
        auto v = g_doc["toggle_pad_btn"].get<std::int64_t>();
        if (v > 0) g_toggle_pad_btn.store(static_cast<std::uint16_t>(v & 0xFFFF));
    }
}

}  // namespace

void load() {
    std::lock_guard lk(g_mtx);

    // Parse the main file. Missing or unreadable => start from {}.
    auto path = config_path();
    std::ifstream f(path, std::ios::binary);
    if (f) {
        auto parsed = json::parse(f, nullptr, false);
        if (parsed.is_object()) {
            g_doc = std::move(parsed);
        } else {
            OPENDOJO_LOG("config: %ls present but unparseable, starting fresh", path.c_str());
        }
    } else {
        OPENDOJO_LOG("config: no config file at %ls — using defaults", path.c_str());
    }

    migrate_legacy_locked();
    hydrate_caches_locked();

    OPENDOJO_LOG("config: loaded toggle_vk=0x%02X toggle_pad_btn=0x%04X", g_toggle_vk.load(),
                 g_toggle_pad_btn.load());
}

void save() {
    std::lock_guard lk(g_mtx);
    write_locked();
}

// ---- Binds ----------------------------------------------------------------

std::uint32_t toggle_vk() {
    return g_toggle_vk.load();
}

void set_toggle_vk(std::uint32_t vk) {
    if (vk == 0) return;
    g_toggle_vk.store(vk);
    {
        std::lock_guard lk(g_mtx);
        g_doc["toggle_vk"] = vk;
        write_locked();
    }
}

void start_capture() {
    g_captured_vk.store(0);
    g_capturing.store(true);
}

void cancel_capture() {
    g_capturing.store(false);
    g_captured_vk.store(0);
}

bool is_capturing() {
    return g_capturing.load();
}

std::uint32_t consume_captured_vk() {
    auto v = g_captured_vk.exchange(0);
    if (v != 0) g_capturing.store(false);
    return v;
}

void notify_captured_vk(std::uint32_t vk) {
    if (g_capturing.load()) g_captured_vk.store(vk);
}

std::uint16_t toggle_pad_btn() {
    return g_toggle_pad_btn.load();
}

void set_toggle_pad_btn(std::uint16_t mask) {
    if (mask == 0) return;
    g_toggle_pad_btn.store(mask);
    {
        std::lock_guard lk(g_mtx);
        g_doc["toggle_pad_btn"] = mask;
        write_locked();
    }
}

void start_pad_capture() {
    g_captured_pad_btn.store(0);
    g_pad_capturing.store(true);
}

void cancel_pad_capture() {
    g_pad_capturing.store(false);
    g_captured_pad_btn.store(0);
}

bool is_pad_capturing() {
    return g_pad_capturing.load();
}

std::uint16_t consume_captured_pad_btn() {
    auto v = g_captured_pad_btn.exchange(0);
    if (v != 0) g_pad_capturing.store(false);
    return v;
}

void notify_captured_pad_btn(std::uint16_t mask) {
    if (g_pad_capturing.load()) g_captured_pad_btn.store(mask);
}

// ---- Cloud author handle --------------------------------------------------

bool author_handle_exists() {
    std::lock_guard lk(g_mtx);
    return g_doc.contains("cloud") && g_doc["cloud"].is_object() &&
           g_doc["cloud"].contains("author_handle") && g_doc["cloud"]["author_handle"].is_string();
}

std::string author_handle() {
    std::lock_guard lk(g_mtx);
    if (!g_doc.contains("cloud") || !g_doc["cloud"].is_object()) return {};
    return g_doc["cloud"].value("author_handle", std::string{});
}

void set_author_handle(const std::string& value) {
    auto trimmed = trim_str(value);
    std::lock_guard lk(g_mtx);
    if (!g_doc.contains("cloud") || !g_doc["cloud"].is_object()) {
        g_doc["cloud"] = json::object();
    }
    g_doc["cloud"]["author_handle"] = trimmed;
    write_locked();
}

// ---- Cloud auth tokens ----------------------------------------------------

AuthTokens auth_tokens() {
    AuthTokens t;
    std::lock_guard lk(g_mtx);
    if (!g_doc.contains("cloud") || !g_doc["cloud"].is_object()) return t;
    if (!g_doc["cloud"].contains("auth") || !g_doc["cloud"]["auth"].is_object()) return t;
    const auto& a = g_doc["cloud"]["auth"];
    t.access_token = a.value("access_token", std::string{});
    t.refresh_token = a.value("refresh_token", std::string{});
    t.user_id = a.value("user_id", std::string{});
    t.expires_at_sec = a.value("expires_at", static_cast<std::int64_t>(0));
    return t;
}

void set_auth_tokens(const AuthTokens& tokens) {
    std::lock_guard lk(g_mtx);
    if (!g_doc.contains("cloud") || !g_doc["cloud"].is_object()) {
        g_doc["cloud"] = json::object();
    }
    json& a = g_doc["cloud"]["auth"];
    if (!a.is_object()) a = json::object();
    a["access_token"] = tokens.access_token;
    a["refresh_token"] = tokens.refresh_token;
    a["user_id"] = tokens.user_id;
    a["expires_at"] = tokens.expires_at_sec;
    write_locked();
}

}  // namespace opendojo::config
