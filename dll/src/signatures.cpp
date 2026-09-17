#include "signatures.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string_view>

#include "log.hpp"
#include "memory.hpp"

namespace opendojo::signatures {

namespace {

// Pattern element: -1 = wildcard, 0..255 = exact byte.
using PatternByte = std::int16_t;

constexpr PatternByte WILD = -1;

struct Pattern {
    const char* name;
    // Source-side notation kept here for review/grep. The decoder reads
    // pairs of hex chars (e.g. "48") or "??" and produces PatternByte.
    const char* notation;
};

// --- Pattern definitions ---------------------------------------------------
//
// Each pattern is anchored on the function's prologue + a distinctive
// near-prologue feature (vtable load, magic constant, etc.). Wildcards
// only on RIP-relative offsets (the 4-byte immediates inside lea/mov/call
// instructions) — everything else is exact.
//
// To regenerate: read the function's first ~50 bytes from the EXE (RVA
// from Ghidra → file offset using PE section headers), then replace
// every 4-byte RIP-relative immediate with `?? ?? ?? ??`.

constexpr Pattern PRACTICE_DTOR_SIG{
    "practice_dtor",
    // FUN at RVA 0x5C8C880 in v3.00.02. MSVC dtor pattern: saves
    // rbx/rsi/rdi, stores vtable ptr (lea rax,[rip+vtable]; mov [rcx],rax),
    // then loads a global singleton ptr.
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B F2 48 8B D9 "
    "48 8D 05 ?? ?? ?? ?? 48 89 01 "
    "48 8B 0D ?? ?? ?? ?? "
    "48 85 C9 74 08 48 8B 01 33 D2 FF 50 28"};

constexpr Pattern PLAYER_REFRESH_SIG{
    "player_refresh",
    // FUN at RVA 0x5E70CD0 in v3.00.02. Two impostors share the prologue:
    //   - FUN_145E70B40 (the analyzed-by-Ghidra sibling) saves an extra
    //     register at the top, so its first 6 bytes differ — already
    //     excluded by `48 89 5C 24 18 57`.
    //   - FUN_145261320 has an IDENTICAL prologue + same `mov ecx, 0x6BC`
    //     magic + same `cmp [rip+_], eax ; jg` — but immediately after
    //     the jg, the impostor does another mov+cmp+jg (`8B 07 39 05`)
    //     while the target does a `mov eax, [rip+_] ; mov [rsp+0x30], eax`
    //     (`8B 05 ?? ?? ?? ?? 89 44 24 30`). That post-jg shape is the
    //     unique discriminator.
    //
    //   mov [rsp+0x18], rbx ; push rdi ; sub rsp, 0x20
    //   mov rax, gs:[0x58]                              ; TIB
    //   mov rbx, rcx                                    ; this
    //   mov edx, [rip+TLS_INDEX]                        ; wildcarded
    //   mov ecx, 0x6BC                                  ; TLS slot offset (magic)
    //   mov rdi, [rax+rdx*8] ; add rdi, rcx             ; resolve TLS slot
    //   mov eax, [rdi]                                  ; load slot value
    //   cmp [rip+GUARD], eax                            ; wildcarded
    //   jg <long>                                       ; 0F 8F + 32-bit disp
    //   mov eax, [rip+_] ; mov [rsp+0x30], eax          ; unique to target
    "48 89 5C 24 18 57 48 83 EC 20 "
    "65 48 8B 04 25 58 00 00 00 "
    "48 8B D9 "
    "8B 15 ?? ?? ?? ?? "
    "B9 BC 06 00 00 "
    "48 8B 3C D0 48 03 F9 "
    "8B 07 39 05 ?? ?? ?? ?? "
    "0F 8F ?? ?? ?? ?? "
    "8B 05 ?? ?? ?? ?? "
    "89 44 24 30"};

constexpr Pattern POOL_INIT_SIG{
    "pool_init",
    // FUN at RVA 0x18E8E00 in v3.00.02. Anchored on the magic allocation
    // size 0xFD32 (= 64818 = 9 * SLOT_PITCH). Very unique — no other
    // function in Polaris allocates this exact size.
    "48 83 EC 28 33 C0 48 89 41 24 "
    "48 39 05 ?? ?? ?? ?? "
    "75 ?? "
    "B9 32 FD 00 00"};

// get_ctx (FUN_1418dba40, RVA 0x18DBA40): the one-instruction
// `mov rax, [rip+CTX]; ret` getter that returns the service-locator
// context pointer. Its RIP-relative load is the only place we can
// recover CTX_PTR_OFFSET from code.
//
// The getter alone (`48 8B 05 ?? ?? ?? ?? C3`) matches ~20 trivial
// global-getters in Polaris. We anchor on the small "return -1" stub
// that happens to live immediately before it (`mov eax, -1 ; ret`)
// plus 2 bytes of CC padding — that whole 16-byte window is unique.
//
// This anchor depends on neighboring function layout. If it fails, subsystem
// lookup is disabled rather than following an obsolete absolute RVA.
constexpr Pattern GET_CTX_SIG{"get_ctx",
                              // `mov eax, -1 ; ret` (the -1-returner) + 2 CC pad + get_ctx body
                              "B8 FF FF FF FF C3 CC CC "
                              "48 8B 05 ?? ?? ?? ?? C3"};

// --- Pattern decode + scanner ----------------------------------------------

// Parse "48 8D 05 ?? ?? ?? ?? 48 89 01" into a byte array.
// Returns the count of bytes parsed, 0 on parse failure.
std::size_t decode_pattern(std::string_view src, PatternByte* out, std::size_t out_cap) {
    std::size_t n = 0;
    auto hex_nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::size_t i = 0;
    while (i < src.size()) {
        if (src[i] == ' ' || src[i] == '\t') {
            ++i;
            continue;
        }
        if (n >= out_cap) return 0;
        if (i + 1 < src.size() && src[i] == '?' && src[i + 1] == '?') {
            out[n++] = WILD;
            i += 2;
            continue;
        }
        if (i + 1 >= src.size()) return 0;
        int hi = hex_nibble(src[i]);
        int lo = hex_nibble(src[i + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[n++] = static_cast<PatternByte>((hi << 4) | lo);
        i += 2;
    }
    return n;
}

bool match_at(const std::uint8_t* mem, const PatternByte* pat, std::size_t plen) {
    for (std::size_t i = 0; i < plen; ++i) {
        if (pat[i] == WILD) continue;
        if (mem[i] != static_cast<std::uint8_t>(pat[i])) return false;
    }
    return true;
}

struct ScanResult {
    std::uintptr_t addr;  // 0 unless hit_count == 1
    int hit_count;        // capped at 4 — we don't care past "ambiguous"
};

// Scan [start, start+size) for `pat`. Returns the unique match address
// and how many matches were seen. addr is 0 unless hit_count == 1.
ScanResult scan_unique(const std::uint8_t* start, std::size_t size, const PatternByte* pat,
                       std::size_t plen) {
    if (plen == 0 || plen > size) return {0, 0};
    std::uintptr_t hit = 0;
    int hit_count = 0;
    // Anchor on first non-wildcard byte (cheap pre-filter on each
    // candidate position). Pure perf; doesn't affect correctness.
    std::size_t anchor_idx = 0;
    while (anchor_idx < plen && pat[anchor_idx] == WILD)
        ++anchor_idx;
    if (anchor_idx == plen) return {0, 0};
    std::uint8_t anchor_byte = static_cast<std::uint8_t>(pat[anchor_idx]);

    const std::size_t end = size - plen;
    for (std::size_t i = 0; i <= end; ++i) {
        const auto found = static_cast<const std::uint8_t*>(
            std::memchr(start + i + anchor_idx, anchor_byte, end - i + 1));
        if (!found) break;
        i = static_cast<std::size_t>(found - start) - anchor_idx;
        if (!match_at(start + i, pat, plen)) continue;
        if (hit_count == 0) hit = reinterpret_cast<std::uintptr_t>(start + i);
        if (++hit_count >= 4) break;  // ambiguous enough — stop scanning
    }
    return {hit_count == 1 ? hit : 0, hit_count};
}

// Locate Polaris's .text section at runtime. Returns {0, 0} on failure.
struct Section {
    std::uintptr_t start;
    std::size_t size;
};

Section find_text_section() {
    auto base = memory::polaris_base();
    if (!base) return {0, 0};

    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return {0, 0};
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return {0, 0};

    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char name[9] = {};
        std::memcpy(name, sec[i].Name, 8);
        if (std::string_view{name} == ".text") {
            return {base + sec[i].VirtualAddress, sec[i].Misc.VirtualSize};
        }
    }
    return {0, 0};
}

// --- Resolved-address storage ---------------------------------------------

std::atomic<std::uintptr_t> g_practice_dtor{0};
std::atomic<std::uintptr_t> g_player_refresh{0};
std::atomic<std::uintptr_t> g_pool_init{0};
std::atomic<std::uintptr_t> g_practice_slot{0};
std::atomic<std::uintptr_t> g_pool1_ptr{0};
std::atomic<std::uintptr_t> g_pool2_ptr{0};
std::atomic<std::uintptr_t> g_ctx_ptr{0};
std::once_flag g_resolve_once;
bool g_resolve_ok = false;

// Decode a 4-byte RIP-relative displacement into the absolute target.
// `instr_addr` is the address of the first byte of the instruction;
// `disp_off` is where the disp32 starts within the instruction; and
// `instr_len` is the full instruction length (RIP is the byte AFTER it).
std::uintptr_t decode_rip32(std::uintptr_t instr_addr, std::size_t disp_off,
                            std::size_t instr_len) {
    std::int32_t disp;
    std::memcpy(&disp, reinterpret_cast<const void*>(instr_addr + disp_off), sizeof(disp));
    return instr_addr + instr_len + static_cast<std::intptr_t>(disp);
}

// Resolve a RIP-relative data reference inside a previously-resolved
// function. `func_addr` is the function start, `scan_window` is how far
// in to search, the pattern matches the instruction shape, and
// disp_off/instr_len describe how to decode the disp32.
//
// Returns the absolute target address, or 0 if the pattern wasn't found.
std::uintptr_t resolve_data_ref(const char* label, std::uintptr_t func_addr,
                                std::size_t scan_window, const char* pattern_notation,
                                std::size_t disp_off, std::size_t instr_len) {
    if (!func_addr) return 0;
    PatternByte buf[32];
    auto n = decode_pattern(pattern_notation, buf, std::size(buf));
    if (n == 0) {
        OPENDOJO_LOG("signatures: %s — pattern decode failed", label);
        return 0;
    }
    DWORD64 image_base = 0;
    const auto function = RtlLookupFunctionEntry(func_addr, &image_base, nullptr);
    if (!function || image_base + function->BeginAddress != func_addr ||
        function->EndAddress <= function->BeginAddress) {
        OPENDOJO_LOG("signatures: %s has no matching unwind function boundary", label);
        return 0;
    }
    const auto function_size = function->EndAddress - function->BeginAddress;
    if (scan_window > function_size) scan_window = function_size;
    auto bytes = reinterpret_cast<const std::uint8_t*>(func_addr);
    const auto text = find_text_section();
    if (func_addr < text.start || func_addr - text.start >= text.size ||
        scan_window > text.size - (func_addr - text.start))
        return 0;
    const auto match = scan_unique(bytes, scan_window, buf, n);
    if (match.hit_count != 1) {
        OPENDOJO_LOG("signatures: %s — xref instruction missing or ambiguous in %s body", label,
                     "resolved function");
        return 0;
    }
    auto target = decode_rip32(match.addr, disp_off, instr_len);
    if (!memory::is_image_data(target, sizeof(std::uintptr_t))) {
        OPENDOJO_LOG("signatures: %s rejected target outside writable image data", label);
        return 0;
    }
    OPENDOJO_LOG("signatures: %s -> 0x%llX", label, static_cast<unsigned long long>(target));
    return target;
}

bool scan_one(const Pattern& sig, const std::uint8_t* text, std::size_t size,
              std::atomic<std::uintptr_t>& out) {
    PatternByte buf[128];
    auto n = decode_pattern(sig.notation, buf, std::size(buf));
    if (n == 0) {
        OPENDOJO_LOG("signatures: %s — pattern decode failed (check notation)", sig.name);
        return false;
    }
    auto r = scan_unique(text, size, buf, n);
    if (r.hit_count == 0) {
        OPENDOJO_LOG("signatures: %s — NOT FOUND (Tekken patched the function body)", sig.name);
        return false;
    }
    if (r.hit_count > 1) {
        OPENDOJO_LOG("signatures: %s — AMBIGUOUS (>=%d matches; pattern needs more bytes)",
                     sig.name, r.hit_count);
        return false;
    }
    DWORD64 image_base = 0;
    const auto function = RtlLookupFunctionEntry(r.addr, &image_base, nullptr);
    if (!function || image_base + function->BeginAddress != r.addr) {
        OPENDOJO_LOG("signatures: %s match is not an unwind function entry", sig.name);
        return false;
    }
    out.store(r.addr, std::memory_order_release);
    OPENDOJO_LOG("signatures: %s -> 0x%llX", sig.name, static_cast<unsigned long long>(r.addr));
    return true;
}

}  // namespace

static bool do_resolve() {
    auto text = find_text_section();
    if (!text.start) {
        OPENDOJO_LOG("signatures: failed to locate Polaris .text section");
        return false;
    }
    OPENDOJO_LOG("signatures: scanning .text @ 0x%llX size=0x%llX",
                 static_cast<unsigned long long>(text.start),
                 static_cast<unsigned long long>(text.size));

    auto text_bytes = reinterpret_cast<const std::uint8_t*>(text.start);
    bool ok = true;
    ok &= scan_one(PRACTICE_DTOR_SIG, text_bytes, text.size, g_practice_dtor);
    ok &= scan_one(PLAYER_REFRESH_SIG, text_bytes, text.size, g_player_refresh);
    ok &= scan_one(POOL_INIT_SIG, text_bytes, text.size, g_pool_init);

    // Phase-2: resolve data slots by scanning the bodies of just-resolved
    // functions for the instructions that touch them. Each xref is a
    // single RIP-relative load/store with a distinctive enough surrounding
    // pattern (xor reg,reg; mov [rip+disp],reg / cmp [rip+disp], rax)
    // whose unique match must stay within the unwind function boundary.

    // Inside the practice dtor: `xor edi, edi ; mov [rip+disp], rdi`
    // clears the practice-controller singleton slot. The disp32 starts
    // at byte +5 of the matched window (the 7-byte mov starts at +2);
    // total instr length 7.
    auto practice_slot = resolve_data_ref("practice_slot", g_practice_dtor.load(),
                                          /*scan_window=*/0x200, "33 FF 48 89 3D ?? ?? ?? ??",
                                          /*disp_off=*/5, /*instr_len=*/9);
    if (practice_slot) {
        g_practice_slot.store(practice_slot, std::memory_order_release);
    } else {
        ok = false;
    }

    // Inside pool_init: `cmp [rip+disp], rax ; jne` near the prologue
    // checks whether POOL1 has already been allocated. First such
    // instruction in the function body is POOL1.
    auto pool1 = resolve_data_ref("pool1_ptr", g_pool_init.load(),
                                  /*scan_window=*/0x40, "48 39 05 ?? ?? ?? ?? 75",
                                  /*disp_off=*/3, /*instr_len=*/7);
    if (pool1 && memory::is_image_data(pool1, 16)) {
        g_pool1_ptr.store(pool1, std::memory_order_release);
        // POOL2 lives 8 bytes after POOL1 — they're adjacent qword
        // pointers in the same struct, written consecutively by
        // pool_init. The relationship is structural, not coincidental.
        g_pool2_ptr.store(pool1 + 8, std::memory_order_release);
        OPENDOJO_LOG("signatures: pool2_ptr -> 0x%llX", static_cast<unsigned long long>(pool1 + 8));
    } else {
        ok = false;
    }

    // CTX is the cornerstone of every subsystem lookup, so it's worth
    // a separate scan. The get_ctx getter (one-instruction `mov rax,
    // [rip+CTX]; ret`) is what we resolve; the disp32 inside it points
    // at CTX. Match offset 8 in the pattern is where the `mov` starts;
    // disp at +3 of that; instr_len 7.
    {
        PatternByte buf[32];
        auto n = decode_pattern(GET_CTX_SIG.notation, buf, std::size(buf));
        auto r = (n == 0) ? ScanResult{0, 0} : scan_unique(text_bytes, text.size, buf, n);
        if (r.hit_count == 1) {
            auto mov_addr = r.addr + 8;  // skip -1-returner + 2 CC padding
            auto ctx_addr = decode_rip32(mov_addr, /*disp_off=*/3, /*instr_len=*/7);
            if (!memory::is_image_data(ctx_addr, 8)) {
                OPENDOJO_LOG("signatures: ctx_ptr rejected target outside writable image data");
                return false;
            }
            g_ctx_ptr.store(ctx_addr, std::memory_order_release);
            OPENDOJO_LOG("signatures: ctx_ptr -> 0x%llX",
                         static_cast<unsigned long long>(ctx_addr));
        } else {
            OPENDOJO_LOG(
                "signatures: ctx_ptr — get_ctx anchor failed (%d matches); "
                "subsystem lookup disabled",
                r.hit_count);
            ok = false;
        }
    }

    return ok;
}

bool resolve_all() {
    std::call_once(g_resolve_once, [] { g_resolve_ok = do_resolve(); });
    return g_resolve_ok;
}

std::uintptr_t practice_dtor() {
    return g_practice_dtor.load(std::memory_order_acquire);
}
std::uintptr_t player_refresh() {
    return g_player_refresh.load(std::memory_order_acquire);
}
std::uintptr_t pool_init() {
    return g_pool_init.load(std::memory_order_acquire);
}
std::uintptr_t practice_slot_addr() {
    return g_practice_slot.load(std::memory_order_acquire);
}
std::uintptr_t pool1_ptr_addr() {
    return g_pool1_ptr.load(std::memory_order_acquire);
}
std::uintptr_t pool2_ptr_addr() {
    return g_pool2_ptr.load(std::memory_order_acquire);
}
std::uintptr_t ctx_ptr_addr() {
    return g_ctx_ptr.load(std::memory_order_acquire);
}

}  // namespace opendojo::signatures
