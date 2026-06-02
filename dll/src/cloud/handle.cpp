#include "handle.hpp"

#include <string>

#include "../config.hpp"
#include "../log.hpp"
#include "../players.hpp"

namespace opendojo::cloud::handle {

namespace {

std::string trim(std::string s) {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

}  // namespace

std::string current() {
    // First ever read: the author_handle key isn't in config.json yet.
    // Seed it from the Steam persona and persist. After this any read
    // — including an empty one — reflects an explicit user choice.
    if (!opendojo::config::author_handle_exists()) {
        auto seed = trim(opendojo::players::local_username());
        opendojo::config::set_author_handle(seed);
        OPENDOJO_LOG("cloud/handle: first-launch seed from Steam: '%s'", seed.c_str());
        return seed;
    }
    return opendojo::config::author_handle();
}

void set(const std::string& value) {
    opendojo::config::set_author_handle(value);
    OPENDOJO_LOG("cloud/handle: set to '%s'", trim(value).c_str());
}

void reset_to_steam() {
    auto steam = trim(opendojo::players::local_username());
    opendojo::config::set_author_handle(steam);
    OPENDOJO_LOG("cloud/handle: reset to Steam persona: '%s'", steam.c_str());
}

}  // namespace opendojo::cloud::handle
