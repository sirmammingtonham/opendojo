#include "drill.hpp"
#include "slot_labels.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>

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
    slot_labels::set(0, "Punish");
    slot_labels::set(0, "");
    check(slot_labels::get(0).empty(), "Clearing must preserve an unnamed slot");
    slot_labels::set(0, "   ");
    check(slot_labels::get(0).empty(), "Whitespace alone is not a custom name");
    slot_labels::set(1, "Throw");
    slot_labels::clear_all();
    check(slot_labels::get(1).empty(), "Replacement must clear old labels");

    drill::Drill d;
    d.name = "Rename compatibility";
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
    check(current.drill.recordings.size() == d.recordings.size() &&
          legacy.drill.recordings.size() == d.recordings.size(), "Recording counts must survive");
    for (std::size_t i = 0; i < d.recordings.size(); ++i) {
        const auto& expected = d.recordings[i];
        check(current.drill.recordings[i].name == expected.name, "Current names must round-trip");
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
    d.recordings[0].name = "line\n--- recording 9\rname: injected";
    check(drill::decode_text(drill::encode_text(d)).drill.recordings.size() == d.recordings.size(),
          "Pasted newlines must not inject recordings");
    std::cout << "Renaming and historical decoder compatibility passed\n";
}
