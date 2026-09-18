#include <windows.h>

#include "background_worker.hpp"
#include "file_io.hpp"
#include "write_batch.hpp"
#include "ui/text_buffers.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <vector>

namespace opendojo::log {
void format(const char*, ...) {}
}  // namespace opendojo::log

static void check(bool value) {
    if (!value) throw std::runtime_error("runtime test failed");
}

int main() {
    {
        char name[opendojo::ui::DRILL_NAME_BUFFER_SIZE];
        char description[opendojo::ui::DESCRIPTION_BUFFER_SIZE];
        std::string unicode;
        for (int i = 0; i < 1000; ++i) unicode += "\xe9\xa2\xa8";
        opendojo::ui::copy_text(description, unicode);
        check(description == unicode);
        opendojo::ui::copy_text(name, std::string(96, 'a'));
        check(std::strlen(name) == 96);
        char short_buffer[5];
        opendojo::ui::copy_text(short_buffer, "ab\xe9\xa2\xa8");
        check(std::string(short_buffer) == "ab");
    }

    using namespace opendojo;
    using namespace std::chrono_literals;
    {
        std::istringstream input("name: Test\r\n---\n");
        std::size_t budget = 4096;
        std::string line;
        check(file_io::bounded_getline(input, line, budget) && line == "name: Test\r");
        check(file_io::bounded_getline(input, line, budget) && line == "---");
        check(!file_io::bounded_getline(input, line, budget));
        std::istringstream oversized(std::string(1024 * 1024, 'x'));
        budget = 4096;
        check(!file_io::bounded_getline(oversized, line, budget));
        check(budget == 0 && line.empty() && oversized.tellg() == 4096);
        std::istringstream unterminated("name: Last");
        budget = 4096;
        check(file_io::bounded_getline(unterminated, line, budget) && line == "name: Last");
    }
    {
        BackgroundWorker worker;
        std::vector<int> order;
        check(worker.submit([&] { order.push_back(1); }));
        worker.start();
        worker.start();
        check(worker.submit([] { throw std::runtime_error("expected"); }));
        check(worker.submit([&] { order.push_back(2); }));
        worker.stop(true);
        check(order == std::vector<int>({1, 2}));
        check(!worker.submit([] {}));
        worker.start();
        check(!worker.submit([] {}));
    }
    {
        BackgroundWorker worker;
        std::promise<void> entered, release;
        auto gate = release.get_future().share();
        worker.start();
        worker.submit([&] {
            entered.set_value();
            gate.wait();
        });
        check(entered.get_future().wait_for(5s) == std::future_status::ready);
        std::atomic<int> pending_ran{0};
        worker.submit([&] { ++pending_ran; });
        auto stopped = std::async(std::launch::async, [&] { worker.stop(false); });
        // Observe rejection to establish that shutdown has taken the queue lock.
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (worker.submit([&] { ++pending_ran; })) {
            check(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        check(stopped.wait_for(0ms) == std::future_status::timeout);
        release.set_value();
        check(stopped.wait_for(5s) == std::future_status::ready);
        stopped.get();
        check(pending_ran == 0);
    }
    {
        // Concurrent submissions all drain once, without racing start or join.
        BackgroundWorker worker;
        worker.start();
        std::atomic<int> ran{0};
        std::vector<std::thread> producers;
        for (int i = 0; i < 4; ++i)
            producers.emplace_back([&] {
                for (int j = 0; j < 500; ++j)
                    check(worker.submit([&] { ++ran; }));
            });
        for (auto& producer : producers)
            producer.join();
        auto stop1 = std::async(std::launch::async, [&] { worker.stop(true); });
        worker.stop(true);
        stop1.get();
        check(ran == 2000);
    }
    {
        std::uint32_t first = 1, second = 2;
        WriteBatch changed;
        changed.add(reinterpret_cast<std::uintptr_t>(&first), 3u);
        changed.add(reinterpret_cast<std::uintptr_t>(&second), 4u);
        second = 9;
        check(changed.commit() == WriteBatch::Result::Rejected);
        check(first == 1 && second == 9);
        WriteBatch valid;
        valid.add(reinterpret_cast<std::uintptr_t>(&first), 5u);
        valid.add(reinterpret_cast<std::uintptr_t>(&first), 6u);
        check(valid.commit() == WriteBatch::Result::Ok && first == 6);
        check(valid.commit() == WriteBatch::Result::Rejected);
        auto page = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        check(page != nullptr);
        WriteBatch protected_page;
        protected_page.add(reinterpret_cast<std::uintptr_t>(&first), 7u);
        protected_page.add(reinterpret_cast<std::uintptr_t>(page), 8u);
        DWORD old = 0;
        check(VirtualProtect(page, 4096, PAGE_READONLY, &old) != 0);
        check(protected_page.commit() == WriteBatch::Result::Rejected && first == 6);
        VirtualFree(page, 0, MEM_RELEASE);
    }
    {
        auto directory = std::filesystem::temp_directory_path() /
                         (L"opendojo-tests-" + std::to_wstring(GetCurrentProcessId()));
        check(std::filesystem::create_directory(directory));
        auto path = directory / L"保存.drill.txt";
        auto read = [&] {
            std::ifstream file(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(file), {});
        };
        check(file_io::replace_file(path, "original"));
        check(read() == "original");
        check(file_io::create_file(path, "must not replace") == file_io::CreateResult::Exists);
        check(read() == "original");
        const auto raced = directory / L"concurrent.drill.txt";
        std::atomic<int> saved{0}, existed{0};
        auto create = [&] {
            const auto result = file_io::create_file(raced, "complete content");
            if (result == file_io::CreateResult::Saved) ++saved;
            if (result == file_io::CreateResult::Exists) ++existed;
        };
        std::thread first(create), second(create);
        first.join(); second.join();
        check(saved == 1 && existed == 1);
        std::ifstream raced_input(raced, std::ios::binary);
        check(std::string(std::istreambuf_iterator<char>(raced_input), {}) == "complete content");
        raced_input.close();
        std::filesystem::remove(raced);
        auto held = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        check(held != INVALID_HANDLE_VALUE);
        check(!file_io::replace_file(path, "must not replace locked file"));
        check(read() == "original");
        CloseHandle(held);
        std::size_t count = 0;
        for (auto& item : std::filesystem::directory_iterator(directory)) {
            (void)item;
            ++count;
        }
        check(count == 1);  // failed replacement cleaned its own temporary file
        const std::string large(3 * 1024 * 1024, 'x');
        check(file_io::replace_file(path, large));
        check(read() == large);
        check(file_io::replace_file(path, ""));
        check(read().empty());
        std::filesystem::remove(path);
        std::filesystem::remove(directory);
    }
    std::cout << "Worker concurrency, write preflight, and atomic file tests passed\n";
}
