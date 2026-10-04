#pragma once

// OS abstraction used by the engines. Windows implementations live in platform/win; Linux ones
// (X11/Wayland + uinput) are planned for the second iteration.

#include <cstdint>
#include <string>
#include <vector>

#include "core/geometry.h"

namespace nm {

struct MonitorInfo {
  Rect rect;  // desktop coordinates, physical pixels
  bool primary = false;
  std::string name;
  friend bool operator==(const MonitorInfo&, const MonitorInfo&) = default;
};

class IDisplays {
 public:
  virtual ~IDisplays() = default;
  virtual std::vector<MonitorInfo> monitors() = 0;
};

// Master side: control of the local system cursor.
class ICursor {
 public:
  virtual ~ICursor() = default;
  virtual void setPos(Point p) = 0;
  virtual void hide() = 0;
  virtual void show() = 0;
  virtual uint8_t pressedButtons() = 0;  // proto::Button mask of physically held buttons
};

// Slave side: synthesizes input on the local system.
class IInjector {
 public:
  virtual ~IInjector() = default;
  virtual void moveTo(Point p) = 0;
  virtual void button(uint8_t button, bool down) = 0;  // a single proto::Button bit
  virtual void wheel(int delta, bool horizontal) = 0;
};

// Receives raw mouse events captured on the master. Return true to swallow the event so the local
// system never sees it.
class IMouseSink {
 public:
  virtual ~IMouseSink() = default;
  virtual bool onMouseMove(Point pt) = 0;
  virtual bool onMouseButton(uint8_t button, bool down) = 0;
  virtual bool onMouseWheel(int delta, bool horizontal) = 0;
  virtual void onPanic() = 0;  // emergency hotkey: return the pointer to the master
};

}  // namespace nm
