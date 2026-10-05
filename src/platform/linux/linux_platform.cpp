#include "platform/linux/linux_platform.h"

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/log.h"
#include "core/protocol.h"

namespace fs = std::filesystem;

namespace nm {
namespace {

constexpr size_t kBitsPerLong = sizeof(unsigned long) * 8;
constexpr size_t longsFor(size_t bits) { return bits / kBitsPerLong + 1; }

bool testBit(const unsigned long* bits, unsigned n) { return (bits[n / kBitsPerLong] >> (n % kBitsPerLong)) & 1UL; }

uint64_t nowMs() {
  using namespace std::chrono;
  return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string readFirstLine(const fs::path& p) {
  std::ifstream in(p);
  std::string line;
  std::getline(in, line);
  return line;
}

struct Probe {
  std::string name;
  bool mouse = false;
  bool keyboard = false;
  bool hiResWheel = false;
  bool hiResHWheel = false;
};

Probe probe(int fd) {
  Probe p;
  char name[256] = {};
  if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0) p.name = name;
  unsigned long ev[longsFor(EV_MAX)] = {};
  unsigned long key[longsFor(KEY_MAX)] = {};
  unsigned long rel[longsFor(REL_MAX)] = {};
  ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev);
  if (testBit(ev, EV_KEY)) ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key)), key);
  if (testBit(ev, EV_REL)) ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel);
  p.mouse = testBit(ev, EV_REL) && testBit(rel, REL_X) && testBit(rel, REL_Y) && testBit(key, BTN_LEFT);
  p.keyboard = testBit(ev, EV_KEY) && testBit(key, KEY_A) && testBit(key, KEY_Z) && testBit(key, KEY_HOME);
#ifdef REL_WHEEL_HI_RES
  p.hiResWheel = testBit(rel, REL_WHEEL_HI_RES);
  p.hiResHWheel = testBit(rel, REL_HWHEEL_HI_RES);
#endif
  return p;
}

bool isOwnDevice(const std::string& name) { return name.rfind("net-mouse", 0) == 0; }

std::vector<std::string> eventNodes() {
  std::vector<std::string> nodes;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator("/dev/input", ec)) {
    const std::string file = entry.path().filename().string();
    if (file.rfind("event", 0) == 0) nodes.push_back(entry.path().string());
  }
  std::sort(nodes.begin(), nodes.end());
  return nodes;
}

uint8_t protoButton(uint16_t code) {
  switch (code) {
    case BTN_LEFT:
      return proto::kButtonLeft;
    case BTN_RIGHT:
      return proto::kButtonRight;
    case BTN_MIDDLE:
      return proto::kButtonMiddle;
    case BTN_SIDE:
    case BTN_BACK:
      return proto::kButtonX1;
    case BTN_EXTRA:
    case BTN_FORWARD:
      return proto::kButtonX2;
    default:
      return 0;
  }
}

uint16_t evdevButton(uint8_t button) {
  switch (button) {
    case proto::kButtonLeft:
      return BTN_LEFT;
    case proto::kButtonRight:
      return BTN_RIGHT;
    case proto::kButtonMiddle:
      return BTN_MIDDLE;
    case proto::kButtonX1:
      return BTN_SIDE;
    case proto::kButtonX2:
      return BTN_EXTRA;
    default:
      return 0;
  }
}

bool anyKeyHeld(int fd) {
  unsigned long state[longsFor(KEY_MAX)] = {};
  if (ioctl(fd, EVIOCGKEY(sizeof(state)), state) < 0) return false;
  for (unsigned long word : state) {
    if (word) return true;
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Displays

std::vector<MonitorInfo> parseMonitorSpec(const std::string& spec) {
  std::vector<MonitorInfo> result;
  std::stringstream ss(spec);
  std::string item;
  while (std::getline(ss, item, ',')) {
    int w = 0, h = 0, x = 0, y = 0;
    if (std::sscanf(item.c_str(), " %dx%d%d%d", &w, &h, &x, &y) == 4 && w > 0 && h > 0) {
      MonitorInfo m;
      m.rect = {x, y, x + w, y + h};
      m.primary = result.empty();
      m.name = "monitor" + std::to_string(result.size() + 1);
      result.push_back(m);
    }
  }
  return result;
}

std::vector<MonitorInfo> detectDrmMonitors() {
  struct Found {
    std::string name;
    int w, h;
  };
  std::vector<Found> found;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator("/sys/class/drm", ec)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("card", 0) != 0 || name.find('-') == std::string::npos) continue;  // connectors only
    if (readFirstLine(entry.path() / "status") != "connected") continue;
    const fs::path enabled = entry.path() / "enabled";
    if (fs::exists(enabled, ec) && readFirstLine(enabled) != "enabled") continue;
    int w = 0, h = 0;
    if (std::sscanf(readFirstLine(entry.path() / "modes").c_str(), "%dx%d", &w, &h) != 2) continue;
    found.push_back({name.substr(name.find('-') + 1), w, h});
  }
  std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.name < b.name; });
  std::vector<MonitorInfo> result;
  int x = 0;
  for (const Found& f : found) {
    MonitorInfo m;
    m.rect = {x, 0, x + f.w, f.h};
    m.primary = result.empty();
    m.name = f.name;
    result.push_back(m);
    x += f.w;
  }
  return result;
}

std::vector<MonitorInfo> LinuxDisplays::monitors() {
  std::vector<MonitorInfo> list = spec_.empty() ? detectDrmMonitors() : parseMonitorSpec(spec_);
  if (list.empty()) {
    MonitorInfo fallback;
    fallback.rect = {0, 0, 1920, 1080};
    fallback.primary = true;
    fallback.name = "default";
    list.push_back(fallback);
  }
  return list;
}

// ---------------------------------------------------------------------------------------------
// Slave injection

void LinuxInjector::moveTo(Point p) {
  if (dryRun_) {
    log::info(log::fmt("[dry-run] move to (%d, %d)", p.x, p.y));
    return;
  }
  device_.moveTo(p);
}

void LinuxInjector::button(uint8_t button, bool down) {
  if (dryRun_) {
    log::info(log::fmt("[dry-run] button %u %s", button, down ? "down" : "up"));
    return;
  }
  if (const uint16_t code = evdevButton(button)) device_.button(code, down);
}

void LinuxInjector::key(uint16_t code, int value) {
  if (dryRun_) {
    log::info(log::fmt("[dry-run] key %u %s", code, value == 0 ? "up" : (value == 2 ? "repeat" : "down")));
    return;
  }
  device_.key(code, value);
}

void LinuxInjector::wheel(int delta, bool horizontal) {
  if (dryRun_) {
    log::info(log::fmt("[dry-run] %s wheel %d", horizontal ? "horizontal" : "vertical", delta));
    return;
  }
  device_.wheel(delta, horizontal);
}

// ---------------------------------------------------------------------------------------------
// Master cursor

void LinuxCursor::setPos(Point p) {
  std::lock_guard lock(mutex_);
  pos_ = p;
  if (!hidden_) device_.moveTo(p);
}

void LinuxCursor::hide() {
  std::lock_guard lock(mutex_);
  hidden_ = true;  // the real cursor stays at the edge where the pointer left
}

void LinuxCursor::show() {
  std::lock_guard lock(mutex_);
  hidden_ = false;  // the engine follows up with setPos(), which moves the real cursor
}

Point LinuxCursor::pos() const {
  std::lock_guard lock(mutex_);
  return pos_;
}

void LinuxCursor::setButton(uint8_t button, bool down) {
  if (down) {
    buttons_.fetch_or(button);
  } else {
    buttons_.fetch_and(static_cast<uint8_t>(~button));
  }
}

// ---------------------------------------------------------------------------------------------
// Device listing

std::vector<InputDeviceInfo> listInputDevices() {
  std::vector<InputDeviceInfo> result;
  for (const std::string& path : eventNodes()) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    const Probe p = probe(fd);
    ::close(fd);
    result.push_back({path, p.name, p.mouse, p.keyboard});
  }
  return result;
}

// ---------------------------------------------------------------------------------------------
// Master capture

bool EvdevCapture::start(IInputSink* sink, std::string& error) {
  sink_ = sink;
  if (pipe2(wakePipe_, O_CLOEXEC | O_NONBLOCK) != 0) {
    error = std::string("pipe2 failed: ") + std::strerror(errno);
    return false;
  }
  for (const MonitorInfo& m : displays_.monitors()) masterScreens_.push_back({m.rect, layout::kMasterOwner});
  scanDevices();
  bool haveMouse = false;
  for (const Device& d : devices_) haveMouse = haveMouse || d.mouse;
  if (!haveMouse) log::info("No mouse found yet; waiting for one to be plugged in");
  running_ = true;
  thread_ = std::thread([this] { run(); });
  return true;
}

void EvdevCapture::stop() {
  if (!thread_.joinable()) return;
  running_ = false;
  const char byte = 1;
  [[maybe_unused]] const ssize_t n = ::write(wakePipe_[1], &byte, 1);
  thread_.join();
  for (Device& d : devices_) closeDevice(d);  // releases the grabs: mice go straight to the system again
  devices_.clear();
  ::close(wakePipe_[0]);
  ::close(wakePipe_[1]);
  wakePipe_[0] = wakePipe_[1] = -1;
}

void EvdevCapture::scanDevices() {
  for (const std::string& path : eventNodes()) {
    const bool known = std::any_of(devices_.begin(), devices_.end(), [&](const Device& d) { return d.path == path; });
    if (known) continue;
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    const Probe p = probe(fd);
    if (isOwnDevice(p.name) || (!p.mouse && !p.keyboard)) {
      ::close(fd);
      continue;
    }
    // Keyboards are only taken over when their keys can be passed back to the system.
    if (!p.mouse && !passthrough_.isOpen()) {
      ::close(fd);
      continue;
    }
    // Grabbing while a key is held would hide its release from the desktop (the key would repeat
    // forever there); try again on the next scan.
    if (anyKeyHeld(fd)) {
      ::close(fd);
      continue;
    }
    if (ioctl(fd, EVIOCGRAB, 1) != 0) {
      log::info(log::fmt("Cannot grab '%s' (%s): %s", p.name.c_str(), path.c_str(), std::strerror(errno)));
      ::close(fd);
      continue;
    }
    Device d;
    d.fd = fd;
    d.path = path;
    d.name = p.name;
    d.mouse = p.mouse;
    d.keyboard = p.keyboard;
    d.hiResWheel = p.hiResWheel;
    d.hiResHWheel = p.hiResHWheel;
    log::info(log::fmt("Using %s '%s' (%s)", p.mouse ? "mouse" : "keyboard", p.name.c_str(), path.c_str()));
    devices_.push_back(std::move(d));
  }
}

void EvdevCapture::closeDevice(Device& d) {
  if (d.fd < 0) return;
  ioctl(d.fd, EVIOCGRAB, 0);
  ::close(d.fd);
  d.fd = -1;
}

void EvdevCapture::run() {
  uint64_t lastScan = nowMs();
  uint64_t lastDisplays = lastScan;
  std::vector<pollfd> fds;
  while (running_) {
    const uint64_t now = nowMs();
    if (now - lastScan >= 2000) {
      lastScan = now;
      scanDevices();
    }
    if (now - lastDisplays >= 1000) {
      lastDisplays = now;
      std::vector<layout::Screen> screens;
      for (const MonitorInfo& m : displays_.monitors()) screens.push_back({m.rect, layout::kMasterOwner});
      masterScreens_ = std::move(screens);
    }

    fds.assign(1, pollfd{wakePipe_[0], POLLIN, 0});
    for (const Device& d : devices_) fds.push_back({d.fd, POLLIN, 0});
    if (poll(fds.data(), fds.size(), 500) < 0 && errno != EINTR) break;
    if (fds[0].revents) break;

    for (size_t i = 1; i < fds.size(); ++i) {
      Device& d = devices_[i - 1];
      if (!fds[i].revents) continue;
      input_event events[64];
      while (true) {
        const ssize_t n = ::read(d.fd, events, sizeof(events));
        if (n < 0) {
          if (errno != EAGAIN && errno != EINTR) {
            log::info("Input device removed: " + d.name);
            closeDevice(d);
          }
          break;
        }
        if (n == 0) break;
        const size_t count = static_cast<size_t>(n) / sizeof(input_event);
        for (size_t k = 0; k < count; ++k) handleEvent(d, events[k].type, events[k].code, events[k].value);
      }
    }
    devices_.erase(std::remove_if(devices_.begin(), devices_.end(), [](const Device& d) { return d.fd < 0; }),
                   devices_.end());
  }
}

void EvdevCapture::handleEvent(Device& d, uint16_t type, uint16_t code, int32_t value) {
  if (d.mouse) {
    if (type == EV_REL) {
      switch (code) {
        case REL_X:
          d.dx += value;
          break;
        case REL_Y:
          d.dy += value;
          break;
        case REL_WHEEL:
          if (!d.hiResWheel) wheel(value * 120, false);
          break;
        case REL_HWHEEL:
          if (!d.hiResHWheel) wheel(value * 120, true);
          break;
#ifdef REL_WHEEL_HI_RES
        case REL_WHEEL_HI_RES:
          wheel(value, false);
          break;
        case REL_HWHEEL_HI_RES:
          wheel(value, true);
          break;
#endif
        default:
          break;
      }
      return;
    }
    if (type == EV_KEY) {
      if (code >= BTN_MOUSE && code < BTN_JOYSTICK) {
        if (value != 2) mouseButton(code, value != 0);
      } else {
        key(code, value);  // some mice have keys
      }
      return;
    }
    if (type == EV_SYN && code == SYN_REPORT && (d.dx != 0 || d.dy != 0)) {
      const int dx = d.dx, dy = d.dy;
      d.dx = d.dy = 0;
      motion(dx, dy);
    }
    return;
  }
  if (d.keyboard && type == EV_KEY) key(code, value);
}

void EvdevCapture::key(uint16_t code, int32_t value) {
  // The engine sends it to the slave that has the pointer (or the one that got the press);
  // otherwise it goes back to this PC through the virtual device.
  if (!sink_->onKey(code, value)) passthrough_.key(code, value);
}

void EvdevCapture::motion(int dx, int dy) {
  double fx = dx * options_.speed;
  double fy = dy * options_.speed;
  if (options_.accel > 0) {
    const double v = std::hypot(dx, dy);
    const double factor = 1.0 + options_.accel * std::clamp((v - 2.0) / 8.0, 0.0, 3.0);
    fx *= factor;
    fy *= factor;
  }
  remX_ += fx;
  remY_ += fy;
  const int ix = static_cast<int>(remX_);
  const int iy = static_cast<int>(remY_);
  remX_ -= ix;
  remY_ -= iy;
  if (ix == 0 && iy == 0) return;

  Point current = cursor_.pos();
  if (!layout::screenAt(masterScreens_, current) && !masterScreens_.empty()) {
    current = masterScreens_.front().rect.clamp(current);
  }
  // Like the OS would: the pointer slides along the outer edges of the master desktop. Reaching
  // an edge that touches a slave is what the engine turns into a switch.
  const Point next = layout::movePointer(masterScreens_, current, ix, iy);
  if (!sink_->onMouseMove(next) && !(next == current)) cursor_.setPos(next);
}

void EvdevCapture::mouseButton(uint16_t code, bool down) {
  const uint8_t button = protoButton(code);
  if (button && sink_->onMouseButton(button, down)) return;  // pointer is on a slave
  if (button) cursor_.setButton(button, down);
  passthrough_.button(code, down);
}

void EvdevCapture::wheel(int delta, bool horizontal) {
  if (sink_->onMouseWheel(delta, horizontal)) return;
  passthrough_.wheel(delta, horizontal);
}

}  // namespace nm
