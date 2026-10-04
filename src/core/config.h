#pragma once

// Flat "key=value" settings file.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace nm {

class Config {
 public:
  bool load(const std::filesystem::path& path);  // remembers path even if the file is missing
  bool save() const;
  const std::filesystem::path& path() const { return path_; }

  bool has(const std::string& key) const { return values_.count(key) != 0; }
  std::string get(const std::string& key, const std::string& def = {}) const;
  int64_t getInt(const std::string& key, int64_t def = 0) const;
  uint64_t getHex(const std::string& key, uint64_t def = 0) const;

  void set(const std::string& key, const std::string& value) { values_[key] = value; }
  void setInt(const std::string& key, int64_t value) { values_[key] = std::to_string(value); }
  void setHex(const std::string& key, uint64_t value);
  void erase(const std::string& key) { values_.erase(key); }

 private:
  std::filesystem::path path_;
  std::map<std::string, std::string> values_;
};

std::string toHex(uint64_t value);

}  // namespace nm
