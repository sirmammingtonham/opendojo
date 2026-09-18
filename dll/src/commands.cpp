#include "commands.hpp"
#include "autosave.hpp"
#include "file_io.hpp"

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
#include "description.hpp"
#include "log.hpp"
#include "memory.hpp"
#include "players.hpp"
#include "game_thread.hpp"
#include "ui/menu.hpp"
#include "slot.hpp"
#include "slot_labels.hpp"
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

// Upper bound on a drill file we'll load into memory. The cloud caps uploaded
// content at 64 KB; a fully-packed local export (8 slots * MAX_EVENTS events *
// ~128 bytes/line) is under 2 MB, so 4 MB comfortably fits any legitimate
// drill while refusing a file crafted to exhaust memory. The decoder's
// per-recording event cap is the real overflow guard; this just bounds I/O.
constexpr std::uintmax_t MAX_DRILL_FILE_BYTES = 4u * 1024u * 1024u;

std::string read_whole_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    auto size = f.tellg();
    if (size < 0 || static_cast<std::uintmax_t>(size) > MAX_DRILL_FILE_BYTES) return {};
    f.seekg(0);
    std::string out(static_cast<std::size_t>(size), '\0');
    f.read(out.data(), out.size());
    if (!f) return {};  // A truncated or failed read must not become a partial drill.
    return out;
}

// Parse just enough of a drill file to populate a DrillHeader. Reads up to
// the first `---` line or 128 KiB, whichever comes first. Allow for UTF-8
// descriptions, explicit blank continuation lines, and format comments.
bool parse_header_only(const std::filesystem::path& path, DrillHeader& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string line;
    constexpr std::size_t LIMIT = 128 * 1024;
    std::size_t remaining = LIMIT;
    opendojo::drill::DescriptionReader description_reader;
    out.path = path;
    while (file_io::bounded_getline(f, line, remaining)) {
        // Strip trailing \r from CRLF.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Stop at the first recording marker.
        if (line.size() >= 3 && line.substr(0, 3) == "---") break;
        const auto read = description_reader.read(line, out.description);
        if (read == opendojo::drill::DescriptionReader::Result::Invalid) return false;
        if (read == opendojo::drill::DescriptionReader::Result::Consumed) continue;
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
        else if (key == "author_handle")
            out.author_handle = val;
        else if (key == "character")
            out.character = val;
        else if (key == "cpu_side")
            out.cpu_side = val;
        else if (key == "cloud_id")
            out.cloud_id = val;
        else if (key == "recordings") {
            try {
                out.recording_count = static_cast<std::size_t>(std::stoul(val));
            } catch (...) {
                out.recording_count = 0;
            }
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

std::filesystem::path write_unique_drill(const std::filesystem::path& dir, std::string_view slug,
                                         std::string_view content, std::string& error) {
    const std::wstring base(slug.begin(), slug.end());
    for (int i = 1; i < 1000; ++i) {
        auto candidate = dir / (base + (i == 1 ? L"" : L"_" + std::to_wstring(i)) + L".drill.txt");
        const auto result = file_io::create_file(candidate, content);
        if (result == file_io::CreateResult::Saved) return candidate;
        if (result == file_io::CreateResult::Failed) {
            error = "failed to write drill file";
            return {};
        }
    }
    error = "filename collision storm - pick a different name";
    return {};
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

static LoadResult apply_drill(const drill::Drill& d, LoadMode mode,
                              const std::filesystem::path& path) {
    LoadResult r;
    std::vector<std::size_t> targets;
    const bool replace = mode == LoadMode::ReplaceAll;
    const auto status = opendojo::slot::import_recordings(d.recordings, replace, targets);
    if (status != opendojo::slot::WriteStatus::Ok) {
        r.message = opendojo::slot::describe(status);
        return r;
    }
    // Publish labels only after the complete memory operation succeeded.
    if (replace) opendojo::slot_labels::clear_all();
    for (std::size_t i = 0; i < targets.size(); ++i)
        opendojo::slot_labels::set(targets[i], d.recordings[i].name);
    if (!opendojo::subsystems::mark_session_loaded(true)) {
        r.message = "recordings written, but session activation failed";
        return r;
    }
    opendojo::slot::publish_snapshot(true);
    r.ok = true;
    r.message = (replace ? "replaced slots with " : "loaded ") + std::to_string(targets.size()) +
                " recordings";
    OPENDOJO_LOG("load_drill: %s (%ls)", r.message.c_str(), path.c_str());
    return r;
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
    if (!opendojo::game_thread::is_current()) {
        const bool queued = opendojo::game_thread::enqueue([d = std::move(d), mode,
                                                            path](bool eligible) {
            try {
                if (eligible) opendojo::autosave::on_manual_action();
                const auto result =
                    eligible
                        ? apply_drill(d, mode, path)
                        : LoadResult{false, "load cancelled: practice changed or request expired"};
                if (result.ok && !path.filename().string().starts_with("_autosave_"))
                    opendojo::menu::queue_export_form(d.name, d.description);
                opendojo::menu::queue_toast(result.message, !result.ok);
            } catch (...) {
                OPENDOJO_LOG("load_drill: queued import threw; recording state may be incomplete");
                opendojo::menu::queue_toast(
                    "load failed unexpectedly; recording state may be incomplete", true);
            }
        });
        return {queued, queued ? "load queued" : "game update unavailable or load queue full"};
    }
    return apply_drill(d, mode, path);
}

std::size_t capture_populated_slots(opendojo::drill::Drill& d, bool snapshot_only) {
    std::array<opendojo::slot::CapturedSlot, opendojo::slot::USER_SLOTS> captured;
    if (snapshot_only ? !opendojo::slot::capture_snapshot(captured, d.character)
                      : !opendojo::slot::capture(captured))
        return 0;
    std::size_t added = 0;
    for (std::size_t i = 0; i < captured.size(); ++i) {
        const auto& slot = captured[i];
        if (slot.kind == opendojo::slot::Kind::Empty) continue;
        auto name = slot.label;
        if (slot.kind == opendojo::slot::Kind::MoveList) {
            d.recordings.push_back(
                opendojo::drill::make_movelist_recording(std::move(name), slot.move_id));
        } else {
            d.recordings.push_back(
                opendojo::drill::make_live_recording(std::move(name), slot.bytes.data()));
        }
        ++added;
    }
    return added;
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
    auto cpu = opendojo::game_thread::current_cpu();

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

    capture_populated_slots(d);
    if (d.recordings.empty()) {
        r.message = "no slots contain recordings to export";
        return r;
    }

    auto text = opendojo::drill::encode_text(d);
    auto slug = opendojo::drill::slugify(d.name);
    auto path = write_unique_drill(drills_dir(), slug, text, r.message);
    if (path.empty()) return r;

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
    auto path = write_unique_drill(drills_dir(), slug, encoded, r.message);
    if (path.empty()) return r;
    r.ok = true;
    r.path = path;
    r.message = "saved as new drill";
    OPENDOJO_LOG("copy_drill: %ls -> %ls", src.c_str(), path.c_str());
    return r;
}

DrillPayload build_current_slots_payload(std::string_view drill_name,
                                         std::string_view description) {
    DrillPayload r;

    auto cpu = opendojo::game_thread::current_cpu();
    if (!cpu.detected || cpu.character_name.empty() || cpu.character_name == "unknown") {
        r.message =
            "CPU character not detected. Wait for practice mode to finish loading before sharing.";
        return r;
    }

    opendojo::drill::Drill d;
    d.name = drill_name.empty() ? timestamp_name() : std::string(drill_name);
    d.description = std::string(description);
    d.character = cpu.detected ? cpu.character_name : "unknown";
    if (cpu.detected) {
        d.cpu_side = opendojo::players::side_to_string(cpu.cpu_side);
    }

    capture_populated_slots(d);
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

// Insert a `cloud_id:` header line into an encoded drill so a downloaded
// file remembers which community drill it came from. Placed right after the
// leading "# OpenDojo drill" comment line; if there's no newline (degenerate
// content) we just append. decode_text / parse_header_only ignore unknown
// header keys, so this never breaks parsing.
std::string stamp_cloud_id(std::string_view content, std::string_view cloud_id) {
    std::string line = "cloud_id:     ";
    line.append(cloud_id);
    line.push_back('\n');

    auto nl = content.find('\n');
    std::string out;
    out.reserve(content.size() + line.size() + 1);
    if (nl == std::string_view::npos) {
        out.append(content);
        if (!out.empty() && out.back() != '\n') out.push_back('\n');
        out.append(line);
    } else {
        out.append(content.substr(0, nl + 1));
        out.append(line);
        out.append(content.substr(nl + 1));
    }
    return out;
}

SaveResult save_drill_text(std::string_view display_name, std::string_view content,
                           std::string_view cloud_id, const DownloadMetadata* metadata) {
    SaveResult r;
    std::string enriched;
    if (metadata) {
        auto decoded = opendojo::drill::decode_text(content);
        if (!decoded.error.empty()) {
            r.message = "downloaded drill is invalid: " + decoded.error;
            return r;
        }
        // The listing is the source of current metadata: edits on the server
        // update these fields without rewriting the original uploaded content.
        decoded.drill.name = display_name;
        decoded.drill.author_handle = metadata->author_handle;
        decoded.drill.description = metadata->description;
        // Cloud metadata can be more complete than an older uploaded header.
        // Never use the active character filter as the drill's identity.
        if (!metadata->character.empty() && metadata->character != "unknown")
            decoded.drill.character = metadata->character;
        if (metadata->cpu_side == "p1" || metadata->cpu_side == "p2")
            decoded.drill.cpu_side = metadata->cpu_side;
        enriched = opendojo::drill::encode_text(decoded.drill);
        content = enriched;
    }
    if (!ensure_drills_dir()) {
        r.message = "couldn't create drills directory";
        return r;
    }
    auto slug = opendojo::drill::slugify(display_name);
    // Stamp the originating cloud id into the header when present, so the
    // Cloud tab can recognize this drill as already in the library.
    std::string stamped;
    std::string_view to_write = content;
    if (!cloud_id.empty()) {
        stamped = stamp_cloud_id(content, cloud_id);
        to_write = stamped;
    }
    auto path = write_unique_drill(drills_dir(), slug, to_write, r.message);
    if (path.empty()) return r;
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
