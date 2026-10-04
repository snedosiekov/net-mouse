// Windows entry point: role selection, tray icon, status/arrangement window, worker threads.
//
// Command line:
//   --config <file>   settings file (default %APPDATA%\NetMouse\netmouse.ini)
//   --role master|slave   set the role without asking
//   --default-role master|slave   role to use if none has been chosen yet (used by the installer)
//   --name <name>     name shown to other PCs (default: computer name)
//   --dry-run         slave only: log received input instead of injecting it
//   --quit            ask running instances in this session to exit, then return
//   --show            open the window at start (by default the app starts minimized to the tray)

#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <memory>
#include <random>
#include <string>
#include <thread>

#include "app/master_engine.h"
#include "app/network_loop.h"
#include "app/slave_engine.h"
#include "core/config.h"
#include "core/log.h"
#include "core/protocol.h"
#include "core/udp.h"
#include "platform/win/win_platform.h"
#include "ui/win/layout_view.h"
#include "ui/win/resource_ids.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;
using namespace nm;

namespace {

// The class name carries the role ("NetMouseMainWindow-master"), so a second start finds the instance
// with the same role; --quit matches the prefix.
constexpr wchar_t kWindowClassPrefix[] = L"NetMouseMainWindow";
constexpr wchar_t kAppTitle[] = L"Net Mouse";
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kTrayId = 1;
enum : UINT { IDM_SHOW = 100, IDM_RETURN, IDM_CHANGE_ROLE, IDM_EXIT };

constexpr char kDefaultGroup[] = "239.255.77.77";
constexpr int kDefaultPort = 47800;

struct App {
  // Command line
  std::wstring configArg;
  std::string roleArg;
  std::string defaultRoleArg;
  bool quit = false;
  bool show = false;
  std::string nameArg;
  bool dryRun = false;

  Config config;
  std::string role;
  uint64_t id = 0;
  std::string name;
  std::string group;
  int port = kDefaultPort;

  UdpMulticast net;
  WinDisplays displays;
  WinCursor cursor;
  std::unique_ptr<WinInjector> injector;
  std::unique_ptr<MasterEngine> master;
  std::unique_ptr<SlaveEngine> slave;
  WinMouseHook hook;
  std::thread netThread;
  std::atomic<bool> running{true};

  HWND hwnd = nullptr;
  HFONT font = nullptr;
  HFONT bigFont = nullptr;
  UINT dpi = 96;
  UINT taskbarCreatedMsg = 0;
  ui::LayoutView layoutView;
  uint64_t paintedChange = ~0ull;
  uint64_t paintedLog = ~0ull;
  bool restart = false;

  HICON iconIdle = nullptr;
  HICON iconActive = nullptr;
  bool trayActive = false;
  std::wstring trayTip;
  bool showTrayHint = false;  // first run: tell the user where the app went
};

App g;

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring windowClass() { return std::wstring(kWindowClassPrefix) + L"-" + widen(g.role); }

uint64_t randomId() {
  std::random_device rd;
  uint64_t id = 0;
  while (id == 0) id = (static_cast<uint64_t>(rd()) << 32) | rd();
  return id;
}

std::string computerName() {
  wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1];
  DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
  return GetComputerNameW(buf, &size) ? narrow(buf) : "pc";
}

fs::path defaultConfigPath() {
  wchar_t buf[MAX_PATH];
  const DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
  fs::path base = (n > 0 && n < MAX_PATH) ? fs::path(buf) : fs::current_path();
  return base / L"NetMouse" / L"netmouse.ini";
}

void parseArgs() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    const bool hasValue = i + 1 < argc;
    if (a == L"--config" && hasValue) {
      g.configArg = argv[++i];
    } else if (a == L"--role" && hasValue) {
      g.roleArg = narrow(argv[++i]);
    } else if (a == L"--name" && hasValue) {
      g.nameArg = narrow(argv[++i]);
    } else if (a == L"--default-role" && hasValue) {
      g.defaultRoleArg = narrow(argv[++i]);
    } else if (a == L"--dry-run") {
      g.dryRun = true;
    } else if (a == L"--quit") {
      g.quit = true;
    } else if (a == L"--show") {
      g.show = true;
    }
  }
  LocalFree(argv);
}

std::string chooseRole() {
  const TASKDIALOG_BUTTON buttons[] = {
      {1001, L"Master\nThis PC's mouse controls this PC and the slave PCs."},
      {1002, L"Slave\nThis PC is controlled by the master PC's mouse."},
  };
  TASKDIALOGCONFIG c{};
  c.cbSize = sizeof(c);
  c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
  c.pszWindowTitle = kAppTitle;
  c.pszMainIcon = TD_INFORMATION_ICON;
  c.pszMainInstruction = L"Choose the role of this PC";
  c.pszContent = L"Run Net Mouse on every PC in the same local network: one master and any number of slaves. "
                 L"The choice is remembered and can be changed later from the tray icon menu.";
  c.pButtons = buttons;
  c.cButtons = ARRAYSIZE(buttons);
  c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
  int pressed = 0;
  if (FAILED(TaskDialogIndirect(&c, &pressed, nullptr, nullptr))) return {};
  if (pressed == 1001) return "master";
  if (pressed == 1002) return "slave";
  return {};
}

// ---------------------------------------------------------------------------------------------
// Worker threads

void networkLoop() { runNetworkLoop(g.net, g.master.get(), g.slave.get(), g.running); }

// ---------------------------------------------------------------------------------------------
// Window

void updateFonts() {
  if (g.font) DeleteObject(g.font);
  if (g.bigFont) DeleteObject(g.bigFont);
  auto make = [](int pt, int weight) {
    return CreateFontW(-MulDiv(pt, static_cast<int>(g.dpi), 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH, L"Segoe UI");
  };
  g.font = make(9, FW_NORMAL);
  g.bigFont = make(11, FW_SEMIBOLD);
}

std::wstring statusText() {
  std::wstring t;
  t += std::format(L"{} • {} • id {} • group {}:{}\n", g.master ? L"MASTER" : L"SLAVE",
                   widen(g.name), widen(toHex(g.id)), widen(g.group), g.port);
  if (g.master) {
    const MasterSnapshot s = g.master->snapshot();
    if (s.remote) {
      std::string target = "?";
      for (const SlaveView& v : s.slaves) {
        if (v.id == s.activeSlave) target = v.name;
      }
      t += std::format(L"Pointer is on slave \"{}\"  (Ctrl+Alt+Home brings it back)\n", widen(target));
    } else {
      t += L"Pointer is on this PC\n";
    }
    if (s.slaves.empty()) t += L"No slaves found yet.\n";
    for (const SlaveView& v : s.slaves) {
      t += std::format(L"  • {}  {}  {}{}  origin ({}, {})\n", widen(v.name), widen(ipToString(v.ip)),
                       v.online ? L"online" : L"offline", v.online && !v.synced ? L", syncing" : L"",
                       v.offset.x, v.offset.y);
    }
    t += L"Drag the green slave screens to where they physically are; the pointer crosses where screens touch.\n";
  } else {
    const SlaveSnapshot s = g.slave->snapshot();
    if (!s.configured) {
      t += s.masterOnline ? std::format(L"Master \"{}\" found at {}, waiting for its configuration…\n",
                                        widen(s.masterName), widen(ipToString(s.masterIp)))
                          : std::wstring(L"Looking for a master on the local network…\n");
    } else {
      t += std::format(L"Master \"{}\" {} • this desktop's origin is at ({}, {}) in the master layout\n",
                       widen(s.masterName.empty() ? toHex(s.masterId) : s.masterName),
                       s.masterOnline ? L"online" : L"offline", s.offset.x, s.offset.y);
    }
    t += s.active ? L"The master's pointer is on this PC\n" : L"Idle\n";
    for (size_t i = 0; i < s.monitors.size(); ++i) {
      const Rect& r = s.monitors[i].rect;
      t += std::format(L"  Monitor {}: {}×{} at ({}, {}){}\n", i + 1, r.width(), r.height(), r.left, r.top,
                       s.monitors[i].primary ? L" primary" : L"");
    }
    if (g.dryRun) t += L"DRY RUN: input is logged, not injected\n";
  }
  t += L"\nRecent events:\n";
  for (const std::string& line : log::recent(6)) t += widen(line) + L"\n";
  return t;
}

void loadIcons(HINSTANCE instance) {
  // LoadIconMetric picks the best image from the .ico for the current DPI.
  if (FAILED(LoadIconMetric(instance, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &g.iconIdle))) {
    g.iconIdle = LoadIconW(nullptr, IDI_APPLICATION);
  }
  if (FAILED(LoadIconMetric(instance, MAKEINTRESOURCEW(IDI_ACTIVE), LIM_SMALL, &g.iconActive))) {
    g.iconActive = g.iconIdle;
  }
}

// Current tray state: "active" means the pointer is on a slave (master) or controlled by the master (slave).
std::pair<bool, std::wstring> trayState() {
  std::wstring tip = std::format(L"Net Mouse – {} ({})", g.master ? L"master" : L"slave", widen(g.name));
  bool active = false;
  if (g.master) {
    const MasterSnapshot s = g.master->snapshot();
    active = s.remote;
    size_t online = 0;
    for (const SlaveView& v : s.slaves) {
      if (v.online) ++online;
      if (s.remote && v.id == s.activeSlave) tip += L"\nPointer on " + widen(v.name);
    }
    if (!s.remote) tip += std::format(L"\n{} slave(s) online", online);
  } else {
    const SlaveSnapshot s = g.slave->snapshot();
    active = s.active;
    if (s.active) {
      tip += L"\nControlled by " + widen(s.masterName);
    } else if (s.masterOnline) {
      tip += L"\nMaster: " + widen(s.masterName);
    } else {
      tip += L"\nLooking for a master…";
    }
  }
  return {active, tip};
}

void addTrayIcon() {
  const auto [active, tip] = trayState();
  g.trayActive = active;
  g.trayTip = tip;
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g.hwnd;
  nid.uID = kTrayId;
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid.uCallbackMessage = WM_TRAY;
  nid.hIcon = active ? g.iconActive : g.iconIdle;
  wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_ADD, &nid);
}

void updateTrayIcon() {
  const auto [active, tip] = trayState();
  if (active == g.trayActive && tip == g.trayTip) return;
  g.trayActive = active;
  g.trayTip = tip;
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g.hwnd;
  nid.uID = kTrayId;
  nid.uFlags = NIF_ICON | NIF_TIP;
  nid.hIcon = active ? g.iconActive : g.iconIdle;
  wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void showTrayHint() {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g.hwnd;
  nid.uID = kTrayId;
  nid.uFlags = NIF_INFO;
  nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
  nid.hBalloonIcon = g.iconIdle;
  wcsncpy_s(nid.szInfoTitle, L"Net Mouse is running", _TRUNCATE);
  wcsncpy_s(nid.szInfo,
            g.master ? L"It runs in the notification area. Click the mouse icon to arrange the slave screens."
                     : L"It runs in the notification area. Click the mouse icon to see the status.",
            _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void removeTrayIcon() {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g.hwnd;
  nid.uID = kTrayId;
  Shell_NotifyIconW(NIM_DELETE, &nid);
}

void showTrayMenu() {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, IDM_SHOW, g.master ? L"Arrange displays…" : L"Status…");
  if (g.master) AppendMenuW(menu, MF_STRING, IDM_RETURN, L"Return pointer to this PC\tCtrl+Alt+Home");
  AppendMenuW(menu, MF_STRING, IDM_CHANGE_ROLE, L"Change role…");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");
  SetMenuDefaultItem(menu, IDM_SHOW, FALSE);
  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(g.hwnd);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g.hwnd, nullptr);
  DestroyMenu(menu);
}

void showWindow() {
  ShowWindow(g.hwnd, SW_SHOWNORMAL);
  SetForegroundWindow(g.hwnd);
}

RECT layoutArea(const RECT& client) {
  RECT r = client;
  r.bottom = std::max(r.top, r.bottom - MulDiv(230, static_cast<int>(g.dpi), 96));
  return r;
}

void paint(HWND hwnd) {
  PAINTSTRUCT ps;
  HDC hdc = BeginPaint(hwnd, &ps);
  RECT client;
  GetClientRect(hwnd, &client);
  HDC dc = CreateCompatibleDC(hdc);
  HBITMAP bmp = CreateCompatibleBitmap(hdc, client.right, client.bottom);
  HGDIOBJ oldBmp = SelectObject(dc, bmp);
  FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));

  RECT textArea = client;
  if (g.master) {
    const RECT area = layoutArea(client);
    g.layoutView.paint(dc, area, g.master->snapshot(), g.font, g.dpi);
    textArea.top = area.bottom;
  }
  const int pad = MulDiv(10, static_cast<int>(g.dpi), 96);
  InflateRect(&textArea, -pad, -pad);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
  HGDIOBJ oldFont = SelectObject(dc, g.master ? g.font : g.bigFont);
  const std::wstring text = statusText();
  DrawTextW(dc, text.c_str(), -1, &textArea, DT_LEFT | DT_NOPREFIX | DT_EXPANDTABS | DT_END_ELLIPSIS);
  SelectObject(dc, oldFont);

  BitBlt(hdc, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
  SelectObject(dc, oldBmp);
  DeleteObject(bmp);
  DeleteDC(dc);
  EndPaint(hwnd, &ps);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (g.taskbarCreatedMsg && msg == g.taskbarCreatedMsg) {
    addTrayIcon();  // Explorer restarted
    return 0;
  }
  switch (msg) {
    case WM_CREATE:
      g.hwnd = hwnd;
      g.dpi = GetDpiForWindow(hwnd);
      updateFonts();
      addTrayIcon();
      if (g.showTrayHint) showTrayHint();
      SetTimer(hwnd, kRefreshTimer, 150, nullptr);
      return 0;
    case WM_TIMER: {
      const uint64_t change = g.master ? g.master->changeCounter() : g.slave->changeCounter();
      const uint64_t logCount = log::counter();
      updateTrayIcon();
      if (IsWindowVisible(hwnd) && (change != g.paintedChange || logCount != g.paintedLog)) {
        g.paintedChange = change;
        g.paintedLog = logCount;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }
    case WM_PAINT:
      paint(hwnd);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      if (wParam == SIZE_MINIMIZED) {
        ShowWindow(hwnd, SW_HIDE);  // minimize to the tray
      } else {
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_DPICHANGED: {
      g.dpi = HIWORD(wParam);
      updateFonts();
      const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
      SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                   suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_LBUTTONDOWN:
      if (g.master && g.layoutView.beginDrag({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)})) {
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_MOUSEMOVE:
      if (g.master && g.layoutView.dragging()) {
        g.layoutView.dragTo({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
      if (g.master && g.layoutView.dragging()) {
        if (msg == WM_LBUTTONUP) g.layoutView.dragTo({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        if (auto drop = g.layoutView.endDrag()) {
          if (!g.master->setSlaveOffset(drop->slaveId, drop->offset)) MessageBeep(MB_ICONWARNING);
        }
        if (msg == WM_LBUTTONUP) ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_TRAY:
      switch (LOWORD(lParam)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
          showWindow();
          break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
          showTrayMenu();
          break;
        default:
          break;
      }
      return 0;
    case WM_COMMAND:
      switch (LOWORD(wParam)) {
        case IDM_SHOW:
          showWindow();
          break;
        case IDM_RETURN:
          if (g.master) g.master->onPanic();
          break;
        case IDM_CHANGE_ROLE:
          if (MessageBoxW(hwnd, L"Net Mouse will restart and ask for the role of this PC.", kAppTitle,
                          MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
            g.restart = true;
            DestroyWindow(hwnd);
          }
          break;
        case IDM_EXIT:
          DestroyWindow(hwnd);
          break;
        default:
          break;
      }
      return 0;
    case WM_ENDSESSION:
      if (wParam) DestroyWindow(hwnd);  // logoff/shutdown or an installer closing the app
      return 0;
    case WM_CLOSE:
      ShowWindow(hwnd, SW_HIDE);  // keep running in the tray
      return 0;
    case WM_DESTROY:
      KillTimer(hwnd, kRefreshTimer);
      removeTrayIcon();
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(hwnd, msg, wParam, lParam);
  }
}

bool createWindow(HINSTANCE instance) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = wndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
  wc.hIconSm = g.iconIdle;
  const std::wstring className = windowClass();
  wc.lpszClassName = className.c_str();
  if (!RegisterClassExW(&wc)) return false;

  g.taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
  const UINT dpi = GetDpiForSystem();
  const int w = MulDiv(g.master ? 900 : 640, static_cast<int>(dpi), 96);
  const int h = MulDiv(g.master ? 680 : 360, static_cast<int>(dpi), 96);
  const std::wstring title = std::format(L"{} – {} ({})", kAppTitle, g.master ? L"Master" : L"Slave", widen(g.name));
  return CreateWindowExW(0, className.c_str(), title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                         nullptr, nullptr, instance, nullptr) != nullptr;
}

void relaunch() {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
  if (!g.configArg.empty()) cmd += L" --config \"" + g.configArg + L"\"";
  if (g.dryRun) cmd += L" --dry-run";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
}

// Posts "Exit" to every Net Mouse window in this session; they shut down cleanly (cursor restored).
int quitRunningInstances() {
  EnumWindows(
      [](HWND hwnd, LPARAM) -> BOOL {
        wchar_t cls[64];
        if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) &&
            wcsncmp(cls, kWindowClassPrefix, wcslen(kWindowClassPrefix)) == 0) {
          PostMessageW(hwnd, WM_COMMAND, IDM_EXIT, 0);
        }
        return TRUE;
      },
      0);
  return 0;
}

int fail(const std::wstring& message) {
  log::info("Fatal: " + narrow(message));
  MessageBoxW(nullptr, message.c_str(), kAppTitle, MB_OK | MB_ICONERROR);
  return 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  // Physical pixels everywhere: hook coordinates, SetCursorPos and SendInput then agree.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);
  parseArgs();
  if (g.quit) return quitRunningInstances();

  const fs::path configPath = g.configArg.empty() ? defaultConfigPath() : fs::path(g.configArg);
  g.config.load(configPath);

  g.role = !g.roleArg.empty() ? g.roleArg : g.config.get("role");
  if (g.role.empty()) g.role = g.defaultRoleArg;
  if (g.role != "master" && g.role != "slave") {
    g.role = chooseRole();
    if (g.role.empty()) return 0;
  }
  g.config.set("role", g.role);
  g.id = g.config.getHex("id");
  if (g.id == 0) {
    g.id = randomId();
    g.config.setHex("id", g.id);
  }
  if (!g.nameArg.empty()) g.config.set("name", g.nameArg);
  g.name = g.config.get("name", computerName());
  g.group = g.config.get("group", kDefaultGroup);
  g.port = static_cast<int>(g.config.getInt("port", kDefaultPort));
  if (!g.config.has("group")) g.config.set("group", g.group);
  if (!g.config.has("port")) g.config.setInt("port", g.port);
  if (!g.config.has("interface")) g.config.set("interface", "");
  if (g.role == "master" && !g.config.has("hide_cursor")) g.config.setInt("hide_cursor", 1);
  g.showTrayHint = g.config.getInt("tray_hint_shown", 0) == 0;
  g.config.setInt("tray_hint_shown", 1);
  g.config.save();

  std::error_code ec;
  fs::create_directories(configPath.parent_path(), ec);
  log::init(configPath.parent_path() / ("netmouse-" + g.role + ".log"));
  log::info(std::format("Starting as {} '{}' (id {}), config {}", g.role, g.name, toHex(g.id), configPath.string()));

  // One instance per role per PC (a master and a slave may run side by side for testing).
  const std::wstring mutexName = L"Local\\NetMouse-" + widen(g.role);
  HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    // Started again (e.g. from the Start menu) while running in the tray: open the existing window.
    if (instanceMutex) CloseHandle(instanceMutex);
    if (HWND existing = FindWindowW(windowClass().c_str(), nullptr)) {
      AllowSetForegroundWindow(ASFW_ANY);
      PostMessageW(existing, WM_COMMAND, IDM_SHOW, 0);
      return 0;
    }
    return fail(L"Net Mouse is already running as " + widen(g.role) + L" on this PC.");
  }

  std::string error;
  if (!g.net.open(g.group, static_cast<uint16_t>(g.port), g.config.get("interface"), error)) {
    CloseHandle(instanceMutex);
    return fail(L"Network setup failed: " + widen(error));
  }

  if (g.role == "master") {
    g.cursor.show();  // restore cursors in case a previous run crashed while they were hidden
    MasterEngine::Options opt{g.id, g.name, g.config.getInt("hide_cursor", 1) != 0};
    g.master = std::make_unique<MasterEngine>(opt, g.config, g.net, g.displays, g.cursor);
  } else {
    g.injector = std::make_unique<WinInjector>(g.dryRun);
    g.slave = std::make_unique<SlaveEngine>(SlaveEngine::Options{g.id, g.name}, g.config, g.net, g.displays,
                                            *g.injector);
  }

  loadIcons(instance);
  if (!createWindow(instance)) {
    CloseHandle(instanceMutex);
    return fail(L"Could not create the main window.");
  }
  g.netThread = std::thread(networkLoop);
  if (g.master && !g.hook.start(g.master.get())) {
    g.running = false;
    g.netThread.join();
    CloseHandle(instanceMutex);
    return fail(L"Could not install the mouse hook.");
  }
  if (g.show) showWindow();  // otherwise the app lives in the tray until the icon is clicked

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  // Shutdown: stop capturing first so the cursor is guaranteed to be back on the master.
  if (g.master) {
    g.hook.stop();
    g.master->onPanic();
    g.cursor.show();
  }
  g.running = false;
  g.netThread.join();
  g.net.close();
  log::info("Stopped");

  if (g.restart) {
    g.config.set("role", "");
    g.config.save();
    CloseHandle(instanceMutex);
    instanceMutex = nullptr;
    relaunch();
  }
  if (instanceMutex) CloseHandle(instanceMutex);
  if (g.font) DeleteObject(g.font);
  if (g.bigFont) DeleteObject(g.bigFont);
  return 0;
}
