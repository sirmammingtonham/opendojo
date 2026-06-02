#pragma once

// Cloud-related ImGui surfaces. Pulled out of menu.cpp so the cloud
// feature has a clean home and menu.cpp doesn't accrete network
// state. Functions here are called from menu::draw() on the render
// thread; background work goes through cloud::worker.

namespace opendojo::cloud::ui {

// Draw the "Browse" tab body. Owns its own state (search query,
// filter, result list cache) inside the .cpp.
void draw_browse_tab();

// Draw the "Upload to OpenDojo Cloud" row at the bottom of the
// Export tab. Disabled if `can_export` is false or cloud is not
// configured. `name` / `description` are pointers to the existing
// menu form buffers — uploads use the same values the local Export
// would have written.
void draw_upload_export_row(bool can_export, const char* name, const char* description);

// Draw the "OpenDojo Cloud" section inside the Settings tab —
// currently just the author handle override field. Renders nothing
// if cloud isn't configured for this build.
void draw_settings_section();

}  // namespace opendojo::cloud::ui
