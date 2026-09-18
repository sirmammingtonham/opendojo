#include "cloud/worker.hpp"
#include <atomic>
#include <future>
#include <stdexcept>
#include <iostream>
namespace opendojo::log { void format(const char*, ...) {} }
int main() {
    using namespace opendojo::cloud;
    std::atomic<int> failures{0};
    std::promise<void> completed;
    worker::start();
    worker::submit([] { throw std::runtime_error("malformed response"); }, [&] { ++failures; });
    worker::submit([&] { completed.set_value(); });
    if (completed.get_future().wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        throw std::runtime_error("worker timeout");
    worker::stop();
    worker::submit([] {}, [&] { ++failures; });
    if (failures != 2) throw std::runtime_error("missing failure completion");
    std::cout << "Worker exceptions and rejection complete callers\n";
}
