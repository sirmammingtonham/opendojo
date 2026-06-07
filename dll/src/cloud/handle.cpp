#include "cloud/handle.hpp"

#include <string>

#include "config.hpp"
#include "log.hpp"
#include "players.hpp"

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
    // Seed it from the Steam persona and persist.
    if (!opendojo::config::author_handle_exists()) {
        auto seed = trim(opendojo::players::local_username());
        opendojo::config::set_author_handle(seed);
        OPENDOJO_LOG("cloud/handle: first-launch seed from Steam: '%s'", seed.c_str());
        return seed;
    }
    // Defensive: if the persisted handle ever drifted to empty (manual
    // edit of config.json, edge case in a prior version), recover by
    // re-seeding from Steam so uploads never go out anonymous.
    auto stored = opendojo::config::author_handle();
    if (trim(stored).empty()) {
        auto seed = trim(opendojo::players::local_username());
        opendojo::config::set_author_handle(seed);
        OPENDOJO_LOG("cloud/handle: persisted handle was empty — re-seeded from Steam: '%s'",
                     seed.c_str());
        return seed;
    }
    return stored;
}

void set(const std::string& value) {
    // Reject empty / whitespace-only overrides. Anonymous uploads aren't
    // allowed — the Settings UI / handle::reset_to_steam() are the only
    // ways back to a valid handle.
    if (trim(value).empty()) {
        OPENDOJO_LOG("cloud/handle: rejected empty override (kept '%s')",
                     opendojo::config::author_handle().c_str());
        return;
    }
    opendojo::config::set_author_handle(value);
    OPENDOJO_LOG("cloud/handle: set to '%s'", trim(value).c_str());
}

void reset_to_steam() {
    auto steam = trim(opendojo::players::local_username());
    opendojo::config::set_author_handle(steam);
    OPENDOJO_LOG("cloud/handle: reset to Steam persona: '%s'", steam.c_str());
}

}  // namespace opendojo::cloud::handle
