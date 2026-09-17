#pragma once

#include <cstdint>
#include <functional>
#include "players.hpp"

namespace opendojo::game_thread {
// Runs after the native scheduler has joined its workers and processed cleanup.
void install();
bool available();
players::CpuInfo current_cpu();
bool is_game_thread();
// Stronger than thread identity: true only inside our serialized update callback.
bool is_current();
// Bounded, nonblocking submission. False callbacks can run on the invalidating
// thread and must not touch game objects; true callbacks run only at the boundary.
bool enqueue(std::function<void(bool)> job);
void invalidate();
std::uint64_t epoch();
void request_flush();
}  // namespace opendojo::game_thread
