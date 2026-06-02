#include "cloud.hpp"

#ifndef OPENDOJO_SUPABASE_URL
#define OPENDOJO_SUPABASE_URL ""
#endif
#ifndef OPENDOJO_SUPABASE_ANON_KEY
#define OPENDOJO_SUPABASE_ANON_KEY ""
#endif
#ifndef OPENDOJO_TEKKEN_VERSION
#define OPENDOJO_TEKKEN_VERSION "unknown"
#endif

namespace opendojo::cloud {

namespace {
const std::string g_base_url{OPENDOJO_SUPABASE_URL};
const std::string g_anon_key{OPENDOJO_SUPABASE_ANON_KEY};
const std::string g_game_version{OPENDOJO_TEKKEN_VERSION};

std::string strip_trailing_slash(const std::string& s) {
    if (!s.empty() && s.back() == '/') return s.substr(0, s.size() - 1);
    return s;
}
}  // namespace

bool configured() {
    return !g_base_url.empty() && !g_anon_key.empty();
}

const std::string& base_url() {
    return g_base_url;
}
const std::string& anon_key() {
    return g_anon_key;
}

const std::string& game_version() {
    return g_game_version;
}

std::string rest_url() {
    return strip_trailing_slash(g_base_url) + "/rest/v1";
}
std::string auth_url() {
    return strip_trailing_slash(g_base_url) + "/auth/v1";
}
std::string functions_url() {
    return strip_trailing_slash(g_base_url) + "/functions/v1";
}

}  // namespace opendojo::cloud
