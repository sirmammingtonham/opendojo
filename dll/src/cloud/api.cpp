#include "cloud/api.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <exception>
#include <sstream>
#include <string>

#include "cloud/auth.hpp"
#include "cloud/cloud.hpp"
#include "cloud/http.hpp"
#include "log.hpp"

namespace opendojo::cloud::api {

namespace {

using nlohmann::json;

// nlohmann's json::value(key, default) returns the default only when the key
// is ABSENT — a key that is present but JSON null throws type_error.302.
// PostgREST sends nullable columns (description, cpu_side, difficulty, …) as
// explicit null, so reading them with value() would throw and abort the whole
// parse — stranding callers (e.g. the in-game browse tab) on a permanent
// "loading". value_or treats null like absent, so one row with an empty
// optional field can't sink the rest.
template <typename T>
T value_or(const json& row, const char* key, const T& fallback) {
    auto it = row.find(key);
    if (it == row.end() || it->is_null()) return fallback;
    return it->get<T>();
}
// Overload so string-literal defaults deduce to std::string rather than
// const char* (which json::get<> can't produce).
inline std::string value_or(const json& row, const char* key, const char* fallback) {
    return value_or<std::string>(row, key, std::string(fallback));
}

// Standard request headers: access key + user JWT bearer + JSON content
// type. Auth must already be valid when this is called.
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

// Case-insensitive substring search. Used to detect Supabase's
// "Project is paused / unhealthy" 503 body without caring about
// exact phrasing or capitalization.
bool body_contains_ci(const std::string& body, const char* needle) {
    auto to_lower = [](unsigned char c) { return std::tolower(c); };
    auto it = std::search(body.begin(), body.end(), needle, needle + std::strlen(needle),
                          [&](char a, char b) { return to_lower(a) == to_lower(b); });
    return it != body.end();
}

// Single classification path for every API call's response. Maps failures to
// clean, user-facing messages and NEVER surfaces raw server text (status
// codes, Postgres messages/hints, raw bodies) — those go only to the local
// log. `trust_body` is set on the upload path, where the Edge Function returns
// its own safe, user-facing copy (validation / profanity / ban) we do want to
// show. `verb` is a short action phrase, e.g. "load drills".
struct FailureInfo {
    bool failed = false;
    std::string message;
    bool rate_limited = false;
};

FailureInfo classify_http_failure(const opendojo::cloud::http::Response& res, const char* verb,
                                  bool trust_body = false) {
    FailureInfo f;
    if (res.transport_error) {
        f.failed = true;
        f.message = "Couldn't reach OpenDojo Cloud. Check your connection and try again.";
        return f;
    }
    if (res.status >= 200 && res.status < 300) return f;

    f.failed = true;
    // The raw response is for local diagnostics only — never shown to the user.
    OPENDOJO_LOG("cloud/api: %s HTTP %ld %s", verb, res.status, res.body.c_str());

    if (res.status == 429) {
        f.rate_limited = true;
        f.message = "Rate limit reached. Try again in a moment.";
        return f;
    }

    // Supabase free-tier auto-pauses after inactivity; the first hit is a 503
    // mentioning "paused"/"unhealthy" and the project is back in ~30s.
    if (res.status == 503 || body_contains_ci(res.body, "paused") ||
        body_contains_ci(res.body, "unhealthy")) {
        f.message = "OpenDojo Cloud is temporarily unavailable. Try again shortly.";
        return f;
    }

    // Upload path: the Edge Function returns safe, user-facing copy in
    // `error` for VALIDATION (400) and BAN (403) errors specifically —
    // those messages are written for end users ("Name too long", "This
    // account has been banned from uploading.", etc.). All other status
    // codes (401 "invalid token", 405 "method not allowed", 413 "request
    // body too large", 500 "internal error", …) carry internal-sounding
    // strings we never want to render. Restrict trust_body to those two
    // statuses so the server can keep adding diagnostic detail to its
    // other branches without it leaking to the UI.
    if (trust_body && (res.status == 400 || res.status == 403)) {
        auto j = json::parse(res.body, nullptr, false);
        if (j.is_object() && j.contains("error") && j["error"].is_string()) {
            f.message = j["error"].get<std::string>();
            return f;
        }
    }

    if (res.status == 401 || res.status == 403) {
        f.message = "Couldn't authenticate with OpenDojo Cloud. Try again in a moment.";
        return f;
    }

    f.message = std::string("Couldn't ") + verb + ". Please try again.";
    return f;
}

DrillSummary parse_summary(const json& row) {
    DrillSummary s;
    // Required (NOT NULL) columns: read strictly. A missing/null value here
    // means the row is genuinely malformed, so let it throw — the caller's
    // per-row try/catch skips just that row and keeps the rest of the list.
    s.id = row.value("id", "");
    s.name = row.value("name", "");
    s.character = row.value("character", "");
    s.recordings_count = row.value("recordings_count", 0);
    s.size_bytes = row.value("size_bytes", static_cast<std::int64_t>(0));
    s.downloads = row.value("downloads", static_cast<std::int64_t>(0));
    s.likes = row.value("likes", static_cast<std::int64_t>(0));
    s.author_handle = row.value("author_handle", "");
    s.created_at = row.value("created_at", "");
    s.description = value_or(row, "description", "");
    s.cpu_side = value_or(row, "cpu_side", "");
    s.difficulty = value_or(row, "difficulty", "");
    s.is_mine = value_or(row, "is_mine", false);
    s.is_liked = value_or(row, "liked_by_me", false);
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
        out.error_message = "Couldn't connect to OpenDojo Cloud. Try again in a moment.";
        return out;
    }

    std::ostringstream url;
    url << opendojo::cloud::rest_url() << "/drill_summaries?";
    url << "select=*";
    if (!q.character_filter.empty()) {
        url << "&character=eq." << url_encode(q.character_filter);
    }
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
    if (q.mine_only) {
        // drill_summaries computes is_mine via auth.uid() server-side, so
        // filtering on it returns just the caller's uploads.
        url << "&is_mine=eq.true";
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
    if (auto fail = classify_http_failure(res, "load drills"); fail.failed) {
        out.error_message = std::move(fail.message);
        return out;
    }

    auto j = json::parse(res.body, nullptr, false);
    if (!j.is_array()) {
        out.error_message = "Couldn't load drills. Please try again.";
        return out;
    }
    out.drills.reserve(j.size());
    int skipped = 0;
    for (const auto& row : j) {
        // Parse each row independently: a single malformed entry is hidden
        // rather than failing the whole list, so valid drills always load.
        // (Empty optional fields — difficulty, description, categories, … —
        // are handled by value_or and never land here.)
        try {
            out.drills.push_back(parse_summary(row));
        } catch (const std::exception& e) {
            ++skipped;
            OPENDOJO_LOG("cloud/api: skipped unparseable drill row: %s", e.what());
        }
    }
    if (skipped)
        OPENDOJO_LOG("cloud/api: list_drills skipped %d of %d rows", skipped,
                     static_cast<int>(j.size()));
    out.ok = true;
    return out;
}

GetResult get_drill(const std::string& id) {
    GetResult out;
    if (!opendojo::cloud::auth::ensure_valid()) {
        out.error_message = "Couldn't connect to OpenDojo Cloud. Try again in a moment.";
        return out;
    }

    auto url = opendojo::cloud::rest_url() + "/rpc/get_drill";
    json body;
    body["p_drill_id"] = id;

    auto res = opendojo::cloud::http::post(url, standard_headers(), body.dump());
    if (auto fail = classify_http_failure(res, "download that drill"); fail.failed) {
        out.error_message = std::move(fail.message);
        out.rate_limited = fail.rate_limited;
        return out;
    }

    auto j = json::parse(res.body, nullptr, false);
    // The function returns a table; PostgREST exposes that as an array.
    if (!j.is_array() || j.empty()) {
        out.error_message = "That drill isn't available anymore.";
        return out;
    }
    const auto& row = j[0];
    // All columns get_drill reads are NOT NULL in the schema, so strict reads
    // are correct — a null here would be a real server-side error.
    out.drill.id = row.value("id", "");
    out.drill.name = row.value("name", "");
    out.drill.character = row.value("character", "");
    out.drill.content = row.value("content", "");
    out.drill.recordings_count = row.value("recordings_count", 0);
    if (out.drill.content.empty()) {
        out.error_message = "Couldn't download that drill. Please try again.";
        return out;
    }
    out.ok = true;
    return out;
}

SubmitResult submit_drill(const SubmitArgs& a) {
    SubmitResult out;
    if (!opendojo::cloud::auth::ensure_valid()) {
        out.error_message = "Couldn't connect to OpenDojo Cloud. Try again in a moment.";
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
    if (!a.dll_version.empty()) body["dll_version"] = a.dll_version;

    auto res = opendojo::cloud::http::post(url, standard_headers(), body.dump());
    if (auto fail = classify_http_failure(res, "upload your drill", /*trust_body=*/true);
        fail.failed) {
        out.error_message = std::move(fail.message);
        out.rate_limited = fail.rate_limited;
        return out;
    }

    auto j = json::parse(res.body, nullptr, false);
    if (!j.is_object()) {
        out.error_message = "Couldn't upload your drill. Please try again.";
        return out;
    }
    out.drill_id = j.value("id", "");
    out.deduped = j.value("deduped", false);
    out.ok = !out.drill_id.empty();
    if (!out.ok) out.error_message = "Couldn't upload your drill. Please try again.";
    return out;
}

LikeResult toggle_like(const std::string& drill_id) {
    LikeResult out;
    if (!opendojo::cloud::auth::ensure_valid()) {
        out.error_message = "Couldn't connect to OpenDojo Cloud. Try again in a moment.";
        return out;
    }

    auto url = opendojo::cloud::rest_url() + "/rpc/toggle_like";
    json body;
    body["p_drill_id"] = drill_id;

    // PostgREST returns a single integer for scalar-returning RPCs.
    // Ask for a "Prefer: return=representation" minimal response shape.
    auto headers = standard_headers();
    auto res = opendojo::cloud::http::post(url, headers, body.dump());
    if (auto fail = classify_http_failure(res, "save your like"); fail.failed) {
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
        out.error_message = "Couldn't connect to OpenDojo Cloud. Try again in a moment.";
        return out;
    }

    auto url = opendojo::cloud::rest_url() + "/rpc/delete_my_drill";
    json body;
    body["p_drill_id"] = drill_id;

    auto res = opendojo::cloud::http::post(url, standard_headers(), body.dump());
    if (auto fail = classify_http_failure(res, "delete that drill"); fail.failed) {
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
    if (!out.deleted) {
        out.error_message = "That drill isn't yours to delete.";
    }
    return out;
}

UpdateResult update_drill(const UpdateArgs& args) {
    UpdateResult out;
    if (!opendojo::cloud::auth::ensure_valid()) {
        out.error_message = "Couldn't connect to OpenDojo Cloud. Try again in a moment.";
        return out;
    }

    // Edits go through the update_drill Edge Function (not a direct RPC) so
    // they get the same server-side validation + profanity screen as uploads.
    auto url = opendojo::cloud::functions_url() + "/update_drill";
    json body;
    body["id"] = args.drill_id;
    body["name"] = args.name;
    body["description"] = args.description;
    body["categories"] = args.categories;  // empty array clears all tags
    if (!args.difficulty.empty()) body["difficulty"] = args.difficulty;

    // trust_body: the Edge Function returns safe, user-facing copy in `error`
    // (validation / profanity / ban), same contract as submit_drill.
    auto res = opendojo::cloud::http::post(url, standard_headers(), body.dump());
    if (auto fail = classify_http_failure(res, "save your changes", /*trust_body=*/true);
        fail.failed) {
        out.error_message = std::move(fail.message);
        return out;
    }

    // Success body is { "updated": bool } — true if the row was found AND
    // owned by the caller. A non-owner / missing id returns updated=false.
    auto j = json::parse(res.body, nullptr, false);
    if (j.is_object()) {
        out.updated = j.value("updated", false);
    }
    out.ok = true;
    if (!out.updated && out.error_message.empty()) {
        out.error_message = "Couldn't save your changes. Please try again.";
    }
    return out;
}

ReportResult report_drill(const std::string& drill_id, const std::string& reason) {
    ReportResult out;
    if (!opendojo::cloud::auth::ensure_valid()) {
        out.error_message = "Couldn't connect to OpenDojo Cloud. Try again in a moment.";
        return out;
    }

    auto url = opendojo::cloud::rest_url() + "/rpc/report_drill";
    json body;
    body["p_drill_id"] = drill_id;
    if (!reason.empty()) body["p_reason"] = reason;

    auto res = opendojo::cloud::http::post(url, standard_headers(), body.dump());
    if (auto fail = classify_http_failure(res, "submit your report"); fail.failed) {
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

ServiceMessageResult get_service_message() {
    ServiceMessageResult out;
    if (!opendojo::cloud::auth::ensure_valid()) {
        out.error_message = "Couldn't connect to OpenDojo Cloud. Try again in a moment.";
        return out;
    }

    // The view returns the latest title text and applicable update independently.
    // Older database versions return the newest matching announcement instead.
    // Selecting all also works before the optional update_version column exists.
    auto url = opendojo::cloud::rest_url() + "/active_service_messages?select=*&limit=1";

    auto headers = standard_headers();
    headers.push_back({"X-OpenDojo-Version", opendojo::cloud::dll_version()});
    auto res = opendojo::cloud::http::get(url, headers);
    if (auto fail = classify_http_failure(res, "load messages"); fail.failed) {
        out.error_message = std::move(fail.message);
        return out;
    }

    auto j = json::parse(res.body, nullptr, false);
    if (!j.is_array()) {
        out.error_message = "Couldn't load messages. Please try again.";
        return out;
    }
    if (!j.empty() && j[0].is_object()) {
        out.message = j[0].value("message", "");
        out.update_version = value_or(j[0], "update_version", "");
        out.present = !out.message.empty();
    }
    out.ok = true;
    return out;
}

}  // namespace opendojo::cloud::api
