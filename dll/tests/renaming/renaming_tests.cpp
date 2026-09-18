#include "drill.hpp"
#include "description.hpp"
#include "slot_labels.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <windows.h>

// The historical decoder uses an identical data model, in its own namespace.
namespace opendojo::legacy_drill {
using TextResult = opendojo::drill::TextResult;
TextResult decode_text(std::string_view);
}

void check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

int main(int argc, char** argv) {
    using namespace opendojo;
    const std::string broken_header = "description: First line\r\nSecond line\r\n";
    const std::string valid_recording = "--- recording 1\nname: slot 1\nn . 5\n";
    check(drill::decode_text("description: one\n\n  Tip: two # literal\n# literal\ncharacter: jin\n" +
                             valid_recording).drill.description == "one\n\n  Tip: two # literal\n# literal",
          "Legacy continuation preserves blank lines, indentation, colons and inline hashes");
    check(!drill::decode_text("unexpected prose\n" + valid_recording).error.empty(),
          "Arbitrary header text outside a description must still fail");
    check(!drill::decode_text("description: ok\ncharacter: jin\nunexpected prose\n" +
                              valid_recording).error.empty(),
          "Description continuation must stop at known metadata");
    check(drill::decode_text("description: " + std::string(4096, 'x') + "\n" +
                             valid_recording).error.empty(), "Description byte limit is inclusive");
    check(!drill::decode_text("description: " + std::string(4097, 'x') + "\n" +
                              valid_recording).error.empty(), "Oversized descriptions must fail");
    check(!drill::decode_text("description: ok\n" + std::string("evil\0text", 9) + "\n" +
                              valid_recording).error.empty(), "NUL in continuation must fail");
    std::string too_many;
    for (int i = 0; i < 9; ++i) too_many += valid_recording;
    check(!drill::decode_text("description: ok\n" + too_many).error.empty(),
          "Description parsing must not bypass the eight-recording cap");
    std::string too_many_events = "description: first\nsecond\n--- recording 1\n";
    for (int i = 0; i < 1801; ++i) too_many_events += "n . 1\n";
    check(!drill::decode_text(too_many_events).error.empty(),
          "Multiline parsing must not bypass the fixed slot event capacity");
    check(drill::decode_text(broken_header + valid_recording).drill.description == "First line\nSecond line",
          "Local decoder must preserve legacy multiline descriptions");
    const auto recovered = drill::decode_text(broken_header + valid_recording);
    check(recovered.error.empty() && recovered.drill.recordings.size() == 1 &&
          recovered.drill.recordings[0].total_frames == 5,
          "Cloud recovery must retain recording data after legacy multiline description");
    check(!drill::decode_text(broken_header).error.empty(),
          "Broken headers without recordings must not be accepted");
    check(!drill::decode_text(broken_header + "--- recording 1\ninvalid . 5\n").error.empty(),
          "Header recovery must still reject invalid recording data");
    check(!drill::decode_text("name: valid\n--- recording 1\ninvalid . 5\n" +
                                    valid_recording).error.empty(),
          "Cloud recovery must never skip a malformed recording");
    if (argc > 2) {
        std::ifstream input(argv[2], std::ios::binary);
        check(input.good(), "Downloaded regression fixture must exist");
        const std::string downloaded((std::istreambuf_iterator<char>(input)), {});
        check(drill::decode_text(downloaded).drill.description.find("\nDf 1,3") != std::string::npos,
              "Actual legacy description continuation must be retained");
        const auto fixed = drill::decode_text(downloaded);
        check(fixed.error.empty() && fixed.drill.recordings.size() == 8,
              "Actual Kunimitsu download must recover all eight recordings");
        const auto roundtrip = drill::decode_text(drill::encode_text(fixed.drill));
        check(roundtrip.error.empty() && roundtrip.drill.recordings.size() == 8,
              "Recovered download must serialize to a valid local drill");
        for (std::size_t i = 0; i < 8; ++i)
            check(roundtrip.drill.recordings[i].slot_bytes == fixed.drill.recordings[i].slot_bytes,
                  "Recovery round trip must preserve exact playback bytes");
    }
    slot_labels::set(0, "Punish");
    slot_labels::set(0, "");
    check(slot_labels::get(0).empty(), "Clearing must preserve an unnamed slot");
    slot_labels::set(0, "   ");
    check(slot_labels::get(0).empty(), "Whitespace alone is not a custom name");
    slot_labels::set(1, "Throw");
    slot_labels::clear_all();
    check(slot_labels::get(1).empty(), "Replacement must clear old labels");
    slot_labels::set(0, std::string(33, 'a'));
    check(slot_labels::get(0) == std::string(32, 'a'), "ASCII names must stop at 32 characters");
    for (const std::string character : {std::string("\xc3\xa9"), std::string("\xe9\xa2\xa8"),
                                        std::string("\xf0\x9f\x91\x8a")}) {
        std::string name;
        for (int i = 0; i < 32; ++i) name += character;
        slot_labels::set(0, name);
        check(slot_labels::get(0) == name, "32 Unicode characters must fit without byte truncation");
        slot_labels::set(0, name + character);
        check(slot_labels::get(0) == name, "Unicode cap must preserve complete characters");
        const auto mixed = std::string(31, 'a') + character;
        check(slot_labels::name_prefix_size(mixed + "z") == mixed.size(), "Mixed UTF-8 boundary");
    }
    slot_labels::clear_all();

    for (int slot = 1; slot <= 8; ++slot) {
        const auto legacy_default = drill::decode_text(
            "# OpenDojo drill\nrecordings: 1\n--- recording 1\nname: slot " +
            std::to_string(slot) + "\nn . 1\n");
        check(legacy_default.error.empty() && legacy_default.drill.recordings[0].name.empty(),
              "Legacy slot N defaults must load as unnamed regardless of destination slot");
    }
    for (const auto& custom : {"slot 1 punish", "Slot 1", "CPU Opponent Action 1", "Throw break"})
        check(recording_name::from_file(custom) == custom, "Descriptive custom names must survive import");

    auto check_untrusted = [](const std::string& input) {
        const auto safe = recording_name::normalize(input);
        check(safe.size() < slot_labels::NAME_BUFFER_SIZE, "Untrusted name exceeds byte buffer");
        check(safe == recording_name::normalize(safe), "Normalization must be idempotent");
        std::size_t chars = 0;
        for (unsigned char c : safe) if ((c & 0xC0) != 0x80) ++chars;
        check(chars <= 32, "Untrusted name exceeds character limit");
        check(safe.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
              safe.data(), static_cast<int>(safe.size()), nullptr, 0) > 0,
              "Windows must accept sanitized UTF-8 without replacement characters");
        slot_labels::set(0, input);
        check(slot_labels::get(0) == safe, "Store must enforce the same bound");
        const auto parsed = drill::decode_text("# OpenDojo drill\nrecordings: 1\n--- recording 1\nname: " +
                                               input + "\nn . 1\n");
        if (parsed.error.empty()) {
            for (const auto& rec : parsed.drill.recordings)
                check(rec.name.size() < slot_labels::NAME_BUFFER_SIZE &&
                      rec.name == recording_name::normalize(rec.name), "Parser admitted unsafe name");
        }
    };
    for (const auto& bad : {std::string(1024 * 1024, '\x80'), std::string(1024 * 1024, 'a'),
             std::string("\xc0\xaf"), std::string("\xe0\x80\xaf"), std::string("\xed\xa0\x80"),
             std::string("\xf4\x90\x80\x80"), std::string("\xf0\x9f"),
             std::string("ok\0evil", 7), std::string("ok\x1b[31m"), std::string("ok\xe2\x80\xae"),
             std::string("%s%n%s%n"), std::string("ok\n--- recording 2\nname: injected")})
        check_untrusted(bad);
    check(recording_name::normalize(std::string(4096, '\x80')).empty(), "Continuation-byte bypass");
    check(recording_name::normalize(std::string("ok\0evil", 7)) == "ok", "NUL must stop the name");
    std::mt19937 rng(0xD0D0);
    for (int trial = 0; trial < 10000; ++trial) {
        std::string input(rng() % 200, '\0');
        for (char& c : input) c = static_cast<char>(rng() & 255);
        check_untrusted(input);
    }

    drill::Drill d;
    d.name = "Rename compatibility";
    d.author_handle = "Author #1 %s";
    d.description = "Practice #1: punish and repeat\n\n  Note: keep spacing\n# literal hashtag\n--- recording 9\ncharacter: injected\n\xe9\xa2\xa8\n";
    d.character = "jin";
    std::vector<std::uint8_t> bytes(drill::SLOT_PITCH, 0);
    bytes[0] = 1; bytes[2] = 0x26; bytes[3] = 0x40;
    bytes[4] = 0xA0; bytes[5] = 12;
    for (const auto& name : {std::string(), std::string("Punish: df+1"),
             std::string("\xe9\xa2\xa8\xe7\xa5\x9e"), std::string("Throw #1"),
             std::string(180, 'x')})
        d.recordings.push_back(drill::make_live_recording(name, bytes.data()));
    d.recordings.push_back(drill::make_movelist_recording("Move list", 123));
    const auto text = drill::encode_text(d);
    const auto current = drill::decode_text(text);
    const auto legacy = legacy_drill::decode_text(text);
    check(current.error.empty() && legacy.error.empty(), "Both decoders must accept named exports");
    check(current.drill.author_handle == d.author_handle && current.drill.description == d.description,
          "Downloaded author and description must survive offline round trips");
    const auto copied = drill::decode_text(drill::encode_text(current.drill));
    check(copied.drill.description == d.description && current.drill.character == "jin",
          "Save/load must preserve multiline text without interpreting injected headers");
    drill::DescriptionReader header_reader;
    std::string header_description;
    std::istringstream header_stream(text);
    std::string header_line;
    while (std::getline(header_stream, header_line) && !header_line.starts_with("---")) {
        check(header_reader.read(header_line, header_description) !=
                  drill::DescriptionReader::Result::Invalid,
              "Library header reader must accept encoded descriptions");
    }
    check(header_description == d.description, "Library hover must retain the same multiline text");
    check(copied.drill.author_handle == d.author_handle, "Copying a drill must retain attribution");
    check(current.drill.recordings.size() == d.recordings.size() &&
          legacy.drill.recordings.size() == d.recordings.size(), "Recording counts must survive");
    for (std::size_t i = 0; i < d.recordings.size(); ++i) {
        const auto& expected = d.recordings[i];
        check(current.drill.recordings[i].name == recording_name::normalize(expected.name),
              "Current names must round-trip within the limit");
        check(legacy.drill.recordings[i].slot_bytes == expected.slot_bytes &&
              legacy.drill.recordings[i].kind == expected.kind &&
              legacy.drill.recordings[i].move_id == expected.move_id,
              "Names must not change playback in older mods");
    }
    check(legacy.drill.recordings[0].name.empty(), "Legacy unnamed stays empty");
    check(legacy.drill.recordings[1].name == d.recordings[1].name &&
          legacy.drill.recordings[2].name == d.recordings[2].name,
          "Legacy decoder must retain ordinary and Unicode names");
    check(drill::decode_text("# OpenDojo drill\nrecordings: 1\n--- recording 1\nn . 1\n")
          .drill.recordings[0].name.empty(), "Old files without names stay unnamed");
    if (argc > 1) { std::ofstream out(argv[1], std::ios::binary); out << text; }
    d.author_handle = "Author\nauthor_handle: injected\r--- recording 9";
    const auto sanitized = drill::decode_text(drill::encode_text(d));
    check(sanitized.error.empty() && sanitized.drill.recordings.size() == d.recordings.size() &&
          sanitized.drill.author_handle.find('\n') == std::string::npos,
          "Author metadata cannot inject drill sections");
    d.recordings[0].name = "line\n--- recording 9\rname: injected";
    check(drill::decode_text(drill::encode_text(d)).drill.recordings.size() == d.recordings.size(),
          "Pasted newlines must not inject recordings");
    std::cout << "Renaming and historical decoder compatibility passed\n";
}
