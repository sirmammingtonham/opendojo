#pragma once

#include <cstdio>
#include <string_view>

// Append-only line logger writing to <game>\Polaris\Binaries\Win64\opendojo.log.
// Thread-safe. Falls silent if the log file can't be opened. We have no console
// in release builds — this file is our only window into runtime behavior.
//
// Uses printf-style formatting (deliberately — keeps us off the bleeding edge
// of C++20 std::format support, which varies by MSVC point release).

namespace opendojo::log {

// Open the log file. Call from DllMain attach. Returns false on failure;
// subsequent write()/format() calls become no-ops.
bool init();

// Flush and close. Call from DllMain detach.
void shutdown();

// Write one line. The line is timestamped and newline-terminated by the
// implementation — don't include a trailing newline yourself.
void write(std::string_view line);

// printf-style. fmt and args follow the usual std::printf conventions.
void format(const char* fmt, ...);

}  // namespace opendojo::log

#define OPENDOJO_LOG(...) ::opendojo::log::format(__VA_ARGS__)
