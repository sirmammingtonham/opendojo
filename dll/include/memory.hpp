#pragma once

#include <cstddef>
#include <cstdint>

// In-process memory access. We're a DLL injected into the game's address
// space, so reads and writes are plain pointer dereferences — no
// ReadProcessMemory dance needed.
//
// All addresses are absolute (i.e. include Polaris's runtime base).
// Use polaris() to add a module-relative offset.

namespace opendojo::memory {

// Runtime base of Polaris-Win64-Shipping.exe. 0 if the module isn't loaded
// (which only happens if the DLL was somehow injected into the wrong
// process — should never occur via the dinput8 proxy).
std::uintptr_t polaris_base();

// Convenience: polaris_base() + offset, or 0 if base unavailable.
inline std::uintptr_t polaris(std::uintptr_t offset) {
    auto base = polaris_base();
    return base ? base + offset : 0;
}

// Typed reads/writes. addr == 0 reads return 0; addr == 0 writes are no-ops.
// (A 0 addr means "didn't find the upstream pointer" — silently no-oping
// keeps the call sites linear.)
std::uint64_t read_u64(std::uintptr_t addr);
std::uint32_t read_u32(std::uintptr_t addr);
std::uint16_t read_u16(std::uintptr_t addr);
std::uint8_t read_u8(std::uintptr_t addr);

void write_u64(std::uintptr_t addr, std::uint64_t v);
void write_u32(std::uintptr_t addr, std::uint32_t v);
void write_u16(std::uintptr_t addr, std::uint16_t v);
void write_u8(std::uintptr_t addr, std::uint8_t v);

void read_bytes(std::uintptr_t addr, void* out, std::size_t n);
void write_bytes(std::uintptr_t addr, const void* src, std::size_t n);

// VirtualQuery-based check that [addr, addr+n) is committed, readable,
// and not a guard page. Use before dereferencing a pointer we don't
// fully trust (e.g. raw fields that might be stale / garbage).
bool is_readable(std::uintptr_t addr, std::size_t n);

// SEH-guarded reads. If the read AVs (e.g. the source pointer is
// freed memory, which can happen mid-character-swap when the
// GlobalPlayerHolder chain is in flux), `*out` is set to 0 and the
// function returns false. Caller should treat false as "treat the
// chain as unresolvable on this tick" — never as "value is 0".
bool try_read_u64(std::uintptr_t addr, std::uint64_t* out);
bool try_read_u32(std::uintptr_t addr, std::uint32_t* out);
bool try_read_u8(std::uintptr_t addr, std::uint8_t* out);

}  // namespace opendojo::memory
