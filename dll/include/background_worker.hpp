#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "log.hpp"

namespace opendojo {

// Serial owned jobs. Never pass game pointers in jobs. stop() must be called
// outside DllMain and outside a job; it joins the active job and optionally
// drains pending work. Once stopped, the worker cannot be restarted.
class BackgroundWorker {
public:
    ~BackgroundWorker() { stop(true); }

    void start() {
        std::lock_guard lifecycle(lifecycle_);
        std::lock_guard lock(mutex_);
        if (started_ || stopping_) return;
        thread_ = std::thread([this] { run(); });
        started_ = true;
    }

    bool submit(std::function<void()> job) {
        if (!job) return false;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return false;
            queue_.push_back(std::move(job));
        }
        ready_.notify_one();
        return true;
    }

    void stop(bool drain) {
        std::lock_guard lifecycle(lifecycle_);
        std::deque<std::function<void()>> discarded;
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            if (!drain || !started_) discarded.swap(queue_);
        }
        ready_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

private:
    void run() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) return;
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            try {
                job();
            } catch (const std::exception& e) {
                OPENDOJO_LOG("background worker: job failed: %s", e.what());
            } catch (...) {
                OPENDOJO_LOG("background worker: job failed with unknown exception");
            }
        }
    }

    std::mutex lifecycle_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> queue_;
    std::thread thread_;
    bool started_ = false;
    bool stopping_ = false;
};

}  // namespace opendojo
