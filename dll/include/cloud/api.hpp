#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// High-level OpenDojo Cloud API. Each function is a blocking call —
// run them on the cloud worker thread, not the render thread.
//
// Every function ensures auth as its first step. Errors (transport,
// auth, server-side validation) come back as Result::error_message
// and the caller surfaces them as a toast.

namespace opendojo::cloud::api {

struct DrillSummary {
    std::string id;  // uuid as string
    std::string name;
    std::string description;  // empty if null
    std::string character;
    std::string cpu_side;
    int recordings_count = 0;
    std::int64_t size_bytes = 0;
    std::int64_t downloads = 0;
    std::int64_t likes = 0;
    std::string author_handle;            // empty if null
    std::vector<std::string> categories;  // canonical ids; empty if untagged
    std::string difficulty;               // "" / "beginner" / "intermediate" / "advanced"
    std::string created_at;               // ISO-8601 from server; for UI only
    bool is_mine = false;                 // server says: uploader_id == auth.uid()
};

struct ListResult {
    bool ok = false;
    std::vector<DrillSummary> drills;
    std::string error_message;
};

enum class SortOrder {
    NewestFirst,
    MostDownloaded,
    MostLiked,
};

struct ListQuery {
    std::string character_filter;              // empty for "all characters"
    std::string search_query;                  // FTS prefix; empty for none
    std::vector<std::string> category_filter;  // OR-match; empty = no filter
    std::string difficulty_filter;             // single id; empty = no filter
    SortOrder sort = SortOrder::NewestFirst;
    int offset = 0;
    int limit = 50;          // server caps at 50 regardless
    bool mine_only = false;  // restrict to drills the caller uploaded
};

ListResult list_drills(const ListQuery& q);

// ---- Single drill fetch -----------------------------------------------------

struct DrillContent {
    std::string id;
    std::string name;
    std::string character;
    std::string content;
    int recordings_count = 0;
};

struct GetResult {
    bool ok = false;
    DrillContent drill;
    std::string error_message;
    bool rate_limited = false;  // distinct from generic errors
};

GetResult get_drill(const std::string& id);

// ---- Upload -----------------------------------------------------------------

struct SubmitArgs {
    std::string name;
    std::string description;
    std::string character;
    std::string cpu_side;  // "p1", "p2", or ""
    int recordings_count = 0;
    std::string content;                  // full drill text (.drill.txt body)
    std::string author_handle;            // optional self-reported handle
    std::vector<std::string> categories;  // canonical ids
    std::string difficulty;               // canonical id; empty = unset
    std::string dll_version;              // pulled from cloud::dll_version()
};

struct SubmitResult {
    bool ok = false;
    std::string drill_id;  // server-assigned uuid on success
    bool deduped = false;  // server matched an existing content hash
    bool rate_limited = false;
    std::string error_message;
};

SubmitResult submit_drill(const SubmitArgs& a);

// ---- Like / unlike ----------------------------------------------------------
//
// One toggle call flips the caller's like state for the drill and
// returns the new total. The server tracks (user_id, drill_id) in
// the `likes` table so re-toggling unlikes.

struct LikeResult {
    bool ok = false;
    std::int64_t likes = 0;  // new total after toggle
    std::string error_message;
};
LikeResult toggle_like(const std::string& drill_id);

// ---- Author delete ----------------------------------------------------------
//
// Hard-deletes one of the caller's own drills. Server enforces
// ownership via the delete_my_drill RPC — if you don't own the
// id, the server silently returns deleted=false.

struct DeleteResult {
    bool ok = false;
    bool deleted = false;  // true only if the row was found and removed
    std::string error_message;
};
DeleteResult delete_my_drill(const std::string& drill_id);

// ---- Author edit ------------------------------------------------------------
//
// Updates name + description on one of the caller's own drills. Server
// enforces ownership via the update_my_drill RPC; non-owners get
// updated=false silently. Server also re-validates lengths (name 1..96,
// description 0..1000) and returns updated=false on violation.

struct UpdateResult {
    bool ok = false;
    bool updated = false;  // true only if the row was found AND owned by caller
    std::string error_message;
};
UpdateResult update_my_drill(const std::string& drill_id, const std::string& name,
                             const std::string& description);

// ---- Report -----------------------------------------------------------------
//
// Filing a moderation complaint. One report per user per drill —
// re-calling returns reported=false with no error since the previous
// call already succeeded. Reason is optional and capped server-side
// at 240 chars.

struct ReportResult {
    bool ok = false;
    bool reported = false;  // false if the user had already reported it
    std::string error_message;
};
ReportResult report_drill(const std::string& drill_id, const std::string& reason);

}  // namespace opendojo::cloud::api
