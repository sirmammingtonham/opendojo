#pragma once

// DLL proxy: load the real Windows DLL we're masquerading as (e.g. the
// system dinput8.dll) and resolve its exports so our forwarders can pass
// calls through transparently. The game must never notice we replaced
// its DLL — every export it asks for has to behave identically.

namespace opendojo::proxy {

// Call from DLL_PROCESS_ATTACH before any forwarded export can run.
// Returns false if the real DLL can't be loaded — at which point we
// should fail the DllMain so the game falls back to its normal path
// (otherwise the game crashes on the first forwarded call).
bool load();

// Call from DLL_PROCESS_DETACH. Safe to call even if load() failed.
void unload();

}  // namespace opendojo::proxy
