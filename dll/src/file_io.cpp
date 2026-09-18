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

static CreateResult write_file(const std::filesystem::path& path, std::string_view contents,
                               bool replace) {
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
        if (GetLastError() != ERROR_FILE_EXISTS) return CreateResult::Failed;
    }
    if (file == INVALID_HANDLE_VALUE) return CreateResult::Failed;
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
    DWORD commit_error = ERROR_SUCCESS;
    if (ok) {
        ok = MoveFileExW(temporary.c_str(), path.c_str(),
                         (replace ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_WRITE_THROUGH) !=
             FALSE;
        if (!ok) commit_error = GetLastError();
    }
    if (!ok) DeleteFileW(temporary.c_str());
    if (ok) return CreateResult::Saved;
    if (!replace && (commit_error == ERROR_ALREADY_EXISTS || commit_error == ERROR_FILE_EXISTS))
        return CreateResult::Exists;
    return CreateResult::Failed;
}

bool replace_file(const std::filesystem::path& path, std::string_view contents) {
    return write_file(path, contents, true) == CreateResult::Saved;
}

CreateResult create_file(const std::filesystem::path& path, std::string_view contents) {
    return write_file(path, contents, false);
}

}  // namespace opendojo::file_io
