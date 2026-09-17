#pragma once

// Practice lifecycle tracking. The native update dispatcher observes entry;
// the controller deleting-destructor hook invalidates queued work before teardown.
// Exit saves use owned snapshots and never read departing game objects.
namespace opendojo::practice_state {
// Cached state; reading this never invokes engine APIs or transition callbacks.
bool is_active();
// Called only at the native scheduler completion boundary.
void poll();
// Install once after executable discovery.
void install_hooks();
}  // namespace opendojo::practice_state
