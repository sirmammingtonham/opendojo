// Map a supplied game executable as inert data and run the actual startup resolver.
// No game instructions execute. Unwind entries are registered only for boundary checks.
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "../src/signatures.cpp"

namespace {
std::uintptr_t game_image = 0;
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
}  // namespace
namespace opendojo::log {
void format(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::puts("");
}
}  // namespace opendojo::log
namespace opendojo::memory {
std::uintptr_t polaris_base() {
    return game_image;
}
bool is_image_data(std::uintptr_t at, std::size_t size) {
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(game_image);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(game_image + dos->e_lfanew);
    auto section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& s = section[i];
        const auto begin = game_image + s.VirtualAddress;
        if ((s.Characteristics & IMAGE_SCN_MEM_WRITE) &&
            !(s.Characteristics & IMAGE_SCN_MEM_EXECUTE) && at >= begin &&
            at - begin < s.Misc.VirtualSize && size <= s.Misc.VirtualSize - (at - begin))
            return true;
    }
    return false;
}
}  // namespace opendojo::memory
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2 || argc == 3,
              "usage: native_layout_tests GAME.exe [relocated|conflict|format|context]");
        const std::wstring mode = argc == 3 ? argv[2] : L"current";
        check(mode == L"current" || mode == L"relocated" || mode == L"conflict" ||
                  mode == L"format" || mode == L"context" || mode == L"runtime" || mode == L"weak",
              "unknown test mode");
        const bool relocated = mode == L"relocated";
        std::ifstream input(std::filesystem::path(argv[1]), std::ios::binary | std::ios::ate);
        check(bool(input), "cannot read executable");
        const auto length = static_cast<std::size_t>(input.tellg());
        check(length >= 4096, "short executable");
        std::vector<char> raw(length);
        input.seekg(0);
        input.read(raw.data(), raw.size());
        check(bool(input), "incomplete executable read");
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
        check(dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0 &&
                  static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) < length,
              "invalid DOS header");
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(raw.data() + dos->e_lfanew);
        check(nt->Signature == IMAGE_NT_SIGNATURE &&
                  nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC,
              "expected PE64");
        const auto image_size = nt->OptionalHeader.SizeOfImage;
        check(image_size && image_size < 1024u * 1024u * 1024u, "invalid image size");
        game_image = reinterpret_cast<std::uintptr_t>(
            VirtualAlloc(nullptr, image_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        check(game_image != 0, "image allocation failed");
        check(nt->OptionalHeader.SizeOfHeaders <= length &&
                  nt->OptionalHeader.SizeOfHeaders <= image_size,
              "invalid headers size");
        std::memcpy(reinterpret_cast<void*>(game_image), raw.data(),
                    nt->OptionalHeader.SizeOfHeaders);
        const auto sections = IMAGE_FIRST_SECTION(nt);
        check(reinterpret_cast<const char*>(sections + nt->FileHeader.NumberOfSections) <=
                  raw.data() + length,
              "invalid sections");
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            const auto& s = sections[i];
            check(s.PointerToRawData <= length && s.SizeOfRawData <= length - s.PointerToRawData &&
                      s.VirtualAddress <= image_size &&
                      s.SizeOfRawData <= image_size - s.VirtualAddress,
                  "invalid section bounds");
            std::memcpy(reinterpret_cast<void*>(game_image + s.VirtualAddress),
                        raw.data() + s.PointerToRawData, s.SizeOfRawData);
        }
        // Historical image addresses are TEST FIXTURES ONLY. Mutate an inert copy,
        // verifying each original operand before simulating a patch. Never run this code.
        auto change = [&](std::uint64_t address, std::uint64_t before, std::uint64_t after,
                          std::size_t width) {
            const auto rva = address - 0x140000000ull;
            check(rva < image_size && width <= image_size - rva && width <= 8,
                  "bad fixture address");
            std::uint64_t actual = 0;
            auto at = reinterpret_cast<void*>(game_image + rva);
            std::memcpy(&actual, at, width);
            check(actual == before, "executable differs from patch fixture");
            std::memcpy(at, &after, width);
        };
        if (relocated) {
            change(0x145E7FD50 + 29, 0x6BC, 0x700, 4);
            change(0x145C9BE40 + 49, 0x28, 0x30, 1);
            change(0x141924510 + 71, 0x484, 0x504, 4);
            change(0x14191FE10 + 78, 8, 12, 1);
            for (auto offset : {12, 58, 72})
                change(0x1418B0010 + offset, 0x65, 0x69, 1);
            change(0x1418842A0 + 55, 0x160, 0x180, 4);
            change(0x1418842A0 + 93, 0xD0, 0xE0, 4);
            change(0x14187F9C0 + 5, 0x2C, 0x34, 1);
            change(0x1418F0FA0 + 26, 0x24, 0x30, 1);
            change(0x1418F0FA0 + 29, 0x28, 0x34, 1);
            change(0x1418EDA70 + 9, 0x24, 0x30, 1);
            change(0x1419161F0 + 8, 0x400000, 0x800000, 4);
            change(0x1419161F0 + 32, 0x88, 0xA0, 4);
            change(0x1419161F0 + 147, 0x3A50, 0x3B00, 4);
            change(0x1419161F0 + 140, 0x3AE0, 0x3BA0, 4);
            change(0x1419161F0 + 161, 0x99, 0xB1, 4);
            for (auto offset : {3, 105})
                change(0x145E7FDDA + offset, 0x30, 0x40, 1);
            change(0x145E7FDDA + 110, 0x38, 0x48, 1);
            change(0x141913AA0 + 49, 0x90, 0xA0, 4);
            change(0x1419049D0 + 11, 0x168, 0x188, 4);
            change(0x141904A60 + 14, 0x168, 0x188, 4);
            for (auto offset : {164, 363})
                change(0x1430A5C10 + offset, 0x10, 0x20, 1);
            change(0x14305C8D0 + 51, 0x10, 0x20, 1);
            change(0x1430A5C10 + 419, 0x18, 0x28, 1);
            change(0x143186820 + 68, 8, 12, 1);
            change(0x1430A5C10 + 95, 0x50, 0x60, 1);
            change(0x1430A5C10 + 291, 0x48, 0x58, 1);
            for (auto offset : {152, 186})
                change(0x14305C8D0 + offset, 0x48, 0x58, 1);
            change(0x1430A5C10 + 425, 0x28, 0x38, 1);
            change(0x14305C8D0 + 99, 0x28, 0x38, 1);
            change(0x1430A5C10 + 219, 0x28, 0x38, 1);
            change(0x1430A5C10 + 232, 0x20, 0x30, 1);
            change(0x14318B350 + 65, 0x4C, 0x54, 1);
            change(0x1430A9BA0 + 117, 0xD8, 0xE8, 4);
            change(0x1431869D1 + 3, 0xB6, 0xC6, 4);
            change(0x1431869D1 + 27, 0xB6, 0xC6, 4);
            change(0x14305C8D0 + 171, 0x2C8, 0x2D0, 4);
            change(0x142FF81B0 + 87, 0x10, 0x20, 1);
            change(0x142FF81B0 + 98, 2, 4, 1);
            // Move pool2 alone: no adjacency assumption may survive in the resolver.
            for (auto offset : {76, 96, 103, 120}) {
                auto at = reinterpret_cast<std::int32_t*>(game_image + 0x18EDA70 + offset + 3);
                *at += 0x100;
            }
            // Previously discovery required these unrelated neighboring bytes.
            change(0x1418E0588, 0xB8, 0x90, 1);
        } else if (mode == L"conflict") {
            change(0x141904A60 + 14, 0x168, 0x188, 4);
            change(0x14305C8D0 + 51, 0x10, 0x20, 1);
            change(0x1418B0010 + 58, 0x65, 0x69, 1);
            change(0x141924510 + 61, 0, 8, 4);
        } else if (mode == L"format") {
            change(0x1418E4594 + 13, 7202, 7210, 4);
        } else if (mode == L"context") {
            change(0x141909760 + 23, 0xE9, 0x90, 1);
        }
        *reinterpret_cast<std::uintptr_t*>(game_image + 0x6D5BB90) = reinterpret_cast<std::uintptr_t>(&GetCurrentThreadId);
        if (mode == L"runtime") *reinterpret_cast<std::uintptr_t*>(game_image + 0x6D5BB90) = 0;
        if (mode == L"weak") change(0x1431B6D30 + 33, 4, 8, 1);
        const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        check(directory.VirtualAddress < image_size &&
                  directory.Size <= image_size - directory.VirtualAddress &&
                  directory.Size % sizeof(RUNTIME_FUNCTION) == 0,
              "invalid unwind directory");
        auto unwind = reinterpret_cast<PRUNTIME_FUNCTION>(game_image + directory.VirtualAddress);
        check(RtlAddFunctionTable(unwind, directory.Size / sizeof(RUNTIME_FUNCTION), game_image) !=
                  0,
              "cannot register unwind table");
        const auto resolved = opendojo::signatures::resolve_all();
        RtlDeleteFunctionTable(unwind);
        check(opendojo::signatures::native_text_abi_supported(game_image + 0x2C491B0),
              "FString ABI rejected");
        change(0x142E88110 + 53, 8, 16, 1);
        check(!opendojo::signatures::native_text_abi_supported(game_image + 0x2C491B0),
              "changed FString ABI accepted");
        check(opendojo::signatures::native_object_array_abi_supported(game_image + 0x31BAB60),
              "object-array ABI rejected");
        change(0x1431B7D00 + 24, 8, 16, 1);
        check(!opendojo::signatures::native_object_array_abi_supported(game_image + 0x31BAB60),
              "changed object-array ABI accepted");
        const auto runtime = opendojo::signatures::runtime_layout();
        if (mode == L"runtime" || mode == L"weak") {
            check(!resolved && !runtime.update && !runtime.weak_assign, "unsupported runtime contract accepted");
            std::wcout << L"Native runtime rejection passed: " << mode << L"\n";
            VirtualFree(reinterpret_cast<void*>(game_image), 0, MEM_RELEASE);
            return 0;
        }
        check(runtime.update == game_image + 0x5BC54A0 && runtime.game_thread_id == game_image + 0x993BCD8 &&
              runtime.engine_free == game_image + 0x2EF7810 && runtime.weak_assign == game_image + 0x31B6D30 &&
              runtime.weak_valid == game_image + 0x31BBFA0 && runtime.weak_get == game_image + 0x31BA7E0,
              "native update/weak-object helpers did not resolve");
        const auto p = opendojo::signatures::player_layout();
        const auto r = opendojo::signatures::reflection_layout();
        const auto state = opendojo::signatures::recording_state_layout();
        const auto session = opendojo::signatures::session_layout();
        if (mode == L"conflict") {
            check(!resolved && !p.p1 && !r.object_class && !state.counter && !session.player_flag &&
                      !opendojo::signatures::slot_flag_base() &&
                      !opendojo::signatures::live_recordings_supported(),
                  "conflicting fields did not disable dependent access");
        } else if (mode == L"format") {
            check(!resolved && !opendojo::signatures::live_recordings_supported() && r.object_class,
                  "incompatible recording format was accepted or disabled independent reflection");
        } else if (mode == L"context") {
            check(!resolved && !opendojo::signatures::ctx_ptr_addr(),
                  "invalid context call chain accepted");
        } else {
            check(resolved, "at least one native layout did not resolve");
            check(p.p1 == (relocated ? 0x40u : 0x30u) && p.p2 == (relocated ? 0x48u : 0x38u) &&
                      p.character == (relocated ? 0x188u : 0x168u) &&
                      p.native_bias == (relocated ? 0xA0u : 0x90u),
                  "unexpected player layout");
            check(opendojo::signatures::slot_flag_base() == (relocated ? 0x504u : 0x484u),
                  "unexpected slot flags");
            check(state.counter == (relocated ? 12u : 8u) &&
                      state.pause == (relocated ? 0x69u : 0x65u) &&
                      state.side_record == (relocated ? 0x294u : 0x25Cu) &&
                      state.recording_state == (relocated ? 0x34u : 0x28u),
                  "unexpected recording state");
            check(session.pending == (relocated ? 0xA0u : 0x88u) &&
                      session.finished == (relocated ? 0xB1u : 0x99u) &&
                      session.player_flag == (relocated ? 0x3B00u : 0x3A50u) &&
                      session.active_mask == (relocated ? 0x800000u : 0x400000u),
                  "unexpected session layout");
            check(r.object_class == (relocated ? 0x20u : 0x10u) &&
                      r.object_name == (relocated ? 0x28u : 0x18u) &&
                      r.native_function == (relocated ? 0xE8u : 0xD8u) &&
                      r.property_offset == (relocated ? 0x54u : 0x4Cu) &&
                      r.name_blocks == (relocated ? 0x20u : 0x10u) &&
                      r.name_text == (relocated ? 4u : 2u) &&
                      r.object_flags == (relocated ? 12u : 8u) &&
                      r.children == (relocated ? 0x58u : 0x48u) &&
                      r.child_properties == (relocated ? 0x60u : 0x50u) &&
                      r.field_next == (relocated ? 0x38u : 0x28u) &&
                      r.ffield_name == (relocated ? 0x38u : 0x28u) &&
                      r.ffield_next == (relocated ? 0x30u : 0x20u) &&
                      r.function_parms_size == (relocated ? 0xC6u : 0xB6u) &&
                      r.super_getter_slot == (relocated ? 0x2D0u : 0x2C8u),
                  "unexpected reflection layout");
            check(opendojo::signatures::pool2_ptr_addr() - opendojo::signatures::pool1_ptr_addr() ==
                      (relocated ? 0x108u : 8u),
                  "pool2 relocation failed");
        }
        std::wcout << L"Native executable layout test passed: " << mode
                   << L" (no game code executed)\n";
        VirtualFree(reinterpret_cast<void*>(game_image), 0, MEM_RELEASE);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
