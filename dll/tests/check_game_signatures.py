"""Read-only patch check: py -3 check_game_signatures.py <Polaris exe>.

Reads signature strings directly from the shipping sources. Does not load or
execute game code. Checks unique matches, unwind entries and data references.
This checks code relocation compatibility, not live object layouts or gameplay.
"""
import argparse
import pathlib
import re
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", type=pathlib.Path)
    args = parser.parse_args()
    data = args.exe.read_bytes()
    u16 = lambda at: struct.unpack_from("<H", data, at)[0]
    u32 = lambda at: struct.unpack_from("<I", data, at)[0]
    assert data[:2] == b"MZ", "Not a PE image"
    nt = u32(0x3C)
    assert data[nt:nt + 4] == b"PE\0\0" and u16(nt + 24) == 0x20B
    image_size = u32(nt + 24 + 56)
    table = nt + 24 + u16(nt + 20)
    sections = []
    for i in range(u16(nt + 6)):
        at = table + 40 * i
        name = data[at:at + 8].rstrip(b"\0")
        size, rva, raw_size, raw = struct.unpack_from("<IIII", data, at + 8)
        sections.append((name, rva, size, raw, raw_size, u32(at + 36)))
    text = next(s for s in sections if s[0] == b".text")
    code = data[text[3]:text[3] + min(text[2], text[4])]

    def raw_offset(rva):
        for _, start, _, raw, length, _ in sections:
            if start <= rva < start + length:
                return raw + rva - start
        raise ValueError(f"Unmapped RVA {rva:X}")

    # Exception directory contains RUNTIME_FUNCTION begin/end/unwind triples.
    exception_rva, exception_size = struct.unpack_from("<II", data, nt + 24 + 112 + 3 * 8)
    exception_raw = raw_offset(exception_rva)
    functions = {}
    for at in range(exception_raw, exception_raw + exception_size, 12):
        begin, end, _ = struct.unpack_from("<III", data, at)
        functions[begin] = end

    def matches(notation, blob):
        pattern = b"".join(b"." if b == "??" else re.escape(bytes([int(b, 16)]))
                           for b in notation.split())
        return [m.start() for m in re.finditer(b"(?=" + pattern + b")", blob, re.DOTALL)]

    def writable(rva, size=8):
        return any(start <= rva and rva + size <= start + length and
                   flags & 0x80000000 and not flags & 0x20000000
                   for _, start, length, _, _, flags in sections)

    def rip(rva, disp_offset, instruction_length):
        target = rva + instruction_length + struct.unpack_from("<i", data, raw_offset(rva) + disp_offset)[0]
        assert writable(target), f"RIP target {target:X} not writable image data"
        return target

    root = pathlib.Path(__file__).resolve().parents[1]
    resolved = {}
    for file, symbols in [("signatures.cpp", ["PRACTICE_DTOR_SIG", "PLAYER_REFRESH_SIG", "POOL_INIT_SIG", "GET_CTX_SIG"]),
                          ("players.cpp", ["PAT_PLAYERS", "PAT_MAIN_INFO"]),
                          ("practice_rename.cpp", ["PAT_FIND_UNREAL_CLASS", "PAT_FIND_OBJECTS_OF_CLASS"])]:
        source = re.sub(r"//[^\n]*", "", (root / "src" / file).read_text(encoding="utf-8"))
        for symbol in symbols:
            declaration = re.search(r"\b" + symbol + r"\s*(?:=|\{)(.*?);", source, re.DOTALL).group(1)
            strings = re.findall(r'"([^"\n]*)"', declaration)
            notation = " ".join(s for s in strings if re.fullmatch(r"[0-9A-Fa-f?\s]+", s))
            hits = matches(notation, code)
            assert len(hits) == 1, f"{symbol}: {len(hits)} matches"
            rva = text[1] + hits[0]
            resolved[symbol] = rva
            if symbol in ("PRACTICE_DTOR_SIG", "PLAYER_REFRESH_SIG", "POOL_INIT_SIG"):
                assert rva in functions, f"{symbol}: not an unwind function entry"
            print(f"{symbol}: unique RVA 0x{rva:X}")

    for symbol, window, notation, disp, length in [
        ("PRACTICE_DTOR_SIG", 0x200, "33 FF 48 89 3D ?? ?? ?? ??", 5, 9),
        ("POOL_INIT_SIG", 0x40, "48 39 05 ?? ?? ?? ?? 75", 3, 7),
    ]:
        start = resolved[symbol]
        length_bytes = min(window, functions[start] - start)
        at = raw_offset(start)
        hits = matches(notation, data[at:at + length_bytes])
        assert len(hits) == 1, f"{symbol}: {len(hits)} data references"
        target = rip(start + hits[0], disp, length)
        if symbol == "POOL_INIT_SIG":
            assert writable(target, 16), "Pool pair crosses data bounds"
        print(f"{symbol}: validated data RVA 0x{target:X}")
    for symbol, offset in [("GET_CTX_SIG", 8), ("PAT_PLAYERS", 0), ("PAT_MAIN_INFO", 0)]:
        print(f"{symbol}: validated data RVA 0x{rip(resolved[symbol] + offset, 3, 7):X}")
    for symbol, displacement in [("PAT_FIND_UNREAL_CLASS", 7), ("PAT_FIND_OBJECTS_OF_CLASS", 1)]:
        site = resolved[symbol]
        target = site + displacement + 4 + struct.unpack_from("<i", data, raw_offset(site) + displacement)[0]
        assert target in functions, f"{symbol}: call target is not an unwind function entry"
        print(f"{symbol}: validated function RVA 0x{target:X}")
    print(f"PASS: PE timestamp 0x{u32(nt + 8):X}, image size 0x{image_size:X}")


if __name__ == "__main__":
    main()
