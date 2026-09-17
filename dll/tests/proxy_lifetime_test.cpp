#include <windows.h>

#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const auto module = LoadLibraryW(argv[1]);
    if (!module) {
        std::cerr << "LoadLibrary failed: " << GetLastError() << '\n';
        return 1;
    }
    const auto exported = GetProcAddress(module, "DllCanUnloadNow");
    if (!exported) return 1;
    const auto can_unload = reinterpret_cast<HRESULT(WINAPI*)()>(exported);
    if (can_unload() != S_FALSE || !FreeLibrary(module)) return 1;
    HMODULE retained = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(exported), &retained) ||
        retained != module)
        return 1;
    std::cout << "Proxy stays loaded after FreeLibrary; process exit follows\n";
    // The test host is not Polaris, so no game hooks or scans are installed.
    return 0;
}
