#include "signatures.hpp"

#include <windows.h>
#include <intrin.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <string_view>

#include "log.hpp"
#include "memory.hpp"
#include "native_scan.hpp"

namespace opendojo::signatures {

namespace {

bool read_pointer_guarded(std::uintptr_t address, std::uintptr_t& value) {
    __try {
        std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        value = 0;
        return false;
    }
}

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
// Short anchors identify candidates; decoded contracts verify the operations and
// relationships used by the mod. Displacements are read from actual instructions.
// ABI constants stay strict where the mod's data representation depends on them.
//
// This is a leaf function without unwind metadata. Match its complete body,
// including both key comparisons, traversal branches and both returns.
constexpr Pattern SUBSYSTEM_LOOKUP_SIG{
    "subsystem_lookup",
    "4C 8B 41 ?? 44 8B 0A 49 8B 88 ?? ?? ?? ?? 49 8B 90 ?? ?? ?? ?? "
    "49 23 C9 48 C1 E1 ?? 49 03 88 ?? ?? ?? ?? 48 8B 41 ?? 48 3B C2 "
    "74 1A 48 8B 09 44 3B 48 ?? 74 13 48 3B C1 74 0C 48 8B 40 ?? "
    "44 3B 48 ?? 75 F1 EB 02 33 C0 48 85 C0 48 0F 44 C2 48 3B C2 "
    "74 05 48 8B 40 ?? C3 33 C0 C3"};

SubsystemLayout subsystem_at(const native_scan::Function& fn) {
    const auto m =
        native_scan::unique(fn,
                            "4C 8B 41 ?? 44 8B 0A 49 8B 88 ?? ?? ?? ?? 49 8B 90 ?? ?? ?? ?? "
                            "49 23 C9 48 C1 E1 ?? 49 03 88 ?? ?? ?? ?? 48 8B 41 ?? 48 3B C2 "
                            "74 ?? 48 8B 09 44 3B 48 ?? 74 ?? 48 3B C1 74 ?? 48 8B 40 ?? "
                            "44 3B 48 ?? 75 ?? EB ?? 33 C0 48 85 C0 48 0F 44 C2 48 3B C2 "
                            "74 ?? 48 8B 40 ?? C3 33 C0 C3");
    if (!m || m[5].decoded.imm.imm8 < 4 || m[5].decoded.imm.imm8 > 8 ||
        m[9].relative() != m[19].address || m[12].relative() != m[20].address ||
        m[14].relative() != m[19].address || m[17].relative() != m[13].address ||
        m[18].relative() != m[20].address || m[23].relative() != m[26].address)
        return {};
    auto field = [&](std::size_t i) { return static_cast<std::uint32_t>(m[i].displacement()); };
    SubsystemLayout l{field(0), field(2),  field(3),  field(6), 1u << m[5].decoded.imm.imm8,
                      field(7), field(11), field(15), field(24)};
    // disp8 operands are signed; accept only positive, aligned fields.
    auto qword = [](std::uint32_t v) { return v && v <= 0x1000 && v % 8 == 0; };
    if (!qword(l.map) || l.map >= 128 || !qword(l.mask) || !qword(l.sentinel) ||
        !qword(l.buckets) || l.mask == l.sentinel || l.mask == l.buckets ||
        l.sentinel == l.buckets || !qword(l.bucket_last) || l.bucket_last >= 128 ||
        l.bucket_last + 8 > l.bucket_stride || !qword(l.previous) || l.previous >= 128 ||
        !qword(l.value) || l.value >= 128 || !l.key || l.key >= 128 || l.key % 4 ||
        l.key != field(16) || l.value == l.previous ||
        (l.key < l.previous + 8 && l.key + 4 > l.previous) ||
        (l.key < l.value + 8 && l.key + 4 > l.value))
        return {};
    return l;
}
SubsystemLayout decode_subsystem_layout(const std::uint8_t* code, std::size_t size) {
    const auto at = reinterpret_cast<std::uintptr_t>(code);
    return subsystem_at(native_scan::decode(at, at + size));
}

constexpr Pattern MOVE_WRAPPER_SIG{"move_wrapper", "0F BE 8B ?? ?? ?? ?? 48 8D 54 24 ?? 83 F1 01"};

constexpr Pattern SLOT_FLAGS_SIG{"slot_flags",
                                 "48 8D 0C CD ?? ?? ?? ?? 48 03 CD 89 9C CE ?? ?? ?? ??"};

constexpr Pattern SESSION_COUNTER_SIG{"session_counter",
                                      "48 8B CB E8 ?? ?? ?? ?? 8B 80 ?? ?? ?? ?? 39 06"};
constexpr Pattern SUBB_PAUSE_SIG{"pause_transition", "80 79 ?? 00 0F B6 DA 48 8B F9"};
constexpr Pattern SUBC_RESET_SIG{"side_record_reset", "48 0F BE C7 48 69 C8 ?? ?? ?? ??"};
constexpr Pattern RECORDING_STATE_SIG{"recording_state", "89 7B ?? 89 7B ?? 83 EF 01"};

constexpr Pattern CHARACTER_JACK_SIG{
    "character_jack",
    "80 B9 ?? ?? ?? ?? 05 74 10 8B 81 ?? ?? ?? ?? 83 F8 0A 74 08 83 F8 77 74 03 32 C0 C3 B0 01 C3"};
constexpr Pattern CHARACTER_ALISA_SIG{
    "character_alisa", "80 B9 ?? ?? ?? ?? 05 75 03 32 C0 C3 83 B9 ?? ?? ?? ?? 12 0F 94 C0 C3"};

constexpr Pattern REFLECTION_FIND_SIG{"reflection_field_iteration",
                                      "48 8B 48 ?? 48 89 4D ?? 48 3B CE"};
constexpr Pattern REFLECTION_INVOKE_SIG{"native_invoke",
                                        "4D 8B C6 48 8B D7 48 89 AF ?? ?? ?? ?? 48 8B CE FF 95"};
constexpr Pattern REFLECTION_STEP_SIG{"reflection_step",
                                      "41 8B 40 ?? 4D 8B C8 4C 8B D1 48 0F BA E0 08"};
// Identify the phase-3 call site, then validate its owning function and callee.
// Prologue, timeout bookkeeping and unrelated scheduler implementation are not anchors.
constexpr Pattern RUNTIME_UPDATE_SIG{"runtime_update_call",
                                     "41 B8 03 00 00 00 0F 28 CE E8 ?? ?? ?? ??"};
constexpr Pattern THREAD_ID_SIG{
    "thread_id", "FF 15 ?? ?? ?? ?? 49 8B CC 44 88 35 ?? ?? ?? ?? 89 05 ?? ?? ?? ??"};
constexpr Pattern ENGINE_FREE_SIG{
    "engine_free",
    "48 85 C9 74 2E 53 48 83 EC 20 48 8B D9 48 8B 0D ?? ?? ?? ?? 48 85 C9 75 0C E8 ?? ?? ?? ?? "
    "48 8B 0D ?? ?? ?? ?? 48 8B 01 48 8B D3 FF 50 ?? 48 83 C4 20 5B C3"};
constexpr Pattern WEAK_ASSIGN_SIG{
    "weak_assign",
    "40 53 48 83 EC 20 48 8B D9 48 85 D2 74 1A 8B 52 ?? 89 11 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? "
    "?? 89 43 04 48 83 C4 20 5B C3 33 C0 48 89 01 48 83 C4 20 5B C3"};
constexpr Pattern WEAK_VALID_SIG{
    "weak_valid",
    "44 8B 41 04 45 85 C0 74 42 8B 01 85 C0 78 3C 3B 05 ?? ?? ?? ?? 7D 34 8B C8 0F B7 C0 48 C1 "
    "E9 10 48 8D 14 40 48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8D 04 D1 48 85 C0 74 13 44 39 40 ?? "
    "75 0D 8B 40 ?? A9 00 00 20 30 75 03 B0 01 C3 32 C0 C3"};
constexpr Pattern WEAK_GET_SIG{
    "weak_get",
    "48 83 EC 08 44 8B 51 04 45 33 C0 4C 8B C9 45 85 D2 74 72 8B 01 85 C0 78 6C 44 8B 1D ?? ?? "
    "?? ?? 41 3B C3 7D 60 8B D0 48 89 1C 24 48 8B 1D ?? ?? ?? ?? 48 C1 EA 10 0F B7 C0 48 8D 0C "
    "40 48 8B 04 D3 48 8D 14 C8 48 85 D2 74 2F 44 39 52 ?? 75 29 41 8B 01 41 3B C3 7D 15 8B D0 "
    "0F B7 C0 48 C1 EA 10 48 8D 0C 40 48 8B 04 D3 4C 8D 04 C8 49 8B 00 48 8B 1C 24 48 83 C4 08 "
    "C3 48 8B 1C 24 49 8B C0 48 83 C4 08 C3 49 8B C0 48 83 C4 08 C3"};
constexpr Pattern PROCESS_EVENT_SIG{"process_event",
                                    "F7 41 ?? 00 00 00 60 0F 85 ?? ?? ?? ?? 45 33 F6 F7 82"};
constexpr Pattern NAME_DECODE_SIG{"name_decoder", "8B CB 0F B7 C3 C1 E9 10"};

constexpr Pattern SESSION_FINALIZE_SIG{"session_finalizer",
                                       "C7 00 01 00 00 00 B0 01 C6 87 ?? ?? ?? ?? 00"};

constexpr Pattern PRACTICE_DTOR_SIG{"practice_teardown",
                                    "48 8B CF E8 ?? ?? ?? ?? 33 FF 48 89 3D ?? ?? ?? ??"};

constexpr Pattern PLAYER_REFRESH_SIG{"player_refresh_fields", "33 C0 48 83 7B ?? 00 48 89 43 ??"};

constexpr Pattern POOL_INIT_SIG{"pool_allocator",
                                "33 D2 41 B8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 83 3D"};
constexpr Pattern POOL_CONSUMER_SIG{"recording_consumer",
                                    "48 69 C8 ?? ?? ?? ?? 48 03 0D ?? ?? ?? ?? 0F B7 11"};
constexpr Pattern POOL_COPY_SIG{"recording_copy",
                                "49 03 C8 48 69 C9 ?? ?? ?? ?? 49 8D 40 ?? 41 B8"};

// Follow the movelist wrapper's actual subsystem accessor. This identifies the
// context getter by its caller and table-lookup target, without neighboring stubs.
constexpr Pattern GET_CTX_SIG{"get_ctx", "48 8B 05 ?? ?? ?? ?? C3"};

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
std::atomic<bool> g_live_recordings_supported{false};
std::atomic<std::uintptr_t> g_session_finalize{0};
MovelistLayout g_movelist_layout;
std::atomic<bool> g_movelist_layout_ready{false};
std::atomic<std::uintptr_t> g_move_wrapper{0};
std::atomic<std::uintptr_t> g_slot_flags_getter{0};
std::atomic<std::uint32_t> g_slot_flags{0};
ReflectionLayout g_reflection_layout;
std::atomic<bool> g_reflection_ready{false};
PlayerLayout g_player_layout;
std::atomic<bool> g_player_layout_ready{false};
RecordingStateLayout g_recording_state_layout;
std::atomic<bool> g_recording_state_ready{false};
SubsystemLayout g_subsystem_layout;
std::atomic<bool> g_subsystem_layout_ready{false};
SessionLayout g_session_layout;
std::atomic<bool> g_session_layout_ready{false};
std::atomic<std::uintptr_t> g_practice_slot{0};
std::atomic<std::uintptr_t> g_pool1_ptr{0};
std::atomic<std::uintptr_t> g_pool2_ptr{0};
std::atomic<std::uintptr_t> g_ctx_ptr{0};
RuntimeLayout g_runtime_layout;
std::atomic<bool> g_runtime_ready{false};
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

}  // namespace

static native_scan::Image code_image() {
    const auto base = memory::polaris_base();
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto text = find_text_section();
    return {base, text.start, text.start + text.size, base + nt->OptionalHeader.SizeOfImage};
}

static bool imported_api(const native_scan::Instruction& call, std::uintptr_t expected) {
    const auto& i = call.decoded;
    if (i.opcode != 0xff || i.modrm_reg != 2 || !call.rip()) return false;
    std::uintptr_t target = 0;
    return code_image().contains(call.rip(), sizeof(target)) &&
           read_pointer_guarded(call.rip(), target) && target == expected;
}

static RuntimeLayout update_at(const native_scan::Function& wrapper) {
    using namespace native_scan;
    const auto image = code_image();
    const auto finish =
        unique(wrapper,
               "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? 41 B8 03 00 00 00 0F 28 CE E8 ?? ?? ?? ?? "
               "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? E8 ?? ?? ?? ?? C6 47 ?? 00 "
               "0F 28 74 24 ?? B0 01 48 83 C4 ?? 5F C3");
    const auto active = unique(wrapper, "80 79 ?? 00 48 8B F9");
    if (!finish || !active || finish[2].relative() != finish[6].address ||
        finish[8].relative() != finish[10].address ||
        active[0].displacement() != finish[10].displacement() ||
        !memory::is_image_data(finish[0].rip(), 8) || !memory::is_image_data(finish[6].rip(), 8) ||
        finish[0].rip() == finish[6].rip() || !image.code(finish[9].relative()))
        return {};
    const auto scheduler = function(image, finish[5].relative());
    if (!scheduler || scheduler.begin != finish[5].relative()) return {};
    // No second call to this scheduler may bypass the validated completion tail.
    for (const auto& instruction : wrapper.instructions)
        if (instruction.decoded.opcode == 0xe8 && instruction.relative() == scheduler.begin &&
            instruction.address != finish[5].address)
            return {};

    const auto release = unique(scheduler,
                                "48 8B B7 ?? ?? ?? ?? 48 8B 9F ?? ?? ?? ?? 48 3B DE 74 ?? "
                                "48 8B 0B 45 33 C0 48 8B 09 41 8D 50 01 FF 15 ?? ?? ?? ?? "
                                "48 83 C3 08 48 3B DE 75 ??");
    const auto wait = unique(scheduler,
                             "48 8B B7 ?? ?? ?? ?? 48 8B 9F ?? ?? ?? ?? 48 3B DE 74 ?? "
                             "48 8B 0B BA FF FF FF FF 48 8B 09 FF 15 ?? ?? ?? ?? "
                             "48 83 C3 08 48 3B DE 75 ??");
    if (!release || !wait || release[0].address >= wait[0].address ||
        release[11].relative() != release[4].address ||
        release[3].relative() != release[11].address + release[11].decoded.len ||
        wait[10].relative() != wait[4].address ||
        wait[3].relative() != wait[10].address + wait[10].decoded.len ||
        release[0].displacement() != release[1].displacement() + 8 ||
        wait[0].displacement() != wait[1].displacement() + 8 ||
        !imported_api(release[8], reinterpret_cast<std::uintptr_t>(&ReleaseSemaphore)) ||
        !imported_api(wait[7], reinterpret_cast<std::uintptr_t>(&WaitForSingleObject)))
        return {};
    bool fiber_between = false;
    for (const auto& instruction : scheduler.instructions) {
        if (instruction.address > release[11].address && instruction.address < wait[0].address &&
            imported_api(instruction, reinterpret_cast<std::uintptr_t>(&SwitchToFiber)))
            fiber_between = true;
        // A return inserted between dispatch and join invalidates the boundary.
        if (instruction.address > release[0].address && instruction.address < wait[10].address &&
            (instruction.decoded.opcode == 0xc3 || instruction.decoded.opcode == 0xc2))
            return {};
    }
    if (!fiber_between || !reaches_before_exit(scheduler, release[0].address, wait[0].address))
        return {};
    RuntimeLayout result;
    result.update = wrapper.begin;
    result.scheduler = scheduler.begin;
    result.scheduler_slot = finish[0].rip();
    return result;
}

static ReflectionLayout reflection_fields_at(const native_scan::Function& find) {
    using namespace native_scan;
    const auto image = code_image();
    const auto properties = unique(find, "48 8B 42 ?? 48 89 45 ?? EB ?? 48 89 5D ??");
    const auto children = unique(find, "48 85 FF 74 ?? 48 8B 47 ?? 48 89 45 ?? EB ??");
    const auto fields = unique(find,
                               "48 8B 48 ?? 48 89 4D ?? 48 3B CE 74 ?? 48 8B 40 ?? 48 8D 4D ?? "
                               "48 89 45 ?? E8 ?? ?? ?? ?? 48 8B 45 ?? 48 85 C0 75 ??");
    const auto objects =
        unique(find,
               "48 39 70 ?? 74 ?? 48 8B 40 ?? 48 8D 4D ?? 48 89 45 ?? E8 ?? ?? ?? ?? "
               "48 8B 45 ?? 48 85 C0 75 ??");
    if (!properties || !children || !fields || !objects ||
        properties[1].displacement() != fields[8].displacement() ||
        children[3].displacement() != objects[6].displacement() ||
        fields[10].relative() != fields[0].address || objects[8].relative() != objects[0].address ||
        !image.code(fields[7].relative()))
        return {};
    const auto next = function(image, objects[5].relative());
    if (!next || next.begin != objects[5].relative()) return {};
    const auto klass = unique(next, "48 8B 43 ?? F6 80 ?? ?? ?? ?? 01 74 ??");
    const auto link = unique(next, "48 8B 5B ?? 48 85 DB 75 ??");
    const auto nested = unique(next, "48 8B 1C C8 48 85 DB 74 ?? 48 8B 5B ?? EB ??");
    const auto parent = unique(next,
                               "48 8B 02 48 8B CA FF 90 ?? ?? ?? ?? 48 8B D0 48 85 C0 74 ?? "
                               "48 8B 58 ?? C7 47 ?? FF FF FF FF E9 ?? ?? ?? ??");
    if (!klass || !link || !nested || !parent ||
        link[0].displacement() != objects[2].displacement() ||
        nested[3].displacement() != children[2].displacement() ||
        parent[6].displacement() != children[2].displacement() ||
        link[2].relative() != klass[0].address)
        return {};
    // The two class checks in the finder must agree with its iterator's class field.
    unsigned class_checks = 0;
    for (const auto& instruction : find.instructions) {
        const auto& d = instruction.decoded;
        if (d.rex == 0x48 && d.opcode == 0x8b && d.modrm == 0x57) {
            if (instruction.displacement() != klass[0].displacement()) return {};
            ++class_checks;
        }
    }
    if (class_checks != 2) return {};
    ReflectionLayout result;
    result.object_class = klass[0].displacement();
    result.object_name = objects[0].displacement();
    result.children = children[2].displacement();
    result.child_properties = properties[0].displacement();
    result.field_next = link[0].displacement();
    result.ffield_name = fields[0].displacement();
    result.ffield_next = fields[4].displacement();
    result.super_getter_slot = parent[2].displacement();
    return result;
}

// Short anchors may match more than one function. Validate every bounded candidate;
// accept exactly one semantic match, never the first byte hit.
static std::vector<std::uintptr_t> candidates(const Pattern& pattern, const std::uint8_t* text,
                                              std::size_t size) {
    PatternByte bytes[256];
    const auto count = decode_pattern(pattern.notation, bytes, std::size(bytes));
    std::vector<std::uintptr_t> result;
    if (!count || count > size || bytes[0] == WILD) return result;
    for (std::size_t at = 0; at <= size - count; ++at) {
        const auto hit = static_cast<const std::uint8_t*>(
            std::memchr(text + at, bytes[0], size - count - at + 1));
        if (!hit) break;
        at = hit - text;
        if (match_at(hit, bytes, count)) {
            if (result.size() == 64) {
                OPENDOJO_LOG("signatures: %s anchor exceeded candidate limit", pattern.name);
                return {};
            }
            result.push_back(reinterpret_cast<std::uintptr_t>(hit));
        }
    }
    return result;
}
static RuntimeLayout discover_update(const std::uint8_t* text, std::size_t size) {
    RuntimeLayout result;
    for (const auto anchor : candidates(RUNTIME_UPDATE_SIG, text, size)) {
        const auto fn = native_scan::function(code_image(), anchor);
        if (!fn || fn.begin == result.update) continue;
        const auto layout = update_at(fn);
        if (!layout.update) continue;
        if (result.update) return {};
        result = layout;
    }
    return result;
}

static ReflectionLayout discover_reflection_fields(const std::uint8_t* text, std::size_t size) {
    ReflectionLayout result;
    std::uintptr_t owner = 0;
    for (const auto anchor : candidates(REFLECTION_FIND_SIG, text, size)) {
        const auto fn = native_scan::function(code_image(), anchor);
        if (!fn || fn.begin == owner) continue;
        const auto fields = reflection_fields_at(fn);
        if (!fields.object_class) continue;
        if (owner) return {};
        result = fields;
        owner = fn.begin;
    }
    return result;
}

// Identification anchors are separate from the contracts validated in each candidate.
// Actual operands/call targets come from decoded instructions, not function+byte offsets.
template <class T, class Validate>
static std::optional<T> discover(const Pattern& anchor, const std::uint8_t* text, std::size_t size,
                                 Validate validate) {
    std::optional<T> result;
    std::uintptr_t owner = 0;
    for (const auto hit : candidates(anchor, text, size)) {
        const auto fn = native_scan::function(code_image(), hit);
        if (!fn || fn.begin == owner) continue;
        auto found = validate(fn);
        if (!found) continue;
        if (result) {
            OPENDOJO_LOG("signatures: %s has multiple validated candidates", anchor.name);
            return {};
        }
        result = std::move(found);
        owner = fn.begin;
    }
    if (!result) OPENDOJO_LOG("signatures: %s instruction contract unavailable", anchor.name);
    return result;
}
struct PracticeDiscovery {
    std::uintptr_t address, slot;
};
static std::optional<PracticeDiscovery> practice_at(const native_scan::Function& fn) {
    using namespace native_scan;
    const auto identity = unique(fn, "48 8D 05 ?? ?? ?? ?? 48 89 01 48 8B 0D ?? ?? ?? ??");
    const auto clear = unique(fn, "48 8B CF E8 ?? ?? ?? ?? 33 FF 48 89 3D ?? ?? ?? ??");
    const auto destroy = unique(fn, "40 F6 C6 01 74 ?? BA ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ??");
    if (!identity || !clear || !destroy || !memory::is_image_data(clear[3].rip(), 8)) return {};
    std::uintptr_t deleting_destructor = 0;
    if (!code_image().contains(identity[0].rip(), 8) ||
        !read_pointer_guarded(identity[0].rip(), deleting_destructor) ||
        deleting_destructor != fn.begin)
        return {};
    return PracticeDiscovery{fn.begin, clear[3].rip()};
}
struct PoolDiscovery {
    std::uintptr_t address, first, second;
    std::uint32_t state, first_size, second_size;
};
static std::optional<PoolDiscovery> pool_at(const native_scan::Function& fn) {
    using namespace native_scan;
    const auto first =
        unique(fn,
               "33 C0 48 89 41 ?? 48 39 05 ?? ?? ?? ?? 75 ?? B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
               "48 8B 0D ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 48 85 C9 74 ?? E8 ?? ?? ?? ?? "
               "48 8B 05 ?? ?? ?? ?? 33 D2 41 B8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ??");
    const auto second =
        unique(fn,
               "48 83 3D ?? ?? ?? ?? 00 75 ?? B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
               "48 8B 0D ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 48 85 C9 74 ?? E8 ?? ?? ?? ?? "
               "48 8B 05 ?? ?? ?? ?? 33 D2 41 B8 ?? ?? ?? ?? 48 8B C8");
    const auto finish = unique(fn, "48 83 C4 ?? E9 ?? ?? ?? ?? 48 83 C4 ?? C3");
    if (!first || !second || !finish || first[3].relative() != second[0].address ||
        first[9].relative() != first[12].address || second[7].relative() != second[10].address ||
        second[1].relative() != finish[2].address || first[5].relative() != second[3].relative() ||
        first[10].relative() != second[8].relative() ||
        first[15].relative() != finish[1].relative())
        return {};
    const auto p1 = first[2].rip(), p2 = second[0].rip();
    if (!p1 || !p2 || p1 == p2 || !memory::is_image_data(p1, 8) || !memory::is_image_data(p2, 8))
        return {};
    for (auto i : {6, 7, 11})
        if (first[i].rip() != p1) return {};
    for (auto i : {4, 5, 9})
        if (second[i].rip() != p2) return {};
    if (first[4].decoded.imm.imm32 != first[13].decoded.imm.imm32 ||
        second[2].decoded.imm.imm32 != second[11].decoded.imm.imm32)
        return {};
    return PoolDiscovery{fn.begin,
                         p1,
                         p2,
                         static_cast<std::uint32_t>(first[1].displacement()),
                         first[4].decoded.imm.imm32,
                         second[2].decoded.imm.imm32};
}
struct FinalizeDiscovery {
    std::uintptr_t address;
    SessionLayout layout;
    std::uint32_t native_flag;
};
static std::optional<FinalizeDiscovery> finalize_at(const native_scan::Function& fn) {
    using namespace native_scan;
    const auto gate = unique(fn, "F7 01 ?? ?? ?? ?? 48 8B F9 75 ?? 32 C0");
    const auto pending = unique(fn,
                                "C7 81 ?? ?? ?? ?? 00 00 00 00 E8 ?? ?? ?? ?? "
                                "48 8D 15 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B D8");
    const auto side = unique(fn, "0F BE 88 ?? ?? ?? ?? 48 8B 03 83 F1 01");
    const auto flags = unique(fn,
                              "48 8B 04 CA 48 85 C0 74 ?? 48 05 ?? ?? ?? ?? EB ?? B8 ?? ?? ?? ?? "
                              "C7 00 01 00 00 00 B0 01 C6 87 ?? ?? ?? ?? 00");
    const auto bounds = unique(fn,
                               "83 F9 FF 74 ?? 48 8B 50 08 48 8B 40 10 48 2B C2 "
                               "48 63 C9 48 C1 F8 03 48 3B C1 76 ??");
    if (!gate || !pending || !side || !flags || !bounds ||
        flags[2].relative() != flags[5].address || flags[4].relative() != flags[6].address ||
        bounds[1].relative() != flags[5].address ||
        bounds[8].address + bounds[8].decoded.len != flags[0].address)
        return {};
    SessionLayout layout{static_cast<std::uint32_t>(pending[0].displacement()),
                         flags[5].decoded.imm.imm32,
                         static_cast<std::uint32_t>(flags[8].displacement()),
                         gate[0].decoded.imm.imm32};
    const auto native = flags[3].decoded.imm.imm32;
    if (!layout.active_mask || (layout.active_mask & (layout.active_mask - 1)) ||
        native < layout.player_flag || native - layout.player_flag < 8 ||
        native - layout.player_flag > 0x1000 || (native - layout.player_flag) % 8 ||
        layout.pending < 4 || layout.pending > 0x1000 || layout.pending % 4 ||
        layout.player_flag < 0x100 || layout.player_flag > 0x10000 || layout.player_flag % 4 ||
        layout.finished < 4 || layout.finished > 0x1000 ||
        (layout.finished >= layout.pending && layout.finished < layout.pending + 4))
        return {};
    return FinalizeDiscovery{fn.begin, layout, native};
}
struct RefreshDiscovery {
    std::uintptr_t address;
    std::uint32_t p1, p2, bias;
};
static std::optional<RefreshDiscovery> refresh_at(const native_scan::Function& fn) {
    using namespace native_scan;
    const auto first = unique(fn, "E8 ?? ?? ?? ?? EB ?? 33 C0 48 89 43 ??");
    const auto second = unique(fn, "E8 ?? ?? ?? ?? EB ?? 33 C0 48 83 7B ?? 00 48 89 43 ??");
    const auto zero = unique(fn, "C7 05 ?? ?? ?? ?? 00 00 00 00");
    const auto one = unique(fn, "C7 05 ?? ?? ?? ?? 01 00 00 00");
    if (!first || !second || !zero || !one || first[0].address >= second[0].address ||
        first[0].relative() != second[0].relative() || first[1].relative() != first[3].address ||
        second[1].relative() != second[3].address ||
        first[3].displacement() != second[3].displacement())
        return {};
    unsigned p1_loads = 0, p2_loads = 0;
    for (const auto& i : fn.instructions) {
        if (i.decoded.opcode != 0x8b || i.decoded.modrm != 0x05 || i.decoded.rex_w) continue;
        if (i.rip() == zero[0].rip() && i.address < first[0].address) ++p1_loads;
        if (i.rip() == one[0].rip() && i.address > first[3].address &&
            i.address < second[0].address)
            ++p2_loads;
    }
    if (p1_loads != 1 || p2_loads != 1 || zero[0].rip() == one[0].rip()) return {};
    const auto getter = function(code_image(), first[0].relative());
    if (!getter || getter.begin != first[0].relative()) return {};
    const auto player = unique(getter,
                               "49 8B 50 ?? 48 8B C8 49 8B 40 ?? 48 2B C2 48 C1 F8 03 "
                               "48 3B C1 76 ?? 48 8B 04 CA 48 85 C0 74 ?? 48 05 ?? ?? ?? ??");
    if (!player || player[2].displacement() != player[0].displacement() + 8) return {};
    const auto p1 = first[3].displacement(), p2 = second[4].displacement();
    const auto bias = player[10].decoded.imm.imm32;
    if (p1 < 8 || p1 >= 128 || p1 % 8 || p2 < 8 || p2 >= 128 || p2 % 8 || p1 == p2 || bias < 8 ||
        bias > 0x1000 || bias % 8)
        return {};
    return RefreshDiscovery{fn.begin, static_cast<std::uint32_t>(p1),
                            static_cast<std::uint32_t>(p2), bias};
}

struct MoveDiscovery {
    std::uintptr_t address, accessor;
    MovelistLayout layout;
};
static std::optional<MoveDiscovery> move_at(const native_scan::Function& fn) {
    using namespace native_scan;
    const auto wrapper =
        unique(fn,
               "E8 ?? ?? ?? ?? 0F BE 8B ?? ?? ?? ?? 48 8D 54 24 ?? 83 F1 01 89 4C 24 ?? "
               "48 8B C8 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 44 8B C7 33 D2 48 8B C8 E8 ?? ?? ?? ??");
    const auto none = unique(fn, "B8 FF FF FF FF");
    if (!wrapper || !none || wrapper[2].displacement() != wrapper[4].displacement() ||
        !reaches_before_exit(fn, wrapper[8].relative(), none[0].address))
        return {};
    const auto element_fn = helper(code_image(), wrapper[6].relative());
    const auto field_fn = helper(code_image(), wrapper[12].relative());
    const auto element = unique(
        element_fn,
        "48 63 02 83 F8 FF 74 ?? 4C 8B 01 4C 8B C8 48 8B 49 ?? 48 B8 ?? ?? ?? ?? ?? ?? ?? ?? "
        "49 2B C8 48 F7 E9 48 C1 FA ?? 48 8B C2 48 C1 E8 3F 48 03 D0 4C 3B CA 73 ?? "
        "49 69 C1 ?? ?? ?? ?? 49 03 C0 C3 33 C0 C3");
    const auto field =
        unique(field_fn,
               "41 83 F8 07 77 ?? 49 63 C0 48 63 D2 48 8D 14 D0 8B 44 91 ?? C3 B8 FF FF FF FF C3");
    if (!element || !field || element[2].relative() != element[18].address ||
        element[14].relative() != element[18].address || field[1].relative() != field[7].address)
        return {};
    MovelistLayout l{static_cast<std::uint32_t>(wrapper[1].displacement()),
                     element[15].decoded.imm.imm32,
                     static_cast<std::uint32_t>(field[5].displacement()),
                     static_cast<std::uint32_t>(element[5].displacement())};
    const auto shift = element[9].decoded.imm.imm8;
    const auto magic = element[6].decoded.imm.imm64;
    if (!l.human_side || l.human_side > 0x10000 || l.element_stride < 64 ||
        l.element_stride > 0x10000 || l.element_stride % 8 || l.vector_end < 8 ||
        l.vector_end >= 128 || l.vector_end % 8 || l.move_ids < 4 || l.move_ids >= 128 ||
        l.move_ids % 4 || l.move_ids + 32 > l.element_stride || shift > 30 || magic >> 63)
        return {};
    std::uint64_t high = 0;
    const auto low = _umul128(magic, l.element_stride, &high);
    if (high != (1ull << shift) || low >= l.element_stride) return {};
    return MoveDiscovery{fn.begin, wrapper[0].relative(), l};
}
static std::optional<std::uint32_t> slot_flags_at(const native_scan::Function& fn) {
    using namespace native_scan;
    const auto limits = unique(fn,
                               "48 63 EA 41 8B C1 41 8B D8 48 8B F1 83 FD 07 0F 87 ?? ?? ?? ?? "
                               "83 F8 02 0F 8D ?? ?? ?? ?? 85 C0 79 ?? E8 ?? ?? ?? ??");
    const auto store = unique(fn,
                              "48 63 C8 48 8D 0C CD ?? ?? ?? ?? 48 03 CD 89 9C CE ?? ?? ?? ?? "
                              "83 FB 02 75 ?? 44 8B CD 41 B0 01 33 D2 48 8B CE");
    if (!limits || !store || limits[5].relative() != limits[7].relative() ||
        limits[9].relative() != store[0].address || store[1].displacement())
        return {};
    const auto field = store[3].displacement();
    if (field < 4 || field > 0x10000 || field % 4) return {};
    return static_cast<std::uint32_t>(field);
}

static std::optional<std::uint32_t> counter_at(const native_scan::Function& fn) {
    using namespace native_scan;
    const auto first = unique(fn,
                              "41 8B F8 48 8B F2 48 8B D9 E8 ?? ?? ?? ?? 8B 80 ?? ?? ?? ?? "
                              "39 06 75 ?? 89 7B ??");
    const auto second =
        unique(fn, "48 8B CB E8 ?? ?? ?? ?? 8B 80 ?? ?? ?? ?? 39 06 75 ?? 89 7B ??");
    if (!first || !second || first[6].relative() != second[0].address ||
        first[4].displacement() != second[2].displacement() ||
        first[7].displacement() == second[5].displacement() ||
        second[4].relative() != second[5].address + second[5].decoded.len)
        return {};
    const auto field = second[5].displacement();
    if (field < 4 || field >= 128 || field % 4) return {};
    return static_cast<std::uint32_t>(field);
}
static std::optional<std::uint32_t> pause_at(const native_scan::Function& fn) {
    using namespace native_scan;
    const auto gate = unique(fn, "80 79 ?? 00 0F B6 DA 48 8B F9 75 ?? 84 D2 74 ??");
    const auto enabled = unique(fn, "B2 01 48 8B C8 E8 ?? ?? ?? ?? 88 5F ??");
    const auto direct = unique(fn, "88 59 ??");
    if (!gate || !enabled || !direct || gate[3].relative() != direct[0].address ||
        gate[5].relative() != direct[0].address ||
        gate[0].displacement() != enabled[3].displacement() ||
        gate[0].displacement() != direct[0].displacement())
        return {};
    const auto field = gate[0].displacement();
    if (field <= 0 || field >= 128) return {};
    return static_cast<std::uint32_t>(field);
}
static std::optional<std::uint32_t> side_record_at(const native_scan::Function& fn) {
    using namespace native_scan;
    const auto zero = unique(fn, "33 F6");
    const auto loop = unique(
        fn,
        "48 0F BE C7 48 69 C8 ?? ?? ?? ?? "
        "48 89 B4 19 ?? ?? ?? ?? 48 89 B4 19 ?? ?? ?? ?? 48 89 B4 19 ?? ?? ?? ?? "
        "89 B4 19 ?? ?? ?? ?? 48 81 C1 ?? ?? ?? ?? 48 03 CB E8 ?? ?? ?? ?? FF C7 83 FF 02 7C ??");
    if (!zero || !loop || zero[0].address >= loop[0].address ||
        loop[11].relative() != loop[0].address)
        return {};
    const auto item_fn = helper(code_image(), loop[8].relative());
    const auto item = unique(item_fn,
                             "33 D2 48 C7 41 ?? FF FF FF FF 33 C0 48 89 11 "
                             "48 89 51 ?? 0F 57 C0 48 89 51 ??");
    if (!item) return {};
    const auto stride = loop[1].decoded.imm.imm32, base = loop[6].decoded.imm.imm32;
    const auto member = item[1].displacement();
    if (base < 4 || base >= 0x10000 || stride < 64 || stride >= 0x10000 || stride % 8 ||
        member < 0 || member >= 128 || member + 8u > stride || (base + stride + member) % 4)
        return {};
    return base + stride + member;
}
static std::optional<std::uint32_t> recording_state_at(const native_scan::Function& fn) {
    using namespace native_scan;
    const auto state = unique(fn,
                              "8B FA 48 8B D9 E8 ?? ?? ?? ?? 84 C0 74 ?? "
                              "89 7B ?? 89 7B ?? 83 EF 01 74 ?? 83 FF 01 75 ??");
    if (!state || state[4].relative() != state[10].relative() ||
        state[6].displacement() != state[5].displacement() + 4)
        return {};
    const auto field = state[6].displacement();
    if (field < 4 || field >= 128 || field % 4) return {};
    return static_cast<std::uint32_t>(field);
}

struct RecordingFormatDiscovery {
    std::uintptr_t pool;
};
static std::optional<RecordingFormatDiscovery> consumer_at(const native_scan::Function& fn) {
    using namespace native_scan;
    const auto head = unique(fn,
                             "48 63 47 ?? 48 69 C8 22 1C 00 00 48 03 0D ?? ?? ?? ?? "
                             "0F B7 11 48 8D 59 02 EB ??");
    const auto duration = unique(fn,
                                 "42 0F B6 4C B3 03 3B 4C EF ?? 77 ?? 44 89 7C EF ?? "
                                 "FF 44 EF ?? 44 8B 74 EF ?? 0F B7 C2 44 3B F0 72 ??");
    const auto keys = unique(fn,
                             "49 63 C6 48 8D 0C 83 0F B7 04 83 25 DF FF 00 00 41 89 45 ?? "
                             "0F B6 01 C0 E8 05 24 01 41 88 04 24");
    if (!head || !duration || !keys || head[1].decoded.imm.imm32 != 7202 ||
        head[0].address >= duration[0].address || duration[0].address >= keys[0].address ||
        duration[1].displacement() != duration[3].displacement() ||
        duration[4].displacement() != duration[5].displacement() ||
        !memory::is_image_data(head[2].rip(), 8))
        return {};
    return RecordingFormatDiscovery{head[2].rip()};
}
static std::optional<RecordingFormatDiscovery> copy_at(const native_scan::Function& fn) {
    using namespace native_scan;
    const auto copy =
        unique(fn,
               "48 63 4F ?? 4C 63 C3 49 03 C8 48 69 C9 ?? ?? ?? ?? "
               "49 8D 40 ?? 41 B8 ?? ?? ?? ?? 48 03 0D ?? ?? ?? ?? 48 69 D0 ?? ?? ?? ?? "
               "48 03 15 ?? ?? ?? ?? E8 ?? ?? ?? ??");
    if (!copy || copy[3].decoded.imm.imm32 != 7202 || copy[5].decoded.imm.imm32 != 7202 ||
        copy[7].decoded.imm.imm32 != 7202 || copy[6].rip() != copy[8].rip() ||
        !memory::is_image_data(copy[6].rip(), 8))
        return {};
    return RecordingFormatDiscovery{copy[6].rip()};
}
static std::optional<std::uint32_t> invoke_at(const native_scan::Function& fn) {
    using namespace native_scan;
    const auto arguments = unique(fn, "48 8B 59 ?? 4D 8B F1 49 8B F8 48 8B F2 48 8B E9");
    const auto invoke = unique(fn,
                               "48 8B 9F ?? ?? ?? ?? 4D 8B C6 48 8B D7 48 89 AF ?? ?? ?? ?? "
                               "48 8B CE FF 95 ?? ?? ?? ??");
    const auto restore = unique(fn, "48 89 9F ?? ?? ?? ??");
    if (!arguments || !invoke || !restore || arguments[0].address >= invoke[0].address ||
        restore[0].address <= invoke[5].address ||
        invoke[0].displacement() != invoke[3].displacement() ||
        invoke[0].displacement() != restore[0].displacement())
        return {};
    const auto field = invoke[5].displacement();
    if (field < 8 || field > 0x1000 || field % 8) return {};
    return static_cast<std::uint32_t>(field);
}
struct EventDiscovery {
    std::uintptr_t address;
    std::uint32_t flags, parms;
};
static std::optional<EventDiscovery> event_at(const native_scan::Function& fn) {
    using namespace native_scan;
    const auto gate =
        unique(fn,
               "4D 8B E0 48 8B F2 4C 8B F9 48 85 C9 0F 84 ?? ?? ?? ?? "
               "F7 41 ?? 00 00 00 60 0F 85 ?? ?? ?? ?? 45 33 F6 F7 82 ?? ?? ?? ?? 00 04 00 00");
    const auto parms = unique(fn,
                              "0F B7 8E ?? ?? ?? ?? 33 D2 44 2B C1 48 03 CF 4D 63 C0 "
                              "E8 ?? ?? ?? ?? 44 0F B7 86 ?? ?? ?? ??");
    if (!gate || !parms || gate[4].relative() != gate[6].relative() ||
        parms[0].displacement() != parms[6].displacement())
        return {};
    const auto flags = gate[5].displacement(), size = parms[0].displacement();
    if (flags < 4 || flags >= 128 || flags % 4 || size < 8 || size > 0x1000 || size % 2) return {};
    return EventDiscovery{fn.begin, static_cast<std::uint32_t>(flags),
                          static_cast<std::uint32_t>(size)};
}
struct NameDiscovery {
    std::uintptr_t pool;
    std::uint32_t blocks, text;
};
static std::optional<NameDiscovery> name_at(const native_scan::Function& fn) {
    using namespace native_scan;
    const auto init = unique(fn,
                             "80 3D ?? ?? ?? ?? 00 48 8B FA 8B 19 74 ?? "
                             "4C 8D 05 ?? ?? ?? ?? EB ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 4C 8B "
                             "C0 C6 05 ?? ?? ?? ?? 01");
    const auto decode =
        unique(fn,
               "8B CB 0F B7 C3 C1 E9 10 89 4C 24 ?? 89 44 24 ?? 48 8B 44 24 ?? "
               "48 C1 E8 20 8D 1C 00 49 03 5C C8 ?? 48 8B CF 44 0F B7 03 48 8D 53 ?? "
               "49 C1 E8 06 E8 ?? ?? ?? ?? 0F B7 03");
    const auto end = unique(fn, "48 C1 E8 06 C6 04 38 00");
    if (!init || !decode || !end || init[0].rip() != init[9].rip() ||
        init[4].rip() != init[6].rip() || init[3].relative() != init[6].address ||
        init[5].relative() != decode[0].address ||
        decode[3].displacement() != decode[5].displacement() ||
        decode[4].displacement() != decode[3].displacement() + 4 ||
        !memory::is_image_data(init[4].rip(), 8))
        return {};
    const auto blocks = decode[8].displacement(), text = decode[11].displacement();
    if (blocks < 8 || blocks >= 128 || blocks % 8 || text < 2 || text >= 128 || text % 2) return {};
    return NameDiscovery{init[4].rip(), static_cast<std::uint32_t>(blocks),
                         static_cast<std::uint32_t>(text)};
}

// Small identity/allocator helpers have no unrelated gameplay work to omit.
// Preserve their operation graph while allowing NOPs and branch widening.
static native_scan::Match graph_contract(const native_scan::Function& fn, const Pattern& pattern) {
    PatternByte expected[256];
    const auto size = decode_pattern(pattern.notation, expected, std::size(expected));
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t i = 0; i < size; ++i)
        bytes[i] = expected[i] < 0 ? 0 : static_cast<std::uint8_t>(expected[i]);
    const auto base = reinterpret_cast<std::uintptr_t>(bytes.data());
    const auto shape = native_scan::decode(base, base + size);
    if (!shape) return {};
    std::vector<std::pair<std::size_t, std::size_t>> edges;
    for (std::size_t i = 0; i < shape.instructions.size(); ++i) {
        const auto& instruction = shape.instructions[i];
        const auto& d = instruction.decoded;
        if (!(d.flags & F_RELATIVE) || d.opcode == 0xE8) continue;
        const auto immediate_size = d.flags & F_IMM32 ? 4u : 1u;
        const auto offset = instruction.address - base + d.len - immediate_size;
        if (expected[offset] < 0) continue;
        const auto target = instruction.relative();
        const auto found =
            std::find_if(shape.instructions.begin(), shape.instructions.end(),
                         [&](const auto& candidate) { return candidate.address == target; });
        if (found == shape.instructions.end()) return {};
        edges.push_back({i, static_cast<std::size_t>(found - shape.instructions.begin())});
        for (std::size_t j = 0; j < immediate_size; ++j)
            expected[offset + j] = WILD;
    }
    constexpr char hex[] = "0123456789ABCDEF";
    std::string notation;
    for (std::size_t i = 0; i < size; ++i) {
        if (expected[i] < 0)
            notation += "?? ";
        else {
            notation += hex[expected[i] >> 4];
            notation += hex[expected[i] & 15];
            notation += ' ';
        }
    }
    auto match = native_scan::unique(fn, notation);
    if (!match) return {};
    for (const auto& [from, to] : edges)
        if (match[from].relative() != match[to].address) return {};
    return match;
}

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
    const auto practice =
        discover<PracticeDiscovery>(PRACTICE_DTOR_SIG, text_bytes, text.size, practice_at);
    const auto refresh =
        discover<RefreshDiscovery>(PLAYER_REFRESH_SIG, text_bytes, text.size, refresh_at);
    const auto pool = discover<PoolDiscovery>(POOL_INIT_SIG, text_bytes, text.size, pool_at);
    const auto finalize =
        discover<FinalizeDiscovery>(SESSION_FINALIZE_SIG, text_bytes, text.size, finalize_at);
    if (practice) {
        g_practice_dtor.store(practice->address);
        g_practice_slot.store(practice->slot);
    } else
        ok = false;
    if (refresh)
        g_player_refresh.store(refresh->address);
    else
        ok = false;
    if (pool) {
        g_pool_init.store(pool->address);
        g_pool1_ptr.store(pool->first);
        g_pool2_ptr.store(pool->second);
    } else
        ok = false;
    if (finalize) {
        g_session_finalize.store(finalize->address);
        g_session_layout = finalize->layout;
        g_session_layout_ready.store(true, std::memory_order_release);
    }
    if (!g_session_layout_ready.load()) {
        OPENDOJO_LOG("signatures: session finalization disabled (layout unavailable)");
        ok = false;
    } else {
        OPENDOJO_LOG("signatures: session pending=0x%X player_flag=0x%X finished=0x%X",
                     g_session_layout.pending, g_session_layout.player_flag,
                     g_session_layout.finished);
    }

    {
        int hits = 0;
        for (auto address :
             candidates({"subsystem_lookup", "4C 8B 41 ?? 44 8B 0A 49 8B 88 ?? ?? ?? ??"},
                        text_bytes, text.size)) {
            auto layout = subsystem_at(native_scan::helper(code_image(), address));
            if (!layout.bucket_stride) continue;
            ++hits;
            g_subsystem_layout = layout;
        }
        g_subsystem_layout_ready.store(hits == 1, std::memory_order_release);
        if (!g_subsystem_layout_ready.load()) {
            OPENDOJO_LOG("signatures: subsystem layout unavailable (%d matches); lookup disabled",
                         hits);
            ok = false;
        } else {
            OPENDOJO_LOG(
                "signatures: subsystem map=0x%X mask=0x%X sentinel=0x%X buckets=0x%X stride=0x%X",
                g_subsystem_layout.map, g_subsystem_layout.mask, g_subsystem_layout.sentinel,
                g_subsystem_layout.buckets, g_subsystem_layout.bucket_stride);
        }
    }

    const auto moves = discover<MoveDiscovery>(MOVE_WRAPPER_SIG, text_bytes, text.size, move_at);
    if (moves) {
        g_move_wrapper.store(moves->address);
        g_movelist_layout = moves->layout;
        g_movelist_layout_ready.store(true, std::memory_order_release);
    }
    if (!g_movelist_layout_ready.load()) {
        OPENDOJO_LOG("signatures: movelist layout unavailable; movelist access disabled");
        ok = false;
    } else {
        OPENDOJO_LOG("signatures: movelist human=0x%X stride=0x%X moves=0x%X end=0x%X",
                     g_movelist_layout.human_side, g_movelist_layout.element_stride,
                     g_movelist_layout.move_ids, g_movelist_layout.vector_end);
    }

    const auto flags =
        discover<std::uint32_t>(SLOT_FLAGS_SIG, text_bytes, text.size, slot_flags_at);
    if (flags) g_slot_flags.store(*flags, std::memory_order_release);
    if (!g_slot_flags.load()) {
        OPENDOJO_LOG("signatures: gameplay slot flags unavailable; slot access disabled");
        ok = false;
    } else
        OPENDOJO_LOG("signatures: gameplay slot flags=0x%X", g_slot_flags.load());

    {
        const auto counter =
            discover<std::uint32_t>(SESSION_COUNTER_SIG, text_bytes, text.size, counter_at);
        const auto pause = discover<std::uint32_t>(SUBB_PAUSE_SIG, text_bytes, text.size, pause_at);
        const auto side =
            discover<std::uint32_t>(SUBC_RESET_SIG, text_bytes, text.size, side_record_at);
        const auto state =
            discover<std::uint32_t>(RECORDING_STATE_SIG, text_bytes, text.size, recording_state_at);
        if (counter && pause && side && state) {
            g_recording_state_layout = {*counter, *pause, *side, *state};
            g_recording_state_ready.store(true, std::memory_order_release);
        }
        if (!g_recording_state_ready.load()) {
            OPENDOJO_LOG(
                "signatures: recording-state fields unavailable; session mutations disabled");
            ok = false;
        } else
            OPENDOJO_LOG(
                "signatures: recording counter=0x%X pause=0x%X side_record=0x%X state=0x%X",
                g_recording_state_layout.counter, g_recording_state_layout.pause,
                g_recording_state_layout.side_record, g_recording_state_layout.recording_state);
    }

    {
        auto character = [&](const Pattern& short_anchor, const Pattern& near_anchor,
                             const Pattern& contract) {
            native_scan::Match result;
            for (const auto& anchor : {short_anchor, near_anchor}) {
                for (auto address : candidates(anchor, text_bytes, text.size)) {
                    auto match =
                        graph_contract(native_scan::helper(code_image(), address), contract);
                    if (!match) continue;
                    if (result) return native_scan::Match{};
                    result = std::move(match);
                }
            }
            return result;
        };
        const auto jack = character({"character_jack", "80 B9 ?? ?? ?? ?? 05 74"},
                                    {"character_jack_near", "80 B9 ?? ?? ?? ?? 05 0F 84"},
                                    CHARACTER_JACK_SIG);
        const auto alisa = character({"character_alisa", "80 B9 ?? ?? ?? ?? 05 75"},
                                     {"character_alisa_near", "80 B9 ?? ?? ?? ?? 05 0F 85"},
                                     CHARACTER_ALISA_SIG);
        if (jack && alisa && refresh) {
            PlayerLayout l{refresh->p1, refresh->p2,
                           static_cast<std::uint32_t>(jack[2].displacement()), refresh->bias};
            if (l.character >= 4 && l.character <= 0x10000 && l.character % 4 == 0 &&
                l.character == static_cast<std::uint32_t>(alisa[4].displacement()) &&
                jack[0].displacement() == alisa[0].displacement()) {
                g_player_layout = l;
                g_player_layout_ready.store(true, std::memory_order_release);
            }
        }
        if (!g_player_layout_ready.load()) {
            OPENDOJO_LOG("signatures: player fields unavailable; player access disabled");
            ok = false;
        } else
            OPENDOJO_LOG("signatures: player p1=0x%X p2=0x%X character=0x%X native_bias=0x%X",
                         g_player_layout.p1, g_player_layout.p2, g_player_layout.character,
                         g_player_layout.native_bias);
        if (g_session_layout_ready.load()) {
            const auto native_flag = finalize ? finalize->native_flag : 0;
            if (!g_player_layout_ready.load() ||
                native_flag - g_session_layout.player_flag != g_player_layout.native_bias) {
                g_session_layout_ready.store(false, std::memory_order_release);
                OPENDOJO_LOG(
                    "signatures: session/player pointer relationship disagrees; finalization "
                    "disabled");
                ok = false;
            }
        }
    }

    {
        const auto fields = discover_reflection_fields(text_bytes, text.size);
        const auto invoke =
            discover<std::uint32_t>(REFLECTION_INVOKE_SIG, text_bytes, text.size, invoke_at);
        const auto event =
            discover<EventDiscovery>(PROCESS_EVENT_SIG, text_bytes, text.size, event_at);
        const auto names = discover<NameDiscovery>(NAME_DECODE_SIG, text_bytes, text.size, name_at);
        const bool matched = fields.object_class && invoke && event && names;
        std::optional<std::uint32_t> property;
        for (auto address : candidates(REFLECTION_STEP_SIG, text_bytes, text.size)) {
            const auto fn = native_scan::helper(code_image(), address);
            const auto fast =
                native_scan::unique(fn, "48 8B 48 ?? 49 89 4A ?? 49 C7 42 ?? 00 00 00 00 C3");
            const auto slow =
                native_scan::unique(fn,
                                    "4C 8B 41 ?? 49 63 41 ?? 49 03 C0 4C 89 41 ?? 48 89 41 ?? 49 "
                                    "8B C9 49 8B 01 48 FF A0 ?? ?? ?? ??");
            if (!fast || !slow || fast[1].displacement() != slow[4].displacement() ||
                fast[2].displacement() != slow[3].displacement())
                continue;
            if (property) {
                property.reset();
                break;
            }
            property = static_cast<std::uint32_t>(slow[1].displacement());
        }
        if (matched && property) {
            ReflectionLayout l{fields.object_class,
                               fields.object_name,
                               event->flags,
                               fields.children,
                               fields.child_properties,
                               fields.field_next,
                               fields.ffield_name,
                               fields.ffield_next,
                               *property,
                               *invoke,
                               fields.super_getter_slot,
                               names->blocks,
                               names->text,
                               event->address,
                               names->pool};
            l.function_parms_size = event->parms;
            bool valid = l.function_parms_size >= 8 && l.function_parms_size <= 0x1000 &&
                         l.function_parms_size % 2 == 0 && memory::is_image_data(l.name_pool, 8) &&
                         l.object_name != l.object_class && l.ffield_next != l.ffield_name &&
                         l.children != l.child_properties && l.super_getter_slot >= 8 &&
                         l.super_getter_slot <= 0x1000 && l.super_getter_slot % 8 == 0;
            for (auto field :
                 {l.object_class, l.object_name, l.children, l.child_properties, l.field_next,
                  l.ffield_name, l.ffield_next, l.native_function, l.name_blocks})
                valid &= field >= 8 && field <= 0x1000 && field % 8 == 0;
            for (auto field : {l.object_class, l.object_name, l.children, l.child_properties,
                               l.field_next, l.ffield_name, l.ffield_next, l.name_blocks})
                valid &= field < 128;  // These addressing forms use signed disp8.
            valid &= l.property_offset >= 4 && l.property_offset < 128 &&
                     l.property_offset % 4 == 0 && l.object_flags >= 4 && l.object_flags < 128 &&
                     l.object_flags % 4 == 0 && l.name_text >= 2 && l.name_text < 128 &&
                     l.name_text % 2 == 0;
            if (valid) {
                g_reflection_layout = l;
                g_reflection_ready.store(true, std::memory_order_release);
            }
        }
        if (!g_reflection_ready.load()) {
            OPENDOJO_LOG("signatures: reflection layout unavailable; native menu rename disabled");
            ok = false;
        } else
            OPENDOJO_LOG(
                "signatures: reflection class=0x%X name=0x%X children=0x%X properties=0x%X "
                "native=0x%X",
                g_reflection_layout.object_class, g_reflection_layout.object_name,
                g_reflection_layout.children, g_reflection_layout.child_properties,
                g_reflection_layout.native_function);
    }

    {
        auto update = discover_update(text_bytes, text.size);
        std::atomic<std::uintptr_t> thread{0};
        auto leaf = [&](const Pattern& p, std::atomic<std::uintptr_t>& out) {
            PatternByte bytes[256];
            const auto n = decode_pattern(p.notation, bytes, std::size(bytes));
            const auto match = scan_unique(text_bytes, text.size, bytes, n);
            out.store(match.addr);
            return match.addr != 0;
        };
        if (leaf(THREAD_ID_SIG, thread)) {
            PatternByte shape[64];
            const auto size = decode_pattern(THREAD_ID_SIG.notation, shape, std::size(shape));
            const auto instructions = native_scan::unique(
                native_scan::decode(thread.load(), thread.load() + size), THREAD_ID_SIG.notation);
            const auto thread_slot = instructions ? instructions[3].rip() : 0;
            const auto iat = instructions ? instructions[0].rip() : 0;
            std::uintptr_t api = 0;
            if (code_image().contains(iat, 8) && read_pointer_guarded(iat, api) &&
                api == reinterpret_cast<std::uintptr_t>(&GetCurrentThreadId) &&
                memory::is_image_data(thread_slot, 4))
                update.game_thread_id = thread_slot;
        }
        // UObject helpers belong to menu renaming, not the import dispatcher.
        const auto free_fn = discover<native_scan::Match>(
            {"engine_free",
             "E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 48 8B D3 FF 50 ?? 48 83 C4 20 5B C3"},
            text_bytes, text.size, [](const auto& fn) -> std::optional<native_scan::Match> {
                auto m = graph_contract(fn, ENGINE_FREE_SIG);
                if (!m || m[5].rip() != m[9].rip() || !memory::is_image_data(m[5].rip(), 8))
                    return {};
                return m;
            });
        const auto assign_fn =
            discover<native_scan::Match>({"weak_assign", "8B 52 ?? 89 11 48 8D 0D ?? ?? ?? ??"},
                                         text_bytes, text.size,
                                         [](const auto& fn) -> std::optional<native_scan::Match> {
                                             auto m = graph_contract(fn, WEAK_ASSIGN_SIG);
                                             if (!m || !memory::is_image_data(m[7].rip(), 8) ||
                                                 !code_image().code(m[8].relative()))
                                                 return {};
                                             return m;
                                         });
        auto weak = [&](const Pattern& anchor, const Pattern& contract) {
            native_scan::Match result;
            for (auto address : candidates(anchor, text_bytes, text.size)) {
                auto m = graph_contract(native_scan::helper(code_image(), address), contract);
                if (!m) continue;
                if (result) return native_scan::Match{};
                result = std::move(m);
            }
            return result;
        };
        const auto valid_fn = weak({"weak_valid", "44 8B 41 04 45 85 C0"}, WEAK_VALID_SIG);
        const auto get_fn =
            weak({"weak_get", "48 83 EC 08 44 8B 51 04 45 33 C0 4C 8B C9"}, WEAK_GET_SIG);
        if (free_fn && assign_fn && valid_fn && get_fn && valid_fn[6].rip() == get_fn[9].rip() &&
            valid_fn[12].rip() == get_fn[14].rip() &&
            valid_fn[17].displacement() == get_fn[22].displacement() &&
            memory::is_image_data(valid_fn[6].rip(), 4) &&
            memory::is_image_data(valid_fn[12].rip(), 8)) {
            update.engine_free = (*free_fn)[0].address;
            update.weak_assign = (*assign_fn)[0].address;
            update.weak_valid = valid_fn[0].address;
            update.weak_get = get_fn[0].address;
        } else {
            OPENDOJO_LOG("signatures: UObject contracts unavailable; menu rename disabled");
            ok = false;
        }
        if (!update.update || !update.scheduler || !update.game_thread_id) {
            update.update = update.scheduler = update.scheduler_slot = update.game_thread_id = 0;
            OPENDOJO_LOG(
                "signatures: scheduler completion contract unavailable; native imports disabled");
            ok = false;
        }
        g_runtime_layout = update;
        g_runtime_ready.store(true, std::memory_order_release);
    }

    const auto pool1 = pool ? pool->first : 0;

    {
        const auto copy =
            discover<RecordingFormatDiscovery>(POOL_COPY_SIG, text_bytes, text.size, copy_at);
        const auto consumer = discover<RecordingFormatDiscovery>(POOL_CONSUMER_SIG, text_bytes,
                                                                 text.size, consumer_at);
        // The file codec supports uint16 count + 1800 four-byte events. Format
        // changes require a codec migration; identification never weakens this check.
        if (pool && copy && consumer && g_recording_state_ready.load() &&
            pool->state + 4u == g_recording_state_layout.recording_state &&
            pool->first_size == 9 * 7202 && pool->second_size > 0 && copy->pool == pool1 &&
            consumer->pool == pool1)
            g_live_recordings_supported.store(true, std::memory_order_release);
        else {
            OPENDOJO_LOG(
                "signatures: native recording storage incompatible; live recording access "
                "disabled");
            ok = false;
        }
    }

    {
        using namespace native_scan;
        const auto accessor = helper(code_image(), moves ? moves->accessor : 0);
        const auto chain = unique(
            accessor, "E8 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8B C8 48 83 C4 28 E9 ?? ?? ?? ??");
        std::uintptr_t ctx = 0;
        if (chain) {
            const auto getter =
                unique(helper(code_image(), chain[0].relative()), GET_CTX_SIG.notation);
            const auto lookup = subsystem_at(helper(code_image(), chain[4].relative()));
            std::uint32_t key = 0;
            if (memory::is_image_data(chain[1].rip(), 4))
                native_scan::copy(chain[1].rip(), &key, sizeof(key));
            if (key == 0xA7A8857B && getter && lookup.bucket_stride &&
                g_subsystem_layout_ready.load())
                ctx = getter[0].rip();
        }
        if (ctx && memory::is_image_data(ctx, 8)) {
            g_ctx_ptr.store(ctx, std::memory_order_release);
            OPENDOJO_LOG("signatures: ctx_ptr -> 0x%llX", static_cast<unsigned long long>(ctx));
        } else {
            OPENDOJO_LOG("signatures: context accessor chain invalid; subsystem lookup disabled");
            ok = false;
        }
    }

    return ok;
}

bool resolve_all() {
    std::call_once(g_resolve_once, [] { g_resolve_ok = do_resolve(); });
    return g_resolve_ok;
}

RuntimeLayout runtime_layout() {
    return g_runtime_ready.load(std::memory_order_acquire) ? g_runtime_layout : RuntimeLayout{};
}

bool native_object_array_abi_supported(std::uintptr_t enumerate) {
    using namespace native_scan;
    const auto fn = helper(code_image(), enumerate);
    const auto callback =
        unique(fn, "48 8D 05 ?? ?? ?? ?? 48 89 44 24 ?? 8B 44 24 ?? 89 44 24 ?? E8 ?? ?? ?? ??");
    if (!callback) return false;
    const auto append = helper(code_image(), callback[0].rip());
    const auto grow = unique(append,
                             "48 8B 19 48 8B 32 48 63 7B 08 8D 47 01 89 43 08 3B 43 0C 76 ?? 8B D7 "
                             "48 8B CB E8 ?? ?? ?? ?? 48 8B 03");
    const auto store = unique(append, "48 89 34 F8");
    return grow && store && grow[6].relative() == grow[10].address &&
           grow[10].address < store[0].address && code_image().code(grow[9].relative());
}

bool native_text_abi_supported(std::uintptr_t raw_text) {
    using namespace native_scan;
    // Follow the reflected wrapper's actual call. The count/capacity fields and
    // UTF-16 element width are ABI requirements, independent of function placement.
    const auto fn = helper(code_image(), raw_text);
    const auto call = unique(fn, "48 03 F8 48 89 7B ?? 48 8D 54 24 ?? 48 8B CE E8 ?? ?? ?? ??");
    if (!call) return false;
    const auto copy = helper(code_image(), call[4].relative());
    const auto source = unique(copy, "48 63 5A 08 4C 8B 3A 89 5D ?? 85 DB");
    const auto resize = unique(copy,
                               "45 33 C0 8B D3 48 8D 4D ?? E8 ?? ?? ?? ?? 4C 8B C3 4D 03 C0 49 8B "
                               "D7 48 8B 4D ?? E8 ?? ?? ?? ??");
    if (!source || !resize || source[0].address >= resize[0].address) return false;
    const auto allocator = helper(code_image(), resize[3].relative());
    const auto allocation =
        unique(allocator, "48 63 D3 41 B8 02 00 00 00 48 03 D2 E8 ?? ?? ?? ?? 48 89 07 89 5F 0C");
    const auto capacity = unique(allocator, "89 77 0C");
    return allocation && capacity && code_image().code(allocation[3].relative());
}

ReflectionLayout reflection_layout() {
    return g_reflection_ready.load(std::memory_order_acquire) ? g_reflection_layout
                                                              : ReflectionLayout{};
}

bool live_recordings_supported() {
    return g_live_recordings_supported.load(std::memory_order_acquire);
}

PlayerLayout player_layout() {
    return g_player_layout_ready.load(std::memory_order_acquire) ? g_player_layout : PlayerLayout{};
}

RecordingStateLayout recording_state_layout() {
    return g_recording_state_ready.load(std::memory_order_acquire) ? g_recording_state_layout
                                                                   : RecordingStateLayout{};
}

std::uint32_t slot_flag_base() {
    return g_slot_flags.load(std::memory_order_acquire);
}

MovelistLayout movelist_layout() {
    return g_movelist_layout_ready.load(std::memory_order_acquire) ? g_movelist_layout
                                                                   : MovelistLayout{};
}

SubsystemLayout subsystem_layout() {
    return g_subsystem_layout_ready.load(std::memory_order_acquire) ? g_subsystem_layout
                                                                    : SubsystemLayout{};
}

SessionLayout session_layout() {
    return g_session_layout_ready.load(std::memory_order_acquire) ? g_session_layout
                                                                  : SessionLayout{};
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
