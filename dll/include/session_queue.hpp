#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>

namespace opendojo {
// Queue ownership is independent of game memory. No callback runs under its lock.
class SessionQueue {
public:
    using Clock = std::chrono::steady_clock;
    using Callback = std::function<void(bool)>;
    explicit SessionQueue(std::function<void()> failure = {}) : failure_(std::move(failure)) {}
    bool push(std::uint64_t epoch, Callback callback, Clock::time_point now = Clock::now()) {
        std::lock_guard lock(mutex_);
        if (jobs_.size() >= 8 || !callback) return false;
        jobs_.push_back({epoch, now + std::chrono::seconds(5), std::move(callback)});
        return true;
    }
    void drain(const std::function<std::uint64_t()>& epoch, Clock::time_point now = Clock::now()) {
        std::deque<Job> jobs;
        {
            std::lock_guard lock(mutex_);
            jobs.swap(jobs_);
        }
        for (auto& job : jobs) {
            try {
                job.callback(job.epoch == epoch() && now < job.deadline);
            } catch (...) {
                if (failure_) failure_();
            }
        }
    }
    void cancel() {
        std::deque<Job> jobs;
        {
            std::lock_guard lock(mutex_);
            jobs.swap(jobs_);
        }
        for (auto& job : jobs) {
            try {
                job.callback(false);
            } catch (...) {
                if (failure_) failure_();
            }
        }
    }

private:
    struct Job {
        std::uint64_t epoch;
        Clock::time_point deadline;
        Callback callback;
    };
    std::function<void()> failure_;
    std::mutex mutex_;
    std::deque<Job> jobs_;
};
}  // namespace opendojo
