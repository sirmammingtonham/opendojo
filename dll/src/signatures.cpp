#include "signatures.hpp"

#include <windows.h>
#include <intrin.h>

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
// Each pattern is anchored on the function's prologue + a distinctive
// near-prologue feature (vtable load, magic constant, etc.). Wildcards
// only on RIP-relative offsets (the 4-byte immediates inside lea/mov/call
// instructions) — everything else is exact.
//
// To regenerate: read the function's first ~50 bytes from the EXE (RVA
// from Ghidra → file offset using PE section headers), then replace
// every 4-byte RIP-relative immediate with `?? ?? ?? ??`.

// This is a leaf function without unwind metadata. Match its complete body,
// including both key comparisons, traversal branches and both returns.
constexpr Pattern SUBSYSTEM_LOOKUP_SIG{
    "subsystem_lookup",
    "4C 8B 41 ?? 44 8B 0A 49 8B 88 ?? ?? ?? ?? 49 8B 90 ?? ?? ?? ?? "
    "49 23 C9 48 C1 E1 ?? 49 03 88 ?? ?? ?? ?? 48 8B 41 ?? 48 3B C2 "
    "74 1A 48 8B 09 44 3B 48 ?? 74 13 48 3B C1 74 0C 48 8B 40 ?? "
    "44 3B 48 ?? 75 F1 EB 02 33 C0 48 85 C0 48 0F 44 C2 48 3B C2 "
    "74 05 48 8B 40 ?? C3 33 C0 C3"};

SubsystemLayout decode_subsystem_layout(const std::uint8_t* code, std::size_t size) {
    if (!code || size < 92) return {};
    auto field = [code](std::size_t at) {
        std::uint32_t result;
        std::memcpy(&result, code + at, 4);
        return result;
    };
    if (code[27] < 4 || code[27] > 8) return {};
    SubsystemLayout l{code[3],  field(10), field(17), field(31), 1u << code[27],
                      code[38], code[50],  code[61],  code[87]};
    // disp8 operands are signed; accept only positive, aligned fields.
    auto qword = [](std::uint32_t v) { return v && v <= 0x1000 && v % 8 == 0; };
    if (!qword(l.map) || l.map >= 128 || !qword(l.mask) || !qword(l.sentinel) ||
        !qword(l.buckets) || l.mask == l.sentinel || l.mask == l.buckets ||
        l.sentinel == l.buckets || !qword(l.bucket_last) || l.bucket_last >= 128 ||
        l.bucket_last + 8 > l.bucket_stride || !qword(l.previous) || l.previous >= 128 ||
        !qword(l.value) || l.value >= 128 || !l.key || l.key >= 128 || l.key % 4 ||
        l.key != code[65] || l.value == l.previous ||
        (l.key < l.previous + 8 && l.key + 4 > l.previous) ||
        (l.key < l.value + 8 && l.key + 4 > l.value))
        return {};
    return l;
}

constexpr Pattern MOVE_WRAPPER_SIG{
    "move_wrapper",
    "48 89 5C 24 10 57 48 83 EC 20 8B FA 48 8B D9 E8 ?? ?? ?? ?? 0F BE 8B ?? ?? ?? ?? 48 8D 54 24 "
    "30 83 F1 01 89 4C 24 30 48 8B C8 E8 ?? ?? ?? ?? 48 85 C0 74 18 44 8B C7 33 D2 48 8B C8 E8 ?? "
    "?? ?? ?? 48 8B 5C 24 38 48 83 C4 20 5F C3 48 8B 5C 24 38 B8 FF FF FF FF 48 83 C4 20 5F C3"};
constexpr Pattern MOVE_ELEMENT_SIG{
    "move_element",
    "48 63 02 83 F8 FF 74 38 4C 8B 01 4C 8B C8 48 8B 49 ?? 48 B8 ?? ?? ?? ?? ?? ?? ?? ?? 49 2B C8 "
    "48 F7 E9 48 C1 FA ?? 48 8B C2 48 C1 E8 3F 48 03 D0 4C 3B CA 73 0B 49 69 C1 ?? ?? ?? ?? 49 03 "
    "C0 C3 33 C0 C3"};
constexpr Pattern MOVE_FIELD_SIG{
    "move_field",
    "41 83 F8 07 77 0F 49 63 C0 48 63 D2 48 8D 14 D0 8B 44 91 ?? C3 B8 FF FF FF FF C3"};

MovelistLayout decode_movelist_layout(const std::uint8_t* wrapper, std::size_t wn,
                                      const std::uint8_t* element, std::size_t en,
                                      const std::uint8_t* field, std::size_t fn) {
    if (!wrapper || !element || !field || wn < 92 || en < 67 || fn < 27) return {};
    MovelistLayout l;
    std::memcpy(&l.human_side, wrapper + 23, 4);
    std::memcpy(&l.element_stride, element + 56, 4);
    l.vector_end = element[17];
    l.move_ids = field[19];
    if (!l.human_side || l.human_side > 0x10000 || l.element_stride < 64 ||
        l.element_stride > 0x10000 || l.element_stride % 8 || l.vector_end < 8 ||
        l.vector_end >= 128 || l.vector_end % 8 || l.move_ids < 4 || l.move_ids >= 128 ||
        l.move_ids % 4 || l.move_ids + 32 > l.element_stride || element[37] > 30)
        return {};
    // The bounds-check division and the indexed address must use the same size.
    std::uint64_t magic = 0, high = 0;
    std::memcpy(&magic, element + 20, 8);
    if (magic >> 63) return {};  // This instruction shape uses positive signed magic.
    const auto low = _umul128(magic, l.element_stride, &high);
    if (high != (1ull << element[37]) || low >= l.element_stride) return {};
    return l;
}

constexpr Pattern SLOT_FLAGS_SIG{
    "slot_flags",
    "48 89 5C 24 ?? 48 89 6C 24 ?? 56 48 83 EC 20 48 63 EA 41 8B C1 41 8B D8 48 8B F1 83 FD 07 0F "
    "87 73 01 00 00 83 F8 02 0F 8D 6A 01 00 00 85 C0 79 05 E8 ?? ?? ?? ?? 48 63 C8 48 8D 0C CD ?? "
    "?? ?? ?? 48 03 CD 89 9C CE ?? ?? ?? ?? 83 FB 02 75 1F 44 8B CD 41 B0 01 33 D2 48 8B CE 48 8B "
    "5C 24 ?? 48 8B 6C 24 ?? 48 83 C4 20 5E E9 ?? ?? ?? ??"};

constexpr Pattern SESSION_COUNTER_SIG{
    "session_counter",
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 41 8B F8 48 8B F2 48 8B D9 E8 ?? ?? ?? ?? 8B 80 "
    "?? ?? ?? ?? 39 06 75 13 89 7B ?? 48 8B 5C 24 30 48 8B 74 24 38 48 83 C4 20 5F C3 48 8B CB E8 "
    "?? ?? ?? ?? 8B 80 ?? ?? ?? ?? 39 06 75 03 89 7B ?? 48 8B 5C 24 30 48 8B 74 24 38 48 83 C4 20 "
    "5F C3"};
constexpr Pattern SUBB_PAUSE_SIG{
    "subb_pause",
    "48 89 5C 24 08 57 48 83 EC 20 80 79 ?? 00 0F B6 DA 48 8B F9 75 30 84 D2 74 2C E8 ?? ?? ?? ?? "
    "48 8D 15 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? B2 01 48 8B C8 E8 ?? ?? ?? ?? 88 5F ?? 48 8B 5C "
    "24 30 48 83 C4 20 5F C3 88 59 ?? 48 8B 5C 24 30 48 83 C4 20 5F C3"};
constexpr Pattern SUBC_RESET_SIG{
    "subc_reset",
    "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 30 33 F6 0F 29 74 24 20 89 71 ?? 48 8B D9 C7 41 ?? "
    "26 00 00 00 8B FE C7 41 ?? 26 00 00 00 0F 1F 40 00 48 0F BE C7 48 69 C8 ?? ?? ?? ?? 48 89 B4 "
    "19 ?? ?? ?? ?? 48 89 B4 19 ?? ?? ?? ?? 48 89 B4 19 ?? ?? ?? ?? 89 B4 19 ?? ?? ?? ?? 48 81 C1 "
    "?? ?? ?? ?? 48 03 CB E8 ?? ?? ?? ?? FF C7 83 FF 02 7C C0"};
constexpr Pattern SUBC_ITEM_SIG{
    "subc_item", "33 D2 48 C7 41 ?? FF FF FF FF 33 C0 48 89 11 48 89 51 ?? 0F 57 C0 48 89 51 ??"};
constexpr Pattern RECORDING_STATE_SIG{
    "recording_state",
    "48 89 5C 24 08 57 48 83 EC 20 8B FA 48 8B D9 E8 ?? ?? ?? ?? 84 C0 74 56 89 7B ?? 89 7B ?? 83 "
    "EF 01 74 1D 83 FF 01 75 46"};

constexpr Pattern PLAYER_FIELDS_SIG{
    "player_fields",
    "48 89 43 ?? 8B 07 39 05 ?? ?? ?? ?? 0F 8F D0 00 00 00 8B 05 ?? ?? ?? ?? 89 44 24 ?? E8 ?? ?? "
    "?? ?? 48 8D 15 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8D 54 24 ?? 48 8B C8 E8 ?? ?? ?? ?? 84 "
    "C0 74 23 E8 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8D 54 24 ?? 48 8B C8 "
    "E8 ?? ?? ?? ?? EB 02 33 C0 48 83 7B ?? 00 48 89 43 ??"};
constexpr Pattern CHARACTER_JACK_SIG{
    "character_jack",
    "80 B9 ?? ?? ?? ?? 05 74 10 8B 81 ?? ?? ?? ?? 83 F8 0A 74 08 83 F8 77 74 03 32 C0 C3 B0 01 C3"};
constexpr Pattern CHARACTER_ALISA_SIG{
    "character_alisa", "80 B9 ?? ?? ?? ?? 05 75 03 32 C0 C3 83 B9 ?? ?? ?? ?? 12 0F 94 C0 C3"};
constexpr Pattern PLAYER_NATIVE_SIG{
    "player_native",
    "48 83 EC 28 48 63 02 4C 8B 01 83 F8 FF 74 2B 49 8B 50 ?? 48 8B C8 49 8B 40 ?? 48 2B C2 48 C1 "
    "F8 03 48 3B C1 76 1B 48 8B 04 CA 48 85 C0 74 0B 48 05 ?? ?? ?? ?? 48 83 C4 28 C3 33 C0 48 83 "
    "C4 28 C3 E8 ?? ?? ?? ?? CC"};

constexpr Pattern REFLECTION_FIND_SIG{
    "reflection_find",
    "48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 40 "
    "45 0F B6 F1 49 8B F0 48 8B FA 4C 8B F9 4D 85 C0 75 20 45 0F B6 C1 41 C0 EE 02 41 0F B6 D1 40 "
    "88 71 ?? 41 80 E0 01 D0 EA 33 DB 48 89 19 E9 ?? ?? ?? ?? 33 DB 48 89 7D ?? 48 85 FF 74 0A 48 "
    "8B 42 ?? 48 89 45 ?? EB 04 48 89 5D ?? 45 0F B6 EE C7 45 ?? FF FF FF FF 41 D0 ED 45 0F B6 E6 "
    "41 80 E4 01 41 C0 EE 02 41 0F B6 C5 44 88 65 ?? 24 01 88 45 ?? 41 F6 C6 01 74 2C 48 85 FF 74 "
    "27 E8 ?? ?? ?? ?? 48 8B 57 ?? 4C 8D 40 ?? 48 63 40 ?? 3B 42 ?? 7F 11 48 8B C8 C6 45 ?? 01 48 "
    "8B 42 ?? 4C 39 04 C8 74 03 88 5D ?? 48 8D 4D ?? E8 ?? ?? ?? ?? 48 8B 45 ?? 48 85 C0 74 27 48 "
    "8B 48 ?? 48 89 4D ?? 48 3B CE 74 45 48 8B 40 ?? 48 8D 4D ?? 48 89 45 ?? E8 ?? ?? ?? ?? 48 8B "
    "45 ?? 48 85 C0 75 D9 41 88 5F ?? 41 0F B6 D5 49 89 1F 45 0F B6 C4 48 85 F6 0F 84 AE 00 00 00 "
    "48 89 7D ?? 48 85 FF 74 23 48 8B 47 ?? 48 89 45 ?? EB 1D 41 88 5F ?? 41 0F B6 D5 49 89 07 45 "
    "0F B6 C4 48 85 C0 74 D0 E9 ?? ?? ?? ?? 48 89 5D ?? 80 E2 01 C7 45 ?? FF FF FF FF 44 88 45 ?? "
    "88 55 ?? 41 F6 C6 01 74 2C 48 85 FF 74 27 E8 ?? ?? ?? ?? 48 8B 57 ?? 4C 8D 40 ?? 48 63 40 ?? "
    "3B 42 ?? 7F 11 48 8B C8 C6 45 ?? 01 48 8B 42 ?? 4C 39 04 C8 74 04 C6 45 ?? 00 48 8D 4D ?? E8 "
    "?? ?? ?? ?? 48 8B 45 ?? 48 85 C0 74 25 48 39 70 ?? 74 1C 48 8B 40 ?? 48 8D 4D ?? 48 89 45 ?? "
    "E8 ?? ?? ?? ?? 48 8B 45 ?? 48 85 C0 75 E0 EB 03 48 8B D8 C6 45 ?? 01 48 89 5D ?? 0F 10 45 ?? "
    "41 0F 11 07 4C 8D 5C 24 ?? 49 8B C7 49 8B 5B ?? 49 8B 73 ?? 49 8B 7B ?? 49 8B E3 41 5F 41 5E "
    "41 5D 41 5C 5D C3"};
constexpr Pattern REFLECTION_NEXT_SIG{
    "reflection_next",
    "48 89 5C 24 ?? 57 48 83 EC 20 48 8B 11 48 8B F9 48 8B 59 ?? 48 85 D2 0F 84 AA 00 00 00 0F 1F "
    "00 48 85 DB 74 44 66 66 66 0F 1F 84 00 ?? ?? 00 00 48 8B 43 ?? F6 80 ?? ?? ?? ?? 01 74 23 80 "
    "7F ?? 00 0F 85 80 00 00 00 8B 80 ?? ?? ?? ?? 48 C1 E8 0F A8 01 74 72 8B 43 ?? 48 C1 E8 1D A8 "
    "01 74 67 48 8B 5B ?? 48 85 DB 75 C7 80 7F ?? 00 74 2E 8B 47 ?? FF C0 89 47 ?? 3B 82 ?? ?? ?? "
    "?? 7D 1E 48 63 C8 48 8B 82 ?? ?? ?? ?? 48 03 C9 48 8B 1C C8 48 85 DB 74 06 48 8B 5B ?? EB 85 "
    "EB 83 80 7F ?? 00 74 24 48 8B 02 48 8B CA FF 90 ?? ?? ?? ?? 48 8B D0 48 85 C0 74 10 48 8B 58 "
    "?? C7 47 ?? FF FF FF FF E9 ?? ?? ?? ?? 48 89 17 48 89 5F ?? 48 8B 5C 24 ?? 48 83 C4 20 5F C3"};
constexpr Pattern REFLECTION_INVOKE_SIG{
    "reflection_invoke",
    "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 41 56 48 83 EC 20 48 8B 59 ?? 4D "
    "8B F1 49 8B F8 48 8B F2 48 8B E9 E8 ?? ?? ?? ?? 48 85 C0 74 28 48 8D 50 ?? 48 63 40 ?? 3B 43 "
    "?? 7F 1B 4C 8B C0 48 8B 43 ?? 4A 39 14 C0 75 0E 48 8B D3 48 8B CE E8 ?? ?? ?? ?? 48 8B F0 48 "
    "8B 9F ?? ?? ?? ?? 4D 8B C6 48 8B D7 48 89 AF ?? ?? ?? ?? 48 8B CE FF 95 ?? ?? ?? ?? 48 8B 6C "
    "24 ?? 48 8B 74 24 ?? 48 89 9F ?? ?? ?? ?? 48 8B 5C 24 ?? 48 8B 7C 24 ?? 48 83 C4 20 41 5E C3"};
constexpr Pattern REFLECTION_STEP_SIG{
    "reflection_step",
    "41 8B 40 ?? 4D 8B C8 4C 8B D1 48 0F BA E0 08 73 29 48 8B 81 ?? ?? ?? ?? 4C 39 00 74 0C 0F 1F "
    "00 48 8B 40 ?? 4C 39 08 75 F7 48 8B 48 ?? 49 89 4A ?? 49 C7 42 ?? 00 00 00 00 C3 4C 8B 41 ?? "
    "49 63 41 ?? 49 03 C0 4C 89 41 ?? 48 89 41 ?? 49 8B C9 49 8B 01 48 FF A0 ?? ?? ?? ??"};
constexpr Pattern RUNTIME_UPDATE_SIG{
    "runtime_update",
    "40 57 48 83 EC 30 80 79 ?? 00 48 8B F9 0F 29 74 24 ?? 0F 28 F1 0F 84 9C 00 00 00 48 8B 0D "
    "?? ?? ?? ?? 48 85 C9 74 61 48 89 5C 24 ?? 8B 99 ?? ?? ?? ?? 85 DB 7E 4D FF CB 85 DB 7E 29 "
    "F3 0F 10 0D ?? ?? ?? ?? 41 B0 01 E8 ?? ?? ?? ?? F3 0F 10 0D ?? ?? ?? ?? 41 B0 01 48 8B 0D "
    "?? ?? ?? ?? E8 ?? ?? ?? ?? EB 11 E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B "
    "05 ?? ?? ?? ?? 89 98 ?? ?? ?? ?? 48 8B 5C 24 ?? 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 0E 41 B8 "
    "03 00 00 00 0F 28 CE E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 05 E8 ?? ?? ?? ?? C6 "
    "47 ?? 00 0F 28 74 24 ?? B0 01 48 83 C4 30 5F C3"};
constexpr Pattern SCHEDULER_SIG{
    "scheduler",
    "4C 8B DC 57 48 81 EC 60 02 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 ?? ?? ?? ?? 80 "
    "39 00 48 8B F9 0F 84 23 05 00 00 49 89 5B ?? 49 89 6B ?? 4D 89 7B ?? F3 0F 11 89 ?? ?? ?? "
    "?? 45 85 C0 74 27 41 83 E8 01 74 19 41 83 E8 01 74 0B 41 83 F8 01 75 15 8B 69 ?? EB 13 8B "
    "69 ?? 8B 59 ?? EB 14 8B 69 ?? 8B 59 ?? EB 0C 8B 69 ?? FF 81 ?? ?? ?? ?? 8B 59 ?? FF 15 ?? "
    "?? ?? ?? 85 C0 74 0B 65 4C 8B 3C 25 ?? ?? ?? ?? EB 0B 33 C9 FF 15 ?? ?? ?? ?? 4C 8B F8 4D "
    "85 FF 75 2A FF 15 ?? ?? ?? ?? 44 8B C0 48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 ?? E8 ?? ?? ?? ?? "
    "48 8D 4C 24 ?? FF 15 ?? ?? ?? ?? E9 ?? ?? ?? ?? 4C 89 A4 24 ?? ?? ?? ?? 4C 63 E3 4C 89 AC "
    "24 ?? ?? ?? ?? 4C 63 ED 4C 89 64 24 ?? 4D 3B EC 0F 8D 33 04 00 00 48 89 B4 24 ?? ?? ?? ?? "
    "4C 89 B4 24 ?? ?? ?? ?? 4E 8D 34 6D ?? ?? ?? ?? 4D 03 F5 4D 2B E5 4E 8D 34 F7 66 0F 1F 44 "
    "00 ?? 89 AF ?? ?? ?? ?? 49 8B 0E 49 8B 5E ?? 48 8B C1 48 2B C3 48 A9 F8 FF FF FF 0F 84 6E "
    "02 00 00 48 3B D9 0F 84 9C 00 00 00 48 8D 73 ?? 0F 1F 40 ?? 48 8B 03 80 78 ?? 00 75 7A 80 "
    "78 ?? 00 74 24 48 8B 97 ?? ?? ?? ?? 48 8D 8F ?? ?? ?? ?? 48 3B 97 ?? ?? ?? ?? 74 31 48 89 "
    "02 48 83 87 ?? ?? ?? ?? 08 EB 2C 48 8B 97 ?? ?? ?? ?? 48 8D 8F ?? ?? ?? ?? 48 3B 97 ?? ?? "
    "?? ?? 74 0D 48 89 02 48 83 87 ?? ?? ?? ?? 08 EB 08 4C 8B C3 E8 ?? ?? ?? ?? 48 8B 03 80 78 "
    "?? 00 74 1B C6 40 ?? 00 48 8B D6 4D 8B 06 48 8B CB 4C 2B C6 E8 ?? ?? ?? ?? 49 83 06 F8 EB "
    "08 48 83 C3 08 48 83 C6 08 49 3B 1E 0F 85 6C FF FF FF 48 8B 87 ?? ?? ?? ?? 48 2B 87 ?? ?? "
    "?? ?? 48 A9 F8 FF FF FF 0F 84 1C 01 00 00 C7 87 ?? ?? ?? ?? 00 00 00 00 48 8B B7 ?? ?? ?? "
    "?? 48 8B 9F ?? ?? ?? ?? 48 3B DE 74 21 0F 1F 44 00 ?? 48 8B 0B 45 33 C0 48 8B 09 41 8D 50 "
    "?? FF 15 ?? ?? ?? ?? 48 83 C3 08 48 3B DE 75 E4 48 8B B7 ?? ?? ?? ?? 48 8B 9F ?? ?? ?? ?? "
    "48 8B C6 48 2B C3 48 A9 F8 FF FF FF 74 75 48 3B DE 74 59 0F 1F 00 48 8B 13 48 89 15 ?? ?? "
    "?? ?? 80 7A ?? 00 75 3D 8B 87 ?? ?? ?? ?? 89 42 ?? 48 8B 42 ?? 48 85 C0 74 25 4C 89 78 ?? "
    "48 8B 4A ?? 8B 41 ?? 83 F8 F2 7E 05 FF C8 89 41 ?? 85 C0 7F 12 48 8B 49 ?? FF 15 ?? ?? ?? "
    "?? EB 06 48 8B CA FF 52 ?? 48 83 C3 08 48 3B DE 75 AA 48 8B 87 ?? ?? ?? ?? 48 3B 87 ?? ?? "
    "?? ?? 74 07 48 89 87 ?? ?? ?? ?? 48 8B B7 ?? ?? ?? ?? 48 8B 9F ?? ?? ?? ?? 48 3B DE 74 1A "
    "48 8B 0B BA FF FF FF FF 48 8B 09 FF 15 ?? ?? ?? ?? 48 83 C3 08 48 3B DE 75 E6 48 8B 87 ?? "
    "?? ?? ?? 48 3B 87 ?? ?? ?? ?? 0F 84 9F 00 00 00 48 89 87 ?? ?? ?? ?? E9 ?? ?? ?? ?? 48 8B "
    "B7 ?? ?? ?? ?? 48 8B 9F ?? ?? ?? ?? 48 8B C6 48 2B C3 48 A9 F8 FF FF FF 74 77 48 3B DE 74 "
    "5B 0F 1F 44 00 ?? 48 8B 13 48 89 15 ?? ?? ?? ?? 80 7A ?? 00 75 3D 8B 87 ?? ?? ?? ?? 89 42 "
    "?? 48 8B 42 ?? 48 85 C0 74 25 4C 89 78 ?? 48 8B 4A ?? 8B 41 ?? 83 F8 F2 7E 05 FF C8 89 41 "
    "?? 85 C0 7F 12 48 8B 49 ?? FF 15 ?? ?? ?? ?? EB 06 48 8B CA FF 52 ?? 48 83 C3 08 48 3B DE "
    "75 AA 48 8B 87 ?? ?? ?? ?? 48 3B 87 ?? ?? ?? ?? 74 07 48 89 87 ?? ?? ?? ?? FF C5 49 83 C6 "
    "18 49 83 EC 01 0F 85 63 FD FF FF 48 8B 74 24 ?? 4E 8D 34 6D ?? ?? ?? ?? 8B 47 ?? 4D 03 F5 "
    "49 2B F5 89 87 ?? ?? ?? ?? 48 89 74 24 ?? 4E 8D 34 F7 49 8B 2E 49 8B 5E ?? 48 8B C5 48 2B "
    "C3 48 A9 F8 FF FF FF 0F 84 0B 01 00 00 48 3B DD 0F 84 FA 00 00 00 48 8B 3B 80 7F ?? 00 75 "
    "0B 48 83 C3 08 48 3B DD 75 EE EB 53 80 7F ?? 00 75 34 48 8B 47 ?? 48 85 C0 74 2B 4C 89 78 "
    "?? 48 8B 47 ?? 44 89 60 ?? 48 8B 4F ?? 8B 41 ?? 83 F8 F2 7E 05 FF C8 89 41 ?? 85 C0 7F 0A "
    "48 8B 49 ?? FF 15 ?? ?? ?? ?? 48 8B 47 ?? 48 85 C0 74 03 4C 89 20 48 8B 07 BA 01 00 00 00 "
    "48 8B CF FF 10 48 3B DD 0F 84 8A 00 00 00 48 8D 73 ?? 48 3B F5 0F 84 78 00 00 00 48 8B 3E "
    "80 7F ?? 00 74 5C 80 7F ?? 00 75 37 48 8B 47 ?? 48 85 C0 74 2E 4C 89 78 ?? 48 8B 47 ?? C7 "
    "40 ?? 00 00 00 00 48 8B 4F ?? 8B 41 ?? 83 F8 F2 7E 05 FF C8 89 41 ?? 85 C0 7F 0A 48 8B 49 "
    "?? FF 15 ?? ?? ?? ?? 48 8B 47 ?? 48 85 C0 74 07 48 C7 00 00 00 00 00 48 8B 07 BA 01 00 00 "
    "00 48 8B CF FF 10 EB 07 48 89 3B 48 83 C3 08 48 83 C6 08 48 3B F5 75 8B 45 33 E4 48 8B 74 "
    "24 ?? 49 3B 1E 74 03 49 89 1E 49 83 C6 18 48 83 EE 01 48 89 74 24 ?? 0F 85 C9 FE FF FF 4C "
    "8B B4 24 ?? ?? ?? ?? 48 8B B4 24 ?? ?? ?? ?? EB 09 8B 47 ?? 89 87 ?? ?? ?? ?? 4C 8B AC 24 "
    "?? ?? ?? ?? 4C 8B A4 24 ?? ?? ?? ?? 48 8B AC 24 ?? ?? ?? ?? 48 8B 9C 24 ?? ?? ?? ?? 4C 8B "
    "BC 24 ?? ?? ?? ?? 48 8B 8C 24 ?? ?? ?? ?? 48 33 CC E8 ?? ?? ?? ?? 48 81 C4 60 02 00 00 5F "
    "C3"};
constexpr Pattern THREAD_ID_SIG{
    "thread_id",
    "FF 15 ?? ?? ?? ?? 49 8B CC 44 88 35 ?? ?? ?? ?? 89 05 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? "
    "?? ??"};
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
constexpr Pattern OBJECT_ENUM_SIG{
    "object_enum",
    "48 83 EC 48 48 8D 44 24 ?? 48 89 54 24 ?? 48 89 44 24 ?? 48 8D 54 24 ?? 48 8D 05 ?? ?? ?? ?? "
    "48 89 44 24 ?? 8B 44 24 ?? 89 44 24 ?? E8 ?? ?? ?? ?? 48 83 C4 48 C3"};
constexpr Pattern OBJECT_APPEND_SIG{
    "object_append",
    "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC 20 48 8B 19 48 8B 32 48 63 7B 08 8D 47 ?? 89 43 08 "
    "3B 43 0C 76 0A 8B D7 48 8B CB E8 ?? ?? ?? ?? 48 8B 03 48 8B 5C 24 ?? 48 89 34 F8 48 8B 74 24 "
    "?? 48 83 C4 20 5F C3"};
constexpr Pattern RAW_TEXT_EXEC_SIG{
    "raw_text_exec",
    "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC 30 48 8B DA 48 8B F1 0F 57 C0 0F 11 44 24 ?? 33 FF "
    "48 89 7C 24 ?? 48 89 7C 24 ?? E8 ?? ?? ?? ?? 48 8B CB 48 39 7B ?? 74 10 4C 8D 44 24 ?? 48 8B "
    "53 ?? E8 ?? ?? ?? ?? EB 1C 4C 8B 83 ?? ?? ?? ?? 49 8B 40 ?? 48 89 83 ?? ?? ?? ?? 48 8D 54 24 "
    "?? E8 ?? ?? ?? ?? 89 7C 24 ?? E8 ?? ?? ?? ?? 48 8B CB 48 83 7B ?? 00 74 10 4C 8D 44 24 ?? 48 "
    "8B 53 ?? E8 ?? ?? ?? ?? EB 1C 4C 8B 83 ?? ?? ?? ?? 49 8B 40 ?? 48 89 83 ?? ?? ?? ?? 48 8D 54 "
    "24 ?? E8 ?? ?? ?? ?? 83 7C 24 ?? 00 41 0F 95 C0 48 8B 43 ?? 48 85 C0 40 0F 95 C7 48 03 F8 48 "
    "89 7B ?? 48 8D 54 24 ?? 48 8B CE E8 ?? ?? ?? ?? 90 48 8B 4C 24 ?? 48 85 C9 74 06 E8 ?? ?? ?? "
    "?? 90 48 8B 5C 24 ?? 48 8B 74 24 ?? 48 83 C4 30 5F C3"};
constexpr Pattern FSTRING_COPY_SIG{
    "fstring_copy",
    "48 89 5C 24 ?? 48 89 74 24 ?? 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC 60 45 0F B6 F0 48 8B "
    "F2 48 8B F9 45 33 E4 48 8D 45 ?? 48 89 45 ?? 4C 89 65 ?? 48 63 5A 08 4C 8B 3A 89 5D ?? 85 DB "
    "75 06 44 89 65 ?? EB 21 45 33 C0 8B D3 48 8D 4D ?? E8 ?? ?? ?? ?? 4C 8B C3 4D 03 C0 49 8B D7 "
    "48 8B 4D ?? E8 ?? ?? ?? ?? 90"};
constexpr Pattern FSTRING_RESIZE_SIG{
    "fstring_resize",
    "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC 20 48 63 DA 41 8B F0 48 8B F9 85 D2 74 1F 48 8B CB "
    "BA 02 00 00 00 48 03 C9 E8 ?? ?? ?? ?? 48 D1 E8 B9 FF FF FF 7F 3B D8 0F 4F C1 8B D8 3B DE 7E "
    "33 48 8B 0F 48 85 C9 75 04 85 DB 74 14 48 63 D3 41 B8 02 00 00 00 48 03 D2 E8 ?? ?? ?? ?? 48 "
    "89 07 89 5F 0C 48 8B 5C 24 ?? 48 8B 74 24 ?? 48 83 C4 20 5F C3 89 77 0C 48 8B 5C 24 ?? 48 8B "
    "74 24 ?? 48 83 C4 20 5F C3"};
constexpr Pattern EVENT_PARMS_SIG{"event_parms",
                                  "0F B7 8E ?? ?? ?? ?? 33 D2 44 2B C1 48 03 CF 4D 63 C0 E8 ?? ?? "
                                  "?? ?? 44 0F B7 86 ?? ?? ?? ?? 49 8B D4 48 8B CF E8 ?? ?? ?? ??"};
constexpr Pattern PROCESS_EVENT_SIG{
    "process_event",
    "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 10 01 00 00 48 8D 6C 24 ?? 48 89 9D ?? ?? ?? ?? "
    "48 8B 05 ?? ?? ?? ?? 48 33 C5 48 89 85 ?? ?? ?? ?? 4D 8B E0 48 8B F2 4C 8B F9 48 85 C9 0F 84 "
    "CF 03 00 00 F7 41 ?? 00 00 00 60 0F 85 C2 03 00 00 45 33 F6 F7 82 ?? ?? ?? ?? 00 04 00 00 74 "
    "32 48 8B 01 45 33 C0 FF 90 ?? ?? ?? ?? 8B D8"};
constexpr Pattern NAME_DECODE_SIG{
    "name_decode",
    "48 89 5C 24 ?? 57 48 83 EC 20 80 3D ?? ?? ?? ?? 00 48 8B FA 8B 19 74 09 4C 8D 05 ?? ?? ?? ?? "
    "EB 16 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 4C 8B C0 C6 05 ?? ?? ?? ?? 01 8B CB 0F B7 C3 C1 E9 "
    "10 89 4C 24 ?? 89 44 24 ?? 48 8B 44 24 ?? 48 C1 E8 20 8D 1C 00 49 03 5C C8 ?? 48 8B CF 44 0F "
    "B7 03 48 8D 53 ?? 49 C1 E8 06 E8 ?? ?? ?? ?? 0F B7 03 48 8B 5C 24 ?? 48 C1 E8 06 C6 04 38 00 "
    "48 83 C4 20 5F C3"};

constexpr Pattern SESSION_FINALIZE_SIG{
    "session_finalize",
    "40 57 48 83 EC 20 F7 01 ?? ?? ?? ?? 48 8B F9 75 08 32 C0 48 83 C4 20 5F "
    "C3 48 89 5C 24 30 C7 81 ?? ?? ?? ?? 00 00 00 00 E8 ?? ?? ?? ?? 48 8D 15 "
    "?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B D8 E8 ?? ?? ?? ?? 48 8D 15 ?? "
    "?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 0F BE 88 ?? ?? ?? ?? 48 8B 03 83 F1 01 "
    "48 8B 5C 24 30 83 F9 FF 74 28 48 8B 50 08 48 8B 40 10 48 2B C2 48 63 C9 "
    "48 C1 F8 03 48 3B C1 76 2B 48 8B 04 CA 48 85 C0 74 08 48 05 ?? ?? ?? ?? "
    "EB 05 B8 ?? ?? ?? ?? C7 00 01 00 00 00 B0 01 C6 87 ?? ?? ?? ?? 00 48 83 "
    "C4 20 5F C3"};

SessionLayout decode_session_layout(const std::uint8_t* code, std::size_t size) {
    if (!code || size < 172) return {};
    auto field = [code](std::size_t offset) {
        std::uint32_t value = 0;
        std::memcpy(&value, code + offset, 4);
        return value;
    };
    SessionLayout layout{field(32), field(147), field(161), field(8)};
    // The native array and holder use different pointer bases; decode their delta.
    // Reject an unexpected pointer relationship rather than guessing a field.
    if (!layout.active_mask || (layout.active_mask & (layout.active_mask - 1)) ||
        field(140) < layout.player_flag || field(140) - layout.player_flag < 8 ||
        field(140) - layout.player_flag > 0x1000 || (field(140) - layout.player_flag) % 8 ||
        layout.pending < 4 || layout.pending > 0x1000 || layout.pending % 4 ||
        layout.player_flag < 0x100 || layout.player_flag > 0x10000 || layout.player_flag % 4 ||
        layout.finished < 4 || layout.finished > 0x1000 ||
        (layout.finished >= layout.pending && layout.finished < layout.pending + 4))
        return {};
    return layout;
}

constexpr Pattern PRACTICE_DTOR_SIG{
    "practice_dtor",
    // FUN at RVA 0x5C8C880 in v3.00.02. MSVC dtor pattern: saves
    // rbx/rsi/rdi, stores vtable ptr (lea rax,[rip+vtable]; mov [rcx],rax),
    // then loads a global singleton ptr.
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B F2 48 8B D9 "
    "48 8D 05 ?? ?? ?? ?? 48 89 01 "
    "48 8B 0D ?? ?? ?? ?? "
    "48 85 C9 74 08 48 8B 01 33 D2 FF 50 ??"};

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
    //   mov ecx, <TLS displacement>                     ; wildcarded
    //   mov rdi, [rax+rdx*8] ; add rdi, rcx             ; resolve TLS slot
    //   mov eax, [rdi]                                  ; load slot value
    //   cmp [rip+GUARD], eax                            ; wildcarded
    //   jg <long>                                       ; 0F 8F + 32-bit disp
    //   mov eax, [rip+_] ; mov [rsp+0x30], eax          ; unique to target
    "48 89 5C 24 18 57 48 83 EC 20 "
    "65 48 8B 04 25 58 00 00 00 "
    "48 8B D9 "
    "8B 15 ?? ?? ?? ?? "
    "B9 ?? ?? ?? ?? "
    "48 8B 3C D0 48 03 F9 "
    "8B 07 39 05 ?? ?? ?? ?? "
    "0F 8F ?? ?? ?? ?? "
    "8B 05 ?? ?? ?? ?? "
    "89 44 24 30"};

constexpr Pattern POOL_INIT_SIG{
    "pool_init",
    "48 83 EC 28 33 C0 48 89 41 ?? 48 39 05 ?? ?? ?? ?? 75 39 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B "
    "0D ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 48 85 C9 74 0C E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 33 D2 "
    "41 B8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 83 3D ?? ?? ?? ?? 00 75 3D B9 ?? ?? ?? ?? E8 ?? "
    "?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 48 85 C9 74 0C E8 ?? ?? ?? ?? 48 8B 05 ?? "
    "?? ?? ?? 33 D2 41 B8 ?? ?? ?? ?? 48 8B C8 48 83 C4 28 E9 ?? ?? ?? ?? 48 83 C4 28 C3"};
constexpr Pattern POOL_CONSUMER_SIG{
    "pool_consumer",
    "48 63 47 ?? 48 69 C8 22 1C 00 00 48 03 0D ?? ?? ?? ?? 0F B7 11 48 8D 59 02 EB 1A 48 8B 47 ?? "
    "48 8B 88 ?? ?? ?? ?? 48 8B C5 48 03 C0 48 8B 5C C1 ?? 0F B7 14 C1 4C 89 74 24 58 4C 63 74 EF "
    "?? 4C 89 7C 24 60 45 33 FF 42 0F B6 4C B3 03 3B 4C EF ?? 77 1F 44 89 7C EF ?? FF 44 EF ?? 44 "
    "8B 74 EF ?? 0F B7 C2 44 3B F0 72 09 C6 47 ?? 01 E9 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? E8 ?? ?? "
    "?? ?? 83 78 ?? 04 75 42 48 8B 8E ?? ?? ?? ?? 48 85 C9 74 36 48 81 C1 ?? ?? ?? ?? E8 ?? ?? ?? "
    "?? 4C 8B C0 B8 BF BF 8C 82 41 F7 E0 C1 EA 07 69 CA FB 00 00 00 44 2B C1 49 63 CE 0F B6 54 8B "
    "02 41 3B D0 74 06 66 C7 47 ?? 01 01 49 63 C6 48 8D 0C 83 0F B7 04 83 25 DF FF 00 00 41 89 45 "
    "?? 0F B6 01 C0 E8 05 24 01 41 88 04 24"};
constexpr Pattern POOL_COPY_SIG{
    "pool_copy",
    "48 63 4F ?? 4C 63 C3 49 03 C8 48 69 C9 ?? ?? ?? ?? 49 8D 40 ?? 41 B8 ?? ?? ?? ?? 48 03 0D ?? "
    "?? ?? ?? 48 69 D0 ?? ?? ?? ?? 48 03 15 ?? ?? ?? ?? E8 ?? ?? ?? ??"};

// Follow the movelist wrapper's actual subsystem accessor. This identifies the
// context getter by its caller and table-lookup target, without neighboring stubs.
constexpr Pattern RECORD_POOL_ACCESSOR_SIG{
    "record_pool_accessor",
    "48 83 EC 28 E8 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8B C8 48 83 C4 28 E9 ?? ?? ?? ??"};
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
    PatternByte buf[2048];
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
    if (scan_one(SESSION_FINALIZE_SIG, text_bytes, text.size, g_session_finalize)) {
        g_session_layout = decode_session_layout(
            reinterpret_cast<const std::uint8_t*>(g_session_finalize.load()), 172);
        g_session_layout_ready.store(g_session_layout.player_flag != 0, std::memory_order_release);
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
        PatternByte pattern[128];
        const auto count =
            decode_pattern(SUBSYSTEM_LOOKUP_SIG.notation, pattern, std::size(pattern));
        const auto match = scan_unique(text_bytes, text.size, pattern, count);
        if (match.hit_count == 1) {
            g_subsystem_layout =
                decode_subsystem_layout(reinterpret_cast<const std::uint8_t*>(match.addr), count);
            g_subsystem_layout_ready.store(g_subsystem_layout.bucket_stride != 0,
                                           std::memory_order_release);
        }
        if (!g_subsystem_layout_ready.load()) {
            OPENDOJO_LOG("signatures: subsystem layout unavailable (%d matches); lookup disabled",
                         match.hit_count);
            ok = false;
        } else {
            OPENDOJO_LOG(
                "signatures: subsystem map=0x%X mask=0x%X sentinel=0x%X buckets=0x%X stride=0x%X",
                g_subsystem_layout.map, g_subsystem_layout.mask, g_subsystem_layout.sentinel,
                g_subsystem_layout.buckets, g_subsystem_layout.bucket_stride);
        }
    }

    if (scan_one(MOVE_WRAPPER_SIG, text_bytes, text.size, g_move_wrapper)) {
        const auto wrapper = g_move_wrapper.load();
        const auto element = decode_rip32(wrapper + 42, 1, 5);
        const auto field = decode_rip32(wrapper + 60, 1, 5);
        auto matches = [&](std::uintptr_t at, const Pattern& pattern) {
            PatternByte bytes[128];
            const auto count = decode_pattern(pattern.notation, bytes, std::size(bytes));
            return count && at >= text.start && at - text.start < text.size &&
                   count <= text.size - (at - text.start) &&
                   match_at(reinterpret_cast<const std::uint8_t*>(at), bytes, count);
        };
        if (matches(element, MOVE_ELEMENT_SIG) && matches(field, MOVE_FIELD_SIG)) {
            g_movelist_layout =
                decode_movelist_layout(reinterpret_cast<const std::uint8_t*>(wrapper), 92,
                                       reinterpret_cast<const std::uint8_t*>(element), 67,
                                       reinterpret_cast<const std::uint8_t*>(field), 27);
            g_movelist_layout_ready.store(g_movelist_layout.element_stride != 0,
                                          std::memory_order_release);
        }
    }
    if (!g_movelist_layout_ready.load()) {
        OPENDOJO_LOG("signatures: movelist layout unavailable; movelist access disabled");
        ok = false;
    } else {
        OPENDOJO_LOG("signatures: movelist human=0x%X stride=0x%X moves=0x%X end=0x%X",
                     g_movelist_layout.human_side, g_movelist_layout.element_stride,
                     g_movelist_layout.move_ids, g_movelist_layout.vector_end);
    }

    if (scan_one(SLOT_FLAGS_SIG, text_bytes, text.size, g_slot_flags_getter)) {
        std::uint32_t field = 0;
        std::memcpy(&field, reinterpret_cast<const void*>(g_slot_flags_getter.load() + 71), 4);
        std::uint32_t index_bias = 0;
        std::memcpy(&index_bias, reinterpret_cast<const void*>(g_slot_flags_getter.load() + 61), 4);
        if (!index_bias && field >= 4 && field <= 0x10000 && field % 4 == 0)
            g_slot_flags.store(field, std::memory_order_release);
    }
    if (!g_slot_flags.load()) {
        OPENDOJO_LOG("signatures: gameplay slot flags unavailable; slot access disabled");
        ok = false;
    } else
        OPENDOJO_LOG("signatures: gameplay slot flags=0x%X", g_slot_flags.load());

    {
        std::atomic<std::uintptr_t> counter{0}, pause{0}, reset{0}, recording{0};
        const bool matched = scan_one(SESSION_COUNTER_SIG, text_bytes, text.size, counter) &
                             scan_one(SUBB_PAUSE_SIG, text_bytes, text.size, pause) &
                             scan_one(SUBC_RESET_SIG, text_bytes, text.size, reset) &
                             scan_one(RECORDING_STATE_SIG, text_bytes, text.size, recording);
        if (matched) {
            const auto item = decode_rip32(reset.load() + 100, 1, 5);
            PatternByte bytes[64];
            const auto count = decode_pattern(SUBC_ITEM_SIG.notation, bytes, std::size(bytes));
            if (item >= text.start && item - text.start < text.size &&
                count <= text.size - (item - text.start) &&
                match_at(reinterpret_cast<const std::uint8_t*>(item), bytes, count)) {
                auto byte = [](std::uintptr_t at) {
                    return *reinterpret_cast<const std::uint8_t*>(at);
                };
                auto word = [](std::uintptr_t at) {
                    std::uint32_t v;
                    std::memcpy(&v, reinterpret_cast<const void*>(at), 4);
                    return v;
                };
                const auto stride = word(reset.load() + 55), base = word(reset.load() + 93);
                RecordingStateLayout l{byte(counter.load() + 78), byte(pause.load() + 12),
                                       base + stride + byte(item + 5), byte(recording.load() + 29)};
                if (l.counter >= 4 && l.counter < 128 && l.counter % 4 == 0 && l.pause &&
                    l.pause < 128 && l.pause == byte(pause.load() + 58) &&
                    l.pause == byte(pause.load() + 72) && base >= 4 && base < 0x10000 &&
                    stride >= 64 && stride < 0x10000 && stride % 8 == 0 && byte(item + 5) < 128 &&
                    byte(item + 5) + 8u <= stride && l.side_record % 4 == 0 &&
                    l.recording_state >= 4 && l.recording_state < 128 &&
                    l.recording_state % 4 == 0 &&
                    l.recording_state == byte(recording.load() + 26) + 4u) {
                    g_recording_state_layout = l;
                    g_recording_state_ready.store(true, std::memory_order_release);
                }
            }
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
        auto unique = [&](const Pattern& pat, const std::uint8_t* begin, std::size_t size) {
            PatternByte bytes[256];
            const auto n = decode_pattern(pat.notation, bytes, std::size(bytes));
            return scan_unique(begin, size, bytes, n).addr;
        };
        const auto jack = unique(CHARACTER_JACK_SIG, text_bytes, text.size);
        const auto alisa = unique(CHARACTER_ALISA_SIG, text_bytes, text.size);
        std::uintptr_t native = 0;
        const auto refresh = g_player_refresh.load();
        DWORD64 image_base = 0;
        const auto unwind = refresh ? RtlLookupFunctionEntry(refresh, &image_base, nullptr)
                                    : nullptr;
        const auto fields = unwind && image_base + unwind->BeginAddress == refresh
                                ? unique(PLAYER_FIELDS_SIG,
                                         reinterpret_cast<const std::uint8_t*>(refresh),
                                         unwind->EndAddress - unwind->BeginAddress)
                                : 0;
        bool native_matches = false;
        if (fields) {
            native = decode_rip32(fields + 93, 1, 5);
            PatternByte pattern[128];
            const auto n = decode_pattern(PLAYER_NATIVE_SIG.notation, pattern, std::size(pattern));
            native_matches = native >= text.start && native - text.start < text.size &&
                             n <= text.size - (native - text.start) &&
                             match_at(reinterpret_cast<const std::uint8_t*>(native), pattern, n);
        }
        if (jack && alisa && fields && native_matches) {
            auto word = [](std::uintptr_t at) {
                std::uint32_t v;
                std::memcpy(&v, reinterpret_cast<const void*>(at), 4);
                return v;
            };
            const auto first = *reinterpret_cast<const std::uint8_t*>(fields + 3);
            const auto again = *reinterpret_cast<const std::uint8_t*>(fields + 105);
            const auto second = *reinterpret_cast<const std::uint8_t*>(fields + 110);
            PlayerLayout l{first, second, word(jack + 11), word(native + 49)};
            if (first >= 8 && first < 128 && first % 8 == 0 && first == again && second >= 8 &&
                second < 128 && second % 8 == 0 && first != second && l.character >= 4 &&
                l.character <= 0x10000 && l.character % 4 == 0 && l.character == word(alisa + 14) &&
                word(jack + 2) == word(alisa + 2) && l.native_bias >= 8 &&
                l.native_bias <= 0x1000 && l.native_bias % 8 == 0) {
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
            std::uint32_t native_flag = 0;
            std::memcpy(&native_flag,
                        reinterpret_cast<const void*>(g_session_finalize.load() + 140), 4);
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
        std::atomic<std::uintptr_t> find{0}, next{0}, invoke{0}, event{0}, names{0};
        const bool matched = scan_one(REFLECTION_FIND_SIG, text_bytes, text.size, find) &
                             scan_one(REFLECTION_NEXT_SIG, text_bytes, text.size, next) &
                             scan_one(REFLECTION_INVOKE_SIG, text_bytes, text.size, invoke) &
                             scan_one(PROCESS_EVENT_SIG, text_bytes, text.size, event) &
                             scan_one(NAME_DECODE_SIG, text_bytes, text.size, names);
        PatternByte pattern[128];
        const auto n = decode_pattern(REFLECTION_STEP_SIG.notation, pattern, std::size(pattern));
        const auto step = scan_unique(text_bytes, text.size, pattern, n).addr;
        std::uintptr_t parms = 0;
        if (event.load()) {
            DWORD64 image_base = 0;
            const auto unwind = RtlLookupFunctionEntry(event.load(), &image_base, nullptr);
            PatternByte bytes[64];
            const auto count = decode_pattern(EVENT_PARMS_SIG.notation, bytes, std::size(bytes));
            if (unwind && image_base + unwind->BeginAddress == event.load())
                parms = scan_unique(reinterpret_cast<const std::uint8_t*>(event.load()),
                                    unwind->EndAddress - unwind->BeginAddress, bytes, count)
                            .addr;
        }
        if (matched && step && parms) {
            auto byte = [](std::uintptr_t at) {
                return *reinterpret_cast<const std::uint8_t*>(at);
            };
            auto word = [](std::uintptr_t at) {
                std::uint32_t v;
                std::memcpy(&v, reinterpret_cast<const void*>(at), 4);
                return v;
            };
            ReflectionLayout l{byte(find.load() + 164),
                               byte(find.load() + 419),
                               byte(event.load() + 68),
                               byte(find.load() + 291),
                               byte(find.load() + 95),
                               byte(find.load() + 425),
                               byte(find.load() + 219),
                               byte(find.load() + 232),
                               byte(step + 65),
                               word(invoke.load() + 117),
                               word(next.load() + 171),
                               byte(names.load() + 87),
                               byte(names.load() + 98),
                               event.load(),
                               decode_rip32(names.load() + 24, 3, 7)};
            l.function_parms_size = word(parms + 3);
            const auto pool_again = decode_rip32(names.load() + 33, 3, 7);
            bool valid = l.function_parms_size == word(parms + 27) && l.function_parms_size >= 8 &&
                         l.function_parms_size <= 0x1000 && l.function_parms_size % 2 == 0 &&
                         l.name_pool == pool_again && memory::is_image_data(l.name_pool, 8) &&
                         l.object_class == byte(find.load() + 363) &&
                         l.object_class == byte(next.load() + 51) &&
                         l.children == byte(next.load() + 152) &&
                         l.children == byte(next.load() + 186) &&
                         l.field_next == byte(next.load() + 99) &&
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
        std::atomic<std::uintptr_t> update{0}, scheduler{0}, thread{0}, free{0}, assign{0},
            valid{0}, get{0};
        bool found = scan_one(RUNTIME_UPDATE_SIG, text_bytes, text.size, update) &
                     scan_one(SCHEDULER_SIG, text_bytes, text.size, scheduler) &
                     scan_one(ENGINE_FREE_SIG, text_bytes, text.size, free) &
                     scan_one(WEAK_ASSIGN_SIG, text_bytes, text.size, assign);
        auto leaf = [&](const Pattern& p, std::atomic<std::uintptr_t>& out) {
            PatternByte bytes[256];
            const auto n = decode_pattern(p.notation, bytes, std::size(bytes));
            const auto match = scan_unique(text_bytes, text.size, bytes, n);
            out.store(match.addr);
            return match.addr != 0;
        };
        found &= leaf(THREAD_ID_SIG, thread) & leaf(WEAK_VALID_SIG, valid) &
                 leaf(WEAK_GET_SIG, get);
        if (found) {
            const auto thread_slot = decode_rip32(thread.load() + 16, 2, 6);
            const auto scheduler_slot = decode_rip32(update.load() + 136, 3, 7);
            const auto thread_iat = decode_rip32(thread.load(), 2, 6);
            // This CALL must really be Windows GetCurrentThreadId, not just any IAT call.
            std::uintptr_t thread_api = 0;
            if (thread_iat >= memory::polaris_base() &&
                thread_iat - memory::polaris_base() < 0x10000000)
                read_pointer_guarded(thread_iat, thread_api);
            if (memory::is_image_data(thread_slot, 4) && memory::is_image_data(scheduler_slot, 8) &&
                thread_api == reinterpret_cast<std::uintptr_t>(&GetCurrentThreadId) &&
                decode_rip32(update.load() + 157, 1, 5) == scheduler.load() &&
                decode_rip32(valid.load() + 15, 2, 6) == decode_rip32(get.load() + 25, 3, 7) &&
                decode_rip32(valid.load() + 36, 3, 7) == decode_rip32(get.load() + 43, 3, 7)) {
                g_runtime_layout = {update.load(), thread_slot,  scheduler_slot, free.load(),
                                    assign.load(), valid.load(), get.load(),     scheduler.load()};
                g_runtime_ready.store(true, std::memory_order_release);
            }
        }
        if (!g_runtime_ready.load()) {
            OPENDOJO_LOG(
                "signatures: game update/UObject contracts unavailable; native operations "
                "disabled");
            ok = false;
        }
    }

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
    // Decode the second pool's own reference; its global need not stay adjacent.
    auto pool2 =
        resolve_data_ref("pool2_ptr", g_pool_init.load(), 0x98, "48 83 3D ?? ?? ?? ?? 00 75", 3, 8);
    if (pool1 && pool2 && pool1 != pool2) {
        g_pool1_ptr.store(pool1, std::memory_order_release);
        g_pool2_ptr.store(pool2, std::memory_order_release);
    } else {
        ok = false;
    }

    {
        PatternByte pattern[128];
        const auto n = decode_pattern(POOL_COPY_SIG.notation, pattern, std::size(pattern));
        const auto copy = scan_unique(text_bytes, text.size, pattern, n).addr;
        PatternByte consumer_pattern[256];
        const auto cn = decode_pattern(POOL_CONSUMER_SIG.notation, consumer_pattern,
                                       std::size(consumer_pattern));
        const auto consumer = scan_unique(text_bytes, text.size, consumer_pattern, cn).addr;
        const auto init = g_pool_init.load();
        auto word = [](std::uintptr_t at) {
            std::uint32_t v;
            std::memcpy(&v, reinterpret_cast<const void*>(at), 4);
            return v;
        };
        // The file codec supports uint16 count + 1800 four-byte events. A native
        // format change requires a codec migration; never reinterpret old files.
        if (copy && consumer && init && pool1 && pool2 &&
            decode_rip32(consumer + 11, 3, 7) == pool1 && g_recording_state_ready.load() &&
            *reinterpret_cast<const std::uint8_t*>(init + 9) + 4u ==
                g_recording_state_layout.recording_state &&
            decode_rip32(init + 29, 3, 7) == pool1 && decode_rip32(init + 36, 3, 7) == pool1 &&
            decode_rip32(init + 53, 3, 7) == pool1 && decode_rip32(init + 96, 3, 7) == pool2 &&
            decode_rip32(init + 103, 3, 7) == pool2 && decode_rip32(init + 120, 3, 7) == pool2 &&
            word(copy + 13) == 7202 && word(copy + 23) == 7202 && word(copy + 37) == 7202 &&
            word(init + 20) == 9 * 7202 && word(init + 64) == 9 * 7202 &&
            word(init + 87) == word(init + 131) && decode_rip32(copy + 27, 3, 7) == pool1 &&
            decode_rip32(copy + 41, 3, 7) == pool1)
            g_live_recordings_supported.store(true, std::memory_order_release);
        else {
            OPENDOJO_LOG(
                "signatures: native recording storage incompatible; live recording access "
                "disabled");
            ok = false;
        }
    }

    {
        auto matches = [&](std::uintptr_t at, const Pattern& pattern) {
            PatternByte bytes[128];
            const auto n = decode_pattern(pattern.notation, bytes, std::size(bytes));
            return n && at >= text.start && at - text.start < text.size &&
                   n <= text.size - (at - text.start) &&
                   match_at(reinterpret_cast<const std::uint8_t*>(at), bytes, n);
        };
        const auto wrapper = g_move_wrapper.load();
        const auto accessor = wrapper ? decode_rip32(wrapper + 15, 1, 5) : 0;
        std::uintptr_t ctx = 0;
        if (matches(accessor, RECORD_POOL_ACCESSOR_SIG)) {
            const auto getter = decode_rip32(accessor + 4, 1, 5);
            const auto lookup = decode_rip32(accessor + 23, 1, 5);
            const auto key_addr = decode_rip32(accessor + 9, 3, 7);
            std::uint32_t key = 0;
            if (memory::is_image_data(key_addr, 4))
                std::memcpy(&key, reinterpret_cast<const void*>(key_addr), 4);
            if (key == 0xA7A8857B && matches(getter, GET_CTX_SIG) &&
                matches(lookup, SUBSYSTEM_LOOKUP_SIG) && g_subsystem_layout_ready.load())
                ctx = decode_rip32(getter, 3, 7);
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
    const auto text = find_text_section();
    auto matches = [&](std::uintptr_t at, const Pattern& pat) {
        PatternByte bytes[128];
        const auto n = decode_pattern(pat.notation, bytes, std::size(bytes));
        return n && at >= text.start && at - text.start < text.size &&
               n <= text.size - (at - text.start) &&
               match_at(reinterpret_cast<const std::uint8_t*>(at), bytes, n);
    };
    if (!matches(enumerate, OBJECT_ENUM_SIG)) return false;
    return matches(decode_rip32(enumerate + 24, 3, 7), OBJECT_APPEND_SIG);
}

bool native_text_abi_supported(std::uintptr_t raw_text) {
    const auto text = find_text_section();
    auto matches = [&](std::uintptr_t at, const Pattern& pat) {
        PatternByte bytes[256];
        const auto n = decode_pattern(pat.notation, bytes, std::size(bytes));
        return n && at >= text.start && at - text.start < text.size &&
               n <= text.size - (at - text.start) &&
               match_at(reinterpret_cast<const std::uint8_t*>(at), bytes, n);
    };
    // Generated wrappers are not globally unique. Validate the actual reflected
    // SetRawText target and the FString copy/resize routines it calls.
    if (!matches(raw_text, RAW_TEXT_EXEC_SIG)) return false;
    const auto copy = decode_rip32(raw_text + 197, 1, 5);
    if (!matches(copy, FSTRING_COPY_SIG)) return false;
    return matches(decode_rip32(copy + 79, 1, 5), FSTRING_RESIZE_SIG);
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
