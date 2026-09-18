#include "slot_labels.hpp"

#include <array>
#include <mutex>

namespace opendojo::slot_labels {

namespace {
std::array<std::string, COUNT> g_names;
std::mutex g_mtx;
}  // namespace

std::size_t name_prefix_size(std::string_view name) {
    return recording_name::prefix_size(name);
}

void set(std::size_t idx, std::string_view name) {
    if (idx >= COUNT) return;
    auto safe_name = recording_name::normalize(name);
    std::lock_guard<std::mutex> lk(g_mtx);
    g_names[idx] = std::move(safe_name);
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
