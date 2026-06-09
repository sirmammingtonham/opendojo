#pragma once

#include <string>

// Cloud-related ImGui surfaces. Pulled out of menu.cpp so the cloud
// feature has a clean home and menu.cpp doesn't accrete network
// state. Functions here are called from menu::draw() on the render
// thread; background work goes through cloud::worker.

namespace opendojo::cloud::ui {

// Draw the "Cloud" tab body. Owns its own state (search query,
// filter, result list cache) inside the .cpp.
void draw_cloud_tab();

// Draw the contents of the "Share with community" card on the
// Export tab: tag chips, difficulty combo, author handle line,
// "Share to OpenDojo Cloud" button, and the persistent last-upload
// status line. Caller is responsible for placing the card (e.g. a
// child window with a border) and rendering the card header.
// Disabled if `can_export` is false or cloud is not configured.
// `name` / `description` are pointers to the menu form buffers —
// uploads use the same values the local Export would have written.
void draw_share_card_body(bool can_export, const char* name, const char* description);

// Draw the "OpenDojo Cloud" section inside the Settings tab —
// currently just the author handle override field. Renders nothing
// if cloud isn't configured for this build.
void draw_settings_section();

// Poll for the operator broadcast message shown in the window title
// bar. Cheap to call every frame: it kicks one cloud-worker fetch on
// the first call and at most one every few minutes thereafter, so the
// render thread never blocks. No-op when cloud isn't configured.
void poll_service_message();

// The current operator broadcast text, or "" if there's none / it
// hasn't been fetched yet. Read on the render thread to build the
// window title. Always safe to call.
std::string service_message();

}  // namespace opendojo::cloud::ui
