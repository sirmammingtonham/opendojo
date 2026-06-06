#pragma once

#include <functional>

// Single background thread that runs blocking cloud calls so the
// render hook never stalls on HTTPS. Each job is a std::function<void()>
// that owns its own state captures and is responsible for publishing
// results back to UI state under that UI's own mutex.
//
// Lifecycle: start() once at DLL init, stop() once at DLL detach.
// submit() is safe to call from any thread, including before start()
// (the job will run as soon as the worker thread comes up).

namespace opendojo::cloud::worker {

void start();
void stop();

// Enqueue a job. Returns immediately. Jobs run in FIFO order on a
// single thread, so a click that fires three jobs sees them complete
// in click order.
void submit(std::function<void()> job);

}  // namespace opendojo::cloud::worker
