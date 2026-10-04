#pragma once

// Virtual input devices created through the kernel's uinput module.
//
// The pointer is *absolute* (like a VM's USB tablet), so the display server (X11, any Wayland
// compositor) maps its range onto the whole desktop and one event puts the cursor exactly where we
// want it. It declares exactly what the kernel recognises as an "absolute mouse" (ABS_X/ABS_Y, the
// left/right/middle buttons, wheels), otherwise joydev would also expose it as a joystick.
// Side buttons and passed-through keys therefore live on a second, relative companion device.

#include <cstdint>
#include <mutex>
#include <string>

#include "core/geometry.h"

namespace nm {

class UinputDevice {
 public:
  ~UinputDevice() { close(); }

  // bounds: desktop area the absolute range covers. withKeys: the companion device also declares
  // keyboard keys, so keys coming from a grabbed mouse (some mice have them) can be passed through.
  bool open(const std::string& name, const Rect& bounds, bool withKeys, std::string& error);
  void close();
  bool isOpen() const { return pointerFd_ >= 0; }
  const Rect& bounds() const { return bounds_; }

  void moveTo(Point p);
  void button(uint16_t code, bool down);   // BTN_* code
  void key(uint16_t code, int32_t value);  // KEY_* code, value 0/1/2 (release/press/repeat)
  // delta in WHEEL_DELTA units (120 = one notch); emitted as high-resolution plus classic events.
  void wheel(int delta, bool horizontal);

 private:
  static void emit(int fd, uint16_t type, uint16_t code, int32_t value);
  void closeLocked();

  std::mutex mutex_;
  int pointerFd_ = -1;  // absolute pointer: position, left/right/middle, wheels
  int extraFd_ = -1;    // relative companion: side buttons, keys
  Rect bounds_;
  int lastX_ = 0;  // a new uinput device starts at (0, 0)
  int lastY_ = 0;
  int wheelRemainderV_ = 0;
  int wheelRemainderH_ = 0;
};

}  // namespace nm
