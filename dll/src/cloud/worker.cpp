#include "worker.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "../log.hpp"

namespace opendojo::cloud::worker {

namespace {

std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<std::function<void()>> g_queue;
std::atomic<bool> g_shutdown{false};
std::atomic<bool> g_running{false};
std::thread g_thread;

void run() {
    OPENDOJO_LOG("cloud/worker: thread started");
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock lk(g_mutex);
            g_cv.wait(lk, [] { return g_shutdown.load() || !g_queue.empty(); });
            if (g_shutdown.load() && g_queue.empty()) break;
            job = std::move(g_queue.front());
            g_queue.pop_front();
        }
        try {
            job();
        } catch (const std::exception& e) {
            OPENDOJO_LOG("cloud/worker: job threw: %s", e.what());
        } catch (...) {
            OPENDOJO_LOG("cloud/worker: job threw non-std exception");
        }
    }
    OPENDOJO_LOG("cloud/worker: thread exiting");
}

}  // namespace

void start() {
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true)) return;
    g_shutdown.store(false);
    g_thread = std::thread(run);
}

void stop() {
    if (!g_running.exchange(false)) return;
    g_shutdown.store(true);
    g_cv.notify_all();
    if (g_thread.joinable()) g_thread.join();
}

void submit(std::function<void()> job) {
    if (!job) return;
    {
        std::lock_guard lk(g_mutex);
        g_queue.push_back(std::move(job));
    }
    g_cv.notify_one();
}

}  // namespace opendojo::cloud::worker
