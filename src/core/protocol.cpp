#include "core/protocol.h"

#include <algorithm>
#include <type_traits>

namespace nm::proto {
namespace {

constexpr size_t kHeaderSize = 4 + 1 + 1 + 8 + 4;                        // magic, version, type, sender, seq
constexpr size_t kSealedAdSize = kHeaderSize + 8 + 1 + crypto::kNonceSize;  // + target, key kind, nonce
constexpr uint8_t kSealedType = 7;

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
  void bytes(const uint8_t* data, size_t size) { buf.insert(buf.end(), data, data + size); }
  template <size_t N>
  void bytes(const std::array<uint8_t, N>& a) {
    bytes(a.data(), N);
  }
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
  void header(uint8_t type, uint64_t sender, uint32_t seq) {
    u32(kMagic);
    u8(kVersion);
    u8(type);
    u64(sender);
    u32(seq);
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
  template <size_t N>
  std::array<uint8_t, N> bytes() {
    std::array<uint8_t, N> a{};
    for (uint8_t& b : a) b = u8();
    return a;
  }
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

uint8_t typeOf(const Body& body) { return static_cast<uint8_t>(body.index() + 1); }

bool isSealedOnly(uint8_t type) { return type == 3 || type == 4 || type == 5 || type == 6 || type == 8; }

void encodeBody(Writer& w, const Body& body) {
  std::visit(
      [&w](const auto& m) {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, MasterAnnounce>) {
          w.str(m.name);
          w.bytes(m.publicKey);
        } else if constexpr (std::is_same_v<T, SlaveHello>) {
          w.str(m.name);
          w.u64(m.masterId);
          w.u32(m.configVersion);
          const size_t n = std::min(m.monitors.size(), kMaxMonitors);
          w.u8(static_cast<uint8_t>(n));
          for (size_t i = 0; i < n; ++i) w.rect(m.monitors[i]);
          w.u32(m.address);
          w.bytes(m.publicKey);
          w.bytes(m.sessionNonce);
          w.u64(m.sessionId);
          w.bytes(m.mac);
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
          w.bytes(m.keys.bits);
        } else if constexpr (std::is_same_v<T, Leave>) {
          w.u64(m.target);
        } else if constexpr (std::is_same_v<T, Key>) {
          w.u64(m.target);
          w.u16(m.code);
          w.u8(m.value);
          w.bytes(m.held.bits);
        } else if constexpr (std::is_same_v<T, SessionInit>) {
          w.u64(m.target);
          w.bytes(m.masterNonce);
          w.bytes(m.slaveNonce);
        }
        // Sealed is never encoded field by field: its datagram is already complete.
      },
      body);
}

std::optional<Body> decodeBody(Reader& r, uint8_t type) {
  switch (type) {
    case 1: {
      MasterAnnounce m;
      m.name = r.str();
      m.publicKey = r.bytes<32>();
      return m;
    }
    case 2: {
      SlaveHello m;
      m.name = r.str();
      m.masterId = r.u64();
      m.configVersion = r.u32();
      const size_t n = r.u8();
      if (n > kMaxMonitors) return std::nullopt;
      for (size_t i = 0; i < n && r.ok(); ++i) m.monitors.push_back(r.rect());
      m.address = r.u32();
      m.publicKey = r.bytes<32>();
      m.sessionNonce = r.bytes<16>();
      m.sessionId = r.u64();
      m.mac = r.bytes<16>();
      return m;
    }
    case 3: {
      SlaveConfig m;
      m.target = r.u64();
      m.version = r.u32();
      m.offset = r.point();
      return m;
    }
    case 4: {
      Pointer m;
      m.target = r.u64();
      m.pos = r.point();
      m.buttons = r.u8();
      m.wheelV = r.i16();
      m.wheelH = r.i16();
      m.keys.bits = r.bytes<32>();
      return m;
    }
    case 5: {
      Leave m;
      m.target = r.u64();
      return m;
    }
    case 6: {
      Key m;
      m.target = r.u64();
      m.code = r.u16();
      m.value = r.u8();
      m.held.bits = r.bytes<32>();
      return m;
    }
    case 8: {
      SessionInit m;
      m.target = r.u64();
      m.masterNonce = r.bytes<16>();
      m.slaveNonce = r.bytes<16>();
      return m;
    }
    default:
      return std::nullopt;
  }
}

uint64_t targetOf(const Body& body) {
  return std::visit(
      [](const auto& m) -> uint64_t {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, MasterAnnounce> || std::is_same_v<T, SlaveHello>) {
          return 0;
        } else {
          return m.target;
        }
      },
      body);
}

}  // namespace

std::vector<uint8_t> encode(const Packet& packet) {
  if (const auto* sealed = std::get_if<Sealed>(&packet.body)) return sealed->datagram;
  Writer w;
  w.header(typeOf(packet.body), packet.sender, packet.seq);
  encodeBody(w, packet.body);
  return std::move(w.buf);
}

std::vector<uint8_t> seal(const Packet& inner, uint64_t target, KeyKind kind, const crypto::Key& key) {
  Writer plain;
  plain.u8(typeOf(inner.body));
  encodeBody(plain, inner.body);

  Writer w;
  w.header(kSealedType, inner.sender, inner.seq);
  w.u64(target);
  w.u8(static_cast<uint8_t>(kind));
  const auto nonce = crypto::randomArray<std::array<uint8_t, crypto::kNonceSize>>();
  w.bytes(nonce);
  std::vector<uint8_t> out = std::move(w.buf);
  const size_t adSize = out.size();
  out.resize(adSize + plain.buf.size() + crypto::kMacSize);
  crypto::seal(key, nonce.data(), out.data(), adSize, plain.buf.data(), plain.buf.size(), out.data() + adSize,
               out.data() + adSize + plain.buf.size());
  return out;
}

std::optional<Packet> decode(const uint8_t* data, size_t size) {
  Reader r(data, size);
  if (r.u32() != kMagic || r.u8() != kVersion) return std::nullopt;
  const uint8_t type = r.u8();
  Packet p;
  p.sender = r.u64();
  p.seq = r.u32();
  if (!r.ok()) return std::nullopt;
  if (type == kSealedType) {
    Sealed s;
    s.target = r.u64();
    const uint8_t kind = r.u8();
    if (!r.ok() || kind > 1 || size < kSealedAdSize + 1 + crypto::kMacSize) return std::nullopt;
    s.kind = static_cast<KeyKind>(kind);
    s.datagram.assign(data, data + size);
    p.body = std::move(s);
    return p;
  }
  if (isSealedOnly(type)) return std::nullopt;  // control messages are never accepted unencrypted
  auto body = decodeBody(r, type);
  if (!body || !r.ok() || r.remaining() != 0) return std::nullopt;
  p.body = std::move(*body);
  return p;
}

std::optional<Packet> open(const Packet& sealedPacket, const crypto::Key& key) {
  const auto* s = std::get_if<Sealed>(&sealedPacket.body);
  if (!s || s->datagram.size() < kSealedAdSize + 1 + crypto::kMacSize) return std::nullopt;
  const uint8_t* data = s->datagram.data();
  const size_t cipherSize = s->datagram.size() - kSealedAdSize - crypto::kMacSize;
  std::vector<uint8_t> plain(cipherSize);
  if (!crypto::open(key, data + kSealedAdSize - crypto::kNonceSize, data, kSealedAdSize, data + kSealedAdSize,
                    cipherSize, data + kSealedAdSize + cipherSize, plain.data())) {
    return std::nullopt;
  }
  Reader r(plain.data(), plain.size());
  const uint8_t type = r.u8();
  if (!isSealedOnly(type)) return std::nullopt;
  auto body = decodeBody(r, type);
  if (!body || !r.ok() || r.remaining() != 0) return std::nullopt;
  if (targetOf(*body) != s->target) return std::nullopt;  // the readable target must match the sealed one
  Packet p;
  p.sender = sealedPacket.sender;
  p.seq = sealedPacket.seq;
  p.body = std::move(*body);
  return p;
}

std::vector<uint8_t> helloAuthData(uint64_t sender, const SlaveHello& hello) {
  SlaveHello copy = hello;
  copy.mac = {};
  Packet p;
  p.sender = sender;
  p.body = std::move(copy);
  return encode(p);
}

}  // namespace nm::proto
