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
    {
        using namespace opendojo::signatures;
        const Pattern contract{"branch_graph", "85 C0 74 02 33 C0 C3"};
        std::vector<std::uint8_t> code{0x85, 0xc0, 0x0f, 0x84, 3, 0, 0, 0, 0x90, 0x33, 0xc0, 0xc3};
        auto match = [&] {
            const auto at = reinterpret_cast<std::uintptr_t>(code.data());
            return graph_contract(opendojo::native_scan::decode(at, at + code.size()), contract);
        };
        check(bool(match()));
        code[4] = 1;
        check(!match());  // Same operations/branch condition, wrong destination.
    }
    {
        using namespace opendojo::signatures;
        PatternByte pattern[128];
        auto n = decode_pattern(SUBSYSTEM_LOOKUP_SIG.notation, pattern, 128);
        check(n == 92);
        std::vector<std::uint8_t> code(n);
        for (std::size_t i = 0; i < n; ++i)
            code[i] = pattern[i] < 0 ? 0 : static_cast<std::uint8_t>(pattern[i]);
        auto put = [&](int at, std::uint32_t value) { std::memcpy(code.data() + at, &value, 4); };
        code[3] = 24;
        put(10, 0x150);
        put(17, 0x120);
        put(31, 0x130);
        code[27] = 5;
        code[38] = 16;
        code[50] = code[65] = 24;
        code[61] = 16;
        code[87] = 32;
        const auto l = decode_subsystem_layout(code.data(), code.size());
        check(l.map == 24 && l.mask == 0x150 && l.bucket_stride == 32 && l.key == 24 &&
              l.value == 32);
        code[65] = 28;
        check(!decode_subsystem_layout(code.data(), code.size()).bucket_stride);
        code[65] = 24;
        code[27] = 64;
        check(!decode_subsystem_layout(code.data(), code.size()).bucket_stride);
        check(!decode_subsystem_layout(code.data(), 91).bucket_stride);
    }

    // Movelist/session field relocation and disagreement are exercised through
    // production discovery in native_layout_tests, rather than legacy byte readers.
    {
        using namespace opendojo::players;
        CompiledPattern pattern;
        check(compile_pattern(PAT_ROUND_COUNTER, pattern));
        auto code = pattern.bytes;
        auto set_field = [&](std::size_t at, std::uint32_t value) {
            std::memcpy(code.data() + at, &value, sizeof(value));
        };
        // Simulate a patch moving all three fields independently. Discovery
        // must follow the encoded layout rather than remembering 0x15D0.
        for (auto at : {2u, 16u, 26u})
            set_field(at, 0x1600);
        for (auto at : {32u, 41u, 51u})
            set_field(at, 0x1700);
        for (auto at : {57u, 66u, 80u})
            set_field(at, 0x1900);
        check(decode_round_counter(code.data(), code.size()) == 0x1700);
        check(decode_round_counter(code.data(), code.size() - 1) == 0);
        set_field(51, 0x1704);
        check(decode_round_counter(code.data(), code.size()) == 0);
        for (auto at : {32u, 41u, 51u})
            set_field(at, 0xFFFFFFFC);
        check(decode_round_counter(code.data(), code.size()) == 0);
        for (auto at : {32u, 41u, 51u})
            set_field(at, 0x1600);
        check(decode_round_counter(code.data(), code.size()) == 0);
    }
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

namespace opendojo::subsystems {
std::uintptr_t lookup(std::uint32_t) {
    return 0;
}
}  // namespace opendojo::subsystems
