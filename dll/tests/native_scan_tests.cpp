#include "native_scan.hpp"
#include <iostream>
#include <stdexcept>
using namespace opendojo::native_scan;
void check(bool value) {
    if (!value) throw std::runtime_error("instruction scanner regression");
}
Function view(std::vector<unsigned char>& code) {
    const auto at = reinterpret_cast<std::uintptr_t>(code.data());
    return decode(at, at + code.size());
}
int main() {
    // Different alignment and branch encoding preserve instruction identity/operand decoding.
    std::vector<unsigned char> code{0x90, 0x48, 0x8b, 0x43, 0x28, 0x0f, 0x1f, 0x00, 0x48,
                                    0x85, 0xc0, 0x0f, 0x85, 0xf0, 0xff, 0xff, 0xff, 0xc3};
    auto fn = view(code);
    auto match = unique(fn, "48 8B 43 ?? 48 85 C0 75 ??");
    check(bool(match) && match[0].displacement() == 0x28 && match[2].decoded.len == 6);
    check(!unique(fn, "48 8B 43 ?? 48 85 C0 74 ??"));  // Different condition is not equivalent.
    // XCHG with R8 is not a NOP and must not be skipped.
    std::vector<unsigned char> xchg{0x48, 0x8b, 0x43, 0x28, 0x41, 0x90, 0x48, 0x85, 0xc0};
    check(!unique(view(xchg), "48 8B 43 ?? 48 85 C0"));
    code.insert(code.end(), {0x48, 0x8b, 0x43, 0x28});
    fn = view(code);
    check(!unique(fn, "48 8B 43 ??"));  // Ambiguous fragments reject.
    std::vector<unsigned char> short_instruction{0x48, 0x8b};
    check(!view(short_instruction));
    // Both arms must reach the join before returning.
    std::vector<unsigned char> graph{0x74, 0x01, 0x90, 0x90, 0xc3};
    fn = view(graph);
    check(reaches_before_exit(fn, fn.begin, fn.begin + 3));
    graph[1] = 2;
    fn = view(graph);
    check(!reaches_before_exit(fn, fn.begin, fn.begin + 3));
    graph[1] = 100;
    fn = view(graph);
    check(!reaches_before_exit(fn, fn.begin, fn.begin + 3));
    // Matches retain their instructions after the temporary function is destroyed.
    const auto owned = unique(view(code), "48 85 C0 75 ??");
    check(bool(owned) && owned[1].decoded.len == 6);
    auto leaf_view = [](std::vector<unsigned char>& bytes) {
        const auto at = reinterpret_cast<std::uintptr_t>(bytes.data());
        return leaf({at, at, at + bytes.size(), at + bytes.size()}, at);
    };
    std::vector<unsigned char> tail{0x48, 0xff, 0x20};
    check(bool(leaf_view(tail)));  // Virtual tail dispatch terminates a helper.
    check(!reaches_before_exit(leaf_view(tail), reinterpret_cast<std::uintptr_t>(tail.data()),
                               reinterpret_cast<std::uintptr_t>(tail.data()) + tail.size()));
    std::vector<unsigned char> overlap{0x74, 0x01, 0x48, 0x89, 0xc0, 0xc3};
    check(!leaf_view(overlap));  // Branch into the middle of an instruction.
    std::vector<unsigned char> escape{0xeb, 0x7f};
    check(!leaf_view(escape));
    std::vector<unsigned char> unreachable{0xc3, 0x48};
    check(leaf_view(unreachable).instructions.size() == 1);
    std::cout << "Instruction alignment, branch encoding, ambiguity and join control flow passed\n";
}
