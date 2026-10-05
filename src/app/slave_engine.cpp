#include "app/slave_engine.h"

#include "core/log.h"
#include "core/udp.h"

namespace nm {
namespace {

constexpr uint64_t kHelloIntervalMs = 1000;
constexpr uint64_t kMonitorRefreshMs = 1000;
constexpr uint64_t kMasterTimeoutMs = 3500;
// The master sends a heartbeat every 100 ms while the pointer is here.
constexpr uint64_t kPointerTimeoutMs = 1500;
constexpr size_t kMaxAnnouncedMasters = 32;

constexpr uint8_t kAllButtons[] = {proto::kButtonLeft, proto::kButtonRight, proto::kButtonMiddle,
                                   proto::kButtonX1, proto::kButtonX2};

}  // namespace

SlaveEngine::SlaveEngine(Options options, Config& config, ITransport& transport, IDisplays& displays,
                         IInjector& injector)
    : options_(std::move(options)),
      config_(config),
      transport_(transport),
      displays_(displays),
      injector_(injector) {
  std::lock_guard lock(mutex_);
  monitors_ = displays_.monitors();
  sessionNonce_ = crypto::randomArray<crypto::Nonce16>();
  masterId_ = config_.getHex("master_id");
  if (masterId_ != 0 && crypto::fromHex(config_.get("master_key"), masterKey_.data(), masterKey_.size())) {
    hasTrusted_ = true;
    pairKey_ = crypto::pairKey(options_.identity, masterKey_, false);
    configVersion_ = static_cast<uint32_t>(config_.getInt("config_version"));
    offset_ = {static_cast<int32_t>(config_.getInt("offset_x")), static_cast<int32_t>(config_.getInt("offset_y"))};
    masterName_ = config_.get("master_name");
  } else {
    masterId_ = 0;  // no pinned key (e.g. settings from an unencrypted version): wait to be connected
  }
}

void SlaveEngine::onPacket(const proto::Packet& packet, uint32_t sourceIp, uint64_t nowMs) {
  if (packet.sender == options_.id) return;
  std::lock_guard lock(mutex_);
  nowMs_ = nowMs;
  if (const auto* announce = std::get_if<proto::MasterAnnounce>(&packet.body)) {
    handleAnnounceLocked(*announce, packet.sender, sourceIp, nowMs);
  } else if (const auto* sealed = std::get_if<proto::Sealed>(&packet.body)) {
    if (sealed->target != options_.id) return;
    if (sealed->kind == proto::KeyKind::Pair) {
      handleSessionInitLocked(packet, sourceIp, nowMs);
    } else {
      handleSealedLocked(packet, nowMs);
    }
  }
}

void SlaveEngine::handleAnnounceLocked(const proto::MasterAnnounce& a, uint64_t sender, uint32_t ip, uint64_t nowMs) {
  if (announcedKeys_.size() >= kMaxAnnouncedMasters && !announcedKeys_.count(sender)) announcedKeys_.clear();
  announcedKeys_[sender] = a.publicKey;
  if (hasTrusted_ && sender != masterId_) return;
  if (hasTrusted_ && a.publicKey != masterKey_) {
    if (!impostorLogged_) {
      log::info(log::fmt("Warning: a master announces itself as '%s' with a different key; ignored", a.name.c_str()));
      impostorLogged_ = true;
    }
    return;
  }
  if (!masterSeen_ || masterName_ != a.name || masterIp_ != ip) ++changes_;
  if (!masterSeen_ || nowMs - lastMasterSeenMs_ > kMasterTimeoutMs) {
    log::info(log::fmt("Master '%s' found at %s", a.name.c_str(), ipToString(ip).c_str()));
  }
  if (hasTrusted_ && masterName_ != a.name) {
    config_.set("master_name", a.name);
    config_.save();
  }
  masterName_ = a.name;
  masterIp_ = ip;
  lastMasterSeenMs_ = nowMs;
  masterSeen_ = true;
}

void SlaveEngine::handleSessionInitLocked(const proto::Packet& sealed, uint32_t ip, uint64_t nowMs) {
  crypto::Key masterKey;
  crypto::Key pairKey;
  if (hasTrusted_) {
    if (sealed.sender != masterId_) {
      if (!refusedLogged_) {
        log::info("Refused a connection from another master: this PC is paired with '" + masterName_ +
                  "' (forget it first to switch)");
        refusedLogged_ = true;
      }
      return;
    }
    masterKey = masterKey_;
    pairKey = pairKey_;
  } else {
    auto it = announcedKeys_.find(sealed.sender);
    if (it == announcedKeys_.end()) return;  // its announcement (with the public key) comes first
    masterKey = it->second;
    pairKey = crypto::pairKey(options_.identity, masterKey, false);
  }
  const auto inner = proto::open(sealed, pairKey);
  const auto* init = inner ? std::get_if<proto::SessionInit>(&inner->body) : nullptr;
  if (!init || init->slaveNonce != sessionNonce_) return;  // forged, or an old (replayed) handshake

  if (!hasTrusted_) {
    hasTrusted_ = true;
    masterId_ = sealed.sender;
    masterKey_ = masterKey;
    pairKey_ = pairKey;
    configVersion_ = 0;
    config_.setHex("master_id", masterId_);
    config_.set("master_key", crypto::toHex(masterKey_.data(), masterKey_.size()));
    config_.erase("config_version");
    if (!config_.save()) log::info("Failed to save " + config_.path().string());
    log::info(log::fmt("Paired with master '%s' (fingerprint %s)", masterName_.c_str(),
                       crypto::fingerprint(masterKey_).c_str()));
  }
  if (active_) deactivateLocked();
  sessionKey_ = crypto::sessionKey(pairKey_, init->masterNonce, sessionNonce_);
  sessionId_ = crypto::keyId(sessionKey_);
  hasSession_ = true;
  sessionNonce_ = crypto::randomArray<crypto::Nonce16>();  // this handshake can never be accepted again
  haveSeq_ = false;
  masterIp_ = ip;
  lastMasterSeenMs_ = nowMs;
  masterSeen_ = true;
  log::info("Encrypted session with the master established");
  ++changes_;
  sendHelloLocked();  // tells the master which session we use
}

void SlaveEngine::handleSealedLocked(const proto::Packet& sealed, uint64_t nowMs) {
  if (!hasSession_ || sealed.sender != masterId_) return;
  const auto inner = proto::open(sealed, sessionKey_);
  if (!inner) return;  // not from our master, altered, or from an old session
  lastMasterSeenMs_ = nowMs;
  if (!acceptSeqLocked(inner->seq)) return;  // duplicate or replayed within this session
  if (const auto* cfg = std::get_if<proto::SlaveConfig>(&inner->body)) {
    handleConfigLocked(*cfg);
  } else if (const auto* pointer = std::get_if<proto::Pointer>(&inner->body)) {
    handlePointerLocked(*pointer, nowMs);
  } else if (const auto* key = std::get_if<proto::Key>(&inner->body)) {
    handleKeyLocked(*key);
  } else if (std::get_if<proto::Leave>(&inner->body)) {
    if (active_ || keyHeldLocked()) deactivateLocked();
  }
}

void SlaveEngine::handleConfigLocked(const proto::SlaveConfig& cfg) {
  if (cfg.version != configVersion_ || !(cfg.offset == offset_)) {
    configVersion_ = cfg.version;
    offset_ = cfg.offset;
    config_.setInt("config_version", configVersion_);
    config_.setInt("offset_x", offset_.x);
    config_.setInt("offset_y", offset_.y);
    config_.set("master_name", masterName_);
    if (!config_.save()) log::info("Failed to save " + config_.path().string());
    log::info(log::fmt("Configuration from master applied: desktop origin at (%d, %d) in master layout", offset_.x,
                       offset_.y));
    ++changes_;
  }
  sendHelloLocked();  // acknowledges the configuration
}

bool SlaveEngine::acceptSeqLocked(uint32_t seq) {
  // Sequence numbers only grow within a session; a new session (new key) starts a new count.
  if (haveSeq_ && static_cast<int32_t>(seq - lastSeq_) <= 0) return false;
  haveSeq_ = true;
  lastSeq_ = seq;
  return true;
}

void SlaveEngine::handleKeyLocked(const proto::Key& k) {
  lastPointerMs_ = nowMs_;  // any input from the master keeps the stream alive
  if (k.value == 0) {
    if (keys_.has(k.code) || k.code >= 256) injector_.key(k.code, 0);
    keys_.set(k.code, false);
  } else {
    // An auto-repeat for a key we never saw pressed (lost press) becomes a press.
    injector_.key(k.code, k.value == 2 && keys_.has(k.code) ? 2 : 1);
    keys_.set(k.code, true);
  }
  releaseKeysNotInLocked(k.held);
}

void SlaveEngine::releaseKeysNotInLocked(const proto::KeySet& held) {
  for (uint16_t code = 0; code < 256; ++code) {
    if (keys_.has(code) && !held.has(code)) {  // its release packet was lost
      injector_.key(code, 0);
      keys_.set(code, false);
    }
  }
}

bool SlaveEngine::keyHeldLocked() const { return !(keys_ == proto::KeySet{}); }

void SlaveEngine::handlePointerLocked(const proto::Pointer& p, uint64_t nowMs) {
  lastPointerMs_ = nowMs;
  if (!active_) {
    active_ = true;
    ++changes_;
  }
  const Point local = clampToMonitorsLocked({p.pos.x - offset_.x, p.pos.y - offset_.y});
  if (!havePos_ || !(local == lastPos_)) {
    injector_.moveTo(local);
    lastPos_ = local;
    havePos_ = true;
  }
  // The packet carries the full button state, so a lost press/release is repaired by the next one.
  for (uint8_t b : kAllButtons) {
    const bool want = (p.buttons & b) != 0;
    const bool have = (buttons_ & b) != 0;
    if (want != have) injector_.button(b, want);
  }
  buttons_ = p.buttons;
  if (p.wheelV) injector_.wheel(p.wheelV, false);
  if (p.wheelH) injector_.wheel(p.wheelH, true);
  releaseKeysNotInLocked(p.keys);
}

void SlaveEngine::deactivateLocked() {
  for (uint8_t b : kAllButtons) {
    if (buttons_ & b) injector_.button(b, false);
  }
  buttons_ = 0;
  releaseKeysNotInLocked(proto::KeySet{});  // never leave a key held down
  active_ = false;
  havePos_ = false;
  ++changes_;
}

Point SlaveEngine::clampToMonitorsLocked(Point p) const {
  if (monitors_.empty()) return p;
  // Inside a monitor: as is. Otherwise: nearest point of the nearest monitor.
  Point best = p;
  int64_t bestDist = -1;
  for (const MonitorInfo& m : monitors_) {
    const Point c = m.rect.clamp(p);
    const int64_t dx = c.x - p.x;
    const int64_t dy = c.y - p.y;
    const int64_t d = dx * dx + dy * dy;
    if (d == 0) return p;
    if (bestDist < 0 || d < bestDist) {
      bestDist = d;
      best = c;
    }
  }
  return best;
}

void SlaveEngine::sendHelloLocked() {
  proto::SlaveHello hello;
  hello.name = options_.name;
  for (const MonitorInfo& m : monitors_) hello.monitors.push_back(m.rect);
  hello.masterId = hasTrusted_ ? masterId_ : 0;
  hello.configVersion = configVersion_;
  hello.address = transport_.primaryAddress();
  hello.publicKey = options_.identity.publicKey;
  hello.sessionNonce = sessionNonce_;
  hello.sessionId = hasSession_ ? sessionId_ : 0;
  if (hasTrusted_) {  // lets the master trust the fields above
    const auto auth = proto::helloAuthData(options_.id, hello);
    hello.mac = crypto::mac(pairKey_, auth.data(), auth.size());
  }
  proto::Packet p;
  p.sender = options_.id;
  p.body = std::move(hello);
  transport_.send(proto::encode(p), kRouteAllInterfaces);
}

void SlaveEngine::tick(uint64_t nowMs) {
  std::lock_guard lock(mutex_);
  nowMs_ = nowMs;
  bool monitorsChanged = false;
  if (nowMs - lastMonitorRefreshMs_ >= kMonitorRefreshMs) {
    lastMonitorRefreshMs_ = nowMs;
    auto current = displays_.monitors();
    if (current != monitors_) {
      monitors_ = std::move(current);
      monitorsChanged = true;
      log::info("Display configuration changed");
      ++changes_;
    }
  }
  if (!helloSent_ || monitorsChanged || nowMs - lastHelloMs_ >= kHelloIntervalMs) {
    helloSent_ = true;
    lastHelloMs_ = nowMs;
    sendHelloLocked();
  }
  if ((active_ || keyHeldLocked()) && nowMs - lastPointerMs_ > kPointerTimeoutMs) {
    log::info("Lost the master's input stream; releasing buttons and keys");
    deactivateLocked();
  }
  if (masterSeen_ && nowMs - lastMasterSeenMs_ > kMasterTimeoutMs) {
    masterSeen_ = false;
    log::info("Master went offline");
    ++changes_;
  }
}

void SlaveEngine::forgetMaster() {
  std::lock_guard lock(mutex_);
  if (active_ || keyHeldLocked()) deactivateLocked();
  if (hasTrusted_) log::info("Forgot master '" + masterName_ + "'; any master can connect this PC now");
  hasTrusted_ = false;
  hasSession_ = false;
  masterId_ = 0;
  masterKey_ = {};
  pairKey_ = {};
  configVersion_ = 0;
  masterSeen_ = false;
  refusedLogged_ = false;
  sessionNonce_ = crypto::randomArray<crypto::Nonce16>();
  for (const char* key : {"master_id", "master_key", "master_name", "config_version", "offset_x", "offset_y"}) {
    config_.erase(key);
  }
  if (!config_.save()) log::info("Failed to save " + config_.path().string());
  ++changes_;
  sendHelloLocked();
}

SlaveSnapshot SlaveEngine::snapshot() const {
  std::lock_guard lock(mutex_);
  SlaveSnapshot s;
  s.paired = hasTrusted_;
  s.connected = hasSession_;
  s.configured = hasTrusted_ && configVersion_ != 0;
  s.masterId = masterId_;
  s.masterName = masterName_;
  s.masterFingerprint = hasTrusted_ ? crypto::fingerprint(masterKey_) : std::string();
  s.masterIp = masterIp_;
  s.masterOnline = masterSeen_;
  s.offset = offset_;
  s.active = active_;
  s.monitors = monitors_;
  s.fingerprint = crypto::fingerprint(options_.identity.publicKey);
  return s;
}

}  // namespace nm
