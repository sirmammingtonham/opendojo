#include "cloud/worker.hpp"

#include "background_worker.hpp"

namespace opendojo::cloud::worker {
namespace {
BackgroundWorker& instance() {
    // The proxy is pinned for process lifetime. Avoid a joinable std::thread
    // destructor under the loader lock when Windows terminates the process.
    static auto* worker = new BackgroundWorker;
    return *worker;
}
}  // namespace

void start() {
    instance().start();
}
void stop() {
    instance().stop(false);
}
void submit(std::function<void()> job, std::function<void()> on_failure) {
    auto guarded = [job = std::move(job), on_failure] {
        try {
            job();
        } catch (...) {
            if (on_failure) on_failure();
            throw;  // The worker still logs the original failure.
        }
    };
    if (!instance().submit(std::move(guarded))) {
        if (on_failure) on_failure();
        OPENDOJO_LOG("cloud/worker: rejected job after shutdown");
    }
}
}  // namespace opendojo::cloud::worker
