#include "players.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "log.hpp"
#include "memory.hpp"
#include "signatures.hpp"
#include "subsystems.hpp"

namespace opendojo::players {

namespace {

// ---------------------------------------------------------------------------
// Patterns and offsets — see players.hpp for provenance.
// ---------------------------------------------------------------------------

// Code site that writes the GlobalPlayerHolder pointer to a global slot.
//   4C 89 35 [rip+disp32]   MOV  [rip+disp32], r14
//   41 88 5E 28             MOV  [r14+0x28], BL
// disp32 at offset +3 is the RIP-relative address of the holder slot.
constexpr const char* PAT_PLAYERS = "4C 89 35 ?? ?? ?? ?? 41 88 5E ??";

// Three consecutive player timers: two saturate at 0xFFFF and the third
// at 0x0FFFFFFF. The middle timer is the existing round gate. Decode its
// displacement from code so a field-layout shift does not leave a stale read.
// Current executable: image address 0x141960138, middle timer +0x15D0.
constexpr const char* PAT_ROUND_COUNTER =
    "8B 8E ?? ?? ?? ?? BA FF FF 00 00 8D 41 01 89 86 ?? ?? ?? ?? "
    "3B CA 72 06 89 96 ?? ?? ?? ?? "
    "8B 8E ?? ?? ?? ?? 8D 41 01 89 86 ?? ?? ?? ?? 3B CA 72 06 89 96 ?? ?? ?? ?? "
    "8B 8E ?? ?? ?? ?? 8D 41 01 89 86 ?? ?? ?? ?? 81 F9 FF FF FF 0F 72 0A "
    "C7 86 ?? ?? ?? ?? FF FF FF 0F";

std::uint32_t decode_round_counter(const std::uint8_t* code, std::size_t size) {
    if (!code || size < 88) return 0;
    auto displacement = [code](std::size_t at) {
        std::uint32_t value = 0;
        std::memcpy(&value, code + at, sizeof(value));
        return value;
    };
    const auto first = displacement(2), round = displacement(32), third = displacement(57);
    for (auto field : {first, round, third})
        if (field < 0x100 || field > 0x10000 || field % 4 != 0) return 0;
    if (first == round || first == third || round == third) return 0;
    // Every load, increment store, and saturation store must refer to the
    // same field. Matching only one displacement could accept unrelated code.
    if (first != displacement(16) || first != displacement(26) || round != displacement(41) ||
        round != displacement(51) || third != displacement(66) || third != displacement(80))
        return 0;
    return round;
}

// ---------------------------------------------------------------------------
// AOB pattern compilation + scan
// ---------------------------------------------------------------------------

struct CompiledPattern {
    std::vector<std::uint8_t> bytes;  // exact byte values (0 where wildcard)
    std::vector<std::uint8_t> mask;   // 1 = compare, 0 = wildcard
};

// Parse "4C 89 ?? 35" -> {{0x4C, 0x89, 0x00, 0x35}, {1, 1, 0, 1}}.
// Tolerates extra spaces. Returns false on malformed input.
bool compile_pattern(const char* p, CompiledPattern& out) {
    out.bytes.clear();
    out.mask.clear();
    auto hex_nibble = [](char c, int& v) {
        if (c >= '0' && c <= '9') {
            v = c - '0';
            return true;
        }
        if (c >= 'A' && c <= 'F') {
            v = c - 'A' + 10;
            return true;
        }
        if (c >= 'a' && c <= 'f') {
            v = c - 'a' + 10;
            return true;
        }
        return false;
    };
    while (*p) {
        while (*p == ' ' || *p == '\t')
            ++p;
        if (!*p) break;
        if (p[0] == '?' && p[1] == '?') {
            out.bytes.push_back(0);
            out.mask.push_back(0);
            p += 2;
        } else {
            int hi, lo;
            if (!hex_nibble(p[0], hi)) return false;
            if (!p[1] || !hex_nibble(p[1], lo)) return false;
            out.bytes.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
            out.mask.push_back(1);
            p += 2;
        }
    }
    return !out.bytes.empty();
}

// Locate the .text section of Polaris. Returns false if Polaris isn't loaded.
bool get_text_range(std::uintptr_t& start, std::size_t& size) {
    auto base = memory::polaris_base();
    if (!base) return false;
    auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = reinterpret_cast<PIMAGE_NT_HEADERS64>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    auto first = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& s = first[i];
        if (std::strncmp(reinterpret_cast<const char*>(s.Name), ".text", 5) == 0) {
            start = base + s.VirtualAddress;
            size = s.Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

// Require a unique code site. Ambiguous patterns disable detection.
std::uintptr_t scan(const CompiledPattern& pat, std::uintptr_t start, std::size_t size) {
    if (pat.bytes.empty() || size < pat.bytes.size()) return 0;
    const auto base = reinterpret_cast<const std::uint8_t*>(start);
    const std::size_t span = size - pat.bytes.size() + 1;
    const std::size_t n = pat.bytes.size();
    std::uintptr_t first_hit = 0;
    int hits = 0;
    const auto anchor = std::find(pat.mask.begin(), pat.mask.end(), 1);
    if (anchor == pat.mask.end()) return 0;
    const auto anchor_idx = static_cast<std::size_t>(anchor - pat.mask.begin());
    for (std::size_t i = 0; i < span; ++i) {
        const auto found = static_cast<const std::uint8_t*>(
            std::memchr(base + i + anchor_idx, pat.bytes[anchor_idx], span - i));
        if (!found) break;
        i = static_cast<std::size_t>(found - base) - anchor_idx;
        bool match = true;
        for (std::size_t j = 0; j < n; ++j) {
            if (pat.mask[j] && base[i + j] != pat.bytes[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            if (!first_hit) first_hit = start + i;
            if (++hits >= 2) {
                OPENDOJO_LOG(
                    "players: WARNING pattern matched %d+ times "
                    "(first=0x%llX)",
                    hits, static_cast<unsigned long long>(first_hit));
                return 0;
            }
        }
    }
    return first_hit;
}

// Resolve a 4-byte RIP-relative displacement at `at` to an absolute address.
std::uintptr_t rip_relative(std::uintptr_t at) {
    auto disp = static_cast<std::int32_t>(memory::read_u32(at));
    return at + 4 + static_cast<std::uintptr_t>(static_cast<std::int64_t>(disp));
}

// ---------------------------------------------------------------------------
// One-time pattern resolution
// ---------------------------------------------------------------------------

struct Resolved {
    std::uintptr_t holder_global_slot = 0;  // *holder_global_slot = GlobalPlayerHolder
    std::uint32_t round_counter_offset = 0;
    bool attempted = false;
    bool ok = false;
};

std::once_flag g_resolve_once;
Resolved g_resolved;

void do_resolve() {
    g_resolved.attempted = true;

    std::uintptr_t text_start = 0;
    std::size_t text_size = 0;
    if (!get_text_range(text_start, text_size)) {
        OPENDOJO_LOG("players: couldn't locate Polaris .text section");
        return;
    }

    CompiledPattern pp;
    if (!compile_pattern(PAT_PLAYERS, pp)) {
        OPENDOJO_LOG("players: internal pattern parse error");
        return;
    }

    auto hit_players = scan(pp, text_start, text_size);
    if (!hit_players) {
        OPENDOJO_LOG("players: holder signature unavailable");
        return;
    }
    g_resolved.holder_global_slot = rip_relative(hit_players + 3);
    if (!memory::is_image_data(g_resolved.holder_global_slot, 8)) {
        OPENDOJO_LOG("players: rejected holder outside writable image data");
        return;
    }
    CompiledPattern counter;
    if (compile_pattern(PAT_ROUND_COUNTER, counter)) {
        const auto hit = scan(counter, text_start, text_size);
        if (hit)
            g_resolved.round_counter_offset = decode_round_counter(
                reinterpret_cast<const std::uint8_t*>(hit), counter.bytes.size());
    }
    OPENDOJO_LOG("players: round counter offset=0x%X%s", g_resolved.round_counter_offset,
                 g_resolved.round_counter_offset ? "" : " (automatic load gate unavailable)");
    g_resolved.ok = true;
    OPENDOJO_LOG("players: resolved holder@0x%llX",
                 static_cast<unsigned long long>(g_resolved.holder_global_slot));
}

bool ensure_resolved() {
    std::call_once(g_resolve_once, do_resolve);
    return g_resolved.ok;
}

// ---------------------------------------------------------------------------
// Character id -> name table (Tekken 8 launch roster + DLC, from info.txt).
// ---------------------------------------------------------------------------

const char* character_name_internal(std::uint32_t id) {
    switch (id) {
        case 0: return "paul";
        case 1: return "law";
        case 2: return "king";
        case 3: return "yoshimitsu";
        case 4: return "hwoarang";
        case 5: return "xiaoyu";
        case 6: return "jin";
        case 7: return "bryan";
        case 8: return "kazuya";
        case 9: return "steve";
        case 10: return "jack8";
        case 11: return "asuka";
        case 12: return "devil_jin";
        case 13: return "feng";
        case 14: return "lili";
        case 15: return "dragunov";
        case 16: return "leo";
        case 17: return "lars";
        case 18: return "alisa";
        case 19: return "claudio";
        case 20: return "shaheen";
        case 21: return "nina";
        case 22: return "lee";
        case 23: return "kuma";
        case 24: return "panda";
        case 25: return "zafina";
        case 26: return "leroy";
        case 27: return "jun";
        case 28: return "reina";
        case 29: return "azucena";
        case 30: return "victor";
        case 31: return "raven";
        case 32: return "azazel";
        case 33: return "eddy";
        case 34: return "lidia";
        case 35: return "heihachi";
        case 36: return "clive";
        case 37: return "anna";
        case 38: return "fahkumram";
        case 39: return "armor_king";
        case 40: return "miary_zo";
        case 41: return "kunimitsu";
        case 42: return "bob";
        case 43: return "roger_jr";
        case 44: return "yujiro";
        case 116: return "practice_dummy";
        case 117: return "angel_jin";
        case 118: return "true_devil_kazuya";
        case 119: return "jack7";
        case 120: return "soldier";
        case 121: return "story_devil_jin";
        case 122: return "tekken_monk";
        case 123: return "seiryu";
        default: return nullptr;
    }
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool resolve_all() {
    return ensure_resolved();
}

const char* character_name(std::uint32_t id) {
    return character_name_internal(id);
}

std::vector<std::string> character_roster() {
    // Iterate the playable-id space (0..99). character_name_internal
    // returns nullptr for unassigned slots and we skip those, so the
    // upper bound is a soft "more than there'll ever be playables"
    // — anything above falls into the NPC range (116+) and shouldn't
    // appear in user-facing filters.
    constexpr std::uint32_t kPlayableIdMax = 99;
    std::vector<std::string> out;
    out.reserve(48);
    for (std::uint32_t id = 0; id <= kPlayableIdMax; ++id) {
        if (auto n = character_name_internal(id)) {
            out.emplace_back(n);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

const char* side_to_string(Side s) {
    return s == Side::p1 ? "p1" : "p2";
}

bool parse_side(std::string_view s, Side& out) {
    if (s == "p1" || s == "P1" || s == "1p" || s == "1P") {
        out = Side::p1;
        return true;
    }
    if (s == "p2" || s == "P2" || s == "2p" || s == "2P") {
        out = Side::p2;
        return true;
    }
    return false;
}

static bool human_side(std::uint8_t& side) {
    const auto layout = signatures::movelist_layout();
    const auto gameplay = subsystems::lookup(subsystems::KEY_GAMEPLAY);
    return layout.human_side && gameplay &&
           memory::try_read_u8(gameplay + layout.human_side, &side) && side <= 1;
}

CpuInfo detect_cpu() {
    CpuInfo c;
    if (!ensure_resolved() || !signatures::player_layout().character) return c;

    // All chain reads are SEH-guarded — during an intra-practice
    // character swap the GlobalPlayerHolder chain transiently points
    // at freed memory. A plain dereference would AV in our render
    // thread and crash the game. Any failed read terminates the walk
    // and we return "not detected" for the tick.
    std::uint64_t holder = 0, p1 = 0, p2 = 0;
    if (!memory::try_read_u64(g_resolved.holder_global_slot, &holder) || !holder) return c;
    if (!memory::try_read_u64(holder + signatures::player_layout().p1, &p1) || !p1) return c;
    if (!memory::try_read_u64(holder + signatures::player_layout().p2, &p2) || !p2) return c;

    std::uint8_t human_player_id = 0;
    if (!human_side(human_player_id)) return c;
    const bool human_is_p1 = (human_player_id == 0);

    auto cpu_ptr = human_is_p1 ? p2 : p1;
    std::uint32_t cid = 0;
    if (!memory::try_read_u32(cpu_ptr + signatures::player_layout().character, &cid)) return c;

    c.cpu_side = human_is_p1 ? Side::p2 : Side::p1;
    c.character_id = cid;
    if (auto name = character_name_internal(c.character_id)) {
        c.character_name = name;
    } else {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "unknown_%u", c.character_id);
        c.character_name = buf;
    }
    c.detected = true;
    return c;
}

bool try_round_counter(std::uint32_t& frames) {
    frames = 0;
    if (!ensure_resolved() || !g_resolved.round_counter_offset || !signatures::player_layout().p1)
        return false;
    std::uint64_t holder = 0, p1 = 0;
    if (!memory::try_read_u64(g_resolved.holder_global_slot, &holder) || !holder) return false;
    if (!memory::try_read_u64(holder + signatures::player_layout().p1, &p1) || !p1) return false;
    // Native saturating round timer; exact input-readiness semantics are separate.
    if (!memory::try_read_u32(p1 + g_resolved.round_counter_offset, &frames)) return false;
    return frames <= 0xFFFF;
}

bool round_active() {
    std::uint32_t frames = 0;
    return try_round_counter(frames) && frames >= 1;
}

// Diagnostic for "the round-active gate never fired". Logs P1 and a window
// of u32s spanning g_resolved.round_counter_offset. Call it repeatedly: a live frame
// counter is the column that climbs between consecutive dumps. If the
// counter sits at a neighbouring offset, this is what makes it visible.
void log_round_probe() {
    std::uint64_t holder = 0, p1 = 0;
    if (!ensure_resolved() || !g_resolved.round_counter_offset || !signatures::player_layout().p1) {
        OPENDOJO_LOG("round_probe: player counter not resolved");
        return;
    }
    if (!memory::try_read_u64(g_resolved.holder_global_slot, &holder) || !holder) {
        OPENDOJO_LOG("round_probe: holder null");
        return;
    }
    if (!memory::try_read_u64(holder + signatures::player_layout().p1, &p1) || !p1) {
        OPENDOJO_LOG("round_probe: p1 null (holder=0x%llX)",
                     static_cast<unsigned long long>(holder));
        return;
    }
    // Wide enough to span the previous value (0x15C0), the current one
    // (0x15D0), and another shift of the same size in either direction if
    // the 2026-09-10 patch moved the struct again.
    constexpr std::ptrdiff_t WINDOW_BEFORE = 0x40;
    constexpr std::ptrdiff_t WINDOW_AFTER = 0x40;
    char line[768];
    int n = std::snprintf(
        line, sizeof(line),
        "round_probe: p1=0x%llX +0x%llX..+0x%llX =", static_cast<unsigned long long>(p1),
        static_cast<unsigned long long>(g_resolved.round_counter_offset - WINDOW_BEFORE),
        static_cast<unsigned long long>(g_resolved.round_counter_offset + WINDOW_AFTER));
    for (std::ptrdiff_t off = g_resolved.round_counter_offset - WINDOW_BEFORE;
         off <= g_resolved.round_counter_offset + WINDOW_AFTER && n > 0 &&
         n < static_cast<int>(sizeof(line));
         off += 4) {
        std::uint32_t v = 0;
        bool ok = memory::try_read_u32(static_cast<std::uintptr_t>(p1) + off, &v);
        // Mark the offset we currently believe in, so the log is readable.
        const char* tag = (off == g_resolved.round_counter_offset) ? "*" : "";
        n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %s%u%s", tag,
                           ok ? v : 0u, ok ? "" : "?");
    }
    OPENDOJO_LOG("%s", line);
}

std::uintptr_t holder_address() {
    if (!ensure_resolved()) return 0;
    std::uint64_t holder = 0;
    if (!memory::try_read_u64(g_resolved.holder_global_slot, &holder)) return 0;
    return holder;
}

std::uintptr_t cpu_player_address() {
    if (!ensure_resolved() || !signatures::player_layout().p1) return 0;
    std::uint64_t holder = 0, p1 = 0, p2 = 0;
    if (!memory::try_read_u64(g_resolved.holder_global_slot, &holder) || !holder) return 0;
    if (!memory::try_read_u64(holder + signatures::player_layout().p1, &p1) || !p1) return 0;
    if (!memory::try_read_u64(holder + signatures::player_layout().p2, &p2) || !p2) return 0;
    std::uint8_t human_player_id = 0;
    if (!human_side(human_player_id)) return 0;
    return (human_player_id == 0) ? p2 : p1;
}

// ---------------------------------------------------------------------------
// Steam persona via the steam_api64.dll flat shim.
//
// Tekken's HUD nameplate is the Steam persona of the launching account;
// we resolve the same value through the API the game itself uses. The
// flat C exports are version-stable in a way the C++ vtable methods
// aren't, so this keeps working across Steam SDK bumps.
//
// We deliberately do NOT call SteamAPI_Init() — Tekken has already done
// that. Calling it again from a foreign DLL risks tripping Steam's
// "another instance" guard. We only consume the already-live state.
// ---------------------------------------------------------------------------
std::string local_username() {
    HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
    if (!steam) {
        // Mod could be running in a non-Steam launch — log once at
        // info level, return empty silently.
        return {};
    }

    using SteamFriendsFn = void* (*)();
    using GetPersonaNameFn = const char* (*)(void* /*self*/);

    // The flat method shim. Stable across SDK versions because
    // ISteamFriends::GetPersonaName has lived at vtable slot 0
    // since SDK 1.0.
    auto get_persona_name = reinterpret_cast<GetPersonaNameFn>(
        GetProcAddress(steam, "SteamAPI_ISteamFriends_GetPersonaName"));
    if (!get_persona_name) {
        OPENDOJO_LOG(
            "players::local_username: steam_api64.dll missing "
            "SteamAPI_ISteamFriends_GetPersonaName — returning empty");
        return {};
    }

    // ISteamFriends* accessor. Modern Steamworks (SDK 1.50+) only
    // exports the *versioned* accessor (e.g. SteamAPI_SteamFriends_v017
    // for SDK 1.53, which Tekken ships); the unversioned `SteamFriends`
    // is an inline in the header, not a DLL export. We try the known
    // span so this survives a Steamworks SDK bump in a future Tekken
    // patch. All versioned pointers are vtable-compatible with the
    // GetPersonaName shim above — ISteamFriends evolves by appending
    // methods, not by reordering existing ones.
    static constexpr const char* kFriendsAccessors[] = {
        "SteamAPI_SteamFriends_v020",
        "SteamAPI_SteamFriends_v019",
        "SteamAPI_SteamFriends_v018",
        "SteamAPI_SteamFriends_v017",
    };
    void* self = nullptr;
    const char* used = nullptr;
    for (const char* accessor : kFriendsAccessors) {
        auto fn = reinterpret_cast<SteamFriendsFn>(GetProcAddress(steam, accessor));
        if (!fn) continue;
        if (auto p = fn()) {
            self = p;
            used = accessor;
            break;
        }
    }
    if (!self) {
        OPENDOJO_LOG(
            "players::local_username: no SteamAPI_SteamFriends_v0XX accessor "
            "returned a pointer — Steam SDK may have moved past v020");
        return {};
    }

    // The shim returns a pointer owned by Steam — copy it before
    // returning so we don't hold a pointer into Steam's address space
    // any longer than we have to.
    const char* name = get_persona_name(self);
    if (!name || !*name) {
        OPENDOJO_LOG("players::local_username: %s returned a pointer but persona was empty", used);
        return {};
    }
    OPENDOJO_LOG("players::local_username: resolved persona '%s' via %s", name, used);
    return std::string(name);
}

}  // namespace opendojo::players
