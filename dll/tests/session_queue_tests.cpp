#include "session_queue.hpp"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

void check(bool ok) {
    if (!ok) throw std::runtime_error("session queue regression");
}
int main() {
    using opendojo::SessionQueue;
    using Clock = SessionQueue::Clock;
    const auto now = Clock::now();
    std::uint64_t epoch = 1;
    int failures = 0, calls = 0;
    SessionQueue q([&] { ++failures; });
    std::vector<bool> eligible;
    check(q.push(
        epoch,
        [&](bool ok) {
            eligible.push_back(ok);
            ++epoch;
        },
        now));
    check(q.push(epoch, [&](bool ok) { eligible.push_back(ok); }, now));
    q.drain([&] { return epoch; }, now);
    check(eligible == std::vector<bool>({true, false}));
    check(q.push(
        epoch,
        [&](bool ok) {
            check(!ok);
            ++calls;
        },
        now));
    q.drain([&] { return epoch; }, now + std::chrono::seconds(5));
    check(calls == 1);
    check(q.push(
        epoch,
        [&](bool ok) {
            check(ok);
            check(q.push(
                epoch,
                [&](bool child) {
                    check(child);
                    ++calls;
                },
                now));
        },
        now));
    q.drain([&] { return epoch; }, now);
    check(calls == 1);  // Reentrant submission is deferred and does not deadlock.
    q.drain([&] { return epoch; }, now);
    check(calls == 2);
    check(q.push(epoch, [](bool) { throw std::runtime_error("request failure"); }, now));
    check(q.push(
        epoch,
        [&](bool ok) {
            check(ok);
            ++calls;
        },
        now));
    q.drain([&] { return epoch; }, now);
    check(failures == 1 && calls == 3);
    for (int i = 0; i < 8; ++i)
        check(q.push(epoch, [&](bool) { ++calls; }, now));
    check(!q.push(epoch, [](bool) {}, now));
    q.drain([&] { return epoch; }, now);
    check(calls == 11);

    check(q.push(epoch, [&](bool ok) {
        check(!ok);
        ++calls;
    }));
    q.cancel();
    q.drain([&] { return epoch; });
    check(calls == 12);

    SessionQueue concurrent;
    std::atomic<int> accepted{0}, delivered{0}, done{0};
    std::vector<std::thread> producers;
    for (int i = 0; i < 4; ++i)
        producers.emplace_back([&] {
            for (int j = 0; j < 500; ++j)
                if (concurrent.push(1, [&](bool ok) {
                        check(ok);
                        ++delivered;
                    }))
                    ++accepted;
            ++done;
        });
    while (done.load() != 4) {
        concurrent.drain([] { return 1; });
        std::this_thread::yield();
    }
    for (auto& producer : producers)
        producer.join();
    concurrent.drain([] { return 1; });
    check(delivered == accepted && accepted > 0);
    std::cout << "Session cancellation, deadlines, capacity, reentrancy and concurrent submission "
                 "passed\n";
}
