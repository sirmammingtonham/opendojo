#pragma once

// D3D12 render hook. Installs MinHook prologue detours on
// IDXGISwapChain::Present and ID3D12CommandQueue::ExecuteCommandLists,
// plus a WndProc hook for input. (A plain vtable swap crashes the game —
// Tekken's anti-tamper scans vtables.) All real work happens off the
// loader lock on a worker thread.
//
// Once the hooks are live, every game-rendered frame calls into our hook
// which draws the OpenDojo menu (when visible) on top of the game's frame.
//
// F12 toggles menu visibility (handled by the WndProc hook).

namespace opendojo::render_hook {

// Kick off installation. Spawns a worker thread that waits for d3d12.dll +
// dxgi.dll to be loaded, creates a dummy device/queue/swapchain to resolve
// the real Present / ExecuteCommandLists addresses (from the shared COM
// vtable), then installs MinHook detours on them. Idempotent — calling
// more than once is a no-op.
void install();

// Programmatically toggle the menu. Also called from the WndProc hook on
// F12. Safe to call from any thread.
void toggle_menu();

bool menu_visible();

}  // namespace opendojo::render_hook
