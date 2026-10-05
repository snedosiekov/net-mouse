#include "core/config.h"

#include <charconv>
#include <fstream>

namespace nm {
namespace {

std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t");
  if (b == std::string::npos) return {};
  const auto e = s.find_last_not_of(" \t");
  return s.substr(b, e - b + 1);
}

}  // namespace

std::string toHex(uint64_t value) {
  char buf[17];
  auto res = std::to_chars(buf, buf + sizeof(buf), value, 16);
  std::string s(buf, res.ptr);
  return std::string(16 - s.size(), '0') + s;
}

bool Config::load(const std::filesystem::path& path) {
  std::lock_guard lock(mutex_);
  path_ = path;
  values_.clear();
  std::ifstream in(path);
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::string t = trim(line);
    if (t.empty() || t[0] == '#') continue;
    const auto eq = t.find('=');
    if (eq == std::string::npos) continue;
    values_[trim(t.substr(0, eq))] = trim(t.substr(eq + 1));
  }
  return true;
}

bool Config::save() const {
  std::lock_guard lock(mutex_);
  std::error_code ec;
  if (path_.has_parent_path()) std::filesystem::create_directories(path_.parent_path(), ec);
  std::filesystem::path tmp = path_;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return false;
    out << "# net-mouse configuration\n";
    for (const auto& [k, v] : values_) out << k << '=' << v << '\n';
    if (!out) return false;
  }
  std::filesystem::rename(tmp, path_, ec);
  return !ec;
}

std::filesystem::path Config::path() const {
  std::lock_guard lock(mutex_);
  return path_;
}

bool Config::has(const std::string& key) const {
  std::lock_guard lock(mutex_);
  return values_.count(key) != 0;
}

void Config::set(const std::string& key, const std::string& value) {
  std::lock_guard lock(mutex_);
  values_[key] = value;
}

void Config::erase(const std::string& key) {
  std::lock_guard lock(mutex_);
  values_.erase(key);
}

std::string Config::get(const std::string& key, const std::string& def) const {
  std::lock_guard lock(mutex_);
  auto it = values_.find(key);
  return it == values_.end() ? def : it->second;
}

int64_t Config::getInt(const std::string& key, int64_t def) const {
  std::lock_guard lock(mutex_);
  auto it = values_.find(key);
  if (it == values_.end()) return def;
  int64_t v = 0;
  const auto& s = it->second;
  auto res = std::from_chars(s.data(), s.data() + s.size(), v);
  return res.ec == std::errc() ? v : def;
}

uint64_t Config::getHex(const std::string& key, uint64_t def) const {
  std::lock_guard lock(mutex_);
  auto it = values_.find(key);
  if (it == values_.end()) return def;
  uint64_t v = 0;
  const auto& s = it->second;
  auto res = std::from_chars(s.data(), s.data() + s.size(), v, 16);
  return res.ec == std::errc() ? v : def;
}

void Config::setHex(const std::string& key, uint64_t value) { set(key, toHex(value)); }

}  // namespace nm
