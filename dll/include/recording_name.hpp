#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace opendojo::recording_name {
inline constexpr std::size_t MAX_CHARS = 32;
inline constexpr std::size_t BUFFER_SIZE = MAX_CHARS * 4 + 1;

// Return only a valid, single-line UTF-8 prefix. Inspect at most 128 bytes;
// never allocate or scan an unbounded attacker-controlled name here.
// Stop at malformed encoding or controls, including embedded NUL, rather
// than passing replacement characters or invisible formatting to the game.
inline std::size_t prefix_size(std::string_view name) {
    std::size_t pos = 0;
    for (std::size_t count = 0; count < MAX_CHARS && pos < name.size(); ++count) {
        const auto lead = static_cast<unsigned char>(name[pos]);
        std::size_t length;
        std::uint32_t cp;
        std::uint32_t minimum;
        if (lead < 0x80) {
            length = 1;
            cp = lead;
            minimum = 0;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            cp = lead & 0x1F;
            minimum = 0x80;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            cp = lead & 0x0F;
            minimum = 0x800;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            cp = lead & 0x07;
            minimum = 0x10000;
        } else
            break;
        if (length > name.size() - pos) break;
        for (std::size_t j = 1; j < length; ++j) {
            const auto next = static_cast<unsigned char>(name[pos + j]);
            if ((next & 0xC0) != 0x80) return pos;
            cp = (cp << 6) | (next & 0x3F);
        }
        if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) break;
        if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || (cp >= 0x200B && cp <= 0x200F) ||
            (cp >= 0x2028 && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x206F) || cp == 0xFEFF)
            break;
        pos += length;
    }
    return pos;
}

inline std::string normalize(std::string_view name) {
    name = name.substr(0, prefix_size(name));
    if (name.find_first_not_of(' ') == std::string_view::npos) return {};
    return std::string(name);
}

inline std::string from_file(std::string_view name) {
    auto normalized = normalize(name);
    // Legacy exports generated these names for otherwise unnamed recordings.
    // Match only that exact convention, not descriptive names containing it.
    if (normalized.size() == 6 && normalized.compare(0, 5, "slot ") == 0 && normalized[5] >= '1' &&
        normalized[5] <= '8')
        return {};
    return normalized;
}

}  // namespace opendojo::recording_name
