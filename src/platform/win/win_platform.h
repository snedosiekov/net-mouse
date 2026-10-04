#pragma once

#include <windows.h>

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

 private:
  bool dryRun_;
};

// Global low-level mouse hook (WH_MOUSE_LL) on a dedicated high-priority thread, plus the
// emergency hotkey Ctrl+Alt+Home.
class WinMouseHook {
 public:
  ~WinMouseHook() { stop(); }
  bool start(IMouseSink* sink);
  void stop();

 private:
  static LRESULT CALLBACK hookProc(int code, WPARAM wParam, LPARAM lParam);
  void run(void* ready);

  IMouseSink* sink_ = nullptr;
  std::thread thread_;
  DWORD threadId_ = 0;
  HHOOK hook_ = nullptr;
};

}  // namespace nm
