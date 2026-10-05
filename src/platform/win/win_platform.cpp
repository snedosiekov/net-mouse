#define OEMRESOURCE  // OCR_* cursor ids
#include "platform/win/win_platform.h"

#include <algorithm>
#include <format>
#include <future>

#include "core/keymap.h"
#include "core/log.h"
#include "core/protocol.h"

namespace nm {
namespace {

constexpr int kPanicHotkeyId = 1;
WinInputHook* g_hook = nullptr;  // the hook procedure has no user pointer

std::string narrow(const wchar_t* w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string s(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

BOOL CALLBACK enumMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
  auto* list = reinterpret_cast<std::vector<MonitorInfo>*>(param);
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (GetMonitorInfoW(monitor, &mi)) {
    MonitorInfo info;
    info.rect = {mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom};
    info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    info.name = narrow(mi.szDevice);
    list->push_back(std::move(info));
  }
  return TRUE;
}

// Maps a virtual-desktop pixel to SendInput's 0..65535 absolute range so that it lands exactly on
// that pixel.
LONG normalize(int offset, int size) {
  if (size <= 1) return 0;
  const long long n = (static_cast<long long>(offset) * 65536 + size - 1) / size;
  return static_cast<LONG>(std::clamp<long long>(n, 0, 65535));
}

void sendMouse(DWORD flags, LONG dx, LONG dy, DWORD data) {
  INPUT in{};
  in.type = INPUT_MOUSE;
  in.mi.dx = dx;
  in.mi.dy = dy;
  in.mi.mouseData = data;
  in.mi.dwFlags = flags;
  SendInput(1, &in, sizeof(in));
}

}  // namespace

// ---------------------------------------------------------------------------------------------

std::vector<MonitorInfo> WinDisplays::monitors() {
  std::vector<MonitorInfo> list;
  EnumDisplayMonitors(nullptr, nullptr, enumMonitor, reinterpret_cast<LPARAM>(&list));
  // Stable order: primary first, then by position.
  std::sort(list.begin(), list.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
    if (a.primary != b.primary) return a.primary;
    if (a.rect.left != b.rect.left) return a.rect.left < b.rect.left;
    return a.rect.top < b.rect.top;
  });
  return list;
}

// ---------------------------------------------------------------------------------------------

void WinCursor::setPos(Point p) { SetCursorPos(p.x, p.y); }

void WinCursor::hide() {
  static const DWORD kIds[] = {OCR_NORMAL, OCR_IBEAM,  OCR_WAIT,   OCR_CROSS,  OCR_UP,
                               OCR_SIZENWSE, OCR_SIZENESW, OCR_SIZEWE, OCR_SIZENS, OCR_SIZEALL,
                               OCR_NO,     OCR_HAND,   OCR_APPSTARTING, 32651 /* OCR_HELP */};
  const int w = GetSystemMetrics(SM_CXCURSOR);
  const int h = GetSystemMetrics(SM_CYCURSOR);
  std::vector<BYTE> andMask(static_cast<size_t>(w * h / 8), 0xFF);
  std::vector<BYTE> xorMask(static_cast<size_t>(w * h / 8), 0x00);
  for (DWORD id : kIds) {
    // SetSystemCursor takes ownership, so every id needs its own copy.
    HCURSOR blank = CreateCursor(GetModuleHandleW(nullptr), 0, 0, w, h, andMask.data(), xorMask.data());
    if (blank) SetSystemCursor(blank, id);
  }
}

void WinCursor::show() { SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0); }

uint8_t WinCursor::pressedButtons() {
  uint8_t mask = 0;
  if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) mask |= proto::kButtonLeft;
  if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) mask |= proto::kButtonRight;
  if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) mask |= proto::kButtonMiddle;
  if (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) mask |= proto::kButtonX1;
  if (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) mask |= proto::kButtonX2;
  return mask;
}

// ---------------------------------------------------------------------------------------------

void WinInjector::moveTo(Point p) {
  if (dryRun_) {
    log::info(std::format("[dry-run] move to ({}, {})", p.x, p.y));
    return;
  }
  const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  sendMouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE_NOCOALESCE,
            normalize(p.x - vx, vw), normalize(p.y - vy, vh), 0);
}

void WinInjector::button(uint8_t button, bool down) {
  if (dryRun_) {
    log::info(std::format("[dry-run] button {} {}", button, down ? "down" : "up"));
    return;
  }
  switch (button) {
    case proto::kButtonLeft:
      sendMouse(down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP, 0, 0, 0);
      break;
    case proto::kButtonRight:
      sendMouse(down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP, 0, 0, 0);
      break;
    case proto::kButtonMiddle:
      sendMouse(down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP, 0, 0, 0);
      break;
    case proto::kButtonX1:
      sendMouse(down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP, 0, 0, XBUTTON1);
      break;
    case proto::kButtonX2:
      sendMouse(down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP, 0, 0, XBUTTON2);
      break;
    default:
      break;
  }
}

void WinInjector::wheel(int delta, bool horizontal) {
  if (dryRun_) {
    log::info(std::format("[dry-run] {} wheel {}", horizontal ? "horizontal" : "vertical", delta));
    return;
  }
  sendMouse(horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL, 0, 0, static_cast<DWORD>(delta));
}

void WinInjector::key(uint16_t code, int value) {
  if (dryRun_) {
    log::info(log::fmt("[dry-run] key %u %s", code, value == 0 ? "up" : (value == 2 ? "repeat" : "down")));
    return;
  }
  keymap::WinKey wk;
  if (!keymap::toWindows(code, wk)) return;  // no Windows equivalent
  INPUT in{};
  in.type = INPUT_KEYBOARD;
  if (wk.vk) {
    in.ki.wVk = wk.vk;
  } else {
    // Scan codes describe the physical key, so this PC's own keyboard layout applies.
    in.ki.wScan = wk.scan;
    in.ki.dwFlags = KEYEVENTF_SCANCODE;
  }
  if (wk.extended) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
  if (value == 0) in.ki.dwFlags |= KEYEVENTF_KEYUP;
  SendInput(1, &in, sizeof(in));
}

// ---------------------------------------------------------------------------------------------

bool WinInputHook::start(IInputSink* sink) {
  sink_ = sink;
  g_hook = this;
  std::promise<bool> ready;
  std::future<bool> result = ready.get_future();
  thread_ = std::thread([this, &ready] { run(&ready); });
  if (!result.get()) {
    thread_.join();
    g_hook = nullptr;
    return false;
  }
  return true;
}

void WinInputHook::run(void* readyPtr) {
  auto* ready = static_cast<std::promise<bool>*>(readyPtr);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  threadId_ = GetCurrentThreadId();
  MSG msg;
  PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // creates the message queue
  mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, &WinInputHook::mouseProc, GetModuleHandleW(nullptr), 0);
  keyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &WinInputHook::keyboardProc, GetModuleHandleW(nullptr), 0);
  if (!mouseHook_ || !keyboardHook_) {
    if (mouseHook_) UnhookWindowsHookEx(mouseHook_);
    if (keyboardHook_) UnhookWindowsHookEx(keyboardHook_);
    mouseHook_ = keyboardHook_ = nullptr;
    ready->set_value(false);
    return;
  }
  if (!RegisterHotKey(nullptr, kPanicHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_HOME)) {
    log::info("Could not register the Ctrl+Alt+Home emergency hotkey (already in use?)");
  }
  ready->set_value(true);  // `ready` must not be touched after this point

  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.message == WM_HOTKEY && msg.wParam == kPanicHotkeyId) sink_->onPanic();
  }
  UnregisterHotKey(nullptr, kPanicHotkeyId);
  UnhookWindowsHookEx(mouseHook_);
  UnhookWindowsHookEx(keyboardHook_);
  mouseHook_ = keyboardHook_ = nullptr;
}

void WinInputHook::stop() {
  if (!thread_.joinable()) return;
  PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
  thread_.join();
  g_hook = nullptr;
}

LRESULT CALLBACK WinInputHook::keyboardProc(int code, WPARAM wParam, LPARAM lParam) {
  if (code == HC_ACTION && g_hook) {
    const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    // Injected keys (other software, remote desktop tools) are left alone.
    if (!(k->flags & LLKHF_INJECTED)) {
      IInputSink* sink = g_hook->sink_;
      const bool up = (k->flags & LLKHF_UP) != 0;
      const uint16_t evdev = keymap::fromWindows(k->vkCode, k->scanCode, (k->flags & LLKHF_EXTENDED) != 0);
      bool swallow;
      if (evdev == 0 || evdev >= g_hook->keysDown_.size()) {
        swallow = sink->isRemote();  // no equivalent on the slave: just keep it away from this PC
      } else {
        int value = 0;
        if (!up) value = g_hook->keysDown_.test(evdev) ? 2 : 1;
        g_hook->keysDown_.set(evdev, !up);
        swallow = sink->onKey(evdev, value);
      }
      if (swallow) return 1;
    }
  }
  return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK WinInputHook::mouseProc(int code, WPARAM wParam, LPARAM lParam) {
  if (code == HC_ACTION && g_hook) {
    const auto* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
    IInputSink* sink = g_hook->sink_;
    bool swallow = false;
    switch (wParam) {
      case WM_MOUSEMOVE:
        swallow = sink->onMouseMove({m->pt.x, m->pt.y});
        break;
      case WM_LBUTTONDOWN:
      case WM_LBUTTONUP:
        swallow = sink->onMouseButton(proto::kButtonLeft, wParam == WM_LBUTTONDOWN);
        break;
      case WM_RBUTTONDOWN:
      case WM_RBUTTONUP:
        swallow = sink->onMouseButton(proto::kButtonRight, wParam == WM_RBUTTONDOWN);
        break;
      case WM_MBUTTONDOWN:
      case WM_MBUTTONUP:
        swallow = sink->onMouseButton(proto::kButtonMiddle, wParam == WM_MBUTTONDOWN);
        break;
      case WM_XBUTTONDOWN:
      case WM_XBUTTONUP:
        swallow = sink->onMouseButton(HIWORD(m->mouseData) == XBUTTON1 ? proto::kButtonX1 : proto::kButtonX2,
                                      wParam == WM_XBUTTONDOWN);
        break;
      case WM_MOUSEWHEEL:
        swallow = sink->onMouseWheel(static_cast<short>(HIWORD(m->mouseData)), false);
        break;
      case WM_MOUSEHWHEEL:
        swallow = sink->onMouseWheel(static_cast<short>(HIWORD(m->mouseData)), true);
        break;
      default:
        break;
    }
    if (swallow) return 1;
  }
  return CallNextHookEx(nullptr, code, wParam, lParam);
}

}  // namespace nm
