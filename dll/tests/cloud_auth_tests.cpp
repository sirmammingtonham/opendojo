#include "cloud/auth.hpp"
#include "cloud/http.hpp"
#include "config.hpp"
#include <stdexcept>
#include <iostream>

namespace {
opendojo::config::AuthTokens disk;
opendojo::cloud::http::Response reply;
int signups = 0;
void check(bool condition) { if (!condition) throw std::runtime_error("auth regression"); }
}
namespace opendojo::log { void format(const char*, ...) {} }
namespace opendojo::config {
AuthTokens auth_tokens() { return disk; }
void set_auth_tokens(const AuthTokens& value) { disk = value; }
}
namespace opendojo::cloud {
bool configured() { return true; }
std::string auth_url() { return "https://test.invalid/auth"; }
const std::string& proxy_key() { static std::string key = "test"; return key; }
}
namespace opendojo::cloud::http {
Response post(const std::string& url, const std::vector<Header>&, std::string_view) {
    if (url.ends_with("/signup")) {
        ++signups;
        return {200, R"({"access_token":"new","refresh_token":"new-refresh","user":{"id":"new-user"}})"};
    }
    return reply;
}
}
int main() {
    using namespace opendojo;
    for (const auto& response : {
        cloud::http::Response{429, "{}"}, cloud::http::Response{500, "{}"},
        cloud::http::Response{403, "{}"}, cloud::http::Response{0, "", true},
        cloud::http::Response{400, R"({"code":"unexpected_error"})"},
        cloud::http::Response{400, R"({"code":123})"}}) {
        cloud::auth::forget();
        disk = {"old", "old-refresh", "old-user", 1};
        reply = response;
        check(!cloud::auth::ensure_valid());
        check(signups == 0 && disk.user_id == "old-user" && disk.refresh_token == "old-refresh");
        reply = {200, R"({"access_token":"renewed","refresh_token":"renewed-refresh","user":{"id":"old-user"}})"};
        check(cloud::auth::ensure_valid() && disk.user_id == "old-user");
    }
    cloud::auth::forget();
    disk = {"old", "old-refresh", "old-user", 1};
    reply = {400, R"({"code":"refresh_token_not_found"})"};
    check(cloud::auth::ensure_valid() && signups == 1 && disk.user_id == "new-user");
    std::cout << "Transient authentication failures preserve identity\n";
}
