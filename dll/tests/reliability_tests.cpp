// Include implementation to exercise the actual internal scanners without
// expanding the shipping DLL API just for tests. No game or hooks required.
#include "../src/signatures.cpp"
#include "../src/players.cpp"

#include <cstdlib>
#include <iostream>
#include <random>

namespace opendojo::log {
void format(const char*, ...) {}
}  // namespace opendojo::log

static void check(bool condition) {
    if (!condition) {
        std::cerr << "Reliability check failed\n";
        std::exit(1);
    }
}

int main() {
    using namespace opendojo::signatures;
    PatternByte decoded[16]{};
    check(decode_pattern("48 ?? 0a FF", decoded, 16) == 4);
    check(decoded[0] == 0x48 && decoded[1] == WILD && decoded[2] == 10);
    check(decode_pattern("4", decoded, 16) == 0);
    check(decode_pattern("GG", decoded, 16) == 0);
    check(decode_pattern("48 49", decoded, 1) == 0);

    // Compare the optimized scan against an exhaustive oracle, including
    // leading wildcards, overlaps, final-byte hits and ambiguous patterns.
    std::mt19937 random(42);
    for (int trial = 0; trial < 10000; ++trial) {
        std::vector<std::uint8_t> bytes(random() % 128);
        for (auto& byte : bytes)
            byte = static_cast<std::uint8_t>(random() % 8);
        std::vector<PatternByte> pattern(random() % 18);
        bool anchored = false;
        opendojo::players::CompiledPattern player_pattern;
        for (auto& byte : pattern) {
            byte = random() % 3 == 0 ? WILD : static_cast<PatternByte>(random() % 8);
            anchored |= byte != WILD;
            player_pattern.bytes.push_back(byte == WILD ? 0 : static_cast<std::uint8_t>(byte));
            player_pattern.mask.push_back(byte == WILD ? 0 : 1);
        }
        int hits = 0;
        std::uintptr_t expected = 0;
        if (anchored && pattern.size() <= bytes.size()) {
            for (std::size_t i = 0; i <= bytes.size() - pattern.size(); ++i) {
                if (match_at(bytes.data() + i, pattern.data(), pattern.size())) {
                    ++hits;
                    expected = reinterpret_cast<std::uintptr_t>(bytes.data() + i);
                }
            }
        }
        if (hits != 1) expected = 0;
        const auto result = scan_unique(bytes.data(), bytes.size(), pattern.data(), pattern.size());
        check(result.addr == expected && result.hit_count == (hits > 4 ? 4 : hits));
        check(opendojo::players::scan(player_pattern,
                                      reinterpret_cast<std::uintptr_t>(bytes.data()),
                                      bytes.size()) == expected);
    }

    // Both displacement signs must work, including references to earlier data.
    std::uint8_t instruction[16]{};
    const auto at = reinterpret_cast<std::uintptr_t>(instruction);
    std::int32_t disp = -7;
    std::memcpy(instruction + 3, &disp, 4);
    check(decode_rip32(at, 3, 7) == at);
    disp = 32;
    std::memcpy(instruction + 3, &disp, 4);
    check(decode_rip32(at, 3, 7) == at + 39);

    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto page_size = info.dwPageSize;
    auto pages = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, page_size * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    check(pages != nullptr);
    const auto address = reinterpret_cast<std::uintptr_t>(pages);
    DWORD old = 0;
    check(VirtualProtect(pages + page_size, page_size, PAGE_READONLY, &old) != 0);
    check(opendojo::memory::is_readable(address, page_size * 2));
    check(!opendojo::memory::is_readable(UINTPTR_MAX - 1, 4));
    check(!opendojo::memory::is_readable(address, 0));
    check(VirtualProtect(pages + page_size, page_size, PAGE_NOACCESS, &old) != 0);
    check(!opendojo::memory::is_readable(address, page_size * 2));
    std::uint64_t value = 123;
    check(!opendojo::memory::try_read_u64(address + page_size, &value) && value == 0);
    check(!opendojo::memory::try_read_u64(address, nullptr));
    check(!opendojo::memory::is_image_data(address, 8));
    check(VirtualProtect(pages + page_size, page_size, PAGE_READWRITE | PAGE_GUARD, &old) != 0);
    check(!opendojo::memory::is_readable(address, page_size * 2));
    VirtualFree(pages, 0, MEM_RELEASE);
    std::cout << "Passed 10,000 scanner comparisons, RIP decoding and memory protection checks\n";
}
