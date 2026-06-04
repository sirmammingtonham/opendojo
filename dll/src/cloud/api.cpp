#include "api.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

#include "../log.hpp"
#include "auth.hpp"
#include "cloud.hpp"
#include "http.hpp"

namespace opendojo::cloud::api {

namespace {

using nlohmann::json;

// Build the standard PostgREST/Functions header set: proxy access key +
// user JWT bearer + JSON content type. The backend apikey is injected by
// the proxy, never sent by the client. Auth must already be valid when
// this is called.
std::vector<opendojo::cloud::http::Header> standard_headers() {
    return {
        {"X-OpenDojo-Key", opendojo::cloud::proxy_key()},
        {"Authorization", "Bearer " + opendojo::cloud::auth::access_token()},
        {"Content-Type", "application/json"},
        {"Accept", "application/json"},
    };
}

// Percent-encode every byte that PostgREST might interpret as a
// filter operator separator. We URL-encode quite aggressively —
// search terms are user-controlled and may contain commas, parens,
// or dots, all of which are syntactic in PostgREST filters.
std::string url_encode(std::string_view in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

// Sniff a server-side error message out of a non-2xx body. Both
// PostgREST and our Edge Function return JSON with either a
// `message` or `error` key; fall back to the raw body.
std::string extract_error(const std::string& body) {
    auto j = json::parse(body, nullptr, false);
    if (j.is_object()) {
        if (j.contains("error") && j["error"].is_string()) return j["error"];
        if (j.contains("message") && j["message"].is_string()) return j["message"];
        if (j.contains("hint") && j["hint"].is_string()) return j["hint"];
    }
    if (body.empty()) return "(no detail)";
    if (body.size() > 200) return body.substr(0, 200) + "...";
    return body;
}

// Case-insensitive substring search. Used to detect Supabase's
// "Project is paused / unhealthy" 503 body without caring about
// exact phrasing or capitalization.
bool body_contains_ci(const std::string& body, const char* needle) {
    auto to_lower = [](unsigned char c) { return std::tolower(c); };
    auto it = std::search(body.begin(), body.end(), needle, needle + std::strlen(needle),
                          [&](char a, char b) { return to_lower(a) == to_lower(b); });
    return it != body.end();
}

// Single classification path for every API call's response. Detects
// transport failures, rate-limit responses (429 or PostgREST's
// 400-with-rate-limit-body), and Supabase-paused 503s — and assembles
// the user-facing message. Returns failed=false for 2xx.
struct FailureInfo {
    bool failed = false;
    std::string message;
    bool rate_limited = false;
};

FailureInfo classify_http_failure(const opendojo::cloud::http::Response& res, const char* verb) {
    FailureInfo f;
    if (res.transport_error) {
        f.failed = true;
        f.message = "couldn't reach OpenDojo Cloud";
        return f;
    }
    if (res.status >= 200 && res.status < 300) return f;

    // Rate limited — server returns 429 directly, or PostgREST
    // surfaces our plpgsql RAISE as a 400 with "rate limit" in the
    // message. Both go through the same UI path.
    if (res.status == 429 || (res.status == 400 && body_contains_ci(res.body, "rate limit"))) {
        f.failed = true;
        f.rate_limited = true;
        f.message = "Daily limit reached — try again tomorrow.";
        return f;
    }

    // Supabase free-tier auto-pauses after 7 days of inactivity. The
    // first request hits a 503 with a body that mentions "paused" or
    // "unhealthy"; the project resumes automatically and is back in
    // ~30s. Show a non-scary message that hints at the wait.
    if (res.status == 503 || body_contains_ci(res.body, "paused") ||
        body_contains_ci(res.body, "unhealthy")) {
        f.failed = true;
        f.message = "OpenDojo Cloud is starting up — try again in a few seconds.";
        return f;
    }

    // Generic failure — log the raw response for diagnostics and
    // surface whatever the server told us, prefixed by the verb so
    // the user knows which action errored.
    f.failed = true;
    f.message = std::string(verb) + " failed: " + extract_error(res.body);
    OPENDOJO_LOG("cloud/api: %s HTTP %ld %s", verb, res.status, res.body.c_str());
    return f;
}

DrillSummary parse_summary(const json& row) {
    DrillSummary s;
    s.id = row.value("id", "");
    s.name = row.value("name", "");
    s.description = row.value("description", "");
    s.character = row.value("character", "");
    s.cpu_side = row.value("cpu_side", "");
    s.recordings_count = row.value("recordings_count", 0);
    s.size_bytes = row.value("size_bytes", static_cast<std::int64_t>(0));
    s.downloads = row.value("downloads", static_cast<std::int64_t>(0));
    s.likes = row.value("likes", static_cast<std::int64_t>(0));
    s.author_handle = row.value("author_handle", "");
    s.difficulty = row.value("difficulty", "");
    s.game_version = row.value("game_version", "");
    s.created_at = row.value("created_at", "");
    s.is_mine = row.value("is_mine", false);
    // categories is a Postgres text[] coming through PostgREST as a
    // JSON array. Missing or non-array => empty list.
    if (row.contains("categories") && row["categories"].is_array()) {
        for (const auto& c : row["categories"]) {
            if (c.is_string()) s.categories.push_back(c.get<std::string>());
        }
    }
    return s;
}

}  // namespace

ListResult list_drills(const ListQuery& q) {
    ListResult out;
    if (!opendojo::cloud::auth::ensure_valid()) {
        out.error_message = "couldn't reach OpenDojo Cloud (auth)";
        return out;
    }

    std::ostringstream url;
    url << opendojo::cloud::rest_url() << "/drill_summaries?";
    url << "select=*";
    if (!q.character_filter.empty()) { url << "&character=eq." << url_encode(q.character_filter); }
    if (!q.search_query.empty()) {
        // PostgREST FTS: ?search_tsv=fts(simple).<term>. Use prefix
        // matching by appending :* so "jin" matches "jin string".
        url << "&search_tsv=fts(simple)." << url_encode(q.search_query + ":*");
    }
    if (!q.category_filter.empty()) {
        // PostgREST array overlap operator: categories=ov.{a,b,c}.
        // Returns drills tagged with ANY of the requested categories.
        // Each id is alphanumeric+underscore, so url-encode is a noop
        // but kept for safety.
        url << "&categories=ov.{";
        for (std::size_t i = 0; i < q.category_filter.size(); ++i) {
            if (i) url << ',';
            url << url_encode(q.category_filter[i]);
        }
        url << '}';
    }
    if (!q.difficulty_filter.empty()) {
        url << "&difficulty=eq." << url_encode(q.difficulty_filter);
    }
    switch (q.sort) {
        case SortOrder::NewestFirst: url << "&order=created_at.desc,id.desc"; break;
        case SortOrder::MostDownloaded: url << "&order=downloads.desc,created_at.desc"; break;
        case SortOrder::MostLiked: url << "&order=likes.desc,created_at.desc"; break;
    }
    int limit = q.limit > 0 ? q.limit : 50;
    int offset = q.offset > 0 ? q.offset : 0;
    url << "&limit=" << limit << "&offset=" << offset;

    auto res = opendojo::cloud::http::get(url.str(), standard_headers());
    if (auto fail = classify_http_failure(res, "list"); fail.failed) {
        out.error_message = std::move(fail.message);
        return out;
    }

    auto j = json::parse(res.body, nullptr, false);
    if (!j.is_array()) {
        out.error_message = "list returned unexpected payload";
        return out;
    }
    out.drills.reserve(j.size());
    for (const auto& row : j)
        out.drills.push_back(parse_summary(row));
    out.ok = true;
    return out;
}

GetResult get_drill(const std::string& id) {
    GetResult out;
    if (!opendojo::cloud::auth::ensure_valid()) {
        out.error_message = "couldn't reach OpenDojo Cloud (auth)";
        return out;
    }

    auto url = opendojo::cloud::rest_url() + "/rpc/get_drill";
    json body;
    body["p_drill_id"] = id;

    auto res = opendojo::cloud::http::post(url, standard_headers(), body.dump());
    if (auto fail = classify_http_failure(res, "download"); fail.failed) {
        out.error_message = std::move(fail.message);
        out.rate_limited = fail.rate_limited;
        return out;
    }

    auto j = json::parse(res.body, nullptr, false);
    // The function returns a table; PostgREST exposes that as an array.
    if (!j.is_array() || j.empty()) {
        out.error_message = "drill not found";
        return out;
    }
    const auto& row = j[0];
    out.drill.id = row.value("id", "");
    out.drill.name = row.value("name", "");
    out.drill.character = row.value("character", "");
    out.drill.content = row.value("content", "");
    out.drill.recordings_count = row.value("recordings_count", 0);
    if (out.drill.content.empty()) {
        out.error_message = "drill has no content";
        return out;
    }
    out.ok = true;
    return out;
}

SubmitResult submit_drill(const SubmitArgs& a) {
    SubmitResult out;
    if (!opendojo::cloud::auth::ensure_valid()) {
        out.error_message = "couldn't reach OpenDojo Cloud (auth)";
        return out;
    }

    auto url = opendojo::cloud::functions_url() + "/submit_drill";
    json body;
    body["name"] = a.name;
    body["description"] = a.description;
    body["character"] = a.character;
    body["cpu_side"] = a.cpu_side;
    body["recordings_count"] = a.recordings_count;
    body["content"] = a.content;
    if (!a.author_handle.empty()) body["author_handle"] = a.author_handle;
    body["categories"] = a.categories;  // empty array is fine
    if (!a.difficulty.empty()) body["difficulty"] = a.difficulty;
    if (!a.game_version.empty()) body["game_version"] = a.game_version;

    auto res = opendojo::cloud::http::post(url, standard_headers(), body.dump());
    if (auto fail = classify_http_failure(res, "upload"); fail.failed) {
        out.error_message = std::move(fail.message);
        out.rate_limited = fail.rate_limited;
        return out;
    }

    auto j = json::parse(res.body, nullptr, false);
    if (!j.is_object()) {
        out.error_message = "upload returned unexpected payload";
        return out;
    }
    out.drill_id = j.value("id", "");
    out.deduped = j.value("deduped", false);
    out.ok = !out.drill_id.empty();
    if (!out.ok) out.error_message = "upload returned no id";
    return out;
}

LikeResult toggle_like(const std::string& drill_id) {
    LikeResult out;
    if (!opendojo::cloud::auth::ensure_valid()) {
        out.error_message = "couldn't reach OpenDojo Cloud (auth)";
        return out;
    }

    auto url = opendojo::cloud::rest_url() + "/rpc/toggle_like";
    json body;
    body["p_drill_id"] = drill_id;

    // PostgREST returns a single integer for scalar-returning RPCs.
    // Ask for a "Prefer: return=representation" minimal response shape.
    auto headers = standard_headers();
    auto res = opendojo::cloud::http::post(url, headers, body.dump());
    if (auto fail = classify_http_failure(res, "like"); fail.failed) {
        out.error_message = std::move(fail.message);
        return out;
    }

    auto j = json::parse(res.body, nullptr, false);
    // PostgREST encodes scalar function returns as a bare number in
    // JSON: e.g. `7`. Defensive: also accept [{..}] / {value:..} shapes.
    if (j.is_number_integer()) {
        out.likes = j.get<std::int64_t>();
    } else if (j.is_array() && !j.empty() && j[0].is_number_integer()) {
        out.likes = j[0].get<std::int64_t>();
    } else {
        out.likes = 0;
    }
    out.ok = true;
    return out;
}

DeleteResult delete_my_drill(const std::string& drill_id) {
    DeleteResult out;
    if (!opendojo::cloud::auth::ensure_valid()) {
        out.error_message = "couldn't reach OpenDojo Cloud (auth)";
        return out;
    }

    auto url = opendojo::cloud::rest_url() + "/rpc/delete_my_drill";
    json body;
    body["p_drill_id"] = drill_id;

    auto res = opendojo::cloud::http::post(url, standard_headers(), body.dump());
    if (auto fail = classify_http_failure(res, "delete"); fail.failed) {
        out.error_message = std::move(fail.message);
        return out;
    }

    // RPC returns the boolean as a bare JSON literal: true / false.
    auto j = json::parse(res.body, nullptr, false);
    if (j.is_boolean()) {
        out.deleted = j.get<bool>();
    } else if (j.is_array() && !j.empty() && j[0].is_boolean()) {
        out.deleted = j[0].get<bool>();
    }
    out.ok = true;
    if (!out.deleted) { out.error_message = "drill not found or not yours"; }
    return out;
}

ReportResult report_drill(const std::string& drill_id, const std::string& reason) {
    ReportResult out;
    if (!opendojo::cloud::auth::ensure_valid()) {
        out.error_message = "couldn't reach OpenDojo Cloud (auth)";
        return out;
    }

    auto url = opendojo::cloud::rest_url() + "/rpc/report_drill";
    json body;
    body["p_drill_id"] = drill_id;
    if (!reason.empty()) body["p_reason"] = reason;

    auto res = opendojo::cloud::http::post(url, standard_headers(), body.dump());
    if (auto fail = classify_http_failure(res, "report"); fail.failed) {
        out.error_message = std::move(fail.message);
        return out;
    }

    auto j = json::parse(res.body, nullptr, false);
    if (j.is_boolean())
        out.reported = j.get<bool>();
    else if (j.is_array() && !j.empty() && j[0].is_boolean())
        out.reported = j[0].get<bool>();
    out.ok = true;
    return out;
}

}  // namespace opendojo::cloud::api
