#pragma once

// Master role: decides whether the pointer is on the master or on a slave, swallows local mouse
// input while it is on a slave, and streams the pointer state to that slave.
//
// Threads: onMouse* run on the capture (hook) thread, onPacket/tick on the network thread,
// snapshot/setSlaveOffset on the UI thread. One mutex guards all state.

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/layout.h"
#include "core/protocol.h"
#include "core/transport.h"
#include "platform/platform.h"

namespace nm {

struct SlaveView {
  uint64_t id = 0;
  std::string name;
  uint32_t ip = 0;
  std::vector<Rect> monitors;  // slave-local coordinates
  Point offset;                // slave-local origin in master layout space
  bool online = false;
  bool synced = false;         // the slave confirmed the current configuration
};

struct MasterSnapshot {
  std::vector<MonitorInfo> masterMonitors;
  std::vector<SlaveView> slaves;
  bool remote = false;
  uint64_t activeSlave = 0;
  Point virtualPos;
};

class MasterEngine : public IMouseSink {
 public:
  struct Options {
    uint64_t id = 0;
    std::string name;
    bool hideCursor = true;
  };

  MasterEngine(Options options, Config& config, ITransport& transport, IDisplays& displays,
               ICursor& cursor);

  // IMouseSink (capture thread)
  bool onMouseMove(Point pt) override;
  bool onMouseButton(uint8_t button, bool down) override;
  bool onMouseWheel(int delta, bool horizontal) override;
  void onPanic() override;

  // Network thread
  void onPacket(const proto::Packet& packet, uint32_t sourceIp, uint64_t nowMs);
  void tick(uint64_t nowMs);

  // UI thread
  MasterSnapshot snapshot() const;
  bool setSlaveOffset(uint64_t slaveId, Point offset);  // false if it would overlap another screen
  uint64_t changeCounter() const { return changes_.load(); }

 private:
  struct Slave : SlaveView {
    uint64_t lastSeenMs = 0;
    uint32_t version = 0;
    uint64_t lastConfigSentMs = 0;
    bool configDue = true;
  };

  void refreshMonitorsLocked();
  void rebuildScreensLocked();
  Slave* findSlaveLocked(uint64_t id);
  uint32_t allocVersionLocked();
  void saveSlaveLocked(const Slave& s);
  void placeNewSlaveLocked(Slave& s);
  void sendConfigLocked(Slave& s, uint64_t nowMs);
  void handleHelloLocked(const proto::SlaveHello& hello, uint64_t sender, uint32_t ip, uint64_t nowMs);

  void enterRemoteLocked(uint64_t slaveId, Point at, Point returnPoint);
  void leaveRemoteLocked(Point at);
  void sendPointerLocked(int16_t wheelV, int16_t wheelH, int repeats);
  void sendLeaveLocked();
  uint32_t targetIpLocked();
  Point parkPointLocked() const;

  const Options options_;
  Config& config_;
  ITransport& transport_;
  IDisplays& displays_;
  ICursor& cursor_;

  mutable std::mutex mutex_;
  std::vector<MonitorInfo> monitors_;
  std::vector<Slave> slaves_;
  std::vector<layout::Screen> screens_;
  std::set<uint64_t> otherMasters_;
  uint32_t seq_;
  uint32_t nextVersion_;

  bool remote_ = false;
  uint64_t target_ = 0;
  Point virtual_;      // pointer position in layout space while remote
  Point park_;         // where the real cursor is held while remote
  Point returnPoint_;  // where to put the cursor back if the slave disappears
  uint8_t remoteButtons_ = 0;
  bool cursorHidden_ = false;

  uint64_t lastAnnounceMs_ = 0;
  uint64_t lastMonitorRefreshMs_ = 0;
  uint64_t lastHeartbeatMs_ = 0;
  bool announced_ = false;

  std::atomic<uint64_t> changes_{0};
};

}  // namespace nm
