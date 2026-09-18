#include "practice_rename.hpp"

#include <windows.h>

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "log.hpp"
#include "memory.hpp"
#include "game_thread.hpp"
#include "signatures.hpp"
#include "slot.hpp"
#include "slot_labels.hpp"
#include "subsystems.hpp"

// =============================================================================
// PRACTICE-MENU ROW RENAME  ("CPU Opponent Action N" -> "OpenDojo Action N")
// =============================================================================
//
// Mechanism (see docs/NATIVE_MENU_FINDINGS.md and practice_rename.hpp):
//   - Resolve the UE5 reflection helpers (FindObject<UClass>, GetObjectsOfClass)
//     by pattern, then UPolarisTextBlock + its SetRawText / SetTextID
//     UFunctions and the practice row class' relevant FProperty offsets.
//   - Each (rate-limited) tick: walk live WBP_UI_PracticeMenu_Button_2_C rows
//     and read each row's list_item.Text — a Gryphon text-id, not the visible
//     label. Resolve that id to its label ONCE via GetString and cache the
//     verdict (see g_id_row_cache); later scans are pure pointer reads. For a
//     "CPU Opponent Action N" row, capture the row's two label text blocks
//     (TB_Menu_OFF / TB_Menu_ON) along with the replacement string, and
//     SetRawText them immediately.
//   - Patch UPolarisTextBlock::SetTextID's UFunction.discovered native function field with a shim
//     that, after the original Gryphon resolve runs, re-applies SetRawText for
//     any captured text block — so the rename survives every re-decode and
//     repaints Slate. Event-driven; no per-frame cost once captured.

namespace opendojo::practice_rename {

namespace {

// ----- Pattern compile / scan / RIP-resolve / .text range -------------------

struct CompiledPattern {
    std::uint8_t bytes[64];
    std::uint8_t mask[64];
    std::size_t len = 0;
};

bool compile_pattern(const char* p, CompiledPattern& out) {
    out.len = 0;
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
    while (*p && out.len < sizeof(out.bytes)) {
        while (*p == ' ' || *p == '\t')
            ++p;
        if (!*p) break;
        if (p[0] == '?' && p[1] == '?') {
            out.bytes[out.len] = 0;
            out.mask[out.len] = 0;
            ++out.len;
            p += 2;
        } else {
            int hi, lo;
            if (!hex_nibble(p[0], hi)) return false;
            if (!p[1] || !hex_nibble(p[1], lo)) return false;
            out.bytes[out.len] = static_cast<std::uint8_t>((hi << 4) | lo);
            out.mask[out.len] = 1;
            ++out.len;
            p += 2;
        }
    }
    while (*p == ' ' || *p == '\t')
        ++p;
    return out.len > 0 && *p == '\0';
}

// Walk Polaris's PE section table. `cb(header, start, size)` returns true to
// stop the walk early; for_each_section then returns true as well. Returns
// false if the headers are unreadable or no callback stopped the walk.
template <typename Fn>
bool for_each_section(Fn cb) {
    auto base = memory::polaris_base();
    if (!base) return false;
    auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = reinterpret_cast<PIMAGE_NT_HEADERS64>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    auto first = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& s = first[i];
        if (cb(s, base + s.VirtualAddress, static_cast<std::size_t>(s.Misc.VirtualSize)))
            return true;
    }
    return false;
}

bool get_text_range(std::uintptr_t& start, std::size_t& size) {
    return for_each_section([&](const IMAGE_SECTION_HEADER& s, std::uintptr_t addr, std::size_t n) {
        if (std::strncmp(reinterpret_cast<const char*>(s.Name), ".text", 5) != 0) return false;
        start = addr;
        size = n;
        return true;
    });
}

std::uintptr_t scan(const CompiledPattern& pat, std::uintptr_t start, std::size_t size) {
    if (pat.len == 0 || size < pat.len) return 0;
    const auto base = reinterpret_cast<const std::uint8_t*>(start);
    const std::size_t span = size - pat.len + 1;
    std::uintptr_t hit = 0;
    std::size_t anchor = 0;
    while (anchor < pat.len && !pat.mask[anchor])
        ++anchor;
    if (anchor == pat.len) return 0;
    for (std::size_t i = 0; i < span; ++i) {
        const auto found = static_cast<const std::uint8_t*>(
            std::memchr(base + i + anchor, pat.bytes[anchor], span - i));
        if (!found) break;
        i = static_cast<std::size_t>(found - base) - anchor;
        bool match = true;
        for (std::size_t j = 0; j < pat.len; ++j) {
            if (pat.mask[j] && base[i + j] != pat.bytes[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            if (hit) {
                OPENDOJO_LOG("practice_rename: ambiguous signature; rename disabled");
                return 0;
            }
            hit = start + i;
        }
    }
    return hit;
}

std::uintptr_t rip_relative(std::uintptr_t at) {
    auto disp = static_cast<std::int32_t>(memory::read_u32(at));
    return at + 4 + static_cast<std::uintptr_t>(static_cast<std::int64_t>(disp));
}

// ----- UE5 reflection primitives --------------------------------------------

constexpr const char* PAT_FIND_UNREAL_CLASS = "45 33 C0 49 8B CF E8 ?? ?? ?? ?? 48 8B 4C 24 60";
constexpr const char* PAT_FIND_OBJECTS_OF_CLASS = "E8 ?? ?? ?? ?? 90 48 89 6C 24 30";

using FindUnrealClassFn = void* (*)(void* outer, const wchar_t* name, bool exact_class);

void engine_free(void* pointer) {
    if (!pointer) return;
    const auto address = signatures::runtime_layout().engine_free;
    if (address) reinterpret_cast<void (*)(void*)>(address)(pointer);
}

// The engine owns both the returned arrays and FString return buffers.
// Never release these with the DLL's C runtime allocator.
struct UE_TArrayRaw {
    void* data = nullptr;
    std::int32_t num = 0;
    std::int32_t max = 0;
    UE_TArrayRaw() = default;
    UE_TArrayRaw(const UE_TArrayRaw&) = delete;
    UE_TArrayRaw& operator=(const UE_TArrayRaw&) = delete;
    ~UE_TArrayRaw() { engine_free(data); }
};
static_assert(sizeof(UE_TArrayRaw) == 16);

struct WeakObject {
    std::int32_t index = 0, serial = 0;
    WeakObject() = default;
    explicit WeakObject(void* object) {
        const auto fn = signatures::runtime_layout().weak_assign;
        if (object && fn && game_thread::is_game_thread())
            reinterpret_cast<void (*)(WeakObject*, void*)>(fn)(this, object);
    }
    bool valid() const {
        const auto fn = signatures::runtime_layout().weak_valid;
        return serial > 0 && fn && game_thread::is_game_thread() &&
               reinterpret_cast<bool (*)(const WeakObject*)>(fn)(this);
    }
    void* get() const {
        const auto fn = signatures::runtime_layout().weak_get;
        return fn && valid() ? reinterpret_cast<void* (*)(const WeakObject*)>(fn)(this) : nullptr;
    }
};
static_assert(sizeof(WeakObject) == 8);
std::vector<WeakObject> g_engine_objects;
WeakObject g_bp_object, g_cdo_object;

using FindObjectsOfClassFn = void (*)(void* class_to_look_for, UE_TArrayRaw* out,
                                      bool include_derived, std::uint32_t exclude_object_flags,
                                      std::uint32_t exclude_internal_flags);

// Discover the shared UObject virtual slot from two independent vtables.
std::uint32_t g_process_event_slot = UINT32_MAX;
using ProcessEventFn = void (*)(void* self, void* function, void* parms);

bool read_super(std::uintptr_t cls, std::uint64_t* super) {
    const auto layout = signatures::reflection_layout();
    std::uint64_t vtable = 0, getter = 0;
    if (!layout.super_getter_slot || !memory::try_read_u64(cls, &vtable) ||
        !memory::try_read_u64(vtable + layout.super_getter_slot, &getter))
        return false;
    std::uint8_t code[8]{};
    if (!memory::try_read_bytes(getter, code, sizeof(code))) return false;
    std::uint32_t field = 0;
    if (code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x41 && code[3] < 128 && code[4] == 0xC3)
        field = code[3];
    else if (code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x81 && code[7] == 0xC3)
        std::memcpy(&field, code + 3, 4);
    else
        return false;
    return field >= 8 && field <= 0x1000 && field % 8 == 0 &&
           memory::try_read_u64(cls + field, super);
}

constexpr std::uint32_t RF_CLASS_DEFAULT_OBJECT = 0x10;
constexpr std::uint32_t RF_ARCHETYPE_OBJECT = 0x20;

// Encoding checked against the native name decoder; reject excessive block IDs.
constexpr std::uint32_t FNAME_BLOCK_MASK = 0x1FFF;
constexpr std::uint32_t FNAME_STRIDE_MASK = 0xFFFF;
constexpr std::uint32_t FNAME_BLOCK_SHIFT = 16;

// Resolved FNamePool base (absolute). 0 until resolve_name_pool() runs.
std::uintptr_t g_name_pool = 0;

// A valid pool's blocks[0] points to FName idx 0 = "None"
// (FNameEntry: u16 header {wide:1, hash:5, len:10}, then chars).
bool block0_is_none(std::uintptr_t pool) {
    std::uint64_t b0 = 0;
    if (!memory::try_read_u64(pool + signatures::reflection_layout().name_blocks, &b0) || !b0)
        return false;
    std::uint16_t hdr = 0;
    if (!memory::try_read_u16(static_cast<std::uintptr_t>(b0), &hdr)) return false;
    if ((hdr & 1) != 0) return false;   // not wide
    if ((hdr >> 6) != 4) return false;  // len == 4
    std::uint32_t chars = 0;
    if (!memory::try_read_u32(
            static_cast<std::uintptr_t>(b0) + signatures::reflection_layout().name_text, &chars))
        return false;
    return chars == 0x656E6F4E;  // "None" little-endian
}

// Validate the code-discovered native pool's first entry before use.
void resolve_name_pool() {
    if (g_name_pool) return;
    const auto pool = signatures::reflection_layout().name_pool;
    if (pool && block0_is_none(pool)) {
        g_name_pool = pool;
        OPENDOJO_LOG("practice_rename: FNamePool discovered at 0x%llX",
                     static_cast<unsigned long long>(pool));
    }
}

bool decode_fname(std::uint32_t idx, char* out_buf, std::size_t out_buf_size) {
    if (!out_buf || out_buf_size == 0) return false;
    out_buf[0] = '\0';
    if (idx == 0) {
        if (out_buf_size >= 5) std::memcpy(out_buf, "None", 5);
        return true;
    }
    if (!g_name_pool) return false;
    auto pool = g_name_pool;
    auto block_idx = idx >> FNAME_BLOCK_SHIFT;
    if (block_idx > FNAME_BLOCK_MASK) return false;
    auto stride = idx & FNAME_STRIDE_MASK;
    std::uint64_t block_ptr = 0;
    if (!memory::try_read_u64(pool + signatures::reflection_layout().name_blocks + block_idx * 8,
                              &block_ptr) ||
        !block_ptr)
        return false;
    auto entry = static_cast<std::uintptr_t>(block_ptr) + static_cast<std::uintptr_t>(stride) * 2;
    std::uint16_t header = 0;
    if (!memory::try_read_u16(entry, &header)) return false;
    bool is_wide = (header & 1) != 0;
    std::uint32_t len = header >> 6;
    if (len == 0 || len > 1023) return false;
    std::size_t i;
    for (i = 0; i < len && i + 1 < out_buf_size; ++i) {
        if (is_wide) {
            std::uint16_t w = 0;
            if (!memory::try_read_u16(entry + signatures::reflection_layout().name_text + i * 2,
                                      &w))
                break;
            out_buf[i] = (w > 0x7F) ? '?' : static_cast<char>(w);
        } else {
            std::uint8_t b = 0;
            if (!memory::try_read_u8(entry + signatures::reflection_layout().name_text + i, &b))
                break;
            out_buf[i] = (b > 0x7F) ? '?' : static_cast<char>(b);
        }
    }
    out_buf[i] = '\0';
    return true;
}

void* find_ufunction_by_name(void* uclass, const char* target) {
    if (!uclass) return nullptr;
    auto cls = reinterpret_cast<std::uintptr_t>(uclass);
    for (int depth = 0; depth < 16 && cls; ++depth) {
        std::uint64_t child = 0;
        if (memory::try_read_u64(cls + signatures::reflection_layout().children, &child) && child) {
            auto fn = static_cast<std::uintptr_t>(child);
            int hops = 0;
            char buf[256];
            while (fn && hops++ < 512) {
                std::uint32_t name_idx = 0;
                if (!memory::try_read_u32(fn + signatures::reflection_layout().object_name,
                                          &name_idx))
                    break;
                if (decode_fname(name_idx, buf, sizeof(buf)) && std::strcmp(buf, target) == 0) {
                    return reinterpret_cast<void*>(fn);
                }
                std::uint64_t nxt = 0;
                if (!memory::try_read_u64(fn + signatures::reflection_layout().field_next, &nxt))
                    break;
                fn = static_cast<std::uintptr_t>(nxt);
            }
        }
        std::uint64_t super = 0;
        if (!read_super(cls, &super)) break;
        cls = static_cast<std::uintptr_t>(super);
    }
    return nullptr;
}

std::uintptr_t walk_field_chain_for_name(std::uintptr_t head, std::ptrdiff_t next_off,
                                         const char* target) {
    auto f = head;
    int hops = 0;
    char buf[256];
    while (f && hops++ < 2048) {
        std::uint32_t name_idx = 0;
        if (!memory::try_read_u32(f + signatures::reflection_layout().ffield_name, &name_idx))
            break;
        if (decode_fname(name_idx, buf, sizeof(buf)) && std::strcmp(buf, target) == 0) {
            return f;
        }
        std::uint64_t nxt = 0;
        if (!memory::try_read_u64(f + next_off, &nxt)) break;
        f = static_cast<std::uintptr_t>(nxt);
    }
    return 0;
}

std::int32_t find_fproperty_offset(void* uclass, const char* target) {
    if (!uclass) return -1;
    auto cls = reinterpret_cast<std::uintptr_t>(uclass);
    for (int depth = 0; depth < 16 && cls; ++depth) {
        std::uint64_t head = 0;
        if (memory::try_read_u64(cls + signatures::reflection_layout().child_properties, &head) &&
            head) {
            auto hit = walk_field_chain_for_name(static_cast<std::uintptr_t>(head),
                                                 signatures::reflection_layout().ffield_next,
                                                 target);
            if (hit) {
                std::uint32_t off = 0;
                if (memory::try_read_u32(hit + signatures::reflection_layout().property_offset,
                                         &off)) {
                    return static_cast<std::int32_t>(off);
                }
            }
        }
        std::uint64_t super = 0;
        if (!read_super(cls, &super)) break;
        cls = static_cast<std::uintptr_t>(super);
    }
    return -1;
}

bool seh_call_find_objects(FindObjectsOfClassFn fn, void* cls, std::uint32_t exclude_flags,
                           UE_TArrayRaw* out) {
    __try {
        fn(cls, out, /*include_derived=*/true, exclude_flags, /*exclude_internal_flags=*/0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* find_class_by_name(FindObjectsOfClassFn fo, void* base_cls, const char* target_name) {
    if (!fo || !base_cls || !target_name) return nullptr;
    UE_TArrayRaw results{};
    if (!seh_call_find_objects(fo, base_cls,
                               /*exclude_object_flags=*/0,  // INCLUDE CDOs so we get class ptrs
                               &results))
        return nullptr;
    if (!results.data || results.num <= 0 || results.num > results.max || results.num > 1000000)
        return nullptr;

    auto* arr = reinterpret_cast<std::uint64_t*>(results.data);
    auto target_len = std::strlen(target_name);
    char name_buf[128];
    for (std::int32_t i = 0; i < results.num; ++i) {
        std::uint64_t obj = 0;
        if (!memory::try_read_u64(reinterpret_cast<std::uintptr_t>(&arr[i]), &obj) || !obj)
            continue;
        std::uint64_t cls = 0;
        if (!memory::try_read_u64(obj + signatures::reflection_layout().object_class, &cls) || !cls)
            continue;
        std::uint32_t cls_idx = 0;
        if (!memory::try_read_u32(cls + signatures::reflection_layout().object_name, &cls_idx))
            continue;
        if (!decode_fname(cls_idx, name_buf, sizeof(name_buf))) continue;
        if (std::strncmp(name_buf, target_name, target_len) == 0 && name_buf[target_len] == '\0') {
            return reinterpret_cast<void*>(cls);
        }
    }
    return nullptr;
}

std::vector<void*> find_all_live_objects_of_class(FindObjectsOfClassFn fn, void* cls) {
    std::vector<void*> out;
    if (!fn || !cls) return out;
    UE_TArrayRaw results{};
    if (!seh_call_find_objects(fn, cls, RF_CLASS_DEFAULT_OBJECT | RF_ARCHETYPE_OBJECT, &results)) {
        return out;
    }
    if (!results.data || results.num <= 0 || results.num > results.max || results.num > 1000000)
        return out;
    auto* arr = reinterpret_cast<std::uint64_t*>(results.data);
    out.reserve(results.num);
    for (std::int32_t i = 0; i < results.num; ++i) {
        std::uint64_t obj = 0;
        if (!memory::try_read_u64(reinterpret_cast<std::uintptr_t>(&arr[i]), &obj) || !obj)
            continue;
        out.push_back(reinterpret_cast<void*>(obj));
    }
    return out;
}

// Find the ClassDefaultObject of a class — the call target for static
// (BlueprintFunctionLibrary) UFunctions. Enumerates without excluding the
// CDO and returns the first instance flagged RF_ClassDefaultObject (0x10).
void* find_cdo(FindObjectsOfClassFn fn, void* cls) {
    if (!fn || !cls) return nullptr;
    UE_TArrayRaw results{};
    if (!seh_call_find_objects(fn, cls, /*exclude=*/0, &results)) return nullptr;
    if (!results.data || results.num <= 0 || results.num > results.max || results.num > 1000000)
        return nullptr;
    auto* arr = reinterpret_cast<std::uint64_t*>(results.data);
    for (std::int32_t i = 0; i < results.num; ++i) {
        std::uint64_t obj = 0;
        if (!memory::try_read_u64(reinterpret_cast<std::uintptr_t>(&arr[i]), &obj) || !obj)
            continue;
        std::uint32_t flags = 0;
        if (!memory::try_read_u32(static_cast<std::uintptr_t>(obj) +
                                      signatures::reflection_layout().object_flags,
                                  &flags))
            continue;
        if (flags & RF_CLASS_DEFAULT_OBJECT) return reinterpret_cast<void*>(obj);
    }
    return nullptr;
}

bool resolve_process_event_slot(void* cls, void* function) {
    const auto target = signatures::reflection_layout().process_event;
    std::uint64_t a = 0, b = 0;
    if (!target || !memory::try_read_u64(reinterpret_cast<std::uintptr_t>(cls), &a) ||
        !memory::try_read_u64(reinterpret_cast<std::uintptr_t>(function), &b) || a == b)
        return false;
    std::uint32_t found = UINT32_MAX;
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint64_t x = 0, y = 0;
        if (!memory::try_read_u64(a + i * 8, &x) || !memory::try_read_u64(b + i * 8, &y)) break;
        if (x == target && y == target) {
            if (found != UINT32_MAX) return false;
            found = i;
        }
    }
    g_process_event_slot = found;
    return found != UINT32_MAX;
}

ProcessEventFn pe_from_self(void* self) {
    if (g_process_event_slot == UINT32_MAX) return nullptr;
    std::uint64_t vtable = 0, fn = 0;
    if (!memory::try_read_u64(reinterpret_cast<std::uintptr_t>(self), &vtable) ||
        !memory::try_read_u64(vtable + g_process_event_slot * 8, &fn))
        return nullptr;
    return reinterpret_cast<ProcessEventFn>(fn);
}

// ----- FString helpers ------------------------------------------------------

struct UE_FString {
    wchar_t* data = nullptr;
    std::int32_t num = 0;  // includes NUL
    std::int32_t max = 0;
};

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Fill a UE_FString parm from `text`. ProcessEvent memcpy's the parm block
// into its own frame and the callee copies the FString from there, so the
// buffer stays ours: free it with free_fstring() after the call returns.
void make_fstring(UE_FString& out, const wchar_t* text) {
    if (!text) text = L"";
    std::size_t n = std::wcslen(text) + 1;
    auto* buf = static_cast<wchar_t*>(std::malloc(n * sizeof(wchar_t)));
    if (!buf) {
        out = {};
        return;
    }
    std::memcpy(buf, text, n * sizeof(wchar_t));
    out.data = buf;
    out.num = static_cast<std::int32_t>(n);
    out.max = static_cast<std::int32_t>(n);
}

void free_fstring(UE_FString& s) {
    std::free(s.data);
    s = {};
}

// Read an FString { wchar_t* data; int32 num; } at `addr` into an ASCII
// buffer (non-ASCII chars become '?'). Returns the character count, or -1
// on failure. `num` includes the trailing NUL.
std::int32_t read_fstring_ascii(std::uintptr_t addr, char* out, std::size_t cap) {
    if (!out || cap == 0) return -1;
    out[0] = '\0';
    std::uint64_t data = 0;
    std::uint32_t num = 0;
    if (!memory::try_read_u64(addr, &data) || !data) return -1;
    if (!memory::try_read_u32(addr + offsetof(UE_FString, num), &num) || num <= 0) return -1;
    std::size_t chars = static_cast<std::size_t>(num) - 1;  // drop NUL
    std::size_t i;
    for (i = 0; i < chars && i + 1 < cap; ++i) {
        std::uint16_t ch = 0;
        if (!memory::try_read_u16(static_cast<std::uintptr_t>(data) + i * 2, &ch)) break;
        out[i] = (ch > 0x7F) ? '?' : static_cast<char>(ch);
    }
    out[i] = '\0';
    return static_cast<std::int32_t>(i);
}

// =============================================================================
// Resolution state.
// =============================================================================

struct Resolved {
    FindUnrealClassFn find_class = nullptr;
    FindObjectsOfClassFn find_objects_of_class = nullptr;

    void* cls_user_widget = nullptr;  // /Script/UMG.UserWidget
    void* cls_text_block = nullptr;   // /Script/Polaris.PolarisTextBlock
    void* cls_button_row = nullptr;   // WBP_UI_PracticeMenu_Button_1_C

    void* ufn_set_raw_text = nullptr;  // UPolarisTextBlock::SetRawText
    void* ufn_set_text_id = nullptr;   // UPolarisTextBlock::SetTextID

    void* cls_gryphon = nullptr;     // /Script/Polaris.GryphonFunctionLibrary
    void* ufn_get_string = nullptr;  // UGryphonFunctionLibrary::GetString
    void* gryphon_cdo = nullptr;     // CDO to ProcessEvent the static call on

    std::int32_t off_list_item = -1;
    std::int32_t off_item_text = -1;
    std::int32_t off_tb_menu_off = -1;
    std::int32_t off_tb_menu_on = -1;

    bool engine_ok = false;
    bool bp_full_ok = false;
};

Resolved g_r;

// Source prefix we rewrite, and the replacement prefix that takes its place.
// The trailing remainder ("  N") of the original label is preserved verbatim.
constexpr const char* SRC_PREFIX = "CPU Opponent Action";

// Text-id -> row verdict cache.
//
// A row's list_item.Text is a Gryphon text-id ("TEXT_000_UI_PRACTICE_001"):
// a stable key, identical on every client. The resolved LABEL is neither —
// it costs one ProcessEvent per row per scan, and it is localized, so
// SRC_PREFIX only matches on an English client.
//
// So resolve each id at most once and remember the answer:
//     value >= 1  this id is "CPU Opponent Action N", N = value
//     value == 0  this id is some other row — skip it, do not re-resolve
//
// After the first scan the menu costs zero GetString calls. Ids are plain
// strings, not UObject pointers, so the cache stays valid across GC and
// across practice re-entry. Touched only from scan_and_apply_rows (one
// thread), so it needs no lock.
//
// This does not by itself make the feature work on a non-English client:
// learning an id still needs one successful English prefix match. It does
// make the fix cheap — the log line below prints the real ids, so the
// mapping can become a hardcoded, locale-independent table later.
std::unordered_map<std::string, int> g_id_row_cache;

std::once_flag g_code_once;

void resolve_code() {
    if (!signatures::reflection_layout().native_function ||
        !signatures::runtime_layout().engine_free)
        return;
    std::uintptr_t ts, sz;
    if (!get_text_range(ts, sz)) {
        OPENDOJO_LOG("practice_rename: .text range unavailable");
        return;
    }
    CompiledPattern p1, p2;
    if (!compile_pattern(PAT_FIND_UNREAL_CLASS, p1)) return;
    if (!compile_pattern(PAT_FIND_OBJECTS_OF_CLASS, p2)) return;
    auto h1 = scan(p1, ts, sz);
    auto h2 = scan(p2, ts, sz);
    if (!h1 || !h2) {
        OPENDOJO_LOG("practice_rename: pattern scan miss (fuc=%d foc=%d)", h1 ? 1 : 0, h2 ? 1 : 0);
        return;
    }
    const auto find_class = rip_relative(h1 + 7);
    const auto find_objects = rip_relative(h2 + 1);
    if (!signatures::native_object_array_abi_supported(find_objects)) {
        OPENDOJO_LOG("practice_rename: native object-array ABI unavailable; rename disabled");
        return;
    }
    for (auto target : {find_class, find_objects}) {
        DWORD64 image_base = 0;
        const auto function = RtlLookupFunctionEntry(target, &image_base, nullptr);
        if (target < ts || target - ts >= sz || !function ||
            image_base + function->BeginAddress != target) {
            OPENDOJO_LOG("practice_rename: rejected call target outside a .text function entry");
            return;
        }
    }
    g_r.find_class = reinterpret_cast<FindUnrealClassFn>(find_class);
    g_r.find_objects_of_class = reinterpret_cast<FindObjectsOfClassFn>(find_objects);
}

void do_resolve() {
    std::call_once(g_code_once, resolve_code);
    if (!g_r.find_class || !g_r.find_objects_of_class) return;
    resolve_name_pool();
    if (!g_name_pool) {
        OPENDOJO_LOG("practice_rename: aborting resolve - no FNamePool");
        return;
    }
    g_r.cls_user_widget = g_r.find_class(nullptr, L"/Script/UMG.UserWidget", true);
    g_r.cls_text_block = g_r.find_class(nullptr, L"/Script/Polaris.PolarisTextBlock", true);
    g_r.ufn_set_raw_text = find_ufunction_by_name(g_r.cls_text_block, "SetRawText");
    g_r.ufn_set_text_id = find_ufunction_by_name(g_r.cls_text_block, "SetTextID");

    g_r.cls_gryphon =
        g_r.find_class(nullptr, L"/Script/GryphonLocalization.GryphonFunctionLibrary", true);
    g_r.ufn_get_string = find_ufunction_by_name(g_r.cls_gryphon, "GetString");

    std::uint16_t raw_parms = 0, get_parms = 0;
    std::uint64_t raw_native = 0;
    const auto layout = signatures::reflection_layout();
    const bool parms_ok =
        layout.function_parms_size && g_r.ufn_set_raw_text && g_r.ufn_get_string &&
        memory::try_read_u16(reinterpret_cast<std::uintptr_t>(g_r.ufn_set_raw_text) +
                                 layout.function_parms_size,
                             &raw_parms) &&
        memory::try_read_u16(reinterpret_cast<std::uintptr_t>(g_r.ufn_get_string) +
                                 layout.function_parms_size,
                             &get_parms) &&
        memory::try_read_u64(reinterpret_cast<std::uintptr_t>(g_r.ufn_set_raw_text) +
                                 layout.native_function,
                             &raw_native) &&
        signatures::native_text_abi_supported(raw_native) && raw_parms == 17 && get_parms == 32;
    g_r.engine_ok = parms_ok && g_r.find_class && g_r.find_objects_of_class &&
                    g_r.cls_user_widget && g_r.cls_text_block && g_r.ufn_set_raw_text &&
                    g_r.ufn_set_text_id && g_r.cls_gryphon && g_r.ufn_get_string &&
                    resolve_process_event_slot(g_r.cls_text_block, g_r.ufn_set_raw_text) &&
                    find_fproperty_offset(g_r.ufn_set_raw_text, "raw_text") == 0 &&
                    find_fproperty_offset(g_r.ufn_set_raw_text, "ReplaceUnsupportedCharacter") ==
                        16 &&
                    find_fproperty_offset(g_r.ufn_get_string, "textId") == 0 &&
                    find_fproperty_offset(g_r.ufn_get_string, "ReturnValue") == 16;

    g_engine_objects.clear();
    if (g_r.engine_ok) {
        for (auto object : {g_r.cls_user_widget, g_r.cls_text_block, g_r.ufn_set_raw_text,
                            g_r.ufn_set_text_id, g_r.cls_gryphon, g_r.ufn_get_string}) {
            g_engine_objects.emplace_back(object);
            if (!g_engine_objects.back().valid()) g_r.engine_ok = false;
        }
    }
    OPENDOJO_LOG(
        "practice_rename: engine resolve %s — find_class=0x%llX "
        "find_objects=0x%llX uw_cls=0x%llX tb_cls=0x%llX "
        "set_raw_text=0x%llX set_text_id=0x%llX "
        "gryphon_cls=0x%llX get_string=0x%llX",
        g_r.engine_ok ? "OK" : "FAILED", reinterpret_cast<unsigned long long>(g_r.find_class),
        reinterpret_cast<unsigned long long>(g_r.find_objects_of_class),
        reinterpret_cast<unsigned long long>(g_r.cls_user_widget),
        reinterpret_cast<unsigned long long>(g_r.cls_text_block),
        reinterpret_cast<unsigned long long>(g_r.ufn_set_raw_text),
        reinterpret_cast<unsigned long long>(g_r.ufn_set_text_id),
        reinterpret_cast<unsigned long long>(g_r.cls_gryphon),
        reinterpret_cast<unsigned long long>(g_r.ufn_get_string));
}

// BP classes resolve lazily (loaded only after the user enters practice mode).
int g_bp_scan_countdown = 0;

void try_resolve_bp_classes() {
    if (g_r.bp_full_ok || !g_r.engine_ok) return;
    if (g_bp_scan_countdown > 0) {
        --g_bp_scan_countdown;
        return;
    }
    g_bp_scan_countdown = 60;

    if (!g_r.cls_button_row) {
        // Right-panel option rows (incl. "CPU Opponent Action N") are
        // WBP_UI_PracticeMenu_Button_2_C; Button_1 is the left menu list.
        g_r.cls_button_row = find_class_by_name(g_r.find_objects_of_class, g_r.cls_user_widget,
                                                "WBP_UI_PracticeMenu_Button_2_C");
        if (g_r.cls_button_row)
            OPENDOJO_LOG("practice_rename: cls_button_row = 0x%llX",
                         reinterpret_cast<unsigned long long>(g_r.cls_button_row));
    }
    if (!g_r.cls_button_row) {
        OPENDOJO_LOG(
            "practice_rename: WBP_UI_PracticeMenu_Button_2_C not live "
            "yet — open the practice pause menu once");
        return;
    }

    auto try_prop = [](std::int32_t& slot, void* cls, const char* name) {
        if (slot >= 0) return;
        slot = find_fproperty_offset(cls, name);
        if (slot >= 0) OPENDOJO_LOG("practice_rename: prop '%s' offset = %d", name, slot);
    };
    try_prop(g_r.off_list_item, g_r.cls_button_row, "list_item");
    try_prop(g_r.off_tb_menu_off, g_r.cls_button_row, "TB_Menu_OFF");
    try_prop(g_r.off_tb_menu_on, g_r.cls_button_row, "TB_Menu_ON");

    // item_text offset: discover the item class from a live row's
    // list_item.ClassPrivate, then resolve its "Text" FProperty.
    if (g_r.off_item_text < 0 && g_r.off_list_item >= 0) {
        auto rows = find_all_live_objects_of_class(g_r.find_objects_of_class, g_r.cls_button_row);
        for (auto* row : rows) {
            std::uint64_t item = 0;
            if (!memory::try_read_u64(reinterpret_cast<std::uintptr_t>(row) + g_r.off_list_item,
                                      &item) ||
                !item)
                continue;
            std::uint64_t cls = 0;
            if (!memory::try_read_u64(static_cast<std::uintptr_t>(item) +
                                          signatures::reflection_layout().object_class,
                                      &cls) ||
                !cls)
                continue;
            g_r.off_item_text = find_fproperty_offset(reinterpret_cast<void*>(cls), "Text");
            if (g_r.off_item_text >= 0)
                OPENDOJO_LOG("practice_rename: prop 'Text' (on item) offset = %d",
                             g_r.off_item_text);
            break;
        }
    }

    if (g_r.off_list_item >= 0 && g_r.off_item_text >= 0 && g_r.off_tb_menu_off >= 0 &&
        g_r.off_tb_menu_on >= 0) {
        g_bp_object = WeakObject(g_r.cls_button_row);
        g_r.bp_full_ok = g_bp_object.valid();
        OPENDOJO_LOG("practice_rename: BP-class resolution COMPLETE");
    } else {
        OPENDOJO_LOG(
            "practice_rename: BP resolve incomplete — "
            "list_item=%d item_text=%d tb_off=%d tb_on=%d "
            "(item_text needs a live row; open the practice menu)",
            g_r.off_list_item, g_r.off_item_text, g_r.off_tb_menu_off, g_r.off_tb_menu_on);
    }
}

// =============================================================================
// Capture list + SetRawText.
// =============================================================================
//
// A captured entry binds a text-block instance to the wide replacement string
// it should always display. Accessed on the game thread by tick and
// the game thread (SetTextID shim), so guarded by a mutex.

struct Capture {
    WeakObject tb;
    std::wstring replacement;
    std::wstring original_id;
};

std::vector<Capture> g_caps;
std::mutex g_caps_mtx;

bool call_process_event(void* self, void* function, void* parms) {
    if (!game_thread::is_game_thread()) return false;
    const auto fn = pe_from_self(self);
    if (!fn || !function) return false;
    __try {
        fn(self, function, parms);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void call_set_raw_text(void* tb, const wchar_t* text) {
    struct {
        UE_FString RawText;
        bool ReplaceUnsupportedChar;
        std::uint8_t _pad[7];
    } parms{};
    make_fstring(parms.RawText, text);
    parms.ReplaceUnsupportedChar = false;
    if (!call_process_event(tb, g_r.ufn_set_raw_text, &parms)) {
        static std::atomic<int> n{0};
        if (n.fetch_add(1) < 3) OPENDOJO_LOG("practice_rename: SetRawText failed");
    }
    free_fstring(parms.RawText);
}

// Resolve a Gryphon text-id to its display string via
// UGryphonFunctionLibrary::GetString(FString TextID) -> FString. Writes the
// ASCII result into `out`; returns char count or -1 on failure.
std::int32_t call_get_string(const wchar_t* text_id, char* out, std::size_t cap) {
    if (!g_r.gryphon_cdo || !g_r.ufn_get_string) return -1;
    struct {
        UE_FString TextID;
        UE_FString ReturnValue;
    } parms{};
    struct ReturnOwner {
        UE_FString& value;
        ~ReturnOwner() { engine_free(value.data); }
    } returned{parms.ReturnValue};
    make_fstring(parms.TextID, text_id);
    const bool ok = call_process_event(g_r.gryphon_cdo, g_r.ufn_get_string, &parms);
    free_fstring(parms.TextID);
    if (!ok) return -1;
    return read_fstring_ascii(reinterpret_cast<std::uintptr_t>(&parms.ReturnValue), out, cap);
}

// =============================================================================
// SetTextID UFunction.Func patch — re-applies the rename after Gryphon
// re-resolves a captured text block (hover/focus/reopen). Event-driven.
// =============================================================================

using NativeFuncFn = void (*)(void* self, void* frame, void* result);

std::atomic<NativeFuncFn> g_set_text_id_orig{nullptr};
std::atomic<bool> g_set_text_id_hooked{false};

void set_text_id_shim(void* self, void* frame, void* result) {
    if (const auto original = g_set_text_id_orig.load(std::memory_order_acquire))
        original(self, frame, result);
    // The Func patch is global and permanent, but captures only make sense
    // inside practice: outside it any surviving pointer is dangling and can
    // alias a recycled text block (rematch screen), stamping a slot label
    // onto an unrelated widget.
    static thread_local bool reapplying = false;
    if (reapplying || !game_thread::is_game_thread() || !opendojo::subsystems::in_practice() ||
        !g_r.engine_ok)
        return;
    struct ReapplyGuard {
        bool& flag;
        ReapplyGuard(bool& f) : flag(f) { flag = true; }
        ~ReapplyGuard() { flag = false; }
    } guard(reapplying);
    // After the original Gryphon resolve runs, restore our label for any
    // captured text block.
    std::wstring repl;
    {
        std::lock_guard<std::mutex> lk(g_caps_mtx);
        for (const auto& c : g_caps) {
            if (c.tb.get() == self) {
                repl = c.replacement;
                break;
            }
        }
    }
    if (!repl.empty()) {
        call_set_raw_text(self, repl.c_str());
        static std::atomic<int> n{0};
        if (n.fetch_add(1) < 8)
            OPENDOJO_LOG("practice_rename: SetTextID re-apply on tb=0x%p", self);
    }
}

bool install_set_text_id_hook() {
    if (g_set_text_id_hooked.load()) return true;
    if (!g_r.ufn_set_text_id) return false;
    auto func_slot = reinterpret_cast<std::uintptr_t>(g_r.ufn_set_text_id) +
                     signatures::reflection_layout().native_function;
    std::uint64_t orig = 0;
    if (!memory::try_read_u64(func_slot, &orig) || !orig) {
        OPENDOJO_LOG("practice_rename: SetTextID Func slot unreadable");
        return false;
    }
    const auto saved_original = g_set_text_id_orig.load(std::memory_order_acquire);
    if (orig == reinterpret_cast<std::uintptr_t>(&set_text_id_shim)) {
        // Rediscovery can find the same already-patched UFunction. Never make
        // the shim its own original: the next native call would recurse forever.
        g_set_text_id_hooked.store(saved_original != nullptr);
        return saved_original != nullptr;
    }
    if (saved_original && saved_original != reinterpret_cast<NativeFuncFn>(orig)) {
        // Previously patched objects may still invoke this shared shim.
        OPENDOJO_LOG("practice_rename: replacement SetTextID thunk changed; hook skipped");
        return false;
    }
    g_set_text_id_orig.store(reinterpret_cast<NativeFuncFn>(orig), std::memory_order_release);
    DWORD oldp = 0;
    if (func_slot % alignof(void*) != 0 ||
        !VirtualProtect(reinterpret_cast<void*>(func_slot), sizeof(void*), PAGE_READWRITE, &oldp)) {
        OPENDOJO_LOG("practice_rename: cannot protect SetTextID Func slot");
        return false;
    }
    const auto previous =
        InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(func_slot),
                                          reinterpret_cast<void*>(&set_text_id_shim),
                                          reinterpret_cast<void*>(orig));
    DWORD ignored = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(func_slot), sizeof(void*), oldp, &ignored))
        OPENDOJO_LOG("practice_rename: failed to restore SetTextID Func page protection");
    if (previous != reinterpret_cast<void*>(orig)) {
        OPENDOJO_LOG("practice_rename: SetTextID Func changed during installation; hook skipped");
        return false;
    }
    g_set_text_id_hooked.store(true);
    OPENDOJO_LOG(
        "practice_rename: SetTextID Func patched "
        "(slot=0x%llX orig=0x%llX shim=0x%llX)",
        static_cast<unsigned long long>(func_slot), static_cast<unsigned long long>(orig),
        reinterpret_cast<unsigned long long>(&set_text_id_shim));
    return true;
}

// =============================================================================
// Row scan — capture CPU-action rows and apply the rename.
// =============================================================================

int g_row_scan_countdown = 0;

void scan_and_apply_rows() {
    if (g_row_scan_countdown > 0) {
        --g_row_scan_countdown;
        return;
    }
    g_row_scan_countdown = 30;  // ~0.5s @60fps — GetObjectsOfClass is not free

    // Gryphon CDO is the call target for GetString; resolve lazily.
    if (!g_r.gryphon_cdo) {
        g_r.gryphon_cdo = find_cdo(g_r.find_objects_of_class, g_r.cls_gryphon);
        if (!g_r.gryphon_cdo) {
            static std::atomic<int> n{0};
            if (n.fetch_add(1) < 4) OPENDOJO_LOG("practice_rename: Gryphon CDO not resolved yet");
            return;
        }
        g_cdo_object = WeakObject(g_r.gryphon_cdo);
        if (!g_cdo_object.valid()) {
            g_r.gryphon_cdo = nullptr;
            return;
        }
        OPENDOJO_LOG("practice_rename: gryphon_cdo = 0x%p", g_r.gryphon_cdo);
    }

    auto rows = find_all_live_objects_of_class(g_r.find_objects_of_class, g_r.cls_button_row);
    if (rows.empty()) return;

    std::vector<Capture> fresh;
    char id_buf[128];
    char label_buf[160];
    const auto plen = std::strlen(SRC_PREFIX);

    for (auto* row : rows) {
        auto raddr = reinterpret_cast<std::uintptr_t>(row);
        std::uint64_t item = 0;
        if (!memory::try_read_u64(raddr + g_r.off_list_item, &item) || !item) continue;

        // item.Text holds the Gryphon text-id (FString); read it.
        if (read_fstring_ascii(static_cast<std::uintptr_t>(item) + g_r.off_item_text, id_buf,
                               sizeof(id_buf)) < 0)
            continue;

        // Only the "CPU Opponent Action N" rows map to recording slots.
        // Consult the cache first; resolve the label only for an id we have
        // never seen.
        int row_n = 0;
        auto cached = g_id_row_cache.find(id_buf);
        if (cached != g_id_row_cache.end()) {
            row_n = cached->second;
        } else {
            wchar_t id_w[128];
            std::size_t k = 0;
            for (; id_buf[k] && k + 1 < 128; ++k)
                id_w[k] = static_cast<wchar_t>(static_cast<unsigned char>(id_buf[k]));
            id_w[k] = L'\0';

            // A failed resolve is transient (Gryphon not ready yet), so do
            // NOT cache it — that would blacklist a real row permanently.
            if (call_get_string(id_w, label_buf, sizeof(label_buf)) < 0) continue;

            if (std::strncmp(label_buf, SRC_PREFIX, plen) == 0) {
                int n = std::atoi(label_buf + plen);  // " 5" -> 5
                if (n >= 1 && n <= static_cast<int>(slot_labels::COUNT)) row_n = n;
            }
            g_id_row_cache.emplace(id_buf, row_n);
            if (row_n > 0)
                OPENDOJO_LOG("practice_rename: text-id '%s' -> CPU Opponent Action %d", id_buf,
                             row_n);
        }
        if (row_n < 1) continue;

        // Slot N's custom name (from the loaded drill). Empty => leave the
        // original game label untouched.
        std::size_t slot = static_cast<std::size_t>(row_n - 1);
        std::string name = slot_labels::get(slot);
        if (name.empty()) continue;

        // Skip un-populated slots, but never permanent-clear the stored name:
        // a transient empty read during a scene rebuild would otherwise destroy
        // the label for good. Skipping drops the slot from the rebuilt capture
        // list (so the SetTextID shim stops re-stamping a genuinely-cleared
        // row); the label re-applies on the next scan once the slot repopulates.
        if (!opendojo::slot::is_populated(slot)) continue;
        std::wstring repl = utf8_to_wide(name);
        if (repl.empty()) continue;

        std::uint64_t tb_off = 0, tb_on = 0;
        memory::try_read_u64(raddr + g_r.off_tb_menu_off, &tb_off);
        memory::try_read_u64(raddr + g_r.off_tb_menu_on, &tb_on);
        const std::wstring original_id(id_buf, id_buf + std::strlen(id_buf));
        if (tb_off)
            fresh.push_back({WeakObject(reinterpret_cast<void*>(tb_off)), repl, original_id});
        if (tb_on) fresh.push_back({WeakObject(reinterpret_cast<void*>(tb_on)), repl, original_id});
    }

    // Publish unconditionally (even when empty): a slot that became
    // un-populated must fall out of g_caps so the shim stops re-stamping it.
    // Apply now so the rename shows without waiting for the next Gryphon
    // resolve.
    //
    // Take a copy under the lock and apply from the copy with the lock
    // RELEASED. call_set_raw_text enters ProcessEvent, which re-enters
    // SetTextID and therefore set_text_id_shim, which locks g_caps_mtx
    // itself. std::mutex is not recursive, so applying while holding the
    // lock self-deadlocks on that path — and blocks the other thread
    // (game vs render) for the whole apply loop even when it does not.
    std::vector<Capture> snapshot;
    {
        std::lock_guard<std::mutex> lk(g_caps_mtx);
        g_caps.swap(fresh);
        snapshot = g_caps;
    }
    // Removed labels must restore the game's localized text immediately.
    // Publish first so our SetTextID hook cannot reapply the removed label.
    for (const auto& previous : fresh) {
        auto* tb = previous.tb.get();
        if (!tb) continue;
        const bool retained = std::any_of(snapshot.begin(), snapshot.end(),
                                          [tb](const Capture& c) { return c.tb.get() == tb; });
        if (retained) continue;
        struct {
            UE_FString TextID;
        } parms{};
        make_fstring(parms.TextID, previous.original_id.c_str());
        call_process_event(tb, g_r.ufn_set_text_id, &parms);
        free_fstring(parms.TextID);
    }
    for (const auto& c : snapshot)
        if (auto* tb = c.tb.get()) call_set_raw_text(tb, c.replacement.c_str());
    const std::size_t applied = snapshot.size();
    static std::atomic<bool> announced{false};
    if (!announced.exchange(true))
        OPENDOJO_LOG("practice_rename: applied rename to %zu text blocks", applied);
}

}  // anonymous namespace

// =============================================================================
// Public API.
// =============================================================================

// Called on the not-in-practice -> in-practice edge (from render_hook, which
// polls in_practice every frame). WBP_UI_PracticeMenu_Button_2_C is a Blueprint
// UClass the engine reloads across a match (the old class is GC'd), and the
// Gryphon CDO is likewise a per-session UObject. Our cached cls_button_row then
// points at a dead class, find_all_live_objects returns 0 rows, and the rename
// silently stops — surviving even a drill reload. Drop those cached pointers so
// the lazy resolver re-finds them when the menu next goes live. FProperty
// offsets are resolved again with the replacement Blueprint class.
void on_practice_reentry() {
    on_practice_exit();
    g_r.cls_button_row = nullptr;
    g_r.gryphon_cdo = nullptr;
    g_r.bp_full_ok = false;
    g_bp_object = {};
    g_cdo_object = {};
    g_r.off_list_item = g_r.off_item_text = g_r.off_tb_menu_off = g_r.off_tb_menu_on = -1;
    g_bp_scan_countdown = 0;  // re-resolve on the next tick, no throttle wait
    OPENDOJO_LOG(
        "practice_rename: practice re-entry — invalidated "
        "cls_button_row + gryphon_cdo for re-resolve");
}

// Called on the in-practice -> not-in-practice edge (from the native update dispatcher).
// The captured text blocks are about to be (or already are) GC'd; keeping
// the pointers risks address-reuse matches in the SetTextID shim. Re-entry
// rebuilds the list from live rows, so nothing is lost.
void on_practice_exit() {
    std::lock_guard<std::mutex> lk(g_caps_mtx);
    if (!g_caps.empty()) {
        g_caps.clear();
        OPENDOJO_LOG("practice_rename: practice exit — cleared captured text blocks");
    }
}

bool prepare_code() {
    std::call_once(g_code_once, resolve_code);
    return g_r.find_class && g_r.find_objects_of_class;
}

void tick() {
    if (!game_thread::is_current()) return;
    if (g_r.engine_ok) {
        for (const auto& object : g_engine_objects) {
            if (!object.valid()) {
                g_r.engine_ok = false;
                g_set_text_id_hooked.store(false);
                break;
            }
        }
    }
    if (!g_r.engine_ok) {
        static int retry = 0;
        if (retry-- > 0) return;
        retry = 60;
        on_practice_reentry();
        do_resolve();
    }
    if (g_r.bp_full_ok && !g_bp_object.valid()) on_practice_reentry();
    if (g_r.gryphon_cdo && !g_cdo_object.valid()) g_r.gryphon_cdo = nullptr;
    if (!g_r.engine_ok) return;

    if (!g_r.bp_full_ok) {
        try_resolve_bp_classes();
        if (!g_r.bp_full_ok) return;
    }

    if (!g_set_text_id_hooked.load()) install_set_text_id_hook();
    scan_and_apply_rows();
}

}  // namespace opendojo::practice_rename
