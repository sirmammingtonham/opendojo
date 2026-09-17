#include "hooks/render_hook.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <xinput.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"

// ImGui's Win32 backend intentionally hides the WndProc handler behind a
// `#if 0` block to keep windows.h out of imgui_impl_win32.h. Forward-
// declare it here so our WndProc subclass can forward messages into it.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

#include "MinHook.h"

#include "autosave.hpp"
#include "config.hpp"
#include "hooks/player_hook.hpp"
#include "log.hpp"
#include "practice_rename.hpp"
#include "slot_labels.hpp"
#include "subsystems.hpp"
#include "ui/menu.hpp"
#include "ui/theme.hpp"

namespace opendojo::render_hook {

namespace {

// ===========================================================================
//  Hook state
// ===========================================================================

// Function pointer types matching the COM vtable entries we're hijacking.
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT,
                                                       ID3D12CommandList* const*);

// Trampolines to the original engine functions, populated by the vtable
// swap. NEVER call the COM method directly inside a hook — that would
// recurse back into our hook.
PresentFn g_present_orig = nullptr;
ExecuteCommandListsFn g_exec_orig = nullptr;

std::atomic<bool> g_installing{false};
std::atomic<bool> g_hooks_live{false};
std::atomic<bool> g_imgui_ready{false};
std::atomic<bool> g_menu_visible{false};

// Captured from the live game. Queue is captured from the first call to
// our ExecuteCommandLists hook; device/swapchain from the first Present.
ID3D12CommandQueue* g_queue = nullptr;
ID3D12Device* g_device = nullptr;
IDXGISwapChain3* g_swapchain = nullptr;
HWND g_hwnd = nullptr;
DXGI_FORMAT g_rtv_format = DXGI_FORMAT_UNKNOWN;

constexpr UINT MAX_FRAMES = 8;

struct FrameContext {
    ID3D12CommandAllocator* allocator = nullptr;
    UINT64 fence_value = 0;
};

ID3D12DescriptorHeap* g_rtv_heap = nullptr;
ID3D12DescriptorHeap* g_srv_heap = nullptr;
ID3D12GraphicsCommandList* g_cmd_list = nullptr;
FrameContext g_frames[MAX_FRAMES]{};
UINT g_buffer_count = 0;
// Single RTV descriptor slot, reused each frame. We acquire the back
// buffer fresh per Present (no held reference), bind an RTV to it in this
// slot, render, release the back buffer. The game retains exclusive
// ownership of its swapchain back buffers between our hook calls — so
// ResizeBuffers always sees zero outstanding refs from us, even though we
// don't hook ResizeBuffers itself.
D3D12_CPU_DESCRIPTOR_HANDLE g_rtv_cpu{};

// Single fence used to gate allocator reuse. Per-frame fence_value lets us
// tell whether a given allocator's previously-submitted work has finished
// on the GPU before we Reset it.
ID3D12Fence* g_fence = nullptr;
HANDLE g_fence_event = nullptr;
UINT64 g_fence_next = 0;

// ===========================================================================
//  SRV descriptor pool (ImGui DX12 backend, 1.92+)
// ===========================================================================
//
// Pre-1.92 the DX12 backend took a single font SRV handle. 1.92's dynamic
// texture system instead asks the app for descriptors on demand (one per
// font / texture atlas it creates, freed when destroyed), via the alloc/free
// callbacks on ImGui_ImplDX12_InitInfo. We back that with a small
// shader-visible CBV/SRV/UAV heap plus this fixed-capacity stack allocator.
// All calls happen on the render thread (Init / NewFrame), so no locking.
constexpr UINT SRV_HEAP_SIZE = 64;

struct SrvDescriptorPool {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_start{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_start{};
    UINT inc = 0;
    UINT free_stack[SRV_HEAP_SIZE]{};
    int free_top = 0;  // count of free indices currently on the stack

    void init(ID3D12Device* dev, ID3D12DescriptorHeap* heap) {
        inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        cpu_start = heap->GetCPUDescriptorHandleForHeapStart();
        gpu_start = heap->GetGPUDescriptorHandleForHeapStart();
        free_top = 0;
        // Push high->low so index 0 is the first one handed out.
        for (UINT i = SRV_HEAP_SIZE; i-- > 0;)
            free_stack[free_top++] = i;
    }
    void alloc(D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu) {
        IM_ASSERT(free_top > 0 && "OpenDojo SRV descriptor pool exhausted");
        const UINT idx = free_stack[--free_top];
        out_cpu->ptr = cpu_start.ptr + static_cast<SIZE_T>(idx) * inc;
        out_gpu->ptr = gpu_start.ptr + static_cast<UINT64>(idx) * inc;
    }
    void release(D3D12_CPU_DESCRIPTOR_HANDLE cpu) {
        const UINT idx = static_cast<UINT>((cpu.ptr - cpu_start.ptr) / inc);
        IM_ASSERT(free_top < static_cast<int>(SRV_HEAP_SIZE));
        free_stack[free_top++] = idx;
    }
};
SrvDescriptorPool g_srv_pool;

void srv_pool_alloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
                    D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    g_srv_pool.alloc(cpu, gpu);
}
void srv_pool_free(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                   D3D12_GPU_DESCRIPTOR_HANDLE) {
    g_srv_pool.release(cpu);
}

// ===========================================================================
//  WndProc subclass (input routing)
// ===========================================================================
//
// Install a WndProc subclass via SetWindowLongPtrW. Every input message
// the game receives first runs through our proc; we forward to ImGui's
// backend handler, then check io.WantCaptureMouse / WantCaptureKeyboard
// to decide whether to swallow (return 1, message consumed) or let it
// through to the original WndProc.
//
// WndProc fires AFTER TranslateMessage, so both WM_KEYDOWN and the
// resulting WM_CHAR reach us independently and we can route both to
// ImGui without losing text input.

WNDPROC g_orig_wndproc = nullptr;
HWND g_subclassed_hwnd = nullptr;

LRESULT CALLBACK opendojo_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    // Flush the autosave on close. The normal "leave practice" and
    // "character switch" triggers don't fire when the user quits the
    // game window directly from practice mode, so we hook the close
    // request here. Safe even if the game cancels the close — saving
    // the same state again is idempotent.
    if (msg == WM_CLOSE) {
        opendojo::autosave::flush_now();
    }

    if (g_imgui_ready.load()) {
        // WM_KEYDOWN handling. lparam bit 30 ("previous key state"):
        // 0 means the key was up immediately before this message,
        // i.e. this is the rising edge of the press.
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
            const bool was_down = (lparam & (1LL << 30)) != 0;

            // Hotkey-rebind capture (Settings tab → "Rebind...").
            // Must run BEFORE the toggle check so the user can rebind
            // the toggle key to a different key. ESC cancels.
            if (!was_down && opendojo::config::is_capturing()) {
                if (wparam == VK_ESCAPE) {
                    opendojo::config::cancel_capture();
                } else {
                    opendojo::config::notify_captured_vk(static_cast<std::uint32_t>(wparam));
                }
                return 0;  // consume the captured key
            }

            // Toggle hotkey — fires on rising edge of the bound VK,
            // but only inside practice mode (the menu is practice-only).
            // We consume the key either way so the game never sees it
            // accidentally do something else.
            const auto vk = opendojo::config::toggle_vk();
            if (wparam == vk) {
                if (!was_down && opendojo::subsystems::in_practice()) toggle_menu();
                return 0;
            }
        }

        if (g_menu_visible.load()) {
            // Feed ImGui. The backend handler does its own filtering.
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);

            ImGuiIO& io = ImGui::GetIO();
            const bool is_mouse_msg = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST);
            const bool is_kbd_msg = (msg >= WM_KEYFIRST && msg <= WM_KEYLAST);
            // WM_INPUT (0x00FF) carries raw input events. Tekken (and
            // many fighting games) reads keyboard / gamepad via Raw
            // Input, NOT via standard WM_KEY messages or
            // GetAsyncKeyState. Swallowing WM_KEYFIRST..LAST alone
            // leaves Raw Input bleeding through, so the user sees
            // keys still trigger game actions. We swallow WM_INPUT
            // (and the related device-change message at 0x00FE) too.
            // ImGui doesn't use Raw Input so we lose nothing.
            const bool is_raw_msg = (msg == WM_INPUT || msg == WM_INPUT_DEVICE_CHANGE);
            // Mouse: only swallow when ImGui actually wants it (hover
            // over menu). Keys + Raw Input: ALWAYS swallow while menu
            // is up — the toggle hotkey was consumed earlier in this
            // WndProc.
            if ((is_mouse_msg && io.WantCaptureMouse) || is_kbd_msg || is_raw_msg) {
                return 1;  // consumed — game's WndProc never sees it
            }
        }
    }
    return CallWindowProcW(g_orig_wndproc, hwnd, msg, wparam, lparam);
}

bool install_message_hook(HWND hwnd) {
    if (!hwnd) return false;
    SetLastError(0);
    auto prev =
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&opendojo_wndproc));
    if (prev == 0 && GetLastError() != 0) {
        OPENDOJO_LOG(
            "render_hook: SetWindowLongPtrW(GWLP_WNDPROC) failed "
            "(GLE=%lu) — input swallow disabled",
            GetLastError());
        return false;
    }
    g_orig_wndproc = reinterpret_cast<WNDPROC>(prev);
    g_subclassed_hwnd = hwnd;
    OPENDOJO_LOG("render_hook: WndProc subclassed (hwnd=0x%p prev=0x%p)", hwnd, g_orig_wndproc);
    return true;
}

// ===========================================================================
//  ImGui DX12 init / teardown
// ===========================================================================

bool init_imgui_resources(IDXGISwapChain3* swapchain) {
    OPENDOJO_LOG("render_hook: init step 1/8 — GetDesc");
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapchain->GetDesc(&desc))) {
        OPENDOJO_LOG("render_hook: swapchain->GetDesc failed");
        return false;
    }
    g_buffer_count = desc.BufferCount;
    OPENDOJO_LOG("render_hook:  buffer_count=%u format=%u hwnd=0x%p w=%u h=%u", g_buffer_count,
                 static_cast<unsigned>(desc.BufferDesc.Format), desc.OutputWindow,
                 desc.BufferDesc.Width, desc.BufferDesc.Height);
    if (g_buffer_count == 0 || g_buffer_count > MAX_FRAMES) {
        OPENDOJO_LOG("render_hook: unsupported buffer count %u", g_buffer_count);
        return false;
    }
    if (!desc.OutputWindow) {
        OPENDOJO_LOG(
            "render_hook: swapchain has NULL HWND — "
            "non-windowed swapchain (CoreWindow / composition) unsupported");
        return false;
    }
    g_rtv_format = desc.BufferDesc.Format;
    g_hwnd = desc.OutputWindow;

    OPENDOJO_LOG("render_hook: init step 2/8 — GetDevice");
    if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&g_device)))) {
        OPENDOJO_LOG("render_hook: swapchain->GetDevice failed");
        return false;
    }

    OPENDOJO_LOG("render_hook: init step 3/8 — RTV heap (single slot)");
    {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        d.NumDescriptors = 1;  // we reuse one slot each frame
        d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(g_device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g_rtv_heap)))) {
            OPENDOJO_LOG("render_hook: RTV heap creation failed");
            return false;
        }
    }
    g_rtv_cpu = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();

    OPENDOJO_LOG("render_hook: init step 4/8 — per-frame allocators");
    for (UINT i = 0; i < g_buffer_count; ++i) {
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&g_frames[i].allocator)))) {
            OPENDOJO_LOG("render_hook: CreateCommandAllocator(%u) failed", i);
            return false;
        }
    }

    OPENDOJO_LOG("render_hook: init step 5/8 — SRV heap");
    {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        // 1.92's backend allocates SRVs on demand for its dynamic textures
        // (see SrvDescriptorPool above), so size the heap as a small pool
        // rather than the single font slot the pre-1.92 backend needed.
        d.NumDescriptors = SRV_HEAP_SIZE;
        d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g_srv_heap)))) {
            OPENDOJO_LOG("render_hook: SRV heap creation failed");
            return false;
        }
    }

    OPENDOJO_LOG("render_hook: init step 6/8 — command list");
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].allocator,
                                           nullptr, IID_PPV_ARGS(&g_cmd_list)))) {
        OPENDOJO_LOG("render_hook: CreateCommandList failed");
        return false;
    }
    g_cmd_list->Close();

    OPENDOJO_LOG("render_hook: init step 7/8 — fence");
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) {
        OPENDOJO_LOG("render_hook: CreateFence failed");
        return false;
    }
    g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_fence_event) {
        OPENDOJO_LOG("render_hook: CreateEvent failed (GLE=%lu)", GetLastError());
        return false;
    }

    OPENDOJO_LOG("render_hook: init step 8/8 — D3D12 resources ready");
    return true;
}

// Secondary "small" font for metadata text (description snippets,
// hashtag chips). Stashed at AddFont time and exposed via
// opendojo::render_hook::small_font().
ImFont* g_small_font = nullptr;

// Load the menu font, rasterized at the target pixel size so it renders
// crisp without runtime upscaling. Also loads a secondary small font
// (~78% of the primary size) for metadata text.
//
// Search order:
//   1. Mods/OpenDojo/font/opendojo.ttf next to the game exe (user override)
//   2. %WINDIR%/Fonts/segoeui.ttf (always present on Windows)
//   3. ImGui's bundled ProggyClean (bitmap; blurry when scaled but at least
//      something renders)
void load_menu_font(ImGuiIO& io, float pixel_size) {
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.PixelSnapH = true;

    // Static so the pointers outlive the AddFont* calls. ImGui holds
    // a reference to these arrays until the texture atlas is built.
    static const ImWchar kLatinRange[] = {
        0x0020,
        0x00FF,  // Basic Latin + Latin-1 Supplement
        // General Punctuation we use in UI captions (en/em dash) plus
        // what shows up in pasted community drill descriptions (curly
        // quotes, bullet, ellipsis). Segoe UI has these glyphs, but if
        // we don't rasterize the range they fall back to '?'.
        0x2010,
        0x2027,  // hyphens, dashes, quotes, bullet, ellipsis
        0,
    };
    // Glyphs we want but the primary text font (Segoe UI) does not
    // ship: pulled in from Segoe UI Symbol via MergeMode below.
    static const ImWchar kSymbolRange[] = {
        0x2605,
        0x2606,  // BLACK STAR, WHITE STAR (Drills tab pin toggle)
        0,
    };

    const float small_px = pixel_size * 0.78f;

    auto try_path = [&](const std::filesystem::path& p, const char* label) -> bool {
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) return false;
        auto utf8 = p.string();
        ImFont* fnt = io.Fonts->AddFontFromFileTTF(utf8.c_str(), pixel_size, &cfg, kLatinRange);
        if (!fnt) {
            OPENDOJO_LOG("render_hook: AddFontFromFileTTF failed for %s (%s)", utf8.c_str(), label);
            return false;
        }
        OPENDOJO_LOG("render_hook: loaded %s font %s @ %.1fpx", label, utf8.c_str(), pixel_size);
        return true;
    };

    // After the primary font is registered (and optionally has symbols
    // merged), add a second instance of the same TTF at ~78% size. We
    // hold the returned pointer so the menu can PushFont() it for
    // metadata-style text without disturbing the primary widget font.
    auto load_small = [&](const std::filesystem::path& p) {
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) return;
        auto utf8 = p.string();
        if (auto* fnt = io.Fonts->AddFontFromFileTTF(utf8.c_str(), small_px, &cfg, kLatinRange)) {
            g_small_font = fnt;
            OPENDOJO_LOG("render_hook: loaded small font %s @ %.1fpx", utf8.c_str(), small_px);
        }
    };

    // Merge symbol glyphs from Segoe UI Symbol onto whatever primary
    // font we ended up with. Segoe UI Symbol ships with every modern
    // Windows install and contains the geometric shapes Segoe UI
    // itself doesn't include (★, ☆, ▲, etc).
    auto merge_symbols = [&](float px) {
        wchar_t windir[MAX_PATH];
        if (!GetWindowsDirectoryW(windir, MAX_PATH)) return;
        auto sym = std::filesystem::path(windir) / L"Fonts" / L"seguisym.ttf";
        std::error_code ec;
        if (!std::filesystem::exists(sym, ec)) {
            OPENDOJO_LOG("render_hook: seguisym.ttf missing — symbol glyphs may render as '?'");
            return;
        }
        ImFontConfig merge = cfg;
        merge.MergeMode = true;
        auto utf8 = sym.string();
        if (io.Fonts->AddFontFromFileTTF(utf8.c_str(), px, &merge, kSymbolRange)) {
            OPENDOJO_LOG("render_hook: merged Segoe UI Symbol for symbol glyphs @ %.1fpx", px);
        }
    };

    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        auto user_font = std::filesystem::path(exe)
                             .parent_path()
                             .append(L"Mods")
                             .append(L"OpenDojo")
                             .append(L"font")
                             .append(L"opendojo.ttf");
        if (try_path(user_font, "user-override")) {
            merge_symbols(pixel_size);
            load_small(user_font);
            return;
        }
    }

    wchar_t windir[MAX_PATH];
    if (GetWindowsDirectoryW(windir, MAX_PATH)) {
        auto segoe = std::filesystem::path(windir) / L"Fonts" / L"segoeui.ttf";
        if (try_path(segoe, "Segoe UI")) {
            merge_symbols(pixel_size);
            load_small(segoe);
            return;
        }
    }

    OPENDOJO_LOG("render_hook: no TTF font available — using ImGui default");
}

bool init_imgui_runtime() {
    OPENDOJO_LOG("render_hook: imgui step 1/4 — context + theme");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // Don't pollute the game folder.
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    // HasGamepad lets ImGui's nav code know the gamepad events we feed
    // via AddKeyEvent / AddKeyAnalogEvent are real input.
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

    // Compute scale BEFORE loading fonts. ImGui defaults assume 96 DPI /
    // ~1080p; Tekken can run at 1440p / 4K / various scaled HiDPI configs.
    // Combine the OS DPI scale (from the Win32 backend) with a resolution-
    // based heuristic so the menu remains readable at high resolutions
    // without becoming oversized at low ones.
    //
    // We rasterize the font at `base_px * scale` so glyphs are crisp at
    // the final on-screen size. Using io.FontGlobalScale would upscale
    // the font bitmap at draw time, which looks blurry.
    const float dpi_scale = ImGui_ImplWin32_GetDpiScaleForHwnd(g_hwnd);
    DXGI_SWAP_CHAIN_DESC desc{};
    g_swapchain->GetDesc(&desc);
    const float h = static_cast<float>(desc.BufferDesc.Height);
    const float res_scale = (h > 0.0f) ? (h / 1080.0f) : 1.0f;
    float scale = dpi_scale * res_scale;
    if (scale < 1.0f) scale = 1.0f;
    if (scale > 2.5f) scale = 2.5f;
    OPENDOJO_LOG(
        "render_hook: ImGui scale = %.2f "
        "(dpi=%.2f resh=%.0f res_scale=%.2f)",
        scale, dpi_scale, h, res_scale);

    constexpr float BASE_FONT_PX = 18.0f;
    load_menu_font(io, BASE_FONT_PX * scale);

    opendojo::theme::apply();
    ImGui::GetStyle().ScaleAllSizes(scale);

    OPENDOJO_LOG("render_hook: imgui step 2/4 — Win32 backend (hwnd=0x%p)", g_hwnd);
    if (!ImGui_ImplWin32_Init(g_hwnd)) {
        OPENDOJO_LOG("render_hook: ImGui_ImplWin32_Init failed");
        return false;
    }

    OPENDOJO_LOG("render_hook: imgui step 3/4 — DX12 backend");
    // 1.92+ init: the backend pulls SRV descriptors from our pool on demand
    // (font atlas + any dynamic textures), so the allocator must be live
    // before Init — Init allocates the first font descriptor through it.
    // g_queue is guaranteed captured by now (hook_present gates init on it).
    g_srv_pool.init(g_device, g_srv_heap);
    ImGui_ImplDX12_InitInfo dx12_info = {};
    dx12_info.Device = g_device;
    dx12_info.CommandQueue = g_queue;
    dx12_info.NumFramesInFlight = static_cast<int>(g_buffer_count);
    dx12_info.RTVFormat = g_rtv_format;
    dx12_info.SrvDescriptorHeap = g_srv_heap;
    dx12_info.SrvDescriptorAllocFn = srv_pool_alloc;
    dx12_info.SrvDescriptorFreeFn = srv_pool_free;
    if (!ImGui_ImplDX12_Init(&dx12_info)) {
        OPENDOJO_LOG("render_hook: ImGui_ImplDX12_Init failed");
        return false;
    }

    OPENDOJO_LOG("render_hook: imgui step 4/4 — installing WndProc subclass");
    // Non-fatal: if this fails, mouse + F12 + gamepad still work, only
    // text-input (the Export tab's name/description fields) is broken.
    install_message_hook(g_hwnd);

    OPENDOJO_LOG("render_hook: ImGui initialized (hwnd=0x%p, buffers=%u, fmt=%u)", g_hwnd,
                 g_buffer_count, static_cast<unsigned>(g_rtv_format));
    return true;
}

// Wraps the init pipeline in Windows SEH so an access violation in ImGui or
// the D3D12 runtime doesn't take the game down. With /EHsc (our default),
// __try/__except catches structured exceptions but not C++ exceptions —
// which is what we want here since we're guarding against AV-style crashes
// originating in third-party code.
bool safe_init(IDXGISwapChain3* sc) {
    __try {
        return init_imgui_resources(sc) && init_imgui_runtime();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OPENDOJO_LOG(
            "render_hook: SEH caught exception 0x%08X during init "
            "— menu disabled this session",
            static_cast<unsigned>(GetExceptionCode()));
        return false;
    }
}

// =============================================================================
// Keyboard input suppression — user32 polling APIs
// =============================================================================
//
// Tekken's IAT imports GetAsyncKeyState + GetKeyState (verified via
// dumpbin /imports). Both read raw OS state and bypass our WndProc
// subclass, so we hook them to return 0 while the menu is up.
// GetKeyboardState is not imported; not hooked.

using GetAsyncKeyState_t = SHORT(WINAPI*)(int);
using GetKeyState_t = SHORT(WINAPI*)(int);

GetAsyncKeyState_t g_gaks_orig = nullptr;
GetKeyState_t g_gks_orig = nullptr;

SHORT WINAPI gaks_hook(int vk) {
    if (g_menu_visible.load()) return 0;
    return g_gaks_orig(vk);
}
SHORT WINAPI gks_hook(int vk) {
    if (g_menu_visible.load()) return 0;
    return g_gks_orig(vk);
}

void install_one(void* target, void* shim, void** orig_slot, const char* label) {
    if (!target) return;
    if (MH_CreateHook(target, shim, orig_slot) == MH_OK && MH_EnableHook(target) == MH_OK) {
        OPENDOJO_LOG("render_hook: %s hooked", label);
    } else {
        OPENDOJO_LOG("render_hook: %s hook failed", label);
    }
}

void install_keyboard_hooks() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        OPENDOJO_LOG("render_hook: user32.dll not loaded — keyboard suppression disabled");
        return;
    }
    install_one(reinterpret_cast<void*>(GetProcAddress(user32, "GetAsyncKeyState")),
                reinterpret_cast<void*>(&gaks_hook), reinterpret_cast<void**>(&g_gaks_orig),
                "GetAsyncKeyState");
    install_one(reinterpret_cast<void*>(GetProcAddress(user32, "GetKeyState")),
                reinterpret_cast<void*>(&gks_hook), reinterpret_cast<void**>(&g_gks_orig),
                "GetKeyState");
}

// =============================================================================
// XInput gamepad — masking hook, chord detection, ImGui input feed
// =============================================================================
//
// Tekken imports XINPUT1_3 (verified via IAT). Plus we lazily probe a few
// other xinput*.dll versions in case middleware (Steam Input, etc.) routes
// through a different one.
//
// The game-facing hooks (xinput_get_state_hook / _ex_hook) do three things:
//   1. Forward the call to the real XInput trampoline to read the raw state.
//   2. Run our chord-detection + pad-bind-capture logic on that pre-mask
//      state. This is the OpenDojo equivalent of a per-frame poll, but it
//      piggybacks on Tekken's own per-frame XInput call — no extra XInput
//      cost beyond the hook trampoline overhead.
//   3. Mask the state to zero before returning to the game whenever the
//      menu is visible, so D-pad / face buttons don't bleed into the match.
//
// ImGui nav input (sticks, buttons) is fed separately from render_frame
// (render thread, only when menu visible) to avoid touching ImGui from
// whatever thread Tekken happens to poll XInput on.

using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

XInputGetState_t g_xinput_orig = nullptr;     // trampoline for non-Ex
XInputGetState_t g_xinput_ex_orig = nullptr;  // trampoline for ordinal-100 Ex

// Edge-detect chord + handle pad-bind capture on Tekken's own XInput call.
// Called from both xinput_get_state_hook variants for user_index 0, after
// the trampoline read but before masking. Thread-safe — only does atomic
// loads/stores and (rarely) a log call.
void process_gamepad_for_chord(WORD buttons) {
    static WORD last_buttons = 0;
    const WORD pressed = buttons & ~last_buttons;
    last_buttons = buttons;

    // Pad-bind capture: first non-Back rising edge wins. Back cancels.
    if (opendojo::config::is_pad_capturing()) {
        if (pressed & XINPUT_GAMEPAD_BACK) {
            opendojo::config::cancel_pad_capture();
        } else if (pressed != 0) {
            WORD pick = pressed & static_cast<WORD>(-static_cast<int>(pressed));
            if (pick != XINPUT_GAMEPAD_BACK) {
                opendojo::config::notify_captured_pad_btn(pick);
            }
        }
        return;  // don't fire the chord while binding
    }

    // Chord: Back held + configured button rising edge → toggle menu.
    const bool back_held = (buttons & XINPUT_GAMEPAD_BACK) != 0;
    if (!back_held) return;
    const WORD bind = opendojo::config::toggle_pad_btn();
    if (pressed & bind) toggle_menu();
}

DWORD WINAPI xinput_get_state_hook(DWORD user_index, XINPUT_STATE* state) {
    DWORD r = g_xinput_orig(user_index, state);
    if (r == ERROR_SUCCESS && state && user_index == 0 && opendojo::subsystems::in_practice()) {
        process_gamepad_for_chord(state->Gamepad.wButtons);
    }
    if (g_menu_visible.load() && r == ERROR_SUCCESS && state) {
        std::memset(&state->Gamepad, 0, sizeof(state->Gamepad));
    }
    return r;
}
DWORD WINAPI xinput_get_state_ex_hook(DWORD user_index, XINPUT_STATE* state) {
    DWORD r = g_xinput_ex_orig(user_index, state);
    if (r == ERROR_SUCCESS && state && user_index == 0 && opendojo::subsystems::in_practice()) {
        process_gamepad_for_chord(state->Gamepad.wButtons);
    }
    if (g_menu_visible.load() && r == ERROR_SUCCESS && state) {
        std::memset(&state->Gamepad, 0, sizeof(state->Gamepad));
    }
    return r;
}

void install_xinput_hooks() {
    static bool installed = false;
    if (installed) return;
    installed = true;

    // Try every xinput*.dll the OS might have loaded. xinput1_3 is what
    // Tekken imports directly; the others cover Steam Input / overlays.
    const wchar_t* candidates[] = {
        L"xinput1_4.dll", L"xinput1_3.dll", L"xinput1_2.dll", L"xinput1_1.dll", L"xinput9_1_0.dll",
    };
    int hooked = 0;
    for (const wchar_t* name : candidates) {
        HMODULE mod = GetModuleHandleW(name);
        if (!mod) mod = LoadLibraryW(name);
        if (!mod) continue;

        auto fn = reinterpret_cast<XInputGetState_t>(GetProcAddress(mod, "XInputGetState"));
        auto fn_ex = reinterpret_cast<XInputGetState_t>(GetProcAddress(mod, MAKEINTRESOURCEA(100)));

        if (fn) {
            XInputGetState_t orig = nullptr;
            if (MH_CreateHook(reinterpret_cast<LPVOID>(fn),
                              reinterpret_cast<LPVOID>(&xinput_get_state_hook),
                              reinterpret_cast<LPVOID*>(&orig)) == MH_OK &&
                MH_EnableHook(reinterpret_cast<LPVOID>(fn)) == MH_OK) {
                if (!g_xinput_orig) g_xinput_orig = orig;
                ++hooked;
            }
        }
        if (fn_ex) {
            XInputGetState_t orig = nullptr;
            if (MH_CreateHook(reinterpret_cast<LPVOID>(fn_ex),
                              reinterpret_cast<LPVOID>(&xinput_get_state_ex_hook),
                              reinterpret_cast<LPVOID*>(&orig)) == MH_OK &&
                MH_EnableHook(reinterpret_cast<LPVOID>(fn_ex)) == MH_OK) {
                if (!g_xinput_ex_orig) g_xinput_ex_orig = orig;
            }
        }
    }
    if (hooked == 0) {
        OPENDOJO_LOG("render_hook: no xinput*.dll hooked — gamepad nav/chord disabled");
    }
}

// Feed the current gamepad state into ImGui as nav input. Called from
// render_frame (render thread, only when the menu is visible) — keeps all
// ImGui calls on a single thread. Edge detection uses its own static
// tracker, independent of the chord tracker in process_gamepad_for_chord.
void feed_gamepad_to_imgui() {
    if (!g_xinput_orig) return;
    XINPUT_STATE state{};
    if (g_xinput_orig(0, &state) != ERROR_SUCCESS) return;

    const WORD buttons = state.Gamepad.wButtons;
    static WORD last_buttons = 0;
    const WORD pressed = buttons & ~last_buttons;
    const WORD released = ~buttons & last_buttons;
    last_buttons = buttons;

    ImGuiIO& io = ImGui::GetIO();
    auto edge = [&](WORD mask, ImGuiKey key) {
        if (pressed & mask) io.AddKeyEvent(key, true);
        if (released & mask) io.AddKeyEvent(key, false);
    };
    edge(XINPUT_GAMEPAD_A, ImGuiKey_GamepadFaceDown);
    edge(XINPUT_GAMEPAD_B, ImGuiKey_GamepadFaceRight);
    edge(XINPUT_GAMEPAD_X, ImGuiKey_GamepadFaceLeft);
    edge(XINPUT_GAMEPAD_Y, ImGuiKey_GamepadFaceUp);
    // ImGui's gamepad nav distinguishes "Activate" (A / GamepadFaceDown)
    // from "Text Input" (Y / GamepadFaceUp): A clicks buttons but does
    // NOT enter InputText edit mode, while Y does. Mirror A onto Y so
    // pressing A on a text field also focuses it for typing. Y has no
    // other binding outside InputText, so this is side-effect-free.
    edge(XINPUT_GAMEPAD_A, ImGuiKey_GamepadFaceUp);
    edge(XINPUT_GAMEPAD_DPAD_LEFT, ImGuiKey_GamepadDpadLeft);
    edge(XINPUT_GAMEPAD_DPAD_RIGHT, ImGuiKey_GamepadDpadRight);
    edge(XINPUT_GAMEPAD_DPAD_UP, ImGuiKey_GamepadDpadUp);
    edge(XINPUT_GAMEPAD_DPAD_DOWN, ImGuiKey_GamepadDpadDown);
    edge(XINPUT_GAMEPAD_LEFT_SHOULDER, ImGuiKey_GamepadL1);
    edge(XINPUT_GAMEPAD_RIGHT_SHOULDER, ImGuiKey_GamepadR1);
    edge(XINPUT_GAMEPAD_START, ImGuiKey_GamepadStart);
    edge(XINPUT_GAMEPAD_BACK, ImGuiKey_GamepadBack);

    // Triggers as analog L2/R2.
    const float lt = static_cast<float>(state.Gamepad.bLeftTrigger) / 255.0f;
    const float rt = static_cast<float>(state.Gamepad.bRightTrigger) / 255.0f;
    io.AddKeyAnalogEvent(ImGuiKey_GamepadL2, lt > 0.10f, lt);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, rt > 0.10f, rt);

    // Left + right sticks as analog nav, with the standard XInput deadzone.
    constexpr float kStickDz = 8689.0f / 32767.0f;
    auto stick = [&](SHORT raw, ImGuiKey neg, ImGuiKey pos) {
        const float v = static_cast<float>(raw) / 32767.0f;
        const float a = v < 0 ? -v : v;
        const float n = a > kStickDz ? (a - kStickDz) / (1.0f - kStickDz) : 0.0f;
        io.AddKeyAnalogEvent(v < 0 ? neg : pos, n > 0.10f, n);
        io.AddKeyAnalogEvent(v < 0 ? pos : neg, false, 0.0f);
    };
    stick(state.Gamepad.sThumbLX, ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight);
    stick(state.Gamepad.sThumbLY, ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickUp);
    stick(state.Gamepad.sThumbRX, ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight);
    stick(state.Gamepad.sThumbRY, ImGuiKey_GamepadRStickDown, ImGuiKey_GamepadRStickUp);
}

// Per-frame rendering. Pulled into its own function so we can wrap it in
// SEH without dragging C++ unwind frames through __try. Acquires a fresh
// back buffer reference for this frame and releases it before returning
// so the game's ResizeBuffers never sees outstanding refs from us.
void render_frame() {
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    feed_gamepad_to_imgui();
    ImGui::NewFrame();
    opendojo::menu::draw();
    ImGui::Render();

    const UINT idx = g_swapchain->GetCurrentBackBufferIndex();
    if (idx >= g_buffer_count) {
        ImGui::EndFrame();
        return;
    }
    FrameContext& fc = g_frames[idx];
    if (!fc.allocator) return;

    ID3D12Resource* back_buffer = nullptr;
    if (FAILED(g_swapchain->GetBuffer(idx, IID_PPV_ARGS(&back_buffer))) || !back_buffer) {
        return;
    }
    g_device->CreateRenderTargetView(back_buffer, nullptr, g_rtv_cpu);

    if (fc.fence_value > 0 && g_fence->GetCompletedValue() < fc.fence_value) {
        g_fence->SetEventOnCompletion(fc.fence_value, g_fence_event);
        WaitForSingleObject(g_fence_event, INFINITE);
    }
    fc.allocator->Reset();

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = back_buffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

    g_cmd_list->Reset(fc.allocator, nullptr);
    g_cmd_list->ResourceBarrier(1, &barrier);
    g_cmd_list->OMSetRenderTargets(1, &g_rtv_cpu, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = {g_srv_heap};
    g_cmd_list->SetDescriptorHeaps(1, heaps);

    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_cmd_list);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_cmd_list->ResourceBarrier(1, &barrier);
    g_cmd_list->Close();

    ID3D12CommandList* lists[] = {g_cmd_list};
    g_queue->ExecuteCommandLists(1, lists);

    fc.fence_value = ++g_fence_next;
    g_queue->Signal(g_fence, fc.fence_value);

    // CRITICAL: release the back buffer ref before the next ResizeBuffers
    // sees us. The submitted command list still references the resource
    // via the GPU, but D3D12 tracks that internally — CPU-side Release
    // just drops our refcount.
    back_buffer->Release();
}

void safe_render_frame() {
    __try {
        render_frame();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Don't spam the log — a crash here on every frame would flood
        // the file. Disable rendering for the rest of the session.
        static bool logged = false;
        if (!logged) {
            OPENDOJO_LOG(
                "render_hook: SEH caught exception 0x%08X during "
                "per-frame render — overlay disabled",
                static_cast<unsigned>(GetExceptionCode()));
            logged = true;
        }
        g_menu_visible.store(false);
        g_device = nullptr;  // forces fast-path passthrough from now on
    }
}

// ===========================================================================
//  Hook bodies
// ===========================================================================

void STDMETHODCALLTYPE hook_execute_command_lists(ID3D12CommandQueue* self, UINT num,
                                                  ID3D12CommandList* const* lists) {
    if (!g_queue) {
        D3D12_COMMAND_QUEUE_DESC desc = self->GetDesc();
        if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            // No AddRef — we hold the pointer for use during render but
            // don't extend its lifetime. The game's main graphics queue
            // outlives every meaningful interaction the menu has with it.
            g_queue = self;
            OPENDOJO_LOG("render_hook: captured command queue 0x%p", g_queue);
        }
    }
    g_exec_orig(self, num, lists);
}

HRESULT STDMETHODCALLTYPE hook_present(IDXGISwapChain* self, UINT sync_interval, UINT flags) {
    // First-call init. Wait until exec hook has captured the queue AND
    // we've let the game's renderer settle for a few frames before we
    // start poking at descriptors.
    //
    // Processes can have multiple swapchains (game main + overlays
    // from NVIDIA/Discord/OBS/Game Bar). The hook fires for whichever
    // one Presents on a given frame. We retry across swapchains until
    // we find one that's actually D3D12-backed.
    if (!g_imgui_ready.load()) {
        static std::atomic<unsigned> present_count{0};
        const unsigned n = present_count.fetch_add(1);
        if (n < 6) {
            OPENDOJO_LOG("render_hook: hook_present #%u (queue=0x%p)", n, g_queue);
        }
        constexpr unsigned WARMUP_FRAMES = 60;
        if (!g_queue || n < WARMUP_FRAMES) {
            return g_present_orig(self, sync_interval, flags);
        }

        // Reject set: swapchains we've already tried that don't work.
        // Static so it persists across calls without owning anything.
        // Cap size at a few entries (overlays don't churn pointers).
        static IDXGISwapChain* rejected[8] = {};
        static int rejected_count = 0;
        static unsigned attempts = 0;
        static unsigned first_attempt_present = 0;

        for (int i = 0; i < rejected_count; ++i) {
            if (rejected[i] == self) {
                return g_present_orig(self, sync_interval, flags);
            }
        }

        if (attempts == 0) first_attempt_present = n;
        ++attempts;

        constexpr unsigned MAX_ATTEMPTS = 300;
        if (attempts > MAX_ATTEMPTS) {
            // Couldn't find a working D3D12 swapchain after ~5 seconds
            // of trying. Stop spamming the log; menu's done for this run.
            if (!g_imgui_ready.exchange(true)) {
                OPENDOJO_LOG(
                    "render_hook: gave up after %u init attempts "
                    "— no D3D12 swapchain found",
                    attempts);
            }
            return g_present_orig(self, sync_interval, flags);
        }

        OPENDOJO_LOG(
            "render_hook: init attempt #%u on swapchain 0x%p "
            "(present #%u, %u frames since first attempt)",
            attempts, self, n, n - first_attempt_present);

        IDXGISwapChain3* sc3 = nullptr;
        if (FAILED(self->QueryInterface(IID_PPV_ARGS(&sc3)))) {
            OPENDOJO_LOG(
                "  rejected: not IDXGISwapChain3 "
                "(non-flip-model or wrapped)");
            if (rejected_count < 8) rejected[rejected_count++] = self;
            return g_present_orig(self, sync_interval, flags);
        }

        // Pre-flight: does this swapchain have an actual D3D12 device?
        // If not, init_imgui_resources's GetDevice will fail. Reject
        // here before allocating anything else.
        ID3D12Device* probe_dev = nullptr;
        if (FAILED(sc3->GetDevice(IID_PPV_ARGS(&probe_dev)))) {
            OPENDOJO_LOG(
                "  rejected: GetDevice(ID3D12Device) failed "
                "— swapchain isn't D3D12-backed");
            sc3->Release();
            if (rejected_count < 8) rejected[rejected_count++] = self;
            return g_present_orig(self, sync_interval, flags);
        }
        probe_dev->Release();  // init_imgui_resources will GetDevice again

        g_swapchain = sc3;
        const bool ok = safe_init(g_swapchain);
        if (ok) {
            g_imgui_ready.store(true);
            OPENDOJO_LOG(
                "render_hook: menu ready on swapchain 0x%p "
                "after %u attempts — press F12 to toggle",
                self, attempts);
            install_keyboard_hooks();
            install_xinput_hooks();
        } else {
            OPENDOJO_LOG("  rejected: init_imgui_resources failed mid-way");
            sc3->Release();
            g_swapchain = nullptr;
            if (rejected_count < 8) rejected[rejected_count++] = self;
        }
        return g_present_orig(self, sync_interval, flags);
    }

    // ---- Practice-mode gate ------------------------------------------------
    // Tekken is frame-rate-critical and gameplay is the whole point of
    // the mod, so OpenDojo does as close to zero work outside practice
    // mode as possible. One hash lookup per frame, then everything else
    // is gated on the result. Inside practice we still keep per-frame
    // work to a minimum (a gamepad poll + an autosave tick that mostly
    // early-returns); the only heavy path (ImGui render) is further
    // gated on the menu being visible.
    const bool in_practice = opendojo::subsystems::in_practice();

    // Detect the not-in-practice -> in-practice edge here (Present runs every
    // frame, including during a match) and invalidate practice_rename's cached
    // UObjects, which the engine reloads across a match. Doing this inside
    // practice_rename::tick() can't work — tick only runs while in practice.
    static bool s_prev_in_practice = false;
    if (in_practice && !s_prev_in_practice) {
        opendojo::practice_rename::on_practice_reentry();
    } else if (!in_practice && s_prev_in_practice) {
        opendojo::practice_rename::on_practice_exit();
    }
    s_prev_in_practice = in_practice;

    // Drop custom slot labels when the CPU character changes to a *different*
    // one. Labels are set by load_drill (manual preset load or autoload) and
    // belong to whatever character was loaded; without this they'd bleed onto
    // the next character's manually-selected slots when no new drill is loaded.
    // Compare non-zero ids only: the detour briefly reports id 0 mid-swap, and
    // a quickmatch return re-detects the SAME id (no change) so its labels are
    // kept. The next load_drill re-clears+sets, so clearing here is safe.
    static std::uint32_t s_prev_cpu_id = 0;
    auto cpu = opendojo::player_hook::current_cpu();
    if (in_practice && cpu.detected && cpu.cpu_character_id != 0) {
        if (s_prev_cpu_id != 0 && cpu.cpu_character_id != s_prev_cpu_id) {
            opendojo::slot_labels::clear_all();
            OPENDOJO_LOG("render_hook: CPU character %u -> %u — cleared slot labels", s_prev_cpu_id,
                         cpu.cpu_character_id);
        }
        s_prev_cpu_id = cpu.cpu_character_id;
    }

    if (!in_practice) {
        // If the user left practice with the menu open, auto-close so we
        // don't keep drawing it on top of menus/replays.
        if (g_menu_visible.exchange(false)) {
            OPENDOJO_LOG("render_hook: left practice — closing menu");
        }
        // Autosave still gets a chance to fire its "leaving practice"
        // save during the brief exit-grace window; it early-returns
        // after grace and does ~one cheap subsystem lookup per frame.
        opendojo::autosave::tick();
        return g_present_orig(self, sync_interval, flags);
    }

    // Keyboard toggle is handled in opendojo_wndproc. Gamepad chord +
    // pad-bind capture run inside the XInput hook itself (driven by
    // Tekken's own per-frame XInput call) — no separate poll. ImGui's
    // gamepad nav feed lives in render_frame so it only runs when the
    // menu is visible. See the XInput section comment for the full
    // rationale.
    opendojo::autosave::tick();
    // Rename the practice-menu "CPU Opponent Action N" rows in place. Cheap
    // after the first capture (event-driven re-apply via a SetTextID patch).
    opendojo::practice_rename::tick();

    if (!g_menu_visible.load()) {
        return g_present_orig(self, sync_interval, flags);
    }
    if (!g_device || !g_swapchain) {
        return g_present_orig(self, sync_interval, flags);
    }
    if (self != static_cast<IDXGISwapChain*>(g_swapchain)) {
        return g_present_orig(self, sync_interval, flags);
    }

    safe_render_frame();
    return g_present_orig(self, sync_interval, flags);
}

// ===========================================================================
//  Installation: create dummies, grab vtables, swap entries
// ===========================================================================

bool do_install() {
    HWND tmp_hwnd = CreateWindowExW(0, L"STATIC", L"opendojo_dummy", WS_POPUP, 0, 0, 100, 100,
                                    nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!tmp_hwnd) {
        OPENDOJO_LOG("render_hook: tmp hwnd creation failed (GLE=%lu)", GetLastError());
        return false;
    }

    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGIFactory4* factory = nullptr;
    IDXGISwapChain1* sc1 = nullptr;
    auto cleanup = [&] {
        if (sc1) sc1->Release();
        if (factory) factory->Release();
        if (queue) queue->Release();
        if (dev) dev->Release();
        DestroyWindow(tmp_hwnd);
    };

    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
        OPENDOJO_LOG("render_hook: D3D12CreateDevice failed — game may be D3D11");
        cleanup();
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) {
        OPENDOJO_LOG("render_hook: CreateCommandQueue (dummy) failed");
        cleanup();
        return false;
    }

    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        OPENDOJO_LOG("render_hook: CreateDXGIFactory1 failed");
        cleanup();
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.BufferCount = 2;
    scd.Width = 100;
    scd.Height = 100;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.SampleDesc.Count = 1;
    scd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    if (FAILED(factory->CreateSwapChainForHwnd(queue, tmp_hwnd, &scd, nullptr, nullptr, &sc1))) {
        OPENDOJO_LOG("render_hook: CreateSwapChainForHwnd (dummy) failed");
        cleanup();
        return false;
    }

    // COM vtables are class-shared in this process, so the dummy object's
    // vtable yields the same Present / ExecuteCommandLists addresses the
    // game's real objects use. We read the addresses here and detour them
    // below; the vtable itself is left untouched.
    void** swapchain_vt = *reinterpret_cast<void***>(sc1);
    void** queue_vt = *reinterpret_cast<void***>(queue);

    void* present_addr = swapchain_vt[8];  // IDXGISwapChain::Present
    void* exec_addr = queue_vt[10];        // ID3D12CommandQueue::ExecuteCommandLists

    // MinHook detour: patches the function prologue with a JMP rather
    // than touching the COM vtable. Tekken's anti-tamper scans vtables,
    // so a plain vtable swap crashes the game after a few thousand
    // frames. The dummy swapchain/queue just serves as a vehicle for
    // resolving the real function addresses; once we have those, we
    // don't need the dummy anymore.
    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        OPENDOJO_LOG("render_hook: MH_Initialize failed");
        cleanup();
        return false;
    }

    MH_STATUS s = MH_CreateHook(present_addr, reinterpret_cast<LPVOID>(&hook_present),
                                reinterpret_cast<LPVOID*>(&g_present_orig));
    if (s != MH_OK) {
        OPENDOJO_LOG("render_hook: MH_CreateHook(Present) failed: %d", s);
        cleanup();
        return false;
    }
    s = MH_CreateHook(exec_addr, reinterpret_cast<LPVOID>(&hook_execute_command_lists),
                      reinterpret_cast<LPVOID*>(&g_exec_orig));
    if (s != MH_OK) {
        OPENDOJO_LOG("render_hook: MH_CreateHook(ExecuteCommandLists) failed: %d", s);
        cleanup();
        return false;
    }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        OPENDOJO_LOG("render_hook: MH_EnableHook(MH_ALL_HOOKS) failed");
        cleanup();
        return false;
    }

    cleanup();

    g_hooks_live.store(true);
    OPENDOJO_LOG("render_hook: MinHook detours installed (present=0x%p, exec=0x%p)", present_addr,
                 exec_addr);
    return true;
}

void install_worker() {
    // Wait for the game's D3D12 / DXGI runtime DLLs to be loaded. They're
    // usually already there by the time our DllMain runs, but be patient
    // since we're racing the game's startup.
    for (int i = 0; i < 120; ++i) {  // up to 60s
        if (GetModuleHandleW(L"d3d12.dll") && GetModuleHandleW(L"dxgi.dll")) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!GetModuleHandleW(L"d3d12.dll")) {
        OPENDOJO_LOG(
            "render_hook: d3d12.dll never loaded — menu disabled "
            "(game is probably D3D11)");
        return;
    }

    if (!do_install()) {
        OPENDOJO_LOG("render_hook: install failed — menu disabled this session");
    }
}

}  // anonymous namespace

// ===========================================================================
//  Public API
// ===========================================================================

void install() {
    bool expected = false;
    if (!g_installing.compare_exchange_strong(expected, true)) return;
    std::thread(install_worker).detach();
}

void toggle_menu() {
    const bool was_visible = g_menu_visible.exchange(!g_menu_visible.load());
    const bool now_visible = !was_visible;
    OPENDOJO_LOG("render_hook: toggle_menu — %s -> %s", was_visible ? "VISIBLE" : "hidden",
                 now_visible ? "VISIBLE" : "hidden");
    if (now_visible) opendojo::menu::invalidate();
}

bool menu_visible() {
    return g_menu_visible.load();
}

ImFont* small_font() {
    return g_small_font;
}

}  // namespace opendojo::render_hook
