#include "api.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <sstream>
#include <string>

#include "../log.hpp"
#include "auth.hpp"
#include "cloud.hpp"
#include "http.hpp"

namespace opendojo::cloud::api {

namespace {

using nlohmann::json;

// Build the standard PostgREST/Functions header set: anon apikey +
// user JWT bearer + JSON content type. Auth must already be valid
// when this is called.
std::vector<opendojo::cloud::http::Header> standard_headers() {
    return {
        {"apikey", opendojo::cloud::anon_key()},
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
    s.author_handle = row.value("author_handle", "");
    s.created_at = row.value("created_at", "");
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
    switch (q.sort) {
        case SortOrder::NewestFirst: url << "&order=created_at.desc,id.desc"; break;
        case SortOrder::MostDownloaded: url << "&order=downloads.desc,created_at.desc"; break;
    }
    int limit = q.limit > 0 ? q.limit : 50;
    int offset = q.offset > 0 ? q.offset : 0;
    url << "&limit=" << limit << "&offset=" << offset;

    auto res = opendojo::cloud::http::get(url.str(), standard_headers());
    if (res.transport_error) {
        out.error_message = "couldn't reach OpenDojo Cloud";
        return out;
    }
    if (res.status < 200 || res.status >= 300) {
        out.error_message = "list failed: " + extract_error(res.body);
        OPENDOJO_LOG("cloud/api: list HTTP %ld %s", res.status, res.body.c_str());
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
    if (res.transport_error) {
        out.error_message = "couldn't reach OpenDojo Cloud";
        return out;
    }
    if (res.status == 429 ||
        (res.status == 400 && res.body.find("rate limit") != std::string::npos)) {
        out.rate_limited = true;
        out.error_message = "daily download limit reached";
        return out;
    }
    if (res.status < 200 || res.status >= 300) {
        out.error_message = "download failed: " + extract_error(res.body);
        OPENDOJO_LOG("cloud/api: get HTTP %ld %s", res.status, res.body.c_str());
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

    auto res = opendojo::cloud::http::post(url, standard_headers(), body.dump());
    if (res.transport_error) {
        out.error_message = "couldn't reach OpenDojo Cloud";
        return out;
    }
    if (res.status == 429) {
        out.rate_limited = true;
        out.error_message = "daily upload limit reached (5/day)";
        return out;
    }
    if (res.status < 200 || res.status >= 300) {
        out.error_message = "upload failed: " + extract_error(res.body);
        OPENDOJO_LOG("cloud/api: submit HTTP %ld %s", res.status, res.body.c_str());
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

}  // namespace opendojo::cloud::api
