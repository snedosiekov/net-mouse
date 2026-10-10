#pragma once

// Minimal JSON for the local control protocol (status replies): escaping for writers and a small
// parser for readers. Not a general-purpose library: no \u escapes beyond the BMP, numbers as double.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nm::json {

std::string escape(const std::string& s);

struct Value {
  enum class Type { Null, Bool, Number, String, Array, Object };
  Type type = Type::Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Value> a;
  std::vector<std::pair<std::string, Value>> o;

  // Missing keys / indexes give a null value, so lookups can be chained.
  const Value& operator[](const std::string& key) const;
  const Value& operator[](size_t index) const;
  size_t size() const { return type == Type::Array ? a.size() : (type == Type::Object ? o.size() : 0); }

  bool asBool(bool def = false) const { return type == Type::Bool ? b : def; }
  double asNumber(double def = 0) const { return type == Type::Number ? n : def; }
  int64_t asInt(int64_t def = 0) const { return type == Type::Number ? static_cast<int64_t>(n) : def; }
  std::string asString(const std::string& def = {}) const { return type == Type::String ? s : def; }
};

std::optional<Value> parse(const std::string& text);

}  // namespace nm::json
