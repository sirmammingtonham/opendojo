#include "config.hpp"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>

#include "commands.hpp"
#include "file_io.hpp"
#include "log.hpp"

namespace opendojo::config {

namespace {

using json = nlohmann::json;

std::mutex g_mtx;
// Settings (binds + author handle): opendojo/config.json, beside the game.
json g_doc = json::object();
// Upload identity (anonymous auth bundle): %LOCALAPPDATA%\OpenDojo\identity.json.
// Kept separate from g_doc so it persists across a game reinstall, which wipes
// the game-dir folder.
json g_identity = json::object();

// Cached fast-paths for hot accessors that hooks/render code call
// every frame. These shadow the json doc; writers update both.
std::atomic<std::uint32_t> g_toggle_vk{VK_F12};
std::atomic<bool> g_capturing{false};
std::atomic<std::uint32_t> g_captured_vk{0};

std::atomic<std::uint16_t> g_toggle_pad_btn{XINPUT_GAMEPAD_Y};
std::atomic<bool> g_pad_capturing{false};
std::atomic<std::uint16_t> g_captured_pad_btn{0};

// Settings file: beside the game executable. Re-downloadable/re-recordable
// state, fine to lose on a reinstall.
std::filesystem::path config_path() {
    return opendojo::commands::drills_dir() / L"config.json";
}

// Identity store: %LOCALAPPDATA%\OpenDojo. Survives a game update or reinstall
// (the game-dir folder is wiped on uninstall), so the anonymous upload identity
// isn't lost — losing it would orphan a user's uploads and reset their account.
// Falls back to the game-dir folder if LOCALAPPDATA is somehow unset.
std::filesystem::path appdata_root() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return std::filesystem::path(buf) / L"OpenDojo";
    }
    return opendojo::commands::drills_dir();
}

std::filesystem::path identity_path() {
    return appdata_root() / L"identity.json";
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

bool write_doc_locked(const std::filesystem::path& path, const json& doc) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    // Serialize before touching disk; a failed replacement keeps the old file.
    if (ec || !file_io::replace_file(path, doc.dump(2))) {
        OPENDOJO_LOG("config: failed to save %ls", path.c_str());
        return false;
    }
    return true;
}

bool write_config_locked() {
    return write_doc_locked(config_path(), g_doc);
}
bool write_identity_locked() {
    return write_doc_locked(identity_path(), g_identity);
}

// Copy the four auth fields from a source object into g_identity.
void adopt_identity_fields_locked(const json& src) {
    for (const char* k : {"access_token", "refresh_token", "user_id", "expires_at"}) {
        if (src.contains(k)) g_identity[k] = src[k];
    }
}

// ---- migration: bring older on-disk layouts up to the current split ----
//
// Layouts we may find:
//   * opendojo/cloud.json  — oldest: the auth bundle in its own file.
//   * opendojo/handle.txt  — oldest: the author handle as plain text.
//   * opendojo/config.json with a `cloud.auth` object — the build that kept
//     the identity embedded in the (game-dir) config file.
//
// Current layout: settings stay in opendojo/config.json; the identity lives in
// %LOCALAPPDATA%\OpenDojo\identity.json. So the auth bundle is routed to
// g_identity, the handle stays in g_doc, and stale copies are deleted.
// Idempotent: missing inputs are skipped, so a fresh install does nothing.
void migrate_legacy_locked() {
    std::error_code ec;
    const auto cloud_path = legacy_cloud_path();
    {
        std::ifstream f(cloud_path, std::ios::binary);
        if (f) {
            auto j = json::parse(f, nullptr, false);
            f.close();
            if (j.is_object()) {
                if (g_identity.empty()) adopt_identity_fields_locked(j);
                if (!g_identity.empty() && write_identity_locked()) {
                    std::filesystem::remove(cloud_path, ec);
                    OPENDOJO_LOG("config: migrated legacy upload identity");
                }
            }
        }
    }

    const auto handle_path = legacy_handle_path();
    {
        std::ifstream f(handle_path, std::ios::binary);
        if (f) {
            std::string value((std::istreambuf_iterator<char>(f)), {});
            const bool read_ok = !f.bad();
            f.close();
            if (read_ok) {
                if (!g_doc.contains("cloud") || !g_doc["cloud"].is_object())
                    g_doc["cloud"] = json::object();
                if (!g_doc["cloud"].contains("author_handle"))
                    g_doc["cloud"]["author_handle"] = trim_str(std::move(value));
                if (write_config_locked()) std::filesystem::remove(handle_path, ec);
            }
        }
    }

    if (g_doc.contains("cloud") && g_doc["cloud"].is_object() && g_doc["cloud"].contains("auth") &&
        g_doc["cloud"]["auth"].is_object()) {
        if (g_identity.empty()) adopt_identity_fields_locked(g_doc["cloud"]["auth"]);
        // Keep the source credentials until their destination is durable.
        if (!g_identity.empty() && write_identity_locked()) {
            g_doc["cloud"].erase("auth");
            write_config_locked();
        }
    }
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

    // Parse a JSON-object file into `dst`. Leaves `dst` untouched and returns
    // false if the file is missing or isn't a JSON object.
    auto try_load = [](const std::filesystem::path& p, json& dst) -> bool {
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        auto parsed = json::parse(f, nullptr, false);
        if (!parsed.is_object()) {
            OPENDOJO_LOG("config: %ls present but unparseable, ignoring", p.c_str());
            return false;
        }
        dst = std::move(parsed);
        return true;
    };

    // Settings (game dir) + identity (AppData). Either may be absent on a
    // fresh install; migrate_legacy_locked() then backfills from older layouts.
    if (!try_load(config_path(), g_doc)) {
        OPENDOJO_LOG("config: no config at %ls — using defaults", config_path().c_str());
    }
    try_load(identity_path(), g_identity);

    migrate_legacy_locked();
    hydrate_caches_locked();

    OPENDOJO_LOG("config: loaded toggle_vk=0x%02X toggle_pad_btn=0x%04X", g_toggle_vk.load(),
                 g_toggle_pad_btn.load());
}

void save() {
    std::lock_guard lk(g_mtx);
    write_config_locked();
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
        write_config_locked();
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
        write_config_locked();
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
    const auto it = g_doc["cloud"].find("author_handle");
    return it != g_doc["cloud"].end() && it->is_string() ? it->get<std::string>() : std::string{};
}

void set_author_handle(const std::string& value) {
    auto trimmed = trim_str(value);
    std::lock_guard lk(g_mtx);
    if (!g_doc.contains("cloud") || !g_doc["cloud"].is_object()) {
        g_doc["cloud"] = json::object();
    }
    g_doc["cloud"]["author_handle"] = trimmed;
    write_config_locked();
}

// ---- Cloud auth tokens (the upload identity; stored in AppData) ------------

AuthTokens auth_tokens() {
    AuthTokens t;
    std::lock_guard lk(g_mtx);
    auto string_field = [](const char* key) {
        const auto it = g_identity.find(key);
        return it != g_identity.end() && it->is_string() ? it->get<std::string>() : std::string{};
    };
    t.access_token = string_field("access_token");
    t.refresh_token = string_field("refresh_token");
    t.user_id = string_field("user_id");
    const auto expiry = g_identity.find("expires_at");
    if (expiry != g_identity.end() && expiry->is_number_integer())
        t.expires_at_sec = expiry->get<std::int64_t>();
    return t;
}

void set_auth_tokens(const AuthTokens& tokens) {
    std::lock_guard lk(g_mtx);
    g_identity = json::object();
    g_identity["access_token"] = tokens.access_token;
    g_identity["refresh_token"] = tokens.refresh_token;
    g_identity["user_id"] = tokens.user_id;
    g_identity["expires_at"] = tokens.expires_at_sec;
    write_identity_locked();
}

// ---- Pinned drills --------------------------------------------------------

std::vector<std::string> pinned_drills() {
    std::vector<std::string> out;
    std::lock_guard lk(g_mtx);
    if (!g_doc.contains("pinned_drills") || !g_doc["pinned_drills"].is_array()) return out;
    out.reserve(g_doc["pinned_drills"].size());
    for (const auto& e : g_doc["pinned_drills"]) {
        if (e.is_string()) out.push_back(e.get<std::string>());
    }
    return out;
}

bool is_drill_pinned(const std::string& filename) {
    if (filename.empty()) return false;
    std::lock_guard lk(g_mtx);
    if (!g_doc.contains("pinned_drills") || !g_doc["pinned_drills"].is_array()) return false;
    for (const auto& e : g_doc["pinned_drills"]) {
        if (e.is_string() && e.get<std::string>() == filename) return true;
    }
    return false;
}

void set_drill_pinned(const std::string& filename, bool pinned) {
    if (filename.empty()) return;
    std::lock_guard lk(g_mtx);
    if (!g_doc.contains("pinned_drills") || !g_doc["pinned_drills"].is_array()) {
        g_doc["pinned_drills"] = json::array();
    }
    auto& arr = g_doc["pinned_drills"];
    // Remove any existing entry — keeps the file de-duplicated and lets
    // an unpin write fall through to the "not present" state cleanly.
    arr.erase(std::remove_if(arr.begin(), arr.end(),
                             [&](const json& e) {
                                 return e.is_string() && e.get<std::string>() == filename;
                             }),
              arr.end());
    if (pinned) arr.push_back(filename);
    write_config_locked();
}

}  // namespace opendojo::config
