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
    if (!addr || !n) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    constexpr DWORD READABLE = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & READABLE) == 0) return false;
    if (mbi.Protect & PAGE_GUARD) return false;
    // The region must cover the requested range.
    auto region_end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return addr + n <= region_end;
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
