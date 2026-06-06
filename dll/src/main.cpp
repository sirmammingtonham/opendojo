// OpenDojo DLL entry point. Lives at <game>\Polaris\Binaries\Win64\dinput8.dll
// (proxy name controlled by CMake's OPENDOJO_PROXY option). When the game
// loads what it thinks is the system dinput8.dll, this DllMain runs first.
//
// Responsibilities:
//   1. Resolve and pin the real dinput8.dll so our forwarded exports work.
//   2. Open the log file.
//   3. Spawn the OpenDojo init thread (off the loader lock).
//
// The init thread:
//   - Logs module base and subsystem-resolution sanity, then
//   - Kicks off the render hook (which itself spawns another thread that
//     waits for the game's D3D12 runtime to be loaded before installing
//     the render detours and standing up the ImGui overlay).

#include <windows.h>

#include <thread>

#include "config.hpp"
#include "log.hpp"
#include "memory.hpp"
#include "player_hook.hpp"
#include "practice_state.hpp"
#include "proxy.hpp"
#include "render_hook.hpp"
#include "signatures.hpp"
#include "subsystems.hpp"

namespace {

void init_thread() {
    OPENDOJO_LOG("OpenDojo v0.1.0 starting up");

    auto base = opendojo::memory::polaris_base();
    if (!base) {
        OPENDOJO_LOG(
            "WARNING: Polaris-Win64-Shipping.exe not loaded — "
            "DLL was injected into the wrong process");
        return;
    }
    OPENDOJO_LOG("polaris_base = 0x%llX", static_cast<unsigned long long>(base));

    // Resolve every patched function via AOB scan so we survive Tekken
    // patches that shift RVAs. Each hook below queries signatures::
    // for its target and silently no-ops if the scan failed.
    if (!opendojo::signatures::resolve_all()) {
        OPENDOJO_LOG(
            "WARNING: one or more signatures didn't resolve — affected features "
            "(autosave / character-switch detect / drill auto-allocate) will no-op. "
            "Tekken likely patched. Update signatures.cpp.");
    }

    // pool1 is lazy — null until the user records once per game launch.
    auto p1 = opendojo::subsystems::pool1();
    OPENDOJO_LOG("pool1 = 0x%llX (%s)", static_cast<unsigned long long>(p1),
                 p1 ? "ready" : "not allocated yet — record once in practice mode");

    // Load persistent settings (hotkey binding etc.). Defaults are
    // applied if no config.json exists yet.
    opendojo::config::load();

    opendojo::render_hook::install();

    // Practice-mode lifecycle: a controller-dtor detour for exit-flush;
    // entry is detected by slot polling. See practice_state.hpp.
    opendojo::practice_state::install_hooks();

    // Player-pointer refresh hook. Replaces per-frame detect_cpu chain
    // walks (which AV'd during character swaps) with an atomic cache
    // updated synchronously when Tekken refreshes holder.p1/p2. See
    // player_hook.hpp.
    opendojo::player_hook::install();

    OPENDOJO_LOG("init thread done — press F12 in game to open menu");
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(module);

            opendojo::log::init();

            if (!opendojo::proxy::load()) {
                OPENDOJO_LOG("proxy::load() failed — refusing to attach");
                opendojo::log::shutdown();
                return FALSE;
            }

            std::thread(init_thread).detach();
            break;
        }
        case DLL_PROCESS_DETACH:
            opendojo::log::shutdown();
            opendojo::proxy::unload();
            break;
    }
    return TRUE;
}
