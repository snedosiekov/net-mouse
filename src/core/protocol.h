#pragma once

// Wire protocol v2. Every datagram goes to one UDP multicast group and starts with a plain header:
// magic, version, type, 64-bit sender id, sequence number (integers little-endian).
//
// Only discovery is plain: MasterAnnounce and SlaveHello carry names, screen sizes and public keys.
// Everything that controls a PC (configuration, pointer, keys) travels in a Sealed envelope:
//   header | target id | key kind | 24-byte nonce | XChaCha20-Poly1305(inner type + fields) | MAC
// The header, target, key kind and nonce are readable (routing) but authenticated. A plain
// datagram of a sealed-only type is rejected by decode().

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "core/crypto.h"
#include "core/geometry.h"

namespace nm::proto {

constexpr uint32_t kMagic = 0x45534D4E;  // "NMSE"
constexpr uint8_t kVersion = 2;
constexpr size_t kMaxString = 63;
constexpr size_t kMaxMonitors = 16;

enum Button : uint8_t {
  kButtonLeft = 1 << 0,
  kButtonRight = 1 << 1,
  kButtonMiddle = 1 << 2,
  kButtonX1 = 1 << 3,
  kButtonX2 = 1 << 4,
};

// Keys held for one slave: Linux (evdev) key codes below 256 as a 256-bit mask. Sent with every key
// event and pointer heartbeat, so a slave can release keys whose release packet was lost.
struct KeySet {
  std::array<uint8_t, 32> bits{};

  bool has(uint16_t code) const { return code < 256 && ((bits[code >> 3] >> (code & 7)) & 1) != 0; }
  void set(uint16_t code, bool held) {
    if (code >= 256) return;
    const uint8_t mask = static_cast<uint8_t>(1u << (code & 7));
    bits[code >> 3] = held ? (bits[code >> 3] | mask) : (bits[code >> 3] & static_cast<uint8_t>(~mask));
  }
  friend bool operator==(const KeySet&, const KeySet&) = default;
};

// --- plain (discovery) -------------------------------------------------------------------------

// Master -> all, once per second: "a master is here", with its public key.
struct MasterAnnounce {
  std::string name;
  crypto::Key publicKey{};
};

// Slave -> all, once per second and right after a session or configuration change.
struct SlaveHello {
  std::string name;
  std::vector<Rect> monitors;      // the slave's own desktop coordinates (physical pixels)
  uint64_t masterId = 0;           // master this slave is paired with (0 = none)
  uint32_t configVersion = 0;      // version of the applied configuration
  uint32_t address = 0;            // slave's main IPv4 (the one on its default route), 0 = unknown
  crypto::Key publicKey{};         // slave's identity
  crypto::Nonce16 sessionNonce{};  // fresh value the next session key must be built from
  uint64_t sessionId = 0;          // keyId of the current session key (0 = none)
  crypto::Mac mac{};               // keyed with the pair key when paired, so the master can trust the above
};

// --- sealed (encrypted + authenticated) ---------------------------------------------------------

// Master -> slave: where the slave's desktop sits in the master's layout space.
// slave_local = master_layout_position - offset.
struct SlaveConfig {
  uint64_t target = 0;
  uint32_t version = 0;
  Point offset;
};

// Master -> slave: complete pointer state in master layout coordinates.
struct Pointer {
  uint64_t target = 0;
  Point pos;
  uint8_t buttons = 0;  // Button bit mask
  int16_t wheelV = 0;   // wheel delta carried by this packet (WHEEL_DELTA units)
  int16_t wheelH = 0;
  KeySet keys;          // keys currently held for this slave
};

// Master -> slave: the pointer left this slave.
struct Leave {
  uint64_t target = 0;
};

// Master -> slave: a key event. Codes are Linux evdev key codes (KEY_A = 30, ...), i.e. physical key
// positions: the slave's own keyboard layout decides which character they produce.
struct Key {
  uint64_t target = 0;
  uint16_t code = 0;
  uint8_t value = 0;  // 0 = release, 1 = press, 2 = auto-repeat
  KeySet held;        // keys held for this slave after this event
};

// Master -> slave, sealed with the pair key: starts a session. The slave accepts it only if
// slaveNonce is its current sessionNonce, so an old SessionInit cannot be replayed.
struct SessionInit {
  uint64_t target = 0;
  crypto::Nonce16 masterNonce{};
  crypto::Nonce16 slaveNonce{};
};

enum class KeyKind : uint8_t { Session = 0, Pair = 1 };

// A received envelope; open() turns it into the inner packet.
struct Sealed {
  uint64_t target = 0;
  KeyKind kind = KeyKind::Session;
  std::vector<uint8_t> datagram;  // the whole datagram (header, nonce, ciphertext, MAC)
};

// Variant order defines the on-wire type id (index + 1).
using Body = std::variant<MasterAnnounce, SlaveHello, SlaveConfig, Pointer, Leave, Key, Sealed, SessionInit>;

struct Packet {
  uint64_t sender = 0;
  uint32_t seq = 0;
  Body body;
};

// Plain encoding (any body; only MasterAnnounce/SlaveHello are accepted by decode()).
std::vector<uint8_t> encode(const Packet& packet);
// Encrypts `inner` (SlaveConfig, Pointer, Leave, Key or SessionInit) for `target`.
std::vector<uint8_t> seal(const Packet& inner, uint64_t target, KeyKind kind, const crypto::Key& key);
// Parses a datagram: discovery messages or a Sealed envelope.
std::optional<Packet> decode(const uint8_t* data, size_t size);
// Decrypts and authenticates a Sealed envelope; nullopt if the key is wrong or anything was altered.
std::optional<Packet> open(const Packet& sealed, const crypto::Key& key);

// Bytes the SlaveHello MAC covers (the hello with its MAC zeroed, plus the sender id).
std::vector<uint8_t> helloAuthData(uint64_t sender, const SlaveHello& hello);

}  // namespace nm::proto
