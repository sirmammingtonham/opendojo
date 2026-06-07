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

// Call immediately after a widget that lives in a scrollable region.
// On the frame gamepad/keyboard nav focuses this widget, scrolls the
// active ImGui window so the widget sits centered in the viewport,
// snapping flush to the scroll extremes when it's the first/last
// focusable item. No-op for mouse users (gated on io.NavActive) and
// fires only once per focus change so it doesn't fight an editing
// user's wheel scroll. Exposed so cloud_ui's share-card widgets can
// drive the parent scroll the same way.
void nav_recenter();

}  // namespace opendojo::menu
