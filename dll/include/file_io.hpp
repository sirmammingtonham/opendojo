#pragma once

#include <filesystem>
#include <istream>
#include <string>
#include <string_view>

namespace opendojo::file_io {

// Write and flush a unique sibling temporary file, then atomically replace
// the destination. A failed write/replace preserves the previous destination.
bool replace_file(const std::filesystem::path& path, std::string_view contents);
enum class CreateResult { Saved, Exists, Failed };
// Atomically publish a complete new file without ever replacing another file.
CreateResult create_file(const std::filesystem::path& path, std::string_view contents);

// Read only complete lines within a shared byte budget. An overlong line is
// rejected without reading or allocating the rest of the file.
bool bounded_getline(std::istream& input, std::string& line, std::size_t& remaining);

}  // namespace opendojo::file_io
