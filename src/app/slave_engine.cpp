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
// A sequence number this far behind the last one means the master restarted.
constexpr int32_t kSeqRestartWindow = 1000;

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
  masterId_ = config_.getHex("master_id");
  configVersion_ = static_cast<uint32_t>(config_.getInt("config_version"));
  offset_ = {static_cast<int32_t>(config_.getInt("offset_x")), static_cast<int32_t>(config_.getInt("offset_y"))};
  masterName_ = config_.get("master_name");
}

void SlaveEngine::onPacket(const proto::Packet& packet, uint32_t sourceIp, uint64_t nowMs) {
  if (packet.sender == options_.id) return;
  std::lock_guard lock(mutex_);
  nowMs_ = nowMs;

  if (const auto* announce = std::get_if<proto::MasterAnnounce>(&packet.body)) {
    if (packet.sender == masterId_ || masterId_ == 0) {
      if (!masterSeen_ || masterName_ != announce->name || masterIp_ != sourceIp) ++changes_;
      if (!masterSeen_ || nowMs - lastMasterSeenMs_ > kMasterTimeoutMs) {
        log::info(log::fmt("Master '%s' found at %s", announce->name.c_str(), ipToString(sourceIp).c_str()));
      }
      if (masterId_ != 0 && masterName_ != announce->name) {
        config_.set("master_name", announce->name);
        config_.save();
      }
      masterName_ = announce->name;
      masterIp_ = sourceIp;
      lastMasterSeenMs_ = nowMs;
      masterSeen_ = true;
    }
  } else if (const auto* cfg = std::get_if<proto::SlaveConfig>(&packet.body)) {
    if (cfg->target != options_.id) return;
    const bool changed = packet.sender != masterId_ || cfg->version != configVersion_ || !(cfg->offset == offset_);
    if (packet.sender != masterId_) {
      if (active_) deactivateLocked();
      haveSeq_ = false;
    }
    masterId_ = packet.sender;
    masterIp_ = sourceIp;
    lastMasterSeenMs_ = nowMs;
    masterSeen_ = true;
    configVersion_ = cfg->version;
    offset_ = cfg->offset;
    if (changed) {
      config_.setHex("master_id", masterId_);
      config_.setInt("config_version", configVersion_);
      config_.setInt("offset_x", offset_.x);
      config_.setInt("offset_y", offset_.y);
      config_.set("master_name", masterName_);
      if (!config_.save()) log::info("Failed to save " + config_.path().string());
      log::info(log::fmt("Configuration from master applied: desktop origin at (%d, %d) in master layout",
                         offset_.x, offset_.y));
      ++changes_;
    }
    sendHelloLocked();  // acknowledges the configuration
  } else if (const auto* pointer = std::get_if<proto::Pointer>(&packet.body)) {
    if (packet.sender != masterId_ || masterId_ == 0) return;
    lastMasterSeenMs_ = nowMs;
    if (pointer->target != options_.id) {
      if (active_) deactivateLocked();  // the master moved on to another slave
      return;
    }
    if (acceptSeqLocked(packet.seq)) handlePointerLocked(*pointer, nowMs);
  } else if (const auto* key = std::get_if<proto::Key>(&packet.body)) {
    if (packet.sender != masterId_ || masterId_ == 0 || key->target != options_.id) return;
    lastMasterSeenMs_ = nowMs;
    if (acceptSeqLocked(packet.seq)) handleKeyLocked(*key);
  } else if (const auto* leave = std::get_if<proto::Leave>(&packet.body)) {
    if (packet.sender == masterId_ && leave->target == options_.id && (active_ || keyHeldLocked())) {
      deactivateLocked();
    }
  }
}

bool SlaveEngine::acceptSeqLocked(uint32_t seq) {
  if (haveSeq_) {
    const int32_t diff = static_cast<int32_t>(seq - lastSeq_);
    if (diff <= 0 && diff > -kSeqRestartWindow) return false;  // duplicate or reordered packet
  }
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
  if (p.hasKeys) releaseKeysNotInLocked(p.keys);
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
  hello.masterId = masterId_;
  hello.configVersion = configVersion_;
  hello.address = transport_.primaryAddress();
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

SlaveSnapshot SlaveEngine::snapshot() const {
  std::lock_guard lock(mutex_);
  SlaveSnapshot s;
  s.configured = masterId_ != 0;
  s.masterId = masterId_;
  s.masterName = masterName_;
  s.masterIp = masterIp_;
  s.masterOnline = masterSeen_;
  s.offset = offset_;
  s.active = active_;
  s.monitors = monitors_;
  return s;
}

}  // namespace nm
