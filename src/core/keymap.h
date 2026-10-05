#pragma once

// Translation between Windows keys (scan code set 1 + extended flag, virtual-key codes) and the
// wire format: Linux evdev key codes, which describe physical key positions.

#include <cstdint>

namespace nm::keymap {

// Linux evdev codes used by the platform-independent code.
constexpr uint16_t kKeyLeftCtrl = 29;
constexpr uint16_t kKeyLeftAlt = 56;
constexpr uint16_t kKeyRightCtrl = 97;
constexpr uint16_t kKeyRightAlt = 100;
constexpr uint16_t kKeyHome = 102;

// How to synthesize a key on Windows: by scan code (vk == 0) or by virtual-key code.
struct WinKey {
  uint16_t vk = 0;
  uint16_t scan = 0;
  bool extended = false;
};

// From a low-level keyboard hook event; 0 if the key has no evdev equivalent (or must be ignored,
// like the fake Left Ctrl that AltGr produces).
uint16_t fromWindows(uint32_t vk, uint32_t scan, bool extended);

// For injection on Windows; false if the key cannot be produced.
bool toWindows(uint16_t code, WinKey& out);

}  // namespace nm::keymap
