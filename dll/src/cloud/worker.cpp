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
void submit(std::function<void()> job) {
    if (!instance().submit(std::move(job))) {
        OPENDOJO_LOG("cloud/worker: rejected job after shutdown");
    }
}
}  // namespace opendojo::cloud::worker
