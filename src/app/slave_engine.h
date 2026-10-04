#pragma once

// Slave role: announces itself, applies the layout configuration received from a master and turns
// the master's pointer stream into local input.

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/protocol.h"
#include "core/transport.h"
#include "platform/platform.h"

namespace nm {

struct SlaveSnapshot {
  bool configured = false;
  uint64_t masterId = 0;
  std::string masterName;
  uint32_t masterIp = 0;
  bool masterOnline = false;
  Point offset;
  bool active = false;  // the master's pointer is currently on this PC
  std::vector<MonitorInfo> monitors;
};

class SlaveEngine {
 public:
  struct Options {
    uint64_t id = 0;
    std::string name;
  };

  SlaveEngine(Options options, Config& config, ITransport& transport, IDisplays& displays,
              IInjector& injector);

  void onPacket(const proto::Packet& packet, uint32_t sourceIp, uint64_t nowMs);
  void tick(uint64_t nowMs);

  SlaveSnapshot snapshot() const;
  uint64_t changeCounter() const { return changes_.load(); }

 private:
  void sendHelloLocked();
  void handlePointerLocked(const proto::Pointer& p, uint32_t seq, uint64_t nowMs);
  void deactivateLocked();
  Point clampToMonitorsLocked(Point p) const;

  const Options options_;
  Config& config_;
  ITransport& transport_;
  IDisplays& displays_;
  IInjector& injector_;

  mutable std::mutex mutex_;
  std::vector<MonitorInfo> monitors_;
  uint64_t masterId_ = 0;
  uint32_t configVersion_ = 0;
  Point offset_;
  std::string masterName_;
  uint32_t masterIp_ = 0;
  uint64_t lastMasterSeenMs_ = 0;
  bool masterSeen_ = false;
  uint64_t nowMs_ = 0;

  bool haveSeq_ = false;
  uint32_t lastSeq_ = 0;
  bool active_ = false;
  bool havePos_ = false;
  Point lastPos_;
  uint8_t buttons_ = 0;
  uint64_t lastPointerMs_ = 0;

  uint64_t lastHelloMs_ = 0;
  bool helloSent_ = false;
  uint64_t lastMonitorRefreshMs_ = 0;

  std::atomic<uint64_t> changes_{0};
};

}  // namespace nm
