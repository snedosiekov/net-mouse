#include "core/log.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>

namespace nm::log {
namespace {

std::mutex g_mutex;
std::ofstream g_file;
std::deque<std::string> g_ring;
uint64_t g_counter = 0;
bool g_echo = false;
constexpr size_t kRingSize = 200;

}  // namespace

void init(const std::filesystem::path& file) {
  std::lock_guard lock(g_mutex);
  g_file.open(file, std::ios::app);
}

void info(const std::string& message) {
  const std::time_t t = std::time(nullptr);
  std::tm tmv{};
#ifdef _WIN32
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  char ts[32];
  std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
  std::string line = std::string(ts) + "  " + message;

  std::lock_guard lock(g_mutex);
  if (g_file) {
    g_file << line << '\n';
    g_file.flush();
  }
  if (g_echo) std::fprintf(stderr, "%s\n", line.c_str());
  g_ring.push_back(std::move(line));
  if (g_ring.size() > kRingSize) g_ring.pop_front();
  ++g_counter;
}

std::vector<std::string> recent(size_t count) {
  std::lock_guard lock(g_mutex);
  const size_t n = std::min(count, g_ring.size());
  return {g_ring.end() - static_cast<ptrdiff_t>(n), g_ring.end()};
}

void setEcho(bool enabled) {
  std::lock_guard lock(g_mutex);
  g_echo = enabled;
}

std::string fmt(const char* format, ...) {
  va_list args;
  va_start(args, format);
  va_list copy;
  va_copy(copy, args);
  const int n = std::vsnprintf(nullptr, 0, format, copy);
  va_end(copy);
  std::string s(n > 0 ? static_cast<size_t>(n) : 0, '\0');
  if (n > 0) std::vsnprintf(s.data(), s.size() + 1, format, args);
  va_end(args);
  return s;
}

uint64_t counter() {
  std::lock_guard lock(g_mutex);
  return g_counter;
}

}  // namespace nm::log
