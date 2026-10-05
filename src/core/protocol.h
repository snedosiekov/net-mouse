#pragma once

// Wire protocol. All packets go to one UDP multicast group; every packet starts with a header
// (magic, version, type, sender id, sequence number). Integers are little-endian.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "core/geometry.h"

namespace nm::proto {

constexpr uint32_t kMagic = 0x45534D4E;  // "NMSE"
constexpr uint8_t kVersion = 1;
constexpr size_t kMaxString = 63;
constexpr size_t kMaxMonitors = 16;

enum Button : uint8_t {
  kButtonLeft = 1 << 0,
  kButtonRight = 1 << 1,
  kButtonMiddle = 1 << 2,
  kButtonX1 = 1 << 3,
  kButtonX2 = 1 << 4,
};

// Master -> all, once per second: "a master is here".
struct MasterAnnounce {
  std::string name;
};

// Slave -> all, once per second and right after it applies a configuration.
struct SlaveHello {
  std::string name;
  std::vector<Rect> monitors;  // the slave's own desktop coordinates (physical pixels)
  uint64_t masterId = 0;       // master whose configuration is applied (0 = none)
  uint32_t configVersion = 0;  // version of that configuration
  uint32_t address = 0;        // slave's main IPv4 (the one on its default route), 0 = unknown
};

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
};

// Master -> slave: the pointer left this slave.
struct Leave {
  uint64_t target = 0;
};

// Variant order defines the on-wire type id (index + 1).
using Body = std::variant<MasterAnnounce, SlaveHello, SlaveConfig, Pointer, Leave>;

struct Packet {
  uint64_t sender = 0;
  uint32_t seq = 0;
  Body body;
};

std::vector<uint8_t> encode(const Packet& packet);
std::optional<Packet> decode(const uint8_t* data, size_t size);

}  // namespace nm::proto
