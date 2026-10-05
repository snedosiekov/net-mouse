#pragma once

// Master role: decides whether the pointer is on the master or on a slave, swallows local mouse and
// keyboard input while it is on a slave, and streams it, encrypted, to that slave.
//
// Pairing: discovered slaves are "available" until the user connects them (pairSlave). Pairing pins
// the slave's public key; from then on a session key is agreed with every (re)start of either side
// and all traffic to the slave is sealed with it.
//
// Threads: onMouse*/onKey run on the capture (hook) thread, onPacket/tick on the network thread,
// snapshot/setSlaveOffset/pair on the UI thread. One mutex guards all state.

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/crypto.h"
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
  bool online = false;         // heard from recently
  bool paired = false;         // the user connected it (its public key is pinned)
  bool connected = false;      // paired and an encrypted session is established
  bool synced = false;         // the slave confirmed the current configuration
  bool keyMismatch = false;    // announces a different public key than the pinned one
  bool otherMaster = false;    // paired with another master
  std::string fingerprint;     // of the public key it announces
};

struct MasterSnapshot {
  std::vector<MonitorInfo> masterMonitors;
  std::vector<SlaveView> slaves;
  bool remote = false;
  uint64_t activeSlave = 0;
  Point virtualPos;
  std::string fingerprint;  // this master's
};

class MasterEngine : public IInputSink {
 public:
  struct Options {
    uint64_t id = 0;
    std::string name;
    bool hideCursor = true;
    crypto::Identity identity;
  };

  MasterEngine(Options options, Config& config, ITransport& transport, IDisplays& displays,
               ICursor& cursor);

  // IInputSink (capture thread)
  bool onMouseMove(Point pt) override;
  bool onMouseButton(uint8_t button, bool down) override;
  bool onMouseWheel(int delta, bool horizontal) override;
  bool onKey(uint16_t code, int value) override;
  bool isRemote() const override;
  void onPanic() override;

  // Network thread
  void onPacket(const proto::Packet& packet, uint32_t sourceIp, uint64_t nowMs);
  void tick(uint64_t nowMs);

  // UI thread
  MasterSnapshot snapshot() const;
  bool setSlaveOffset(uint64_t slaveId, Point offset);  // false if it would overlap another screen
  bool pairSlave(uint64_t slaveId);                     // "connect": trust the key it announces now
  bool unpairSlave(uint64_t slaveId);                   // "disconnect": forget its key
  uint64_t changeCounter() const { return changes_.load(); }

 private:
  struct Slave : SlaveView {
    uint64_t lastSeenMs = 0;
    uint32_t version = 0;
    uint64_t lastConfigSentMs = 0;
    bool configDue = true;
    bool placed = false;
    crypto::Key announcedKey{};    // from its latest hello
    crypto::Key pinnedKey{};       // trusted key (valid when paired)
    crypto::Key pairKey{};         // static master/slave key
    crypto::Nonce16 slaveNonce{};  // from its latest hello: input for the next session
    bool hasSession = false;
    crypto::Key sessionKey{};
    uint64_t sessionId = 0;
    uint64_t lastSessionInitMs = 0;
  };

  void refreshMonitorsLocked();
  void rebuildScreensLocked();
  Slave* findSlaveLocked(uint64_t id);
  uint32_t allocVersionLocked();
  void saveSlaveLocked(const Slave& s);
  void ensurePlacedLocked(Slave& s);
  void sendConfigLocked(Slave& s, uint64_t nowMs);
  void startSessionLocked(Slave& s, uint64_t nowMs);
  void handleHelloLocked(const proto::SlaveHello& hello, uint64_t sender, uint32_t ip, uint64_t nowMs);
  void setConnectedLocked(Slave& s, bool connected);

  void enterRemoteLocked(uint64_t slaveId, Point at, Point returnPoint);
  void leaveRemoteLocked(Point at);
  bool sendSealedLocked(uint64_t slaveId, proto::Body body, int repeats);
  void sendPointerLocked(int16_t wheelV, int16_t wheelH, int repeats);
  void sendLeaveLocked();
  void sendKeyLocked(uint64_t slaveId, uint16_t code, int value);
  proto::KeySet keysOfLocked(uint64_t owner) const;
  bool heldLocked(uint16_t code) const { return keyOwner_.count(code) != 0; }
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
  // Every held key and where its press went (0 = this PC): the release must go to the same place,
  // even if the pointer moved to another screen in between.
  std::map<uint16_t, uint64_t> keyOwner_;

  uint64_t lastAnnounceMs_ = 0;
  uint64_t lastMonitorRefreshMs_ = 0;
  uint64_t lastHeartbeatMs_ = 0;
  bool announced_ = false;
  uint64_t nowMs_ = 0;  // latest time seen by the network thread (for UI-initiated actions)

  std::atomic<uint64_t> changes_{0};
};

}  // namespace nm
