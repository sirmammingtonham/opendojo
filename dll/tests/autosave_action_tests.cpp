#include <windows.h>
#include "autosave.hpp"
#include "commands.hpp"
#include "hooks/player_hook.hpp"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <iostream>

namespace {
std::filesystem::path directory;
int loads = 0;
bool populated = false;
void check(bool value) { if (!value) throw std::runtime_error("autosave action regression"); }
}
namespace opendojo::log { void format(const char*, ...) {} }
namespace opendojo::commands {
std::filesystem::path drills_dir() { return directory; }
std::size_t capture_populated_slots(drill::Drill&, bool) { return 0; }
LoadResult load_drill(const std::filesystem::path&, LoadMode) {
    ++loads; populated = true; return {true, "loaded"};
}
}
namespace opendojo::drill { std::string encode_text(const Drill&) { return {}; } }
namespace opendojo::file_io {
bool replace_file(const std::filesystem::path&, std::string_view) { return false; }
}
namespace opendojo::slot { bool is_populated(std::size_t slot) { return populated && slot == 0; } }
namespace opendojo::subsystems {
bool in_practice() { return true; }
std::uintptr_t pool1() { return 1; }
void ensure_pool_allocated() {}
bool mark_session_loaded(bool) { return true; }
}
namespace opendojo::players {
const char* character_name(std::uint32_t) { return "jin"; }
bool round_active() { return true; }
void log_round_probe() {}
}
namespace opendojo::player_hook {
Cached current_cpu() { return {true, 1}; }
void ensure_fresh() {}
}
int main() {
    using namespace opendojo;
    directory = std::filesystem::temp_directory_path() /
        ("opendojo-autosave-actions-" + std::to_string(GetCurrentProcessId()));
    check(std::filesystem::create_directory(directory));
    const auto scratch = directory / "_autosave_jin.drill.txt";
    { std::ofstream file(scratch); file << "test fixture"; }
    autosave::set_enabled(true);
    autosave::on_practice_entered();
    for (int i = 0; i < 65; ++i) autosave::tick();
    check(loads == 1 && populated);
    populated = false; // A manual Clear while the recovery watchdog is active.
    autosave::on_manual_action();
    for (int i = 0; i < 360; ++i) autosave::tick();
    check(loads == 1 && !populated);
    autosave::on_practice_entered();
    autosave::tick(); // A pending autoload has not yet reached its readiness delay.
    autosave::on_manual_action();
    for (int i = 0; i < 90; ++i) autosave::tick();
    check(loads == 1 && !populated);
    autosave::set_enabled(false);
    std::filesystem::remove(scratch);
    std::filesystem::remove(directory);
    std::cout << "Manual actions supersede pending autoload and recovery\n";
}
