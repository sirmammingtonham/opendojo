#include <windows.h>

#include "file_io.hpp"

#include <algorithm>
#include <atomic>

namespace opendojo::file_io {

bool bounded_getline(std::istream& input, std::string& line, std::size_t& remaining) {
    line.clear();
    char ch;
    while (remaining > 0) {
        if (!input.get(ch)) return input.eof() && !input.bad() && !line.empty();
        --remaining;
        if (ch == '\n') return true;
        line.push_back(ch);
    }
    line.clear();
    return false;
}

bool replace_file(const std::filesystem::path& path, std::string_view contents) {
    static std::atomic<unsigned long long> sequence{0};
    auto temporary = path;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        temporary = path;
        temporary += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
                     std::to_wstring(sequence.fetch_add(1));
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS) return false;
    }
    if (file == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    while (!contents.empty()) {
        const auto size = static_cast<DWORD>((std::min)(contents.size(), std::size_t{1 << 20}));
        DWORD written = 0;
        if (!WriteFile(file, contents.data(), size, &written, nullptr) || written == 0) {
            ok = false;
            break;
        }
        contents.remove_prefix(written);
    }
    if (ok) ok = FlushFileBuffers(file) != FALSE;
    if (!CloseHandle(file)) ok = false;
    if (ok) {
        ok = MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (!ok) DeleteFileW(temporary.c_str());
    return ok;
}

}  // namespace opendojo::file_io
