#pragma once

#include <windows.h>

#include <bitset>
#include <thread>

#include "platform/platform.h"

namespace nm {

class WinDisplays : public IDisplays {
 public:
  std::vector<MonitorInfo> monitors() override;
};

// Hides the cursor by temporarily replacing the system cursors with a blank one; show() reloads
// the user's cursor scheme from the registry.
class WinCursor : public ICursor {
 public:
  void setPos(Point p) override;
  void hide() override;
  void show() override;
  uint8_t pressedButtons() override;
};

class WinInjector : public IInjector {
 public:
  explicit WinInjector(bool dryRun) : dryRun_(dryRun) {}
  void moveTo(Point p) override;
  void button(uint8_t button, bool down) override;
  void wheel(int delta, bool horizontal) override;
  void key(uint16_t code, int value) override;

 private:
  bool dryRun_;
};

// Global low-level mouse and keyboard hooks (WH_MOUSE_LL, WH_KEYBOARD_LL) on a dedicated
// high-priority thread, plus the emergency hotkey Ctrl+Alt+Home (for when keys are not swallowed).
class WinInputHook {
 public:
  ~WinInputHook() { stop(); }
  bool start(IInputSink* sink);
  void stop();

 private:
  static LRESULT CALLBACK mouseProc(int code, WPARAM wParam, LPARAM lParam);
  static LRESULT CALLBACK keyboardProc(int code, WPARAM wParam, LPARAM lParam);
  void run(void* ready);

  IInputSink* sink_ = nullptr;
  std::thread thread_;
  DWORD threadId_ = 0;
  HHOOK mouseHook_ = nullptr;
  HHOOK keyboardHook_ = nullptr;
  std::bitset<1024> keysDown_;  // by evdev code, to tell auto-repeat from a new press
};

}  // namespace nm
