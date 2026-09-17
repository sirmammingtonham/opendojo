#include <windows.h>

#include "config.hpp"
#include "file_io.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

static std::filesystem::path test_root;
namespace opendojo::commands {
std::filesystem::path drills_dir() { return test_root / "game"; }
}
namespace opendojo::log {
void format(const char*, ...) {}
}
static void check(bool condition) {
    if (!condition) throw std::runtime_error("config persistence regression");
}
static std::string read(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
int main(int argc, char** argv) {
    using namespace opendojo;
    check(argc == 2);
    test_root = std::filesystem::temp_directory_path() /
                (L"opendojo-config-tests-" + std::to_wstring(GetCurrentProcessId()));
    check(std::filesystem::create_directory(test_root));
    const auto game = test_root / "game";
    const auto appdata = test_root / "appdata";
    std::filesystem::create_directories(game);
    std::filesystem::create_directories(appdata / "OpenDojo");
    check(SetEnvironmentVariableW(L"LOCALAPPDATA", appdata.c_str()) != 0);
    const auto identity = appdata / "OpenDojo" / "identity.json";
    const auto settings = game / "config.json";
    const std::string mode = argv[1];
    if (mode == "legacy" || mode == "embedded") {
        const std::string auth = R"({"access_token":"test-only","refresh_token":"refresh-only","user_id":"test-user","expires_at":123})";
        const auto source = mode == "legacy" ? game / "cloud.json" : settings;
        const auto contents = mode == "legacy" ? auth : "{\"cloud\":{\"auth\":" + auth + "}}";
        check(file_io::replace_file(source, contents));
        // A directory at the destination reliably makes replacement fail.
        check(std::filesystem::create_directory(identity));
        config::load();
        config::save();
        check(read(source).find("refresh-only") != std::string::npos);
        check(std::filesystem::remove(identity));
        config::load();
        check(read(identity).find("refresh-only") != std::string::npos);
        if (mode == "legacy") check(!std::filesystem::exists(source));
        else check(read(source).find("refresh-only") == std::string::npos);
    } else if (mode == "settings") {
        check(file_io::replace_file(settings, R"({"toggle_vk":123})"));
        check(file_io::replace_file(game / "handle.txt", "test-author"));
        const auto held = CreateFileW(settings.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(held != INVALID_HANDLE_VALUE);
        config::load();
        config::set_toggle_vk(124);
        check(read(settings) == R"({"toggle_vk":123})");
        check(std::filesystem::exists(game / "handle.txt"));
        CloseHandle(held);
        config::load();
        check(!std::filesystem::exists(game / "handle.txt"));
        check(config::author_handle() == "test-author");
    } else if (mode == "types") {
        check(file_io::replace_file(settings, R"({"cloud":{"author_handle":42}})"));
        check(file_io::replace_file(identity, R"({"access_token":false,"refresh_token":[],"user_id":{},"expires_at":"invalid"})"));
        config::load();
        check(config::author_handle().empty());
        const auto tokens = config::auth_tokens();
        check(tokens.access_token.empty() && tokens.refresh_token.empty() && tokens.user_id.empty());
        check(tokens.expires_at_sec == 0);
    } else return 2;
    // Only this process's freshly created temporary directory is removed.
    std::filesystem::remove_all(test_root);
    std::cout << "Config " << mode << " passed\n";
}
