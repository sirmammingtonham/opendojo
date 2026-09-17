#include "slot_labels.hpp"

#include <array>
#include <mutex>

namespace opendojo::slot_labels {

namespace {
std::array<std::string, COUNT> g_names;
std::mutex g_mtx;
}  // namespace

void set(std::size_t idx, std::string name) {
    if (idx >= COUNT) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    g_names[idx] = std::move(name);
}

void clear_all() {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto& n : g_names)
        n.clear();
}

std::string get(std::size_t idx) {
    if (idx >= COUNT) return {};
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_names[idx];
}

}  // namespace opendojo::slot_labels
