#include "app/master_engine.h"

#include <algorithm>
#include <random>

#include "core/keymap.h"
#include "core/log.h"
#include "core/udp.h"

namespace nm {
namespace {

constexpr uint64_t kAnnounceIntervalMs = 1000;
constexpr uint64_t kMonitorRefreshMs = 1000;
constexpr uint64_t kSlaveTimeoutMs = 3500;
constexpr uint64_t kConfigRetryMs = 500;
constexpr uint64_t kHeartbeatMs = 100;
// Button and wheel packets are sent twice; the slave drops the duplicate by sequence number.
constexpr int kReliableRepeats = 2;

uint32_t randomU32() {
  std::random_device rd;
  return static_cast<uint32_t>(rd());
}

std::string slaveKey(uint64_t id, const char* field) { return "slave." + toHex(id) + "." + field; }

int16_t clampWheel(int delta) { return static_cast<int16_t>(std::clamp(delta, -32768, 32767)); }

}  // namespace

MasterEngine::MasterEngine(Options options, Config& config, ITransport& transport,
                           IDisplays& displays, ICursor& cursor)
    : options_(std::move(options)),
      config_(config),
      transport_(transport),
      displays_(displays),
      cursor_(cursor),
      seq_(randomU32()),
      nextVersion_(randomU32()) {
  std::lock_guard lock(mutex_);
  refreshMonitorsLocked();
}

// ---------------------------------------------------------------------------------------------
// Capture thread

bool MasterEngine::onMouseMove(Point pt) {
  std::lock_guard lock(mutex_);
  if (!remote_) {
    const layout::Screen* here = layout::screenAt(screens_, pt);
    if (!here) return false;
    std::optional<Point> entry;
    Point returnPoint = pt;
    if (here->owner != layout::kMasterOwner) {
      entry = pt;  // the OS reported a position beyond the master desktop
      returnPoint = park_;
    } else {
      entry = layout::findEdgeCrossing(screens_, pt);
    }
    if (!entry) return false;
    if (cursor_.pressedButtons() != 0) return false;  // never carry a drag across machines
    enterRemoteLocked(layout::screenAt(screens_, *entry)->owner, *entry, returnPoint);
    return true;
  }

  // Remote: the real cursor is parked; each event's offset from the park point is the motion.
  const int dx = pt.x - park_.x;
  const int dy = pt.y - park_.y;
  if (dx == 0 && dy == 0) return true;

  Point next = layout::movePointer(screens_, virtual_, dx, dy);
  const layout::Screen* s = layout::screenAt(screens_, next);
  if (s && s->owner != target_ && remoteButtons_ != 0) {
    // Keep a drag inside the slave where it started.
    if (const layout::Screen* current = layout::screenAt(screens_, virtual_)) {
      next = current->rect.clamp(next);
      s = current;
    }
  }
  if (!s) return true;
  virtual_ = next;

  if (s->owner == layout::kMasterOwner) {
    leaveRemoteLocked(next);
    return true;
  }
  if (s->owner != target_) {
    sendLeaveLocked();
    target_ = s->owner;
    if (const Slave* slave = findSlaveLocked(target_)) log::info("Pointer moved to slave '" + slave->name + "'");
    ++changes_;
  }
  sendPointerLocked(0, 0, 1);
  return true;
}

bool MasterEngine::onMouseButton(uint8_t button, bool down) {
  std::lock_guard lock(mutex_);
  if (!remote_) return false;
  if (down) {
    remoteButtons_ |= button;
  } else {
    remoteButtons_ &= static_cast<uint8_t>(~button);
  }
  sendPointerLocked(0, 0, kReliableRepeats);
  return true;
}

bool MasterEngine::onMouseWheel(int delta, bool horizontal) {
  std::lock_guard lock(mutex_);
  if (!remote_) return false;
  sendPointerLocked(horizontal ? 0 : clampWheel(delta), horizontal ? clampWheel(delta) : 0, kReliableRepeats);
  return true;
}

bool MasterEngine::onKey(uint16_t code, int value) {
  std::lock_guard lock(mutex_);
  if (value == 0) {
    // A release goes wherever the press went, even if the pointer has moved since.
    auto it = keyOwner_.find(code);
    const uint64_t owner = it != keyOwner_.end() ? it->second : (remote_ ? target_ : 0);
    if (it != keyOwner_.end()) keyOwner_.erase(it);
    if (owner == 0) return false;
    sendKeyLocked(owner, code, 0);
    return true;
  }

  // Ctrl+Alt+Home: while keys are swallowed the OS hotkey cannot fire, so detect it here.
  if (value == 1 && code == keymap::kKeyHome && remote_ &&
      (heldLocked(keymap::kKeyLeftCtrl) || heldLocked(keymap::kKeyRightCtrl)) &&
      (heldLocked(keymap::kKeyLeftAlt) || heldLocked(keymap::kKeyRightAlt))) {
    log::info("Emergency hotkey: pointer returned to the master");
    leaveRemoteLocked(returnPoint_);  // the slave releases its keys when it gets the Leave
    return true;
  }

  uint64_t owner = remote_ ? target_ : 0;
  auto it = keyOwner_.find(code);
  if (value == 2 && it != keyOwner_.end()) {
    owner = it->second;  // auto-repeat follows the press
  } else {
    keyOwner_[code] = owner;
  }
  if (owner == 0) return false;
  sendKeyLocked(owner, code, value);
  return true;
}

bool MasterEngine::isRemote() const {
  std::lock_guard lock(mutex_);
  return remote_;
}

void MasterEngine::onPanic() {
  std::lock_guard lock(mutex_);
  if (remote_) {
    log::info("Emergency hotkey: pointer returned to the master");
    leaveRemoteLocked(returnPoint_);
  }
}

void MasterEngine::enterRemoteLocked(uint64_t slaveId, Point at, Point returnPoint) {
  remote_ = true;
  target_ = slaveId;
  virtual_ = at;
  returnPoint_ = returnPoint;
  remoteButtons_ = 0;
  park_ = parkPointLocked();
  // Hide before parking, so a platform that draws the cursor itself never shows it jumping to the park point.
  if (options_.hideCursor && !cursorHidden_) {
    cursor_.hide();
    cursorHidden_ = true;
  }
  cursor_.setPos(park_);
  if (const Slave* slave = findSlaveLocked(slaveId)) log::info("Pointer moved to slave '" + slave->name + "'");
  sendPointerLocked(0, 0, 1);
  ++changes_;
}

void MasterEngine::leaveRemoteLocked(Point at) {
  sendLeaveLocked();
  remote_ = false;
  target_ = 0;
  remoteButtons_ = 0;
  if (cursorHidden_) {
    cursor_.show();
    cursorHidden_ = false;
  }
  cursor_.setPos(at);
  log::info("Pointer returned to the master");
  ++changes_;
}

void MasterEngine::sendPointerLocked(int16_t wheelV, int16_t wheelH, int repeats) {
  proto::Packet p;
  p.sender = options_.id;
  p.seq = ++seq_;
  p.body = proto::Pointer{target_, virtual_, remoteButtons_, wheelV, wheelH, true, keysOfLocked(target_)};
  const auto bytes = proto::encode(p);
  const uint32_t ip = targetIpLocked();
  for (int i = 0; i < repeats; ++i) transport_.send(bytes, ip);
}

void MasterEngine::sendKeyLocked(uint64_t slaveId, uint16_t code, int value) {
  proto::Packet p;
  p.sender = options_.id;
  p.seq = ++seq_;
  p.body = proto::Key{slaveId, code, static_cast<uint8_t>(value), keysOfLocked(slaveId)};
  const auto bytes = proto::encode(p);
  const uint32_t ip = slaveIpLocked(slaveId);
  // Presses and releases are sent twice (duplicates are dropped by sequence number); repeats once.
  for (int i = 0; i < (value == 2 ? 1 : kReliableRepeats); ++i) transport_.send(bytes, ip);
}

proto::KeySet MasterEngine::keysOfLocked(uint64_t owner) const {
  proto::KeySet keys;
  for (const auto& [code, keyOwner] : keyOwner_) {
    if (keyOwner == owner && owner != 0) keys.set(code, true);
  }
  return keys;
}

uint32_t MasterEngine::slaveIpLocked(uint64_t slaveId) {
  const Slave* s = findSlaveLocked(slaveId);
  return s ? s->ip : kRouteDefault;
}

void MasterEngine::sendLeaveLocked() {
  if (target_ == 0) return;
  proto::Packet p;
  p.sender = options_.id;
  p.seq = ++seq_;
  p.body = proto::Leave{target_};
  const auto bytes = proto::encode(p);
  const uint32_t ip = targetIpLocked();
  for (int i = 0; i < kReliableRepeats; ++i) transport_.send(bytes, ip);
}

uint32_t MasterEngine::targetIpLocked() { return slaveIpLocked(target_); }

Point MasterEngine::parkPointLocked() const {
  for (const MonitorInfo& m : monitors_) {
    if (m.primary) return m.rect.center();
  }
  return monitors_.empty() ? Point{} : monitors_.front().rect.center();
}

// ---------------------------------------------------------------------------------------------
// Network thread

void MasterEngine::onPacket(const proto::Packet& packet, uint32_t sourceIp, uint64_t nowMs) {
  if (packet.sender == options_.id) return;
  std::lock_guard lock(mutex_);
  if (const auto* hello = std::get_if<proto::SlaveHello>(&packet.body)) {
    handleHelloLocked(*hello, packet.sender, sourceIp, nowMs);
  } else if (const auto* announce = std::get_if<proto::MasterAnnounce>(&packet.body)) {
    if (otherMasters_.insert(packet.sender).second) {
      log::info(log::fmt("Warning: another master '%s' (%s) is on the network", announce->name.c_str(),
                         toHex(packet.sender).c_str()));
    }
  }
}

void MasterEngine::handleHelloLocked(const proto::SlaveHello& hello, uint64_t sender, uint32_t ip,
                                     uint64_t nowMs) {
  if (hello.monitors.empty()) return;
  Slave* s = findSlaveLocked(sender);
  bool topologyChanged = false;
  if (!s) {
    Slave fresh;
    fresh.id = sender;
    fresh.version = allocVersionLocked();
    slaves_.push_back(std::move(fresh));
    s = &slaves_.back();
  }
  s->name = hello.name;
  // A multi-homed slave announces itself from every interface. Use the address it calls its main one
  // when we can reach it; otherwise keep a reachable source address instead of flipping between them.
  uint32_t address = s->ip;
  if (hello.address && transport_.isLocalSubnet(hello.address)) {
    address = hello.address;
  } else if (s->ip == 0 || (!transport_.isLocalSubnet(s->ip) && transport_.isLocalSubnet(ip))) {
    address = ip;
  }
  if (address != s->ip) {
    s->ip = address;
    ++changes_;
  }
  s->lastSeenMs = nowMs;
  if (s->monitors != hello.monitors) {
    s->monitors = hello.monitors;
    topologyChanged = true;
  }
  if (!s->online) {
    s->online = true;
    topologyChanged = true;
    if (config_.has(slaveKey(s->id, "x"))) {
      s->offset = {static_cast<int32_t>(config_.getInt(slaveKey(s->id, "x"))),
                   static_cast<int32_t>(config_.getInt(slaveKey(s->id, "y")))};
    } else {
      placeNewSlaveLocked(*s);
    }
    log::info(log::fmt("Slave '%s' online at %s", s->name.c_str(), ipToString(ip).c_str()));
  }
  const bool synced = hello.masterId == options_.id && hello.configVersion == s->version;
  if (synced != s->synced) {
    s->synced = synced;
    ++changes_;
  }
  if (!synced && (s->configDue || nowMs - s->lastConfigSentMs >= kConfigRetryMs)) sendConfigLocked(*s, nowMs);
  if (topologyChanged) {
    rebuildScreensLocked();
    ++changes_;
  }
}

void MasterEngine::placeNewSlaveLocked(Slave& s) {
  std::vector<Rect> occupied;
  Rect primary;
  for (const MonitorInfo& m : monitors_) {
    occupied.push_back(m.rect);
    if (m.primary) primary = m.rect;
  }
  if (primary.empty() && !monitors_.empty()) primary = monitors_.front().rect;
  for (const Slave& other : slaves_) {
    if (other.id == s.id || !other.online) continue;
    for (const Rect& r : other.monitors) occupied.push_back(r.translated(other.offset));
  }
  s.offset = layout::autoPlace(occupied, primary, boundingRect(s.monitors));
  saveSlaveLocked(s);
  log::info(log::fmt("Placed new slave '%s' to the right at (%d, %d)", s.name.c_str(), s.offset.x, s.offset.y));
}

void MasterEngine::sendConfigLocked(Slave& s, uint64_t nowMs) {
  proto::Packet p;
  p.sender = options_.id;
  p.seq = ++seq_;
  p.body = proto::SlaveConfig{s.id, s.version, s.offset};
  transport_.send(proto::encode(p), s.ip);
  s.lastConfigSentMs = nowMs;
  s.configDue = false;
}

void MasterEngine::tick(uint64_t nowMs) {
  std::lock_guard lock(mutex_);
  if (nowMs - lastMonitorRefreshMs_ >= kMonitorRefreshMs) {
    lastMonitorRefreshMs_ = nowMs;
    refreshMonitorsLocked();
  }
  if (!announced_ || nowMs - lastAnnounceMs_ >= kAnnounceIntervalMs) {
    announced_ = true;
    lastAnnounceMs_ = nowMs;
    proto::Packet p;
    p.sender = options_.id;
    p.seq = ++seq_;
    p.body = proto::MasterAnnounce{options_.name};
    transport_.send(proto::encode(p), kRouteAllInterfaces);
  }

  bool topologyChanged = false;
  for (Slave& s : slaves_) {
    if (s.online && nowMs - s.lastSeenMs > kSlaveTimeoutMs) {
      s.online = false;
      s.synced = false;
      s.configDue = true;
      topologyChanged = true;
      log::info("Slave '" + s.name + "' went offline");
    }
    if (s.online && !s.synced && (s.configDue || nowMs - s.lastConfigSentMs >= kConfigRetryMs)) {
      sendConfigLocked(s, nowMs);
    }
  }
  if (topologyChanged) {
    rebuildScreensLocked();
    ++changes_;
  }

  if (remote_) {
    const layout::Screen* s = layout::screenAt(screens_, virtual_);
    if (!s || s->owner != target_) {
      leaveRemoteLocked(returnPoint_);  // slave vanished or the layout changed under the pointer
    } else if (nowMs - lastHeartbeatMs_ >= kHeartbeatMs) {
      lastHeartbeatMs_ = nowMs;
      sendPointerLocked(0, 0, 1);  // lets the slave resync after packet loss
    }
  }
}

// ---------------------------------------------------------------------------------------------
// UI thread

MasterSnapshot MasterEngine::snapshot() const {
  std::lock_guard lock(mutex_);
  MasterSnapshot snap;
  snap.masterMonitors = monitors_;
  snap.slaves.assign(slaves_.begin(), slaves_.end());
  snap.remote = remote_;
  snap.activeSlave = target_;
  snap.virtualPos = virtual_;
  return snap;
}

bool MasterEngine::setSlaveOffset(uint64_t slaveId, Point offset) {
  std::lock_guard lock(mutex_);
  Slave* s = findSlaveLocked(slaveId);
  if (!s) return false;
  std::vector<Rect> moved;
  for (const Rect& r : s->monitors) moved.push_back(r.translated(offset));
  std::vector<Rect> others;
  for (const MonitorInfo& m : monitors_) others.push_back(m.rect);
  for (const Slave& o : slaves_) {
    if (o.id == slaveId || !o.online) continue;
    for (const Rect& r : o.monitors) others.push_back(r.translated(o.offset));
  }
  if (layout::overlapsAny(moved, others)) return false;
  if (s->offset == offset) return true;
  s->offset = offset;
  s->version = allocVersionLocked();
  s->synced = false;
  s->configDue = true;
  saveSlaveLocked(*s);
  rebuildScreensLocked();
  log::info(log::fmt("Slave '%s' moved to (%d, %d)", s->name.c_str(), offset.x, offset.y));
  ++changes_;
  return true;
}

// ---------------------------------------------------------------------------------------------
// Helpers

void MasterEngine::refreshMonitorsLocked() {
  std::vector<MonitorInfo> current = displays_.monitors();
  if (current == monitors_) return;
  monitors_ = std::move(current);
  rebuildScreensLocked();
  if (!remote_) park_ = parkPointLocked();
  ++changes_;
}

void MasterEngine::rebuildScreensLocked() {
  screens_.clear();
  for (const MonitorInfo& m : monitors_) screens_.push_back({m.rect, layout::kMasterOwner});
  for (const Slave& s : slaves_) {
    if (!s.online) continue;
    for (const Rect& r : s.monitors) screens_.push_back({r.translated(s.offset), s.id});
  }
}

MasterEngine::Slave* MasterEngine::findSlaveLocked(uint64_t id) {
  for (Slave& s : slaves_) {
    if (s.id == id) return &s;
  }
  return nullptr;
}

uint32_t MasterEngine::allocVersionLocked() {
  if (++nextVersion_ == 0) ++nextVersion_;  // 0 means "not configured" on the slave
  return nextVersion_;
}

void MasterEngine::saveSlaveLocked(const Slave& s) {
  config_.setInt(slaveKey(s.id, "x"), s.offset.x);
  config_.setInt(slaveKey(s.id, "y"), s.offset.y);
  config_.set(slaveKey(s.id, "name"), s.name);
  if (!config_.save()) log::info("Failed to save " + config_.path().string());
}

}  // namespace nm
