#include "commands.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "drill.hpp"
#include "log.hpp"
#include "memory.hpp"
#include "players.hpp"
#include "slot.hpp"
#include "subsystems.hpp"

namespace opendojo::commands {

std::filesystem::path drills_dir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    auto root = (n > 0 && n < MAX_PATH) ? std::filesystem::path(buf).parent_path()
                                        : std::filesystem::path(L".");
    return root / L"opendojo";
}

namespace {

bool ensure_drills_dir() {
    std::error_code ec;
    std::filesystem::create_directories(drills_dir(), ec);
    return !ec;
}

std::string read_whole_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    auto size = f.tellg();
    f.seekg(0);
    std::string out(static_cast<std::size_t>(size), '\0');
    f.read(out.data(), out.size());
    return out;
}

bool write_whole_file(const std::filesystem::path& p, const void* data, std::size_t n) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
    return f.good();
}

// Parse just enough of a drill file to populate a DrillHeader. Reads up to
// the first `---` line or 4 KiB, whichever comes first.
bool parse_header_only(const std::filesystem::path& path, DrillHeader& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string line;
    std::size_t bytes = 0;
    constexpr std::size_t LIMIT = 4096;
    out.path = path;
    while (std::getline(f, line) && bytes < LIMIT) {
        bytes += line.size() + 1;
        // Strip trailing \r from CRLF.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Stop at the first recording marker.
        if (line.size() >= 3 && line.substr(0, 3) == "---") break;
        if (line.empty() || line[0] == '#') continue;
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        auto key = line.substr(0, colon);
        auto val = line.substr(colon + 1);
        // Trim ascii whitespace.
        auto trim = [](std::string& s) {
            std::size_t a = 0;
            while (a < s.size() && (s[a] == ' ' || s[a] == '\t'))
                ++a;
            std::size_t b = s.size();
            while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t'))
                --b;
            s = s.substr(a, b - a);
        };
        trim(key);
        trim(val);
        if (key == "name")
            out.name = val;
        else if (key == "description")
            out.description = val;
        else if (key == "character")
            out.character = val;
        else if (key == "cpu_side")
            out.cpu_side = val;
        else if (key == "recordings") {
            try {
                out.recording_count = static_cast<std::size_t>(std::stoul(val));
            } catch (...) { out.recording_count = 0; }
        }
    }
    if (out.character.empty()) out.character = "unknown";
    if (out.name.empty()) out.name = path.stem().string();
    // Detect autosaves by the leading-underscore convention used by autosave.cpp.
    auto stem = path.stem().string();                         // foo.drill (without .txt)
    auto core = std::filesystem::path(stem).stem().string();  // foo
    out.is_autosave = (core.rfind("_autosave_", 0) == 0);
    // mtime drives "Newest" sort. Falls back to file_time_type{} (epoch)
    // on error, which sorts as oldest — fine for a corrupt/inaccessible file.
    std::error_code mec;
    out.mtime = std::filesystem::last_write_time(path, mec);
    return true;
}

std::filesystem::path resolve_collision(const std::filesystem::path& dir, std::string_view slug) {
    auto base = std::filesystem::path(std::wstring(slug.begin(), slug.end()));
    auto candidate = dir / (base.wstring() + L".drill.txt");
    if (!std::filesystem::exists(candidate)) return candidate;
    for (int i = 2; i < 1000; ++i) {
        wchar_t suffix[16];
        swprintf_s(suffix, L"_%d.drill.txt", i);
        candidate = dir / (base.wstring() + suffix);
        if (!std::filesystem::exists(candidate)) return candidate;
    }
    return {};  // collision storm — caller will treat as failure
}

std::string timestamp_name() {
    using clock = std::chrono::system_clock;
    std::time_t t = clock::to_time_t(clock::now());
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "drill_%04d%02d%02d_%02d%02d%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

}  // namespace

// ---------------------------------------------------------------------------

std::vector<DrillHeader> list_drills() {
    std::vector<DrillHeader> out;
    std::error_code ec;
    auto dir = drills_dir();
    if (!std::filesystem::exists(dir, ec)) return out;

    for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        auto p = entry.path();
        // We use a doubled extension `.drill.txt` so Discord and other
        // text-aware previewers render the contents inline. Match on the
        // stem's extension to filter only our files.
        if (p.extension() != L".txt") continue;
        if (p.stem().extension() != L".drill") continue;
        DrillHeader h;
        if (parse_header_only(p, h)) out.push_back(std::move(h));
    }

    // Caller chooses the sort order (see SortMode in menu.cpp).
    return out;
}

LoadResult load_drill(const std::filesystem::path& path, LoadMode mode) {
    LoadResult r;
    auto text = read_whole_file(path);
    if (text.empty()) {
        r.message = "couldn't read drill file";
        return r;
    }
    auto decoded = opendojo::drill::decode_text(text);
    if (!decoded.error.empty()) {
        r.message = "decode failed: " + decoded.error;
        return r;
    }
    auto& d = decoded.drill;

    // pool1 is only required if the drill contains live recordings.
    // Movelist-only drills can be imported without pool1 being allocated.
    bool needs_pool1 = false;
    for (const auto& rec : d.recordings) {
        if (rec.kind == opendojo::drill::Kind::Live) {
            needs_pool1 = true;
            break;
        }
    }
    if (needs_pool1 && !opendojo::subsystems::pool1()) {
        // Pool isn't allocated yet — first practice load of this
        // process, user hasn't recorded anything. Force-allocate it
        // via the same path the game would use on its first record.
        opendojo::subsystems::ensure_pool_allocated();
        if (!opendojo::subsystems::pool1()) {
            r.message = "not ready - enter practice mode first";
            return r;
        }
    }

    if (d.recordings.size() > opendojo::slot::USER_SLOTS) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "drill has %zu recordings (max %zu)", d.recordings.size(),
                      opendojo::slot::USER_SLOTS);
        r.message = buf;
        return r;
    }
    // Install one decoded recording into the chosen target slot. Live
    // recordings get the 7202 bytes + the live flag chain; movelist
    // recordings get a move_id + the movelist flag.
    auto install = [&](std::size_t target,
                       const opendojo::drill::Recording& rec) -> opendojo::slot::WriteStatus {
        if (rec.kind == opendojo::drill::Kind::MoveList) {
            return opendojo::slot::set_movelist(target, rec.move_id);
        }
        auto addr = opendojo::slot::address(target);
        if (!addr) return opendojo::slot::WriteStatus::PoolNotAllocated;
        opendojo::memory::write_bytes(addr, rec.slot_bytes.data(), opendojo::slot::SLOT_PITCH);
        return opendojo::slot::set_recorded_flag(target, true);
    };

    if (mode == LoadMode::ReplaceAll) {
        for (std::size_t i = 0; i < opendojo::slot::USER_SLOTS; ++i) {
            opendojo::slot::set_recorded_flag(i, false);
        }
        for (std::size_t i = 0; i < d.recordings.size(); ++i) {
            auto s = install(i, d.recordings[i]);
            if (s != opendojo::slot::WriteStatus::Ok) {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "slot %zu: %s", i + 1, opendojo::slot::describe(s));
                r.message = buf;
                return r;
            }
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "replaced all slots with %zu recordings",
                      d.recordings.size());
        r.ok = true;
        r.message = buf;
        OPENDOJO_LOG("load_drill: %s (%ls)", r.message.c_str(), path.c_str());
        return r;
    }

    // AppendToFree.
    std::vector<std::size_t> free_slots;
    for (std::size_t i = 0; i < opendojo::slot::USER_SLOTS; ++i) {
        if (!opendojo::slot::is_populated(i)) free_slots.push_back(i);
    }
    if (free_slots.size() < d.recordings.size()) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "drill needs %zu recording slots, %zu free - use Replace instead",
                      d.recordings.size(), free_slots.size());
        r.message = buf;
        return r;
    }
    for (std::size_t i = 0; i < d.recordings.size(); ++i) {
        auto s = install(free_slots[i], d.recordings[i]);
        if (s != opendojo::slot::WriteStatus::Ok) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "slot %zu: %s", free_slots[i] + 1,
                          opendojo::slot::describe(s));
            r.message = buf;
            return r;
        }
    }
    char buf[160];
    std::snprintf(buf, sizeof(buf), "loaded %zu recordings into free slots", d.recordings.size());
    r.ok = true;
    r.message = buf;
    OPENDOJO_LOG("load_drill: %s (%ls)", r.message.c_str(), path.c_str());
    return r;
}

ExportResult export_current_slots(std::string_view drill_name, std::string_view description,
                                  std::string_view character, std::string_view cpu_side) {
    ExportResult r;

    // We don't gate on pool1 here — movelist slots live in the recordpool
    // subsystem and don't require pool1 to be allocated. If there are
    // genuinely no recordings (or we're outside practice), the empty-set
    // check below catches it with a clearer message.
    if (!ensure_drills_dir()) {
        r.message = "couldn't create drills directory";
        return r;
    }

    // Auto-detect from the live game state and let any explicit caller
    // override. detect_cpu() returns detected=false outside a match.
    auto cpu = opendojo::players::detect_cpu();

    opendojo::drill::Drill d;
    d.name = drill_name.empty() ? timestamp_name() : std::string(drill_name);
    d.description = std::string(description);
    if (!character.empty()) {
        d.character = std::string(character);
    } else if (cpu.detected) {
        d.character = cpu.character_name;
    } else {
        d.character = "unknown";
    }
    if (!cpu_side.empty()) {
        d.cpu_side = std::string(cpu_side);
    } else if (cpu.detected) {
        d.cpu_side = opendojo::players::side_to_string(cpu.cpu_side);
    }

    for (std::size_t i = 0; i < opendojo::slot::USER_SLOTS; ++i) {
        auto slot_kind = opendojo::slot::kind(i);
        if (slot_kind == opendojo::slot::Kind::Empty) continue;
        char rec_name[32];
        std::snprintf(rec_name, sizeof(rec_name), "slot %zu", i + 1);
        if (slot_kind == opendojo::slot::Kind::MoveList) {
            auto move_id = opendojo::slot::movelist_move_id(i);
            d.recordings.push_back(opendojo::drill::make_movelist_recording(rec_name, move_id));
        } else {
            std::uint8_t bytes[opendojo::slot::SLOT_PITCH];
            if (!opendojo::slot::read(i, bytes)) continue;
            d.recordings.push_back(opendojo::drill::make_live_recording(rec_name, bytes));
        }
    }
    if (d.recordings.empty()) {
        r.message = "no slots contain recordings to export";
        return r;
    }

    auto text = opendojo::drill::encode_text(d);
    auto slug = opendojo::drill::slugify(d.name);
    auto path = resolve_collision(drills_dir(), slug);
    if (path.empty()) {
        r.message = "filename collision storm - pick a different name";
        return r;
    }
    if (!write_whole_file(path, text.data(), text.size())) {
        r.message = "failed to write drill file";
        return r;
    }

    char buf[160];
    std::snprintf(buf, sizeof(buf), "exported %zu recordings", d.recordings.size());
    r.ok = true;
    r.path = path;
    r.message = buf;
    OPENDOJO_LOG("export_current_slots: %s -> %ls", r.message.c_str(), path.c_str());
    return r;
}

CopyResult copy_drill(const std::filesystem::path& src, std::string_view new_name) {
    CopyResult r;
    auto text = read_whole_file(src);
    if (text.empty()) {
        r.message = "couldn't read source drill";
        return r;
    }
    auto decoded = opendojo::drill::decode_text(text);
    if (!decoded.error.empty()) {
        r.message = "decode failed: " + decoded.error;
        return r;
    }
    auto& d = decoded.drill;
    if (!new_name.empty()) d.name = std::string(new_name);
    // The new file's mtime is its creation time — that's what "Newest"
    // sort keys off, so no need for an in-file timestamp.

    if (!ensure_drills_dir()) {
        r.message = "couldn't create drills directory";
        return r;
    }
    auto encoded = opendojo::drill::encode_text(d);
    auto slug = opendojo::drill::slugify(d.name);
    auto path = resolve_collision(drills_dir(), slug);
    if (path.empty()) {
        r.message = "filename collision storm - pick a different name";
        return r;
    }
    if (!write_whole_file(path, encoded.data(), encoded.size())) {
        r.message = "failed to write drill file";
        return r;
    }
    r.ok = true;
    r.path = path;
    r.message = "saved as new drill";
    OPENDOJO_LOG("copy_drill: %ls -> %ls", src.c_str(), path.c_str());
    return r;
}

DrillPayload build_current_slots_payload(std::string_view drill_name,
                                         std::string_view description) {
    DrillPayload r;

    auto cpu = opendojo::players::detect_cpu();

    opendojo::drill::Drill d;
    d.name = drill_name.empty() ? timestamp_name() : std::string(drill_name);
    d.description = std::string(description);
    d.character = cpu.detected ? cpu.character_name : "unknown";
    if (cpu.detected) { d.cpu_side = opendojo::players::side_to_string(cpu.cpu_side); }

    for (std::size_t i = 0; i < opendojo::slot::USER_SLOTS; ++i) {
        auto slot_kind = opendojo::slot::kind(i);
        if (slot_kind == opendojo::slot::Kind::Empty) continue;
        char rec_name[32];
        std::snprintf(rec_name, sizeof(rec_name), "slot %zu", i + 1);
        if (slot_kind == opendojo::slot::Kind::MoveList) {
            auto move_id = opendojo::slot::movelist_move_id(i);
            d.recordings.push_back(opendojo::drill::make_movelist_recording(rec_name, move_id));
        } else {
            std::uint8_t bytes[opendojo::slot::SLOT_PITCH];
            if (!opendojo::slot::read(i, bytes)) continue;
            d.recordings.push_back(opendojo::drill::make_live_recording(rec_name, bytes));
        }
    }
    if (d.recordings.empty()) {
        r.message = "no slots contain recordings to upload";
        return r;
    }

    r.text = opendojo::drill::encode_text(d);
    r.name = d.name;
    r.description = d.description;
    r.character = d.character;
    r.cpu_side = d.cpu_side;
    r.recordings_count = static_cast<int>(d.recordings.size());
    r.ok = true;
    return r;
}

SaveResult save_drill_text(std::string_view display_name, std::string_view content) {
    SaveResult r;
    if (!ensure_drills_dir()) {
        r.message = "couldn't create drills directory";
        return r;
    }
    auto slug = opendojo::drill::slugify(display_name);
    auto path = resolve_collision(drills_dir(), slug);
    if (path.empty()) {
        r.message = "filename collision storm — pick a different name";
        return r;
    }
    if (!write_whole_file(path, content.data(), content.size())) {
        r.message = "failed to write drill file";
        return r;
    }
    r.ok = true;
    r.path = path;
    r.message = "saved to " + path.filename().string();
    OPENDOJO_LOG("save_drill_text: -> %ls", path.c_str());
    return r;
}

DeleteResult delete_drill(const std::filesystem::path& path) {
    DeleteResult r;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        r.message = "file already gone";
        return r;
    }
    if (!std::filesystem::remove(path, ec)) {
        r.message = ec ? ec.message() : "failed to remove file";
        OPENDOJO_LOG("delete_drill: %ls -> %s", path.c_str(), r.message.c_str());
        return r;
    }
    r.ok = true;
    r.message = "deleted " + path.filename().string();
    OPENDOJO_LOG("delete_drill: removed %ls", path.c_str());
    return r;
}

void show_status() {
    OPENDOJO_LOG("=== OpenDojo status ===");
    auto base = opendojo::memory::polaris_base();
    OPENDOJO_LOG("  polaris_base = 0x%llX", static_cast<unsigned long long>(base));

    auto p1 = opendojo::subsystems::pool1();
    OPENDOJO_LOG("  pool1        = 0x%llX %s", static_cast<unsigned long long>(p1),
                 p1 ? "" : "(NULL — record once in practice mode)");

    // Log all resolved subsystem addresses — used to find character ID offsets.
    struct {
        const char* name;
        std::uint32_t key;
    } subsys[] = {
        {"gameplay", opendojo::subsystems::KEY_GAMEPLAY},
        {"singleton", opendojo::subsystems::KEY_SINGLETON},
        {"subB", opendojo::subsystems::KEY_SUBB},
        {"subC", opendojo::subsystems::KEY_SUBC},
        {"subD", opendojo::subsystems::KEY_SUBD},
    };
    for (auto& s : subsys) {
        auto addr = opendojo::subsystems::lookup(s.key);
        OPENDOJO_LOG("  %-9s = 0x%llX", s.name, static_cast<unsigned long long>(addr));
    }

    if (p1) {
        for (std::size_t i = 0; i < opendojo::slot::USER_SLOTS; ++i) {
            if (!opendojo::slot::is_populated(i)) {
                OPENDOJO_LOG("  slot %zu: empty", i + 1);
            } else {
                auto n = opendojo::slot::event_count(i);
                OPENDOJO_LOG("  slot %zu: %u events", i + 1, static_cast<unsigned>(n));
            }
        }
    }
    OPENDOJO_LOG("  drill dir    = %ls", drills_dir().c_str());
}

}  // namespace opendojo::commands
