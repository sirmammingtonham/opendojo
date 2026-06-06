#pragma once

#include <string>

// ImGui menu surface. Drawn from the render hook each frame the menu is
// visible. State (drill list cache, form buffers, toast) is owned by the
// .cpp — there's only one instance, lifetime equals process lifetime.

namespace opendojo::menu {

// Draw one frame of the menu. Caller must already have called ImGui::NewFrame
// for this frame and will call ImGui::Render afterward.
void draw();

// Force a re-scan of opendojo/ on the next draw. Cheap to call.
void invalidate();

// Thread-safe entry points for the cloud worker. The worker thread
// publishes UI effects via these; the render thread drains them at
// the top of each draw().
void queue_toast(std::string text, bool is_error);
void queue_drills_refresh();

}  // namespace opendojo::menu
