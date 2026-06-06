// Forwarded exports for dinput8.dll proxy + keyboard-state suppression
// when the OpenDojo menu is visible.
//
// Forwarding strategy: at DllMain attach we LoadLibraryW the real
// C:\Windows\System32\dinput8.dll and GetProcAddress each export. The
// exported symbols below are pure trampolines that call the resolved
// pointer.
//
// Keyboard suppression: Tekken reads keyboard state via DirectInput8,
// which polls raw OS state directly — our WndProc subclass + user32
// hooks (GetAsyncKeyState / GetKeyState / GetKeyboardState) don't cover
// it. We intercept the chain by:
//   1. Wrapping DirectInput8Create -> MinHook IDirectInput8::CreateDevice
//   2. When CreateDevice produces a keyboard device, MinHook its
//      IDirectInputDevice8::GetDeviceState and GetDeviceData
//   3. While the menu is up, the hooks zero the state / report empty
//      data so the game sees no keypresses.

#include <windows.h>

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>  // GUID_SysKeyboard resolved via dxguid.lib
#include <unknwn.h>

#include <atomic>
#include <cstring>

#include "MinHook.h"

#include "hooks/proxy.hpp"
#include "hooks/render_hook.hpp"
#include "log.hpp"

namespace {

// clang-format off
using DirectInput8Create_fn  = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using DllCanUnloadNow_fn     = HRESULT (WINAPI*)();
using DllGetClassObject_fn   = HRESULT (WINAPI*)(REFCLSID, REFIID, LPVOID*);
using DllRegisterServer_fn   = HRESULT (WINAPI*)();
using DllUnregisterServer_fn = HRESULT (WINAPI*)();
using GetdfDIJoystick_fn     = LPCDIDATAFORMAT (WINAPI*)();

HMODULE                g_real                = nullptr;
DirectInput8Create_fn  p_DirectInput8Create  = nullptr;
DllCanUnloadNow_fn     p_DllCanUnloadNow     = nullptr;
DllGetClassObject_fn   p_DllGetClassObject   = nullptr;
DllRegisterServer_fn   p_DllRegisterServer   = nullptr;
DllUnregisterServer_fn p_DllUnregisterServer = nullptr;
GetdfDIJoystick_fn     p_GetdfDIJoystick     = nullptr;
// clang-format on

// ---- Keyboard suppression --------------------------------------------------

using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(LPDIRECTINPUT8, REFGUID, LPDIRECTINPUTDEVICE8*,
                                                   LPUNKNOWN);
using GetDeviceState_t = HRESULT(STDMETHODCALLTYPE*)(LPDIRECTINPUTDEVICE8, DWORD, LPVOID);
using GetDeviceData_t = HRESULT(STDMETHODCALLTYPE*)(LPDIRECTINPUTDEVICE8, DWORD,
                                                    LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

CreateDevice_t g_create_device_orig = nullptr;
GetDeviceState_t g_get_state_orig = nullptr;
GetDeviceData_t g_get_data_orig = nullptr;

bool ensure_minhook() {
    static std::atomic<bool> initialized{false};
    if (initialized.load()) return true;
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        OPENDOJO_LOG("dinput: MH_Initialize failed: %d", s);
        return false;
    }
    initialized.store(true);
    return true;
}

HRESULT STDMETHODCALLTYPE get_state_hook(LPDIRECTINPUTDEVICE8 self, DWORD cb, LPVOID data) {
    HRESULT hr = g_get_state_orig(self, cb, data);
    if (SUCCEEDED(hr) && data && opendojo::render_hook::menu_visible()) {
        std::memset(data, 0, cb);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE get_data_hook(LPDIRECTINPUTDEVICE8 self, DWORD cbObjectData,
                                        LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut,
                                        DWORD dwFlags) {
    if (opendojo::render_hook::menu_visible() && pdwInOut) {
        // Drain the buffer if the caller asked us to (DIGDD_PEEK clears
        // this flag) so a queued backlog doesn't all fire when the menu
        // closes. Then report zero events.
        DWORD requested = *pdwInOut;
        HRESULT hr = g_get_data_orig(self, cbObjectData, rgdod, &requested, dwFlags);
        (void)hr;
        *pdwInOut = 0;
        return DI_OK;
    }
    return g_get_data_orig(self, cbObjectData, rgdod, pdwInOut, dwFlags);
}

void hook_keyboard_device(LPDIRECTINPUTDEVICE8 dev) {
    static std::atomic<bool> done{false};
    bool expected = false;
    if (!done.compare_exchange_strong(expected, true)) return;
    if (!dev || !ensure_minhook()) return;

    // IDirectInputDevice8 vtable layout (after IUnknown's 3 slots):
    //   3 GetCapabilities, 4 EnumObjects, 5 GetProperty, 6 SetProperty,
    //   7 Acquire, 8 Unacquire, 9 GetDeviceState, 10 GetDeviceData, ...
    void** vt = *reinterpret_cast<void***>(dev);

    if (!(MH_CreateHook(vt[9], reinterpret_cast<LPVOID>(&get_state_hook),
                        reinterpret_cast<LPVOID*>(&g_get_state_orig)) == MH_OK &&
          MH_EnableHook(vt[9]) == MH_OK)) {
        OPENDOJO_LOG("dinput: GetDeviceState hook failed");
    }
    if (!(MH_CreateHook(vt[10], reinterpret_cast<LPVOID>(&get_data_hook),
                        reinterpret_cast<LPVOID*>(&g_get_data_orig)) == MH_OK &&
          MH_EnableHook(vt[10]) == MH_OK)) {
        OPENDOJO_LOG("dinput: GetDeviceData hook failed");
    }
}

HRESULT STDMETHODCALLTYPE create_device_hook(LPDIRECTINPUT8 self, REFGUID rguid,
                                             LPDIRECTINPUTDEVICE8* lplp, LPUNKNOWN unk) {
    HRESULT hr = g_create_device_orig(self, rguid, lplp, unk);
    if (SUCCEEDED(hr) && lplp && *lplp && IsEqualGUID(rguid, GUID_SysKeyboard)) {
        hook_keyboard_device(*lplp);
    }
    return hr;
}

void hook_dinput_instance(LPDIRECTINPUT8 di) {
    static std::atomic<bool> done{false};
    bool expected = false;
    if (!done.compare_exchange_strong(expected, true)) return;
    if (!di || !ensure_minhook()) return;

    // IDirectInput8 vtable: 3 CreateDevice, 4 EnumDevices, ...
    void** vt = *reinterpret_cast<void***>(di);
    if (!(MH_CreateHook(vt[3], reinterpret_cast<LPVOID>(&create_device_hook),
                        reinterpret_cast<LPVOID*>(&g_create_device_orig)) == MH_OK &&
          MH_EnableHook(vt[3]) == MH_OK)) {
        OPENDOJO_LOG("dinput: CreateDevice hook failed");
    }
}

}  // namespace

bool opendojo::proxy::load() {
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 16) return false;
    wcscat_s(path, MAX_PATH, L"\\dinput8.dll");

    g_real = LoadLibraryW(path);
    if (!g_real) {
        OPENDOJO_LOG("proxy: LoadLibraryW(%ls) failed, GetLastError=%lu", path, GetLastError());
        return false;
    }

    p_DirectInput8Create =
        reinterpret_cast<DirectInput8Create_fn>(GetProcAddress(g_real, "DirectInput8Create"));
    p_DllCanUnloadNow =
        reinterpret_cast<DllCanUnloadNow_fn>(GetProcAddress(g_real, "DllCanUnloadNow"));
    p_DllGetClassObject =
        reinterpret_cast<DllGetClassObject_fn>(GetProcAddress(g_real, "DllGetClassObject"));
    p_DllRegisterServer =
        reinterpret_cast<DllRegisterServer_fn>(GetProcAddress(g_real, "DllRegisterServer"));
    p_DllUnregisterServer =
        reinterpret_cast<DllUnregisterServer_fn>(GetProcAddress(g_real, "DllUnregisterServer"));
    p_GetdfDIJoystick =
        reinterpret_cast<GetdfDIJoystick_fn>(GetProcAddress(g_real, "GetdfDIJoystick"));

    // DirectInput8Create is the only one Tekken is known to call. Missing it
    // means we have the wrong DLL or a Windows version we haven't seen.
    if (!p_DirectInput8Create) {
        OPENDOJO_LOG("proxy: real dinput8.dll missing DirectInput8Create — aborting");
        return false;
    }
    return true;
}

void opendojo::proxy::unload() {
    if (g_real) {
        FreeLibrary(g_real);
        g_real = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Forwarded exports. extern "C" + the matching .def entry pins these names
// so the linker exports them undecorated, exactly as dinput8.dll does.
// ---------------------------------------------------------------------------
extern "C" {

HRESULT WINAPI DirectInput8Create(HINSTANCE h, DWORD v, REFIID r, LPVOID* p, LPUNKNOWN u) {
    if (!p_DirectInput8Create) return E_FAIL;
    HRESULT hr = p_DirectInput8Create(h, v, r, p, u);
    if (SUCCEEDED(hr) && p && *p) {
        // Both IDirectInput8A and IDirectInput8W have an identical vtable
        // layout, so we can patch either without distinguishing.
        hook_dinput_instance(static_cast<LPDIRECTINPUT8>(*p));
    }
    return hr;
}
HRESULT WINAPI DllCanUnloadNow(void) {
    return p_DllCanUnloadNow ? p_DllCanUnloadNow() : S_OK;
}
HRESULT WINAPI DllGetClassObject(REFCLSID c, REFIID r, LPVOID* p) {
    return p_DllGetClassObject ? p_DllGetClassObject(c, r, p) : E_FAIL;
}
HRESULT WINAPI DllRegisterServer(void) {
    return p_DllRegisterServer ? p_DllRegisterServer() : S_OK;
}
HRESULT WINAPI DllUnregisterServer(void) {
    return p_DllUnregisterServer ? p_DllUnregisterServer() : S_OK;
}
LPCDIDATAFORMAT WINAPI GetdfDIJoystick(void) {
    return p_GetdfDIJoystick ? p_GetdfDIJoystick() : nullptr;
}

}  // extern "C"
