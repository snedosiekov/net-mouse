#include "core/protocol.h"

#include <algorithm>
#include <type_traits>

namespace nm::proto {
namespace {

class Writer {
 public:
  void u8(uint8_t v) { buf.push_back(v); }
  void u16(uint16_t v) {
    u8(static_cast<uint8_t>(v));
    u8(static_cast<uint8_t>(v >> 8));
  }
  void u32(uint32_t v) {
    u16(static_cast<uint16_t>(v));
    u16(static_cast<uint16_t>(v >> 16));
  }
  void u64(uint64_t v) {
    u32(static_cast<uint32_t>(v));
    u32(static_cast<uint32_t>(v >> 32));
  }
  void i16(int16_t v) { u16(static_cast<uint16_t>(v)); }
  void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
  void str(const std::string& s) {
    const size_t n = std::min(s.size(), kMaxString);
    u8(static_cast<uint8_t>(n));
    buf.insert(buf.end(), s.begin(), s.begin() + static_cast<ptrdiff_t>(n));
  }
  void point(Point p) {
    i32(p.x);
    i32(p.y);
  }
  void rect(const Rect& r) {
    i32(r.left);
    i32(r.top);
    i32(r.right);
    i32(r.bottom);
  }

  std::vector<uint8_t> buf;
};

class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  bool ok() const { return ok_; }

  size_t remaining() const { return ok_ ? size_ - pos_ : 0; }

  uint8_t u8() {
    if (!ok_ || pos_ >= size_) {
      ok_ = false;
      return 0;
    }
    return data_[pos_++];
  }
  uint16_t u16() {
    const uint16_t lo = u8();
    const uint16_t hi = u8();
    return static_cast<uint16_t>(lo | (hi << 8));
  }
  uint32_t u32() {
    const uint32_t lo = u16();
    const uint32_t hi = u16();
    return lo | (hi << 16);
  }
  uint64_t u64() {
    const uint64_t lo = u32();
    const uint64_t hi = u32();
    return lo | (hi << 32);
  }
  int16_t i16() { return static_cast<int16_t>(u16()); }
  int32_t i32() { return static_cast<int32_t>(u32()); }
  std::string str() {
    const size_t n = u8();
    if (!ok_ || pos_ + n > size_) {
      ok_ = false;
      return {};
    }
    std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
    pos_ += n;
    return s;
  }
  Point point() {
    Point p;
    p.x = i32();
    p.y = i32();
    return p;
  }
  Rect rect() {
    Rect r;
    r.left = i32();
    r.top = i32();
    r.right = i32();
    r.bottom = i32();
    return r;
  }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
  bool ok_ = true;
};

}  // namespace

std::vector<uint8_t> encode(const Packet& packet) {
  Writer w;
  w.u32(kMagic);
  w.u8(kVersion);
  w.u8(static_cast<uint8_t>(packet.body.index() + 1));
  w.u64(packet.sender);
  w.u32(packet.seq);
  std::visit(
      [&w](const auto& m) {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, MasterAnnounce>) {
          w.str(m.name);
        } else if constexpr (std::is_same_v<T, SlaveHello>) {
          w.str(m.name);
          w.u64(m.masterId);
          w.u32(m.configVersion);
          const size_t n = std::min(m.monitors.size(), kMaxMonitors);
          w.u8(static_cast<uint8_t>(n));
          for (size_t i = 0; i < n; ++i) w.rect(m.monitors[i]);
          w.u32(m.address);  // appended field: older decoders ignore trailing bytes
        } else if constexpr (std::is_same_v<T, SlaveConfig>) {
          w.u64(m.target);
          w.u32(m.version);
          w.point(m.offset);
        } else if constexpr (std::is_same_v<T, Pointer>) {
          w.u64(m.target);
          w.point(m.pos);
          w.u8(m.buttons);
          w.i16(m.wheelV);
          w.i16(m.wheelH);
        } else if constexpr (std::is_same_v<T, Leave>) {
          w.u64(m.target);
        }
      },
      packet.body);
  return std::move(w.buf);
}

std::optional<Packet> decode(const uint8_t* data, size_t size) {
  Reader r(data, size);
  if (r.u32() != kMagic || r.u8() != kVersion) return std::nullopt;
  const uint8_t type = r.u8();
  Packet p;
  p.sender = r.u64();
  p.seq = r.u32();
  switch (type) {
    case 1: {
      MasterAnnounce m;
      m.name = r.str();
      p.body = std::move(m);
      break;
    }
    case 2: {
      SlaveHello m;
      m.name = r.str();
      m.masterId = r.u64();
      m.configVersion = r.u32();
      const size_t n = r.u8();
      if (n > kMaxMonitors) return std::nullopt;
      for (size_t i = 0; i < n && r.ok(); ++i) m.monitors.push_back(r.rect());
      if (r.remaining() >= 4) m.address = r.u32();  // absent in hellos from older slaves
      p.body = std::move(m);
      break;
    }
    case 3: {
      SlaveConfig m;
      m.target = r.u64();
      m.version = r.u32();
      m.offset = r.point();
      p.body = m;
      break;
    }
    case 4: {
      Pointer m;
      m.target = r.u64();
      m.pos = r.point();
      m.buttons = r.u8();
      m.wheelV = r.i16();
      m.wheelH = r.i16();
      p.body = m;
      break;
    }
    case 5: {
      Leave m;
      m.target = r.u64();
      p.body = m;
      break;
    }
    default:
      return std::nullopt;
  }
  if (!r.ok()) return std::nullopt;
  return p;
}

}  // namespace nm::proto
