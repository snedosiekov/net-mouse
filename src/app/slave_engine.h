#pragma once

// Slave role: announces itself, pairs with one master (the first one that connects it; others are
// refused until forgetMaster()), agrees a fresh session key with it on every (re)start, and turns
// the master's sealed pointer/keyboard stream into local input.

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/crypto.h"
#include "core/protocol.h"
#include "core/transport.h"
#include "platform/platform.h"

namespace nm {

struct SlaveSnapshot {
  bool paired = false;      // trusts a master (its key is pinned)
  bool connected = false;   // encrypted session with it established
  bool configured = false;  // received its position from the master
  uint64_t masterId = 0;
  std::string masterName;
  std::string masterFingerprint;
  uint32_t masterIp = 0;
  bool masterOnline = false;
  Point offset;
  bool active = false;  // the master's pointer is currently on this PC
  std::vector<MonitorInfo> monitors;
  std::string fingerprint;  // this PC's
};

class SlaveEngine {
 public:
  struct Options {
    uint64_t id = 0;
    std::string name;
    crypto::Identity identity;
  };

  SlaveEngine(Options options, Config& config, ITransport& transport, IDisplays& displays,
              IInjector& injector);

  void onPacket(const proto::Packet& packet, uint32_t sourceIp, uint64_t nowMs);
  void tick(uint64_t nowMs);

  SlaveSnapshot snapshot() const;
  uint64_t changeCounter() const { return changes_.load(); }
  // Drops the trusted master, so another master can connect this PC.
  void forgetMaster();

 private:
  void handleAnnounceLocked(const proto::MasterAnnounce& a, uint64_t sender, uint32_t ip, uint64_t nowMs);
  void handleSessionInitLocked(const proto::Packet& sealed, uint32_t ip, uint64_t nowMs);
  void handleSealedLocked(const proto::Packet& sealed, uint64_t nowMs);
  void handleConfigLocked(const proto::SlaveConfig& cfg);
  void sendHelloLocked();
  bool acceptSeqLocked(uint32_t seq);
  void handlePointerLocked(const proto::Pointer& p, uint64_t nowMs);
  void handleKeyLocked(const proto::Key& k);
  void releaseKeysNotInLocked(const proto::KeySet& held);
  bool keyHeldLocked() const;
  void deactivateLocked();
  Point clampToMonitorsLocked(Point p) const;

  const Options options_;
  Config& config_;
  ITransport& transport_;
  IDisplays& displays_;
  IInjector& injector_;

  mutable std::mutex mutex_;
  std::vector<MonitorInfo> monitors_;

  // Trusted master (pinned public key) and the current session with it.
  bool hasTrusted_ = false;
  uint64_t masterId_ = 0;
  crypto::Key masterKey_{};
  crypto::Key pairKey_{};
  crypto::Nonce16 sessionNonce_{};  // the next session must be built from this; renewed after each use
  bool hasSession_ = false;
  crypto::Key sessionKey_{};
  uint64_t sessionId_ = 0;
  std::map<uint64_t, crypto::Key> announcedKeys_;  // masters seen on the network (for pairing)
  bool refusedLogged_ = false;
  bool impostorLogged_ = false;

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
  proto::KeySet keys_;  // keys this slave has pressed and not released yet
  uint64_t lastPointerMs_ = 0;

  uint64_t lastHelloMs_ = 0;
  bool helloSent_ = false;
  uint64_t lastMonitorRefreshMs_ = 0;

  std::atomic<uint64_t> changes_{0};
};

}  // namespace nm
