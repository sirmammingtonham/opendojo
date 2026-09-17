#include <windows.h>

#include "memory.hpp"

#include <cstring>

namespace {

constexpr wchar_t POLARIS_MODULE[] = L"Polaris-Win64-Shipping.exe";

template <typename T>
T read_at(std::uintptr_t addr) {
    if (!addr) return T{};
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(addr), sizeof(T));
    return value;
}

template <typename T>
void write_at(std::uintptr_t addr, T value) {
    if (!addr) return;
    std::memcpy(reinterpret_cast<void*>(addr), &value, sizeof(T));
}

}  // namespace

std::uintptr_t opendojo::memory::polaris_base() {
    return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(POLARIS_MODULE));
}

std::uint64_t opendojo::memory::read_u64(std::uintptr_t addr) {
    return read_at<std::uint64_t>(addr);
}
std::uint32_t opendojo::memory::read_u32(std::uintptr_t addr) {
    return read_at<std::uint32_t>(addr);
}
std::uint16_t opendojo::memory::read_u16(std::uintptr_t addr) {
    return read_at<std::uint16_t>(addr);
}
std::uint8_t opendojo::memory::read_u8(std::uintptr_t addr) {
    return read_at<std::uint8_t>(addr);
}

void opendojo::memory::write_u64(std::uintptr_t addr, std::uint64_t v) {
    write_at(addr, v);
}
void opendojo::memory::write_u32(std::uintptr_t addr, std::uint32_t v) {
    write_at(addr, v);
}
void opendojo::memory::write_u16(std::uintptr_t addr, std::uint16_t v) {
    write_at(addr, v);
}
void opendojo::memory::write_u8(std::uintptr_t addr, std::uint8_t v) {
    write_at(addr, v);
}

void opendojo::memory::read_bytes(std::uintptr_t addr, void* out, std::size_t n) {
    if (!addr || !out || !n) return;
    std::memcpy(out, reinterpret_cast<const void*>(addr), n);
}

bool opendojo::memory::is_readable(std::uintptr_t addr, std::size_t n) {
    if (!addr || !n || n > UINTPTR_MAX - addr) return false;
    const auto end = addr + n;
    while (addr < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) ||
            mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD))
            return false;
        constexpr DWORD READABLE = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                   PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                   PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & READABLE) == 0) return false;
        const auto region = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        if (mbi.RegionSize > UINTPTR_MAX - region) return false;
        const auto next = region + mbi.RegionSize;
        if (next <= addr) return false;
        addr = next;
    }
    return true;
}

bool opendojo::memory::is_image_data(std::uintptr_t addr, std::size_t n) {
    const auto base = polaris_base();
    if (!base || addr < base || !n || n > UINTPTR_MAX - addr) return false;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return false;
    const auto rva = addr - base;
    const auto image_size = nt->OptionalHeader.SizeOfImage;
    if (rva >= image_size || n > image_size - rva) return false;
    const auto sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& section = sections[i];
        if (!(section.Characteristics & IMAGE_SCN_MEM_WRITE) ||
            (section.Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        if (rva >= section.VirtualAddress &&
            rva - section.VirtualAddress < section.Misc.VirtualSize &&
            n <= section.Misc.VirtualSize - (rva - section.VirtualAddress)) {
            return is_readable(addr, n);
        }
    }
    return false;
}

void opendojo::memory::write_bytes(std::uintptr_t addr, const void* src, std::size_t n) {
    if (!addr || !src || !n) return;
    std::memcpy(reinterpret_cast<void*>(addr), src, n);
}

// --- SEH-guarded reads -----------------------------------------------------
// __try/__except can't live in functions that have C++ objects with
// destructors in scope, so these are kept as small leaf helpers operating
// on POD only.

namespace {
template <typename T>
bool try_read_at(std::uintptr_t addr, T* out) {
    if (!out) return false;
    *out = T{};
    if (!addr) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(addr), sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out = T{};
        return false;
    }
}
}  // namespace

bool opendojo::memory::try_read_u64(std::uintptr_t addr, std::uint64_t* out) {
    return try_read_at(addr, out);
}
bool opendojo::memory::try_read_u32(std::uintptr_t addr, std::uint32_t* out) {
    return try_read_at(addr, out);
}
bool opendojo::memory::try_read_u16(std::uintptr_t addr, std::uint16_t* out) {
    return try_read_at(addr, out);
}
bool opendojo::memory::try_read_u8(std::uintptr_t addr, std::uint8_t* out) {
    return try_read_at(addr, out);
}
