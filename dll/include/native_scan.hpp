#pragma once
// Bounded instruction views. HDE is the same pinned decoder used by MinHook.
#include <windows.h>
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>
#include "hde64.h"

namespace opendojo::native_scan {
inline bool copy(std::uintptr_t source, void* destination, std::size_t size) {
    __try {
        std::memcpy(destination, reinterpret_cast<const void*>(source), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
struct Instruction {
    std::uintptr_t address{};
    hde64s decoded{};
    std::array<unsigned char, 16> bytes{};
    std::int32_t displacement() const {
        return decoded.flags & F_DISP32  ? static_cast<std::int32_t>(decoded.disp.disp32)
               : decoded.flags & F_DISP8 ? static_cast<std::int8_t>(decoded.disp.disp8)
                                         : 0;
    }
    std::uintptr_t relative() const {
        if (!(decoded.flags & F_RELATIVE)) return 0;
        const auto offset = decoded.flags & F_IMM32 ? static_cast<std::int32_t>(decoded.imm.imm32)
                                                    : static_cast<std::int8_t>(decoded.imm.imm8);
        return address + decoded.len + offset;
    }
    std::uintptr_t rip() const {
        return decoded.modrm_mod == 0 && decoded.modrm_rm == 5 && !decoded.p_67
                   ? address + decoded.len + displacement()
                   : 0;
    }
    bool nop() const {
        return (decoded.opcode == 0x90 && !decoded.rex_b) ||
               (decoded.opcode == 0x0f && decoded.opcode2 == 0x1f);
    }
};
struct Image {
    std::uintptr_t base{}, text{}, end{}, image_end{};
    bool contains(std::uintptr_t at, std::size_t n) const {
        return at >= base && at <= image_end && n <= image_end - at;
    }
    bool code(std::uintptr_t at, std::size_t n = 1) const {
        return at >= text && at <= end && n <= end - at;
    }
};
// Resolve chained unwind records to their primary function, rejecting malformed chains.
inline RUNTIME_FUNCTION root(const Image& image, RUNTIME_FUNCTION fn) {
    for (int depth = 0; depth < 16; ++depth) {
        const auto at = image.base + fn.UnwindData;
        if (!image.contains(at, 4)) return {};
        unsigned char bytes[4]{};
        if (!copy(at, bytes, sizeof(bytes))) return {};
        if ((bytes[0] & 7) != 1 && (bytes[0] & 7) != 2) return {};
        if (!(bytes[0] & (4 << 3))) return fn;
        const auto chain = at + 4 + ((bytes[2] + 1u) & ~1u) * 2;
        if (!image.contains(chain, sizeof(fn))) return {};
        RUNTIME_FUNCTION parent;
        if (!copy(chain, &parent, sizeof(parent))) return {};
        if (parent.BeginAddress >= fn.BeginAddress) return {};
        fn = parent;
    }
    return {};
}
struct Function {
    std::uintptr_t begin{}, end{};
    std::vector<Instruction> instructions;
    explicit operator bool() const { return !instructions.empty(); }
};
inline Function decode(std::uintptr_t begin, std::uintptr_t end) {
    if (end <= begin || end - begin > 16384) return {};
    Function out{begin, end};
    for (auto at = out.begin; at < out.end;) {
        Instruction i;
        i.address = at;
        const auto remaining = out.end - at;
        if (!copy(at, i.bytes.data(), remaining < 16 ? remaining : 16)) return {};
        hde64_disasm(i.bytes.data(), &i.decoded);
        if (!i.decoded.len || i.decoded.flags & F_ERROR || i.decoded.len > remaining) return {};
        out.instructions.push_back(i);
        at += i.decoded.len;
    }
    return out;
}
inline Function function(const Image& image, std::uintptr_t address) {
    if (!image.code(address)) return {};
    DWORD64 image_base = 0;
    const auto entry = RtlLookupFunctionEntry(address, &image_base, nullptr);
    if (!entry || image_base != image.base) return {};
    const auto primary = root(image, *entry);
    if (!primary.EndAddress || primary.EndAddress <= primary.BeginAddress) return {};
    Function out{image.base + primary.BeginAddress, image.base + primary.EndAddress};
    for (int parts = 0; parts < 64; ++parts) {
        DWORD64 next_base = 0;
        const auto next = RtlLookupFunctionEntry(out.end, &next_base, nullptr);
        if (!next || next_base != image.base || image.base + next->BeginAddress != out.end) break;
        const auto parent = root(image, *next);
        if (parent.BeginAddress != primary.BeginAddress) break;
        if (next->EndAddress <= next->BeginAddress) return {};
        out.end = image.base + next->EndAddress;
    }
    if (address < out.begin || address >= out.end || out.end - out.begin > 16384 ||
        !image.code(out.begin, out.end - out.begin))
        return {};
    return decode(out.begin, out.end);
}
// Small helpers often have no unwind entry. Recover only reachable instructions
// inside a strict window; never infer their extent from the next symbol or padding.
inline Function leaf(const Image& image, std::uintptr_t address) {
    if (!image.code(address)) return {};
    std::vector<std::uintptr_t> pending{address};
    Function out{address, address};
    while (!pending.empty()) {
        const auto at = pending.back();
        pending.pop_back();
        if (std::any_of(out.instructions.begin(), out.instructions.end(),
                        [&](const Instruction& i) { return i.address == at; }))
            continue;
        if (at < address || at - address >= 1024 || !image.code(at)) return {};
        Instruction i;
        i.address = at;
        const auto remaining = (std::min)(std::uintptr_t{16}, image.end - at);
        if (!copy(at, i.bytes.data(), remaining)) return {};
        hde64_disasm(i.bytes.data(), &i.decoded);
        const auto& d = i.decoded;
        if (!d.len || d.flags & F_ERROR || d.len > remaining) return {};
        for (const auto& prior : out.instructions)
            if (at < prior.address + prior.decoded.len && prior.address < at + d.len) return {};
        out.instructions.push_back(i);
        out.end = (std::max)(out.end, at + d.len);
        if (d.opcode == 0xc3 || d.opcode == 0xc2 || d.opcode == 0xcc) continue;
        if (d.opcode == 0xff && (d.modrm_reg == 4 || d.modrm_reg == 5))
            continue;  // Tail dispatch ends this helper.
        const bool jump = d.opcode == 0xe9 || d.opcode == 0xeb;
        const bool conditional = (d.opcode >= 0x70 && d.opcode <= 0x7f) ||
                                 (d.opcode == 0x0f && d.opcode2 >= 0x80 && d.opcode2 <= 0x8f);
        if (jump || conditional) pending.push_back(i.relative());
        if (!jump) pending.push_back(at + d.len);
    }
    std::sort(out.instructions.begin(), out.instructions.end(),
              [](const Instruction& a, const Instruction& b) { return a.address < b.address; });
    return out;
}
inline Function helper(const Image& image, std::uintptr_t address) {
    DWORD64 base = 0;
    if (RtlLookupFunctionEntry(address, &base, nullptr)) {
        auto fn = function(image, address);
        return fn.begin == address ? std::move(fn) : Function{};
    }
    return leaf(image, address);
}

// Every returning path after dispatch must encounter the join loop header.
// Cycles are harmless for this check: no completion callback runs until native return.
inline bool reaches_before_exit(const Function& fn, std::uintptr_t from, std::uintptr_t target) {
    std::vector<std::uintptr_t> todo{from}, seen;
    while (!todo.empty()) {
        const auto address = todo.back();
        todo.pop_back();
        if (address == target) continue;
        if (std::find(seen.begin(), seen.end(), address) != seen.end()) continue;
        seen.push_back(address);
        const auto it = std::find_if(fn.instructions.begin(), fn.instructions.end(),
                                     [&](const Instruction& i) { return i.address == address; });
        if (it == fn.instructions.end()) return false;
        const auto& d = it->decoded;
        if (d.opcode == 0xc3 || d.opcode == 0xc2 ||
            (d.opcode == 0xff && (d.modrm_reg == 4 || d.modrm_reg == 5)))
            return false;
        const bool jump = d.opcode == 0xe9 || d.opcode == 0xeb;
        const bool conditional = (d.opcode >= 0x70 && d.opcode <= 0x7f) ||
                                 (d.opcode == 0x0f && d.opcode2 >= 0x80 && d.opcode2 <= 0x8f) ||
                                 (d.opcode >= 0xe0 && d.opcode <= 0xe3);
        if (jump || conditional) todo.push_back(it->relative());
        if (!jump) todo.push_back(it->address + d.len);
    }
    return true;
}

struct Match {
    // Own the result so callers can safely match a temporary decoded function.
    std::vector<Instruction> instructions;
    explicit operator bool() const { return !instructions.empty(); }
    const Instruction& operator[](std::size_t i) const { return instructions.at(i); }
};
// Fragments match at decoded instruction boundaries, skipping alignment NOPs.
// Branch displacements are deliberately wildcards; callers validate relevant targets.
inline Match unique(const Function& fn, std::string_view pattern) {
    std::vector<int> bytes;
    for (std::size_t p = 0; p < pattern.size();) {
        if (pattern[p] == ' ') {
            ++p;
            continue;
        }
        if (p + 1 >= pattern.size()) return {};
        auto hex = [](char c) {
            return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        };
        if (pattern[p] == '?' && pattern[p + 1] == '?')
            bytes.push_back(-1);
        else {
            const auto a = hex(pattern[p]), b = hex(pattern[p + 1]);
            if (a < 0 || b < 0) return {};
            bytes.push_back(a * 16 + b);
        }
        p += 2;
    }
    struct Expected {
        std::vector<int> bytes;
        hde64s decoded;
    };
    std::vector<Expected> expected;
    for (std::size_t at = 0; at < bytes.size();) {
        std::array<unsigned char, 16> raw{};
        for (std::size_t j = 0; j < 16 && at + j < bytes.size(); ++j)
            raw[j] = bytes[at + j] < 0 ? 0 : static_cast<unsigned char>(bytes[at + j]);
        hde64s decoded{};
        hde64_disasm(raw.data(), &decoded);
        if (!decoded.len || decoded.flags & F_ERROR || decoded.len > bytes.size() - at) return {};
        expected.push_back({{bytes.begin() + at, bytes.begin() + at + decoded.len}, decoded});
        at += decoded.len;
    }
    if (expected.empty()) return {};
    Match result;
    for (std::size_t start = 0; start < fn.instructions.size(); ++start) {
        if (fn.instructions[start].nop()) continue;
        Match candidate;
        auto cursor = start;
        for (const auto& instruction : expected) {
            while (cursor < fn.instructions.size() && fn.instructions[cursor].nop())
                ++cursor;
            if (cursor == fn.instructions.size()) break;
            const auto& actual = fn.instructions[cursor++];
            auto condition = [](const hde64s& d) -> int {
                if (d.opcode >= 0x70 && d.opcode <= 0x7f) return d.opcode & 15;
                if (d.opcode == 0x0f && d.opcode2 >= 0x80 && d.opcode2 <= 0x8f)
                    return d.opcode2 & 15;
                if (d.opcode == 0xe9 || d.opcode == 0xeb) return 16;
                return -1;
            };
            const auto branch = condition(instruction.decoded);
            const auto immediate_size = instruction.decoded.flags & F_IMM32 ? 4u : 1u;
            const bool wildcard_branch = branch >= 0 &&
                                         std::all_of(instruction.bytes.end() - immediate_size,
                                                     instruction.bytes.end(),
                                                     [](int byte) { return byte == -1; });
            bool equal = false;
            if (wildcard_branch)
                equal = condition(actual.decoded) == branch;
            else if (actual.decoded.len == instruction.bytes.size()) {
                equal = true;
                for (std::size_t j = 0; j < instruction.bytes.size(); ++j)
                    if (instruction.bytes[j] >= 0 && actual.bytes[j] != instruction.bytes[j]) {
                        equal = false;
                        break;
                    }
            }
            if (!equal) break;
            candidate.instructions.push_back(actual);
        }
        if (candidate.instructions.size() == expected.size()) {
            if (result) return {};  // Ambiguity never selects the first hit.
            result = std::move(candidate);
        }
    }
    return result;
}
}  // namespace opendojo::native_scan
