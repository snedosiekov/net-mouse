#include "core/json.h"

#include <cstdio>
#include <cstdlib>

namespace nm::json {
namespace {

const Value kNull;

class Parser {
 public:
  explicit Parser(const std::string& text) : t_(text) {}

  std::optional<Value> document() {
    Value v;
    if (!value(v, 0)) return std::nullopt;
    skip();
    if (pos_ != t_.size()) return std::nullopt;
    return v;
  }

 private:
  void skip() {
    while (pos_ < t_.size() && (t_[pos_] == ' ' || t_[pos_] == '\t' || t_[pos_] == '\n' || t_[pos_] == '\r')) ++pos_;
  }
  bool literal(const char* word) {
    size_t i = 0;
    while (word[i]) {
      if (pos_ + i >= t_.size() || t_[pos_ + i] != word[i]) return false;
      ++i;
    }
    pos_ += i;
    return true;
  }
  bool value(Value& v, int depth) {
    if (depth > 32) return false;
    skip();
    if (pos_ >= t_.size()) return false;
    const char c = t_[pos_];
    if (c == '{') return object(v, depth);
    if (c == '[') return array(v, depth);
    if (c == '"') {
      v.type = Value::Type::String;
      return string(v.s);
    }
    if (literal("true")) {
      v.type = Value::Type::Bool;
      v.b = true;
      return true;
    }
    if (literal("false")) {
      v.type = Value::Type::Bool;
      return true;
    }
    if (literal("null")) return true;
    return number(v);
  }
  bool number(Value& v) {
    const char* start = t_.c_str() + pos_;
    char* end = nullptr;
    const double d = std::strtod(start, &end);
    if (end == start) return false;
    pos_ += static_cast<size_t>(end - start);
    v.type = Value::Type::Number;
    v.n = d;
    return true;
  }
  bool string(std::string& out) {
    ++pos_;  // opening quote
    while (pos_ < t_.size()) {
      const char c = t_[pos_++];
      if (c == '"') return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (pos_ >= t_.size()) return false;
      const char e = t_[pos_++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          if (pos_ + 4 > t_.size()) return false;
          const unsigned cp = static_cast<unsigned>(std::strtoul(t_.substr(pos_, 4).c_str(), nullptr, 16));
          pos_ += 4;
          if (cp < 0x80) {
            out += static_cast<char>(cp);
          } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
          } else {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
          }
          break;
        }
        default:
          return false;
      }
    }
    return false;
  }
  bool array(Value& v, int depth) {
    v.type = Value::Type::Array;
    ++pos_;
    skip();
    if (pos_ < t_.size() && t_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (true) {
      Value item;
      if (!value(item, depth + 1)) return false;
      v.a.push_back(std::move(item));
      skip();
      if (pos_ >= t_.size()) return false;
      if (t_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (t_[pos_] == ']') {
        ++pos_;
        return true;
      }
      return false;
    }
  }
  bool object(Value& v, int depth) {
    v.type = Value::Type::Object;
    ++pos_;
    skip();
    if (pos_ < t_.size() && t_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (true) {
      skip();
      std::string key;
      if (pos_ >= t_.size() || t_[pos_] != '"' || !string(key)) return false;
      skip();
      if (pos_ >= t_.size() || t_[pos_] != ':') return false;
      ++pos_;
      Value item;
      if (!value(item, depth + 1)) return false;
      v.o.emplace_back(std::move(key), std::move(item));
      skip();
      if (pos_ >= t_.size()) return false;
      if (t_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (t_[pos_] == '}') {
        ++pos_;
        return true;
      }
      return false;
    }
  }

  const std::string& t_;
  size_t pos_ = 0;
};

}  // namespace

std::string escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char hex[8];
          std::snprintf(hex, sizeof(hex), "\\u%04x", c);
          out += hex;
        } else {
          out += c;
        }
    }
  }
  return out;
}

const Value& Value::operator[](const std::string& key) const {
  if (type == Type::Object) {
    for (const auto& [k, v] : o) {
      if (k == key) return v;
    }
  }
  return kNull;
}

const Value& Value::operator[](size_t index) const {
  return type == Type::Array && index < a.size() ? a[index] : kNull;
}

std::optional<Value> parse(const std::string& text) { return Parser(text).document(); }

}  // namespace nm::json
