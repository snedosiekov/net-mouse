#pragma once

// Linux platform layer. Works below the display server, so the same code serves X11, every
// Wayland compositor and the text console:
//   * input is read from /dev/input/event* (evdev); the master grabs its mice exclusively,
//   * input is produced through a uinput virtual absolute pointer,
//   * the monitor layout comes from the config ("monitors=") or the DRM connectors in sysfs.

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/layout.h"
#include "platform/linux/uinput_device.h"
#include "platform/platform.h"

namespace nm {

// "1920x1080+0+0,2560x1440+1920+0"; the first entry is the primary monitor.
std::vector<MonitorInfo> parseMonitorSpec(const std::string& spec);

class LinuxDisplays : public IDisplays {
 public:
  // spec: optional explicit layout (see parseMonitorSpec); otherwise DRM connectors are used.
  explicit LinuxDisplays(std::string spec) : spec_(std::move(spec)) {}
  std::vector<MonitorInfo> monitors() override;

 private:
  std::string spec_;
};

// DRM connectors that are connected and enabled, with their preferred mode, placed left to right.
std::vector<MonitorInfo> detectDrmMonitors();

// Slave: turns the master's pointer stream into uinput events.
class LinuxInjector : public IInjector {
 public:
  LinuxInjector(UinputDevice& device, bool dryRun) : device_(device), dryRun_(dryRun) {}
  void moveTo(Point p) override;
  void button(uint8_t button, bool down) override;
  void wheel(int delta, bool horizontal) override;

 private:
  UinputDevice& device_;
  bool dryRun_;
};

// Master: the daemon owns the cursor position. Mice are grabbed, so every local movement is
// re-injected through the virtual absolute pointer; while the pointer is on a slave the real
// cursor simply stays where it left the screen.
class LinuxCursor : public ICursor {
 public:
  explicit LinuxCursor(UinputDevice& device) : device_(device) {}
  void setPos(Point p) override;
  void hide() override;
  void show() override;
  uint8_t pressedButtons() override { return buttons_.load(); }

  Point pos() const;
  void setButton(uint8_t button, bool down);

 private:
  UinputDevice& device_;
  mutable std::mutex mutex_;
  Point pos_;
  bool hidden_ = false;
  std::atomic<uint8_t> buttons_{0};
};

struct InputDeviceInfo {
  std::string path;
  std::string name;
  bool mouse = false;     // relative X/Y + left button
  bool keyboard = false;  // has letter keys
};

// Classifies /dev/input/event* (diagnostics: net-mouse-daemon --list-devices).
std::vector<InputDeviceInfo> listInputDevices();

// Master: reads the physical mice (grabbed) and keyboards (not grabbed, only for the emergency
// hotkey Ctrl+Alt+Home), feeds the engine like the Windows hook does, and passes everything the
// engine does not swallow back to the system through the virtual device.
class EvdevCapture {
 public:
  struct Options {
    double speed = 1.0;  // pointer speed multiplier
    double accel = 0.0;  // 0 = flat; >0 adds a simple speed-dependent acceleration
  };

  EvdevCapture(Options options, IDisplays& displays, LinuxCursor& cursor, UinputDevice& passthrough)
      : options_(options), displays_(displays), cursor_(cursor), passthrough_(passthrough) {}
  ~EvdevCapture() { stop(); }

  bool start(IMouseSink* sink, std::string& error);
  void stop();

 private:
  struct Device {
    int fd = -1;
    std::string path;
    std::string name;
    bool mouse = false;
    bool keyboard = false;
    bool hiResWheel = false;
    bool hiResHWheel = false;
    int dx = 0;
    int dy = 0;
  };

  void run();
  void scanDevices();
  void closeDevice(Device& d);
  void handleEvent(Device& d, uint16_t type, uint16_t code, int32_t value);
  void motion(int dx, int dy);
  void mouseButton(uint16_t code, bool down);
  void wheel(int delta, bool horizontal);

  Options options_;
  IDisplays& displays_;
  LinuxCursor& cursor_;
  UinputDevice& passthrough_;
  IMouseSink* sink_ = nullptr;

  std::thread thread_;
  std::atomic<bool> running_{false};
  int wakePipe_[2] = {-1, -1};
  std::vector<Device> devices_;
  std::vector<layout::Screen> masterScreens_;
  double remX_ = 0;
  double remY_ = 0;
  uint8_t modifiers_ = 0;  // held Ctrl/Alt keys, for the emergency hotkey
};

}  // namespace nm
