#include "platform/linux/uinput_device.h"

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <functional>

namespace nm {
namespace {

constexpr uint16_t kVendor = 0x1209;  // pid.codes open-source vendor id
constexpr uint16_t kPointerProduct = 0x4E4D;
constexpr uint16_t kExtraProduct = 0x4E4E;

bool set(int fd, unsigned long request, int value) { return ioctl(fd, request, value) == 0; }

// Opens /dev/uinput, lets `declare` announce the capabilities and creates the device.
int createDevice(const std::string& name, uint16_t product, const std::function<bool(int)>& declare,
                 std::string& error) {
  const int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    error = std::string("cannot open /dev/uinput: ") + std::strerror(errno) +
            " (is the uinput module loaded and are we root?)";
    return -1;
  }
  uinput_setup setup{};
  setup.id.bustype = BUS_VIRTUAL;
  setup.id.vendor = kVendor;
  setup.id.product = product;
  setup.id.version = 1;
  std::strncpy(setup.name, name.c_str(), UINPUT_MAX_NAME_SIZE - 1);
  if (!declare(fd) || ioctl(fd, UI_DEV_SETUP, &setup) != 0 || ioctl(fd, UI_DEV_CREATE) != 0) {
    error = "uinput setup of '" + name + "' failed: " + std::strerror(errno);
    ::close(fd);
    return -1;
  }
  return fd;
}

void destroyDevice(int& fd) {
  if (fd < 0) return;
  ioctl(fd, UI_DEV_DESTROY);
  ::close(fd);
  fd = -1;
}

bool isPrimaryButton(uint16_t code) { return code == BTN_LEFT || code == BTN_RIGHT || code == BTN_MIDDLE; }

}  // namespace

bool UinputDevice::open(const std::string& name, const Rect& bounds, bool withKeys, std::string& error) {
  std::lock_guard lock(mutex_);
  closeLocked();
  if (bounds.empty()) {
    error = "empty desktop bounds";
    return false;
  }

  pointerFd_ = createDevice(name, kPointerProduct, [&](int fd) {
    bool ok = set(fd, UI_SET_EVBIT, EV_KEY) && set(fd, UI_SET_EVBIT, EV_ABS) && set(fd, UI_SET_EVBIT, EV_REL) &&
              set(fd, UI_SET_EVBIT, EV_MSC) && set(fd, UI_SET_EVBIT, EV_SYN);
    ok = ok && set(fd, UI_SET_KEYBIT, BTN_LEFT) && set(fd, UI_SET_KEYBIT, BTN_RIGHT) &&
         set(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ok = ok && set(fd, UI_SET_MSCBIT, MSC_SCAN);
    ok = ok && set(fd, UI_SET_RELBIT, REL_WHEEL) && set(fd, UI_SET_RELBIT, REL_HWHEEL);
#ifdef REL_WHEEL_HI_RES
    ok = ok && set(fd, UI_SET_RELBIT, REL_WHEEL_HI_RES) && set(fd, UI_SET_RELBIT, REL_HWHEEL_HI_RES);
#endif
    ok = ok && set(fd, UI_SET_ABSBIT, ABS_X) && set(fd, UI_SET_ABSBIT, ABS_Y);
    uinput_abs_setup abs{};
    abs.code = ABS_X;
    abs.absinfo.maximum = bounds.width() - 1;
    ok = ok && ioctl(fd, UI_ABS_SETUP, &abs) == 0;
    abs.code = ABS_Y;
    abs.absinfo.maximum = bounds.height() - 1;
    return ok && ioctl(fd, UI_ABS_SETUP, &abs) == 0;
  }, error);
  if (pointerFd_ < 0) return false;

  // REL_X/REL_Y make the companion a regular mouse for udev/libinput; no motion is ever sent on it.
  extraFd_ = createDevice(name + " (buttons)", kExtraProduct, [&](int fd) {
    bool ok = set(fd, UI_SET_EVBIT, EV_KEY) && set(fd, UI_SET_EVBIT, EV_REL) && set(fd, UI_SET_EVBIT, EV_SYN);
    ok = ok && set(fd, UI_SET_RELBIT, REL_X) && set(fd, UI_SET_RELBIT, REL_Y);
    ok = ok && set(fd, UI_SET_KEYBIT, BTN_LEFT);
    for (int code = BTN_SIDE; code <= BTN_TASK; ++code) ok = ok && set(fd, UI_SET_KEYBIT, code);
    if (withKeys) {
      // Keyboard keys (1..255) and the extended keys from KEY_OK on, but not the joystick
      // "trigger happy" buttons at 0x2c0 and above, which would make it look like a gamepad.
      for (int code = KEY_ESC; code <= 255; ++code) ok = ok && set(fd, UI_SET_KEYBIT, code);
      for (int code = KEY_OK; code < 0x2c0; ++code) ok = ok && set(fd, UI_SET_KEYBIT, code);
    }
    return ok;
  }, error);
  if (extraFd_ < 0) {
    closeLocked();
    return false;
  }
  bounds_ = bounds;
  lastX_ = lastY_ = 0;
  return true;
}

void UinputDevice::close() {
  std::lock_guard lock(mutex_);
  closeLocked();
}

void UinputDevice::closeLocked() {
  destroyDevice(pointerFd_);
  destroyDevice(extraFd_);
}

void UinputDevice::emit(int fd, uint16_t type, uint16_t code, int32_t value) {
  input_event ev{};
  ev.type = type;
  ev.code = code;
  ev.value = value;
  // A single event is never written partially; a failure (device gone) is not recoverable here.
  [[maybe_unused]] const ssize_t n = ::write(fd, &ev, sizeof(ev));
}

void UinputDevice::moveTo(Point p) {
  std::lock_guard lock(mutex_);
  if (pointerFd_ < 0) return;
  const int x = std::clamp(p.x - bounds_.left, 0, bounds_.width() - 1);
  const int y = std::clamp(p.y - bounds_.top, 0, bounds_.height() - 1);
  if (x == lastX_ && y == lastY_) {
    // The kernel drops absolute values equal to the previous ones, so re-entering at the same spot
    // (after the local mouse moved the cursor elsewhere) would not move the cursor. Nudge first.
    emit(pointerFd_, EV_ABS, ABS_X, x > 0 ? x - 1 : x + 1);
    emit(pointerFd_, EV_SYN, SYN_REPORT, 0);
  }
  emit(pointerFd_, EV_ABS, ABS_X, x);
  emit(pointerFd_, EV_ABS, ABS_Y, y);
  emit(pointerFd_, EV_SYN, SYN_REPORT, 0);
  lastX_ = x;
  lastY_ = y;
}

void UinputDevice::button(uint16_t code, bool down) {
  std::lock_guard lock(mutex_);
  const int fd = isPrimaryButton(code) ? pointerFd_ : extraFd_;
  if (fd < 0) return;
  emit(fd, EV_KEY, code, down ? 1 : 0);
  emit(fd, EV_SYN, SYN_REPORT, 0);
}

void UinputDevice::key(uint16_t code, int32_t value) {
  std::lock_guard lock(mutex_);
  if (extraFd_ < 0) return;
  emit(extraFd_, EV_KEY, code, value);
  emit(extraFd_, EV_SYN, SYN_REPORT, 0);
}

void UinputDevice::wheel(int delta, bool horizontal) {
  std::lock_guard lock(mutex_);
  if (pointerFd_ < 0 || delta == 0) return;
#ifdef REL_WHEEL_HI_RES
  emit(pointerFd_, EV_REL, horizontal ? REL_HWHEEL_HI_RES : REL_WHEEL_HI_RES, delta);
#endif
  // Classic notch events for clients that ignore high-resolution scrolling.
  int& remainder = horizontal ? wheelRemainderH_ : wheelRemainderV_;
  remainder += delta;
  const int notches = remainder / 120;
  if (notches != 0) {
    remainder -= notches * 120;
    emit(pointerFd_, EV_REL, horizontal ? REL_HWHEEL : REL_WHEEL, notches);
  }
  emit(pointerFd_, EV_SYN, SYN_REPORT, 0);
}

}  // namespace nm
