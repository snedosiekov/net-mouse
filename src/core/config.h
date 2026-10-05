#pragma once

// Flat "key=value" settings file. Thread-safe: the engines (network thread) and the UI change it
// concurrently.

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

namespace nm {

class Config {
 public:
  bool load(const std::filesystem::path& path);  // remembers path even if the file is missing
  bool save() const;
  std::filesystem::path path() const;

  bool has(const std::string& key) const;
  std::string get(const std::string& key, const std::string& def = {}) const;
  int64_t getInt(const std::string& key, int64_t def = 0) const;
  uint64_t getHex(const std::string& key, uint64_t def = 0) const;

  void set(const std::string& key, const std::string& value);
  void setInt(const std::string& key, int64_t value) { set(key, std::to_string(value)); }
  void setHex(const std::string& key, uint64_t value);
  void erase(const std::string& key);

 private:
  mutable std::mutex mutex_;
  std::filesystem::path path_;
  std::map<std::string, std::string> values_;
};

std::string toHex(uint64_t value);

}  // namespace nm
