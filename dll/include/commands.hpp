#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Top-level OpenDojo operations. The menu is the primary consumer.
//
// All filesystem state lives under <game>\Polaris\Binaries\Win64\opendojo\.
// Each file is one drill (one or more recordings); the filename is a slug
// of the drill's `name` field with a `_2`/`_3` collision suffix.

namespace opendojo::commands {

// Absolute path of the opendojo/ data directory next to the game exe.
// Returned for every call — no caching, in case the game gets moved.
std::filesystem::path drills_dir();

// One drill on disk, summarized from its header lines. Cheap to populate —
// the menu uses this to list available drills without parsing event data.
struct DrillHeader {
    std::filesystem::path path;
    std::string name;
    std::string description;
    std::string character;                    // lowercase id; "unknown" if unset
    std::string cpu_side;                     // "p1" / "p2" / "" (unset)
    std::filesystem::file_time_type mtime{};  // filesystem mtime, drives "Newest" sort
    std::size_t recording_count = 0;
    bool is_autosave = false;  // filename starts with "_autosave_"
};

// Scan opendojo/ and return one entry per drill file (`*.drill.txt`).
// Errors per file are silently skipped (the menu shouldn't disappear
// because one file is malformed). Caller is responsible for sorting.
std::vector<DrillHeader> list_drills();

// Copy an existing drill file into a fresh `.drill.txt` (typically used
// to "save" an autosave to a permanent drill). Returns the new path.
struct CopyResult {
    bool ok = false;
    std::filesystem::path path;
    std::string message;
};
CopyResult copy_drill(const std::filesystem::path& src, std::string_view new_name);

// Permanently delete a drill file from disk. Returns ok=false with a
// user-facing message on failure (file missing / locked / permission).
struct DeleteResult {
    bool ok = false;
    std::string message;
};
DeleteResult delete_drill(const std::filesystem::path& path);

// How an import places its recordings into the 8 user slots.
enum class LoadMode {
    AppendToFree,  // Fill the lowest-index empty slots in order. If the
                   // drill needs more slots than are free, the load
                   // refuses without touching any slot.
    ReplaceAll,    // Clear all 8 slots, then load the drill's recordings
                   // into slots 1..N starting from slot 1.
};

struct LoadResult {
    bool ok = false;
    std::string message;  // user-facing toast string
};

LoadResult load_drill(const std::filesystem::path& path, LoadMode mode);

struct ExportResult {
    bool ok = false;
    std::filesystem::path path;  // saved path (empty on failure)
    std::string message;
};

// Snapshot every currently-occupied slot (event_count > 0) into one drill
// file. `drill_name` becomes both the `name:` field and the basis for the
// filename slug. Empty `drill_name` -> "drill_YYYYmmdd_HHMMSS".
//
// `description` is written into the header verbatim. `character` and
// `cpu_side` are auto-detected from the live game state at export time
// — pass empty strings to let detection fill them; non-empty values
// override the detection.
ExportResult export_current_slots(std::string_view drill_name, std::string_view description,
                                  std::string_view character, std::string_view cpu_side);

// Diagnostic: print module base, pool1 state, and per-slot event counts to
// the log. Useful from the menu's "Show status" button.
void show_status();

// Write a drill text blob (already-formatted .drill.txt body) into
// opendojo/, using `display_name` as the basis for the filename slug.
// Collisions get a `_2`/`_3` suffix. On success returns the resulting
// path; on failure path is empty and `message` says why. Used by the
// cloud Browse tab when a downloaded drill needs to land on disk
// before the player imports it via the existing Drills tab buttons.
struct SaveResult {
    bool ok = false;
    std::filesystem::path path;
    std::string message;
};
SaveResult save_drill_text(std::string_view display_name, std::string_view content);

// Compose an encoded drill (the same text export_current_slots would
// write to disk) from the live slot state, plus the metadata the
// upload API needs alongside it. Returns ok=false with `message` if
// there are no recordings to capture. No filesystem I/O.
//
// Used by the cloud Upload path so we can ship a drill straight to
// the server without forcing a local file. The character / cpu_side
// fields autodetect the same way export_current_slots does.
struct DrillPayload {
    bool ok = false;
    std::string message;
    std::string text;
    std::string name;  // normalized name (timestamp if blank)
    std::string description;
    std::string character;  // lowercase id; "unknown" if unset
    std::string cpu_side;   // "p1", "p2", or ""
    int recordings_count = 0;
};
DrillPayload build_current_slots_payload(std::string_view drill_name, std::string_view description);

}  // namespace opendojo::commands
