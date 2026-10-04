#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nm::log {

void init(const std::filesystem::path& file);
void info(const std::string& message);
// Last `count` lines, oldest first.
std::vector<std::string> recent(size_t count);
// Increments on every new line; lets the UI repaint only when something changed.
uint64_t counter();

// Sends every line to stderr as well (Linux service: captured by journald).
void setEcho(bool enabled);

// printf-style formatting (std::format is unavailable in GCC < 13).
std::string fmt(const char* format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

}  // namespace nm::log
