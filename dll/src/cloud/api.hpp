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
    std::string author_handle;  // empty if null
    std::string created_at;     // ISO-8601 from server; for UI only
};

struct ListResult {
    bool ok = false;
    std::vector<DrillSummary> drills;
    std::string error_message;
};

enum class SortOrder {
    NewestFirst,
    MostDownloaded,
};

struct ListQuery {
    std::string character_filter;  // empty for "all characters"
    std::string search_query;      // FTS prefix; empty for none
    SortOrder sort = SortOrder::NewestFirst;
    int offset = 0;
    int limit = 50;  // server caps at 50 regardless
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
    std::string content;        // full drill text (.drill.txt body)
    std::string author_handle;  // optional self-reported handle
};

struct SubmitResult {
    bool ok = false;
    std::string drill_id;  // server-assigned uuid on success
    bool deduped = false;  // server matched an existing content hash
    bool rate_limited = false;
    std::string error_message;
};

SubmitResult submit_drill(const SubmitArgs& a);

}  // namespace opendojo::cloud::api
