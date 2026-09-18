#pragma once
#include <algorithm>
#include <cstring>
#include <string_view>

namespace opendojo::ui {
inline constexpr std::size_t DRILL_NAME_BUFFER_SIZE = 96 * 4 + 1;
inline constexpr std::size_t DESCRIPTION_BUFFER_SIZE = 4096 + 1;

// All valid cloud metadata fits; oversized local text stops at a UTF-8 boundary.
template <std::size_t N>
void copy_text(char (&out)[N], std::string_view text) {
    static_assert(N > 0);
    std::size_t n = (std::min)(text.size(), N - 1);
    if (n < text.size())
        while (n && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) --n;
    if (n) std::memcpy(out, text.data(), n);
    out[n] = '\0';
}
}
