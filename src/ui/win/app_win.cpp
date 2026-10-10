// Windows entry point. Dispatches to the service (--service), the agent (--agent) or, by default,
// the control app: tray icon, status/arrangement window and dialogs. The control app holds no
// engine; it shows what the agent reports over the control pipe and sends it the user's commands.
//
// Command line (control app):
//   --show         open the window at start (by default the app starts in the tray)
//   --pipe NAME    control pipe of the agent (default net-mouse-control)
//   --quit         ask Net Mouse windows in this session to close, then return
// Agent (normally started by the service):
//   --agent [--config FILE] [--pipe NAME] [--quit-event NAME] [--role master|slave] [--dry-run]

#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "app/master_engine.h"
#include "app/slave_engine.h"
#include "core/config.h"
#include "core/json.h"
#include "core/udp.h"
#include "platform/win/win_common.h"
#include "ui/win/layout_view.h"
#include "ui/win/network_dialog.h"
#include "ui/win/resource_ids.h"
#include "win/entry.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;
using namespace nm;
using nm::win::narrow;
using nm::win::widen;

namespace {

// Older versions used "NetMouseMainWindow-<role>"; --quit matches every "NetMouse..." window.
constexpr wchar_t kWindowClass[] = L"NetMouseControlWindow";
constexpr wchar_t kWindowClassPrefix[] = L"NetMouse";
constexpr wchar_t kAppTitle[] = L"Net Mouse";
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_STATUS = WM_APP + 2;  // the poll thread got a new status
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kTrayId = 1;
enum : UINT { IDM_SHOW = 100, IDM_RETURN, IDM_CHANGE_ROLE, IDM_FORGET_MASTER, IDM_DISCONNECT, IDM_EXIT, IDM_NETWORK,
              IDM_START_SERVICE };

// What the agent reports ("status" over the pipe), converted for the UI.
struct Status {
  bool reachable = false;
  std::string role;  // "" = no role chosen yet
  std::string name;
  std::string fingerprint;
  std::string group;
  int port = 0;
  std::vector<std::string> log;
  std::vector<std::string> filter;
  std::vector<UdpMulticast::Choice> choices;
  MasterSnapshot master;
  SlaveSnapshot slave;
};

struct App {
  std::wstring pipe = win::kDefaultPipe;
  bool show = false;
  fs::path prefsPath;  // %APPDATA%\NetMouse\control.ini (window hints only)

  std::mutex statusMutex;
  Status status;
  std::string lastRaw;
  std::atomic<bool> polling{true};
  std::thread poller;
  bool roleAsked = false;

  HWND hwnd = nullptr;
  HFONT font = nullptr;
  HFONT bigFont = nullptr;
  UINT dpi = 96;
  UINT taskbarCreatedMsg = 0;
  ui::LayoutView layoutView;

  HICON iconIdle = nullptr;
  HICON iconActive = nullptr;
  bool trayActive = false;
  std::wstring trayTip;
  bool showTrayHint = false;
  bool closeHintShown = false;  // "still running" balloon after the first window close
  uint64_t menuSlave = 0;       // slave the canvas context menu is about
};

App g;

Status currentStatus() {
  std::lock_guard lock(g.statusMutex);
  return g.status;
}

bool isMaster(const Status& s) { return s.reachable && s.role == "master"; }

uint64_t parseHexId(const std::string& text) {
  try {
    return std::stoull(text, nullptr, 16);
  } catch (...) {
    return 0;
  }
}

uint32_t parseIpOrZero(const std::string& text) {
  uint32_t ip = 0;
  return parseIp(text, ip) ? ip : 0;
}

std::vector<MonitorInfo> parseMonitors(const json::Value& list) {
  std::vector<MonitorInfo> out;
  for (const json::Value& m : list.a) {
    const int32_t x = static_cast<int32_t>(m["x"].asInt()), y = static_cast<int32_t>(m["y"].asInt());
    out.push_back({{x, y, x + static_cast<int32_t>(m["w"].asInt()), y + static_cast<int32_t>(m["h"].asInt())},
                   m["primary"].asBool(), m["name"].asString()});
  }
  return out;
}

Status parseStatus(const json::Value& v) {
  Status s;
  s.reachable = true;
  s.role = v["role"].asString();
  s.name = v["name"].asString();
  s.fingerprint = v["fingerprint"].asString();
  s.group = v["group"].asString();
  s.port = static_cast<int>(v["port"].asInt());
  for (const json::Value& line : v["log"].a) s.log.push_back(line.asString());
  s.filter = parseInterfaceList(v["interface_filter"].asString());
  for (const json::Value& i : v["interfaces"].a) {
    s.choices.push_back({NetInterface{parseIpOrZero(i["ip"].asString()), 0, i["name"].asString()}, i["selected"].asBool()});
  }
  if (s.role == "master") {
    MasterSnapshot& m = s.master;
    m.masterMonitors = parseMonitors(v["monitors"]);
    m.remote = v["remote"].asBool();
    m.activeSlave = parseHexId(v["active"].asString());
    m.virtualPos = {static_cast<int32_t>(v["pointer"]["x"].asInt()), static_cast<int32_t>(v["pointer"]["y"].asInt())};
    m.fingerprint = s.fingerprint;
    for (const json::Value& j : v["slaves"].a) {
      SlaveView sv;
      sv.id = parseHexId(j["id"].asString());
      sv.name = j["name"].asString();
      sv.ip = parseIpOrZero(j["ip"].asString());
      sv.online = j["online"].asBool();
      sv.paired = j["paired"].asBool();
      sv.connected = j["connected"].asBool();
      sv.synced = j["synced"].asBool();
      sv.keyMismatch = j["key_mismatch"].asBool();
      sv.otherMaster = j["other_master"].asBool();
      sv.fingerprint = j["fingerprint"].asString();
      sv.offset = {static_cast<int32_t>(j["offset"]["x"].asInt()), static_cast<int32_t>(j["offset"]["y"].asInt())};
      for (const MonitorInfo& mi : parseMonitors(j["monitors"])) sv.monitors.push_back(mi.rect);
      m.slaves.push_back(std::move(sv));
    }
  } else if (s.role == "slave") {
    SlaveSnapshot& sl = s.slave;
    sl.paired = v["paired"].asBool();
    sl.connected = v["connected"].asBool();
    sl.configured = v["configured"].asBool();
    sl.active = v["active"].asBool();
    sl.offset = {static_cast<int32_t>(v["offset"]["x"].asInt()), static_cast<int32_t>(v["offset"]["y"].asInt())};
    sl.monitors = parseMonitors(v["monitors"]);
    sl.fingerprint = s.fingerprint;
    const json::Value& m = v["master"];
    sl.masterId = parseHexId(m["id"].asString());
    sl.masterName = m["name"].asString();
    sl.masterIp = parseIpOrZero(m["ip"].asString());
    sl.masterOnline = m["online"].asBool();
    sl.masterFingerprint = m["fingerprint"].asString();
  }
  return s;
}

// Polls the agent; the window is told when something changed.
void pollLoop() {
  while (g.polling) {
    const std::optional<std::string> reply = win::pipeRequest(g.pipe, "status", 1000);
    std::optional<json::Value> parsed = reply ? json::parse(*reply) : std::nullopt;
    const std::string raw = parsed ? *reply : std::string();
    bool changed = false;
    {
      std::lock_guard lock(g.statusMutex);
      if (raw != g.lastRaw || (parsed.has_value() != g.status.reachable)) {
        g.lastRaw = raw;
        g.status = parsed ? parseStatus(*parsed) : Status{};
        changed = true;
      }
    }
    if (changed && g.hwnd) PostMessageW(g.hwnd, WM_STATUS, 0, 0);
    for (int i = 0; i < 6 && g.polling; ++i) Sleep(50);
  }
}

// Sends a command to the agent; shows its error, if any. True on success.
bool command(HWND hwnd, const std::string& request) {
  const std::optional<std::string> reply = win::pipeRequest(g.pipe, request, 3000);
  if (!reply) {
    MessageBoxW(hwnd, L"The Net Mouse service is not running.", kAppTitle, MB_OK | MB_ICONWARNING);
    return false;
  }
  if (reply->rfind("error", 0) == 0) {
    MessageBoxW(hwnd, widen(reply->substr(std::min<size_t>(6, reply->size()))).c_str(), kAppTitle,
                MB_OK | MB_ICONWARNING);
    return false;
  }
  return true;
}

std::string chooseRole(HWND owner) {
  const TASKDIALOG_BUTTON buttons[] = {
      {1001, L"Master\nThis PC's mouse and keyboard control this PC and the slave PCs."},
      {1002, L"Slave\nThis PC is controlled by the master PC's mouse and keyboard."},
  };
  TASKDIALOGCONFIG c{};
  c.cbSize = sizeof(c);
  c.hwndParent = owner;
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

std::wstring statusText(const Status& st) {
  std::wstring t;
  if (!st.reachable) {
    return L"The Net Mouse service is not running on this PC.\n\n"
           L"Start it from the tray menu (“Start Net Mouse service”), or in Windows “Services”. "
           L"Mouse and keyboard sharing runs in that service; this window only shows and changes its settings.";
  }
  if (st.role.empty()) return L"No role chosen yet for this PC: choose one in the dialog, or from the tray menu.";
  if (st.role == "master") {
    const MasterSnapshot& s = st.master;
    t += std::format(L"MASTER • {} • fingerprint {} • group {}:{}\n", widen(st.name),
                     widen(st.fingerprint), widen(st.group), st.port);
    if (s.remote) {
      std::string target = "?";
      for (const SlaveView& v : s.slaves) {
        if (v.id == s.activeSlave) target = v.name;
      }
      t += std::format(L"Pointer and keyboard are on \"{}\"  (Ctrl+Alt+Home brings them back)\n", widen(target));
    } else {
      t += L"Pointer is on this PC\n";
    }
    // Only PCs that are on the network right now; offline ones are remembered but not shown.
    const bool anyOnline = std::any_of(s.slaves.begin(), s.slaves.end(), [](const SlaveView& v) { return v.online; });
    if (!anyOnline) t += L"No other PCs on the network right now.\n";
    for (const SlaveView& v : s.slaves) {
      if (!v.online) continue;
      std::wstring state;
      if (v.keyMismatch) {
        state = L"KEY CHANGED — not trusted";
      } else if (!v.paired) {
        state = v.otherMaster ? L"paired with another master" : L"available";
      } else if (!v.connected) {
        state = L"connecting…";
      } else {
        state = v.synced ? L"connected (encrypted)" : L"connected, syncing";
      }
      t += std::format(L"  • {}  {}  {}  •  fingerprint {}\n", widen(v.name), widen(ipToString(v.ip)), state,
                       widen(v.fingerprint));
    }
    t += L"Click an available PC to connect it, drag connected screens to where they are; right-click one to "
         L"disconnect.\n";
  } else {
    const SlaveSnapshot& s = st.slave;
    t += std::format(L"SLAVE • {} • fingerprint {}\n", widen(st.name), widen(st.fingerprint));
    if (!s.paired) {
      t += s.masterOnline
               ? std::format(L"Not connected. On the master \"{}\", click this PC (fingerprint {}) under Available.\n",
                             widen(s.masterName), widen(st.fingerprint))
               : std::wstring(L"Not connected. Looking for a master on the local network…\n");
    } else {
      t += std::format(L"Paired with master \"{}\" (fingerprint {}) • {}\n", widen(s.masterName),
                       widen(s.masterFingerprint),
                       !s.masterOnline ? L"offline" : (s.connected ? L"connected (encrypted)" : L"connecting…"));
      if (s.configured) {
        t += std::format(L"This desktop's origin is at ({}, {}) in the master layout\n", s.offset.x, s.offset.y);
      }
    }
    t += s.active ? L"The master's pointer and keyboard are on this PC\n" : L"Idle\n";
    for (size_t i = 0; i < s.monitors.size(); ++i) {
      const Rect& r = s.monitors[i].rect;
      t += std::format(L"  Monitor {}: {}×{} at ({}, {}){}\n", i + 1, r.width(), r.height(), r.left, r.top,
                       s.monitors[i].primary ? L" primary" : L"");
    }
  }
  t += L"Network: " + (st.filter.empty() ? std::wstring(L"all interfaces") : widen(joinInterfaceList(st.filter))) +
       L"  (tray menu → Network…)\n";
  t += L"\nRecent events:\n";
  const size_t from = st.log.size() > 6 ? st.log.size() - 6 : 0;
  for (size_t i = from; i < st.log.size(); ++i) t += widen(st.log[i]) + L"\n";
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
std::pair<bool, std::wstring> trayState(const Status& st) {
  if (!st.reachable) return {false, L"Net Mouse – service not running"};
  if (st.role.empty()) return {false, L"Net Mouse – no role chosen"};
  std::wstring tip = std::format(L"Net Mouse – {} ({})", widen(st.role), widen(st.name));
  bool active = false;
  if (st.role == "master") {
    const MasterSnapshot& s = st.master;
    active = s.remote;
    size_t online = 0;
    for (const SlaveView& v : s.slaves) {
      if (v.online && v.connected) ++online;
      if (s.remote && v.id == s.activeSlave) tip += L"\nPointer on " + widen(v.name);
    }
    if (!s.remote) tip += std::format(L"\n{} slave(s) connected", online);
  } else {
    const SlaveSnapshot& s = st.slave;
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

void setTrayIcon(DWORD action) {
  const auto [active, tip] = trayState(currentStatus());
  if (action == NIM_MODIFY && active == g.trayActive && tip == g.trayTip) return;
  g.trayActive = active;
  g.trayTip = tip;
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g.hwnd;
  nid.uID = kTrayId;
  nid.uFlags = NIF_ICON | NIF_TIP | (action == NIM_ADD ? NIF_MESSAGE : 0);
  nid.uCallbackMessage = WM_TRAY;
  nid.hIcon = active ? g.iconActive : g.iconIdle;
  wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
  Shell_NotifyIconW(action, &nid);
}

void showTrayBalloon(const wchar_t* title, const wchar_t* text) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g.hwnd;
  nid.uID = kTrayId;
  nid.uFlags = NIF_INFO;
  nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
  nid.hBalloonIcon = g.iconIdle;
  wcsncpy_s(nid.szInfoTitle, title, _TRUNCATE);
  wcsncpy_s(nid.szInfo, text, _TRUNCATE);
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
  const Status st = currentStatus();
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, IDM_SHOW, isMaster(st) ? L"Arrange displays…" : L"Status…");
  if (!st.reachable) {
    AppendMenuW(menu, MF_STRING, IDM_START_SERVICE, L"Start Net Mouse service");
  } else {
    if (isMaster(st)) AppendMenuW(menu, MF_STRING, IDM_RETURN, L"Return pointer to this PC\tCtrl+Alt+Home");
    if (st.role == "slave" && st.slave.paired) AppendMenuW(menu, MF_STRING, IDM_FORGET_MASTER, L"Forget master…");
    if (!st.role.empty()) AppendMenuW(menu, MF_STRING, IDM_NETWORK, L"Network…");
    AppendMenuW(menu, MF_STRING, IDM_CHANGE_ROLE, st.role.empty() ? L"Choose role…" : L"Change role…");
  }
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Close Net Mouse");
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
  const Status st = currentStatus();
  PAINTSTRUCT ps;
  HDC hdc = BeginPaint(hwnd, &ps);
  RECT client;
  GetClientRect(hwnd, &client);
  HDC dc = CreateCompatibleDC(hdc);
  HBITMAP bmp = CreateCompatibleBitmap(hdc, client.right, client.bottom);
  HGDIOBJ oldBmp = SelectObject(dc, bmp);
  FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));

  RECT textArea = client;
  if (isMaster(st)) {
    const RECT area = layoutArea(client);
    g.layoutView.paint(dc, area, st.master, g.font, g.dpi);
    textArea.top = area.bottom;
  }
  const int pad = MulDiv(10, static_cast<int>(g.dpi), 96);
  InflateRect(&textArea, -pad, -pad);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
  HGDIOBJ oldFont = SelectObject(dc, isMaster(st) ? g.font : g.bigFont);
  const std::wstring text = statusText(st);
  DrawTextW(dc, text.c_str(), -1, &textArea, DT_LEFT | DT_NOPREFIX | DT_EXPANDTABS | DT_WORDBREAK | DT_END_ELLIPSIS);
  SelectObject(dc, oldFont);

  BitBlt(hdc, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
  SelectObject(dc, oldBmp);
  DeleteObject(bmp);
  DeleteDC(dc);
  EndPaint(hwnd, &ps);
}

const SlaveView* findSlave(const MasterSnapshot& snap, uint64_t id) {
  for (const SlaveView& v : snap.slaves) {
    if (v.id == id) return &v;
  }
  return nullptr;
}

// Pairing: the user confirms the PC by its fingerprint, which the slave shows in its own window.
void connectSlave(HWND hwnd, uint64_t id) {
  const Status st = currentStatus();
  const SlaveView* v = findSlave(st.master, id);
  if (!v) return;
  if (v->otherMaster) {
    const std::wstring text = std::format(
        L"\"{}\" is paired with another master.\n\nOn that PC, choose \"Forget master\" in the Net Mouse tray menu "
        L"(or run: net-mouse-ctl forget-master), then connect it here.",
        widen(v->name));
    MessageBoxW(hwnd, text.c_str(), kAppTitle, MB_OK | MB_ICONINFORMATION);
    return;
  }
  const std::wstring warning =
      v->keyMismatch ? L"\n\nWARNING: this PC now has a different key than the one connected before. Only continue "
                       L"if it was reinstalled."
                     : L"";
  const std::wstring text = std::format(
      L"Connect \"{}\" ({})?\n\nFingerprint: {}\n\nCheck that the Net Mouse window on that PC shows the same "
      L"fingerprint. Once connected, this master controls it over an encrypted channel.{}",
      widen(v->name), widen(ipToString(v->ip)), widen(v->fingerprint), warning);
  if (MessageBoxW(hwnd, text.c_str(), kAppTitle, MB_OKCANCEL | (v->keyMismatch ? MB_ICONWARNING : MB_ICONQUESTION)) !=
      IDOK) {
    return;
  }
  const std::string hex = toHex(id);
  if (v->keyMismatch) command(hwnd, "unpair " + hex);
  command(hwnd, "pair " + hex);
}

void disconnectSlave(HWND hwnd, uint64_t id) {
  const Status st = currentStatus();
  const SlaveView* v = findSlave(st.master, id);
  if (!v) return;
  const std::wstring text =
      std::format(L"Disconnect \"{}\"?\n\nThis master stops controlling it; it goes back to Available.", widen(v->name));
  if (MessageBoxW(hwnd, text.c_str(), kAppTitle, MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
    command(hwnd, "unpair " + toHex(id));
  }
}

void askRole(HWND hwnd) {
  const std::string role = chooseRole(hwnd);
  if (!role.empty()) command(hwnd, "set-role " + role);  // the service restarts the agent with it
}

// Starting the service needs administrator rights: ask via UAC.
void startService(HWND hwnd) {
  ShellExecuteW(hwnd, L"runas", L"sc.exe", L"start NetMouse", nullptr, SW_HIDE);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (g.taskbarCreatedMsg && msg == g.taskbarCreatedMsg) {
    setTrayIcon(NIM_ADD);  // Explorer restarted
    return 0;
  }
  switch (msg) {
    case WM_CREATE:
      g.hwnd = hwnd;
      g.dpi = GetDpiForWindow(hwnd);
      updateFonts();
      setTrayIcon(NIM_ADD);
      if (g.showTrayHint) {
        showTrayBalloon(L"Net Mouse is running",
                        L"It runs in the notification area. Click the mouse icon for the settings.");
      }
      SetTimer(hwnd, kRefreshTimer, 1000, nullptr);
      return 0;
    case WM_STATUS: {
      setTrayIcon(NIM_MODIFY);
      if (IsWindowVisible(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
      const Status st = currentStatus();
      if (st.reachable && st.role.empty() && !g.roleAsked) {
        g.roleAsked = true;  // ask once per run; afterwards from the tray menu
        askRole(hwnd);
      }
      return 0;
    }
    case WM_TIMER:
      setTrayIcon(NIM_MODIFY);
      return 0;
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
    case WM_LBUTTONDOWN: {
      if (!isMaster(currentStatus())) return 0;
      const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      if (auto id = g.layoutView.availableAt(pt)) {
        connectSlave(hwnd, *id);
        return 0;
      }
      if (g.layoutView.beginDrag(pt)) {
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }
    case WM_MOUSEMOVE:
      if (g.layoutView.dragging()) {
        g.layoutView.dragTo({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
      if (g.layoutView.dragging()) {
        if (msg == WM_LBUTTONUP) g.layoutView.dragTo({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        if (auto drop = g.layoutView.endDrag()) {
          const auto reply = win::pipeRequest(g.pipe, std::format("set-offset {} {} {}", toHex(drop->slaveId),
                                                                 drop->offset.x, drop->offset.y));
          if (!reply || reply->rfind("ok", 0) != 0) MessageBeep(MB_ICONWARNING);
        }
        if (msg == WM_LBUTTONUP) ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_RBUTTONUP:
      if (isMaster(currentStatus())) {
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (auto id = g.layoutView.slaveAt(pt)) {
          g.menuSlave = *id;
          HMENU menu = CreatePopupMenu();
          AppendMenuW(menu, MF_STRING, IDM_DISCONNECT, L"Disconnect this PC…");
          ClientToScreen(hwnd, &pt);
          TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
          DestroyMenu(menu);
        }
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
          command(hwnd, "return");
          break;
        case IDM_DISCONNECT:
          disconnectSlave(hwnd, g.menuSlave);
          break;
        case IDM_NETWORK: {
          const Status st = currentStatus();
          if (auto selection = ui::showNetworkDialog(hwnd, st.choices, st.filter.empty(), g.font, g.dpi)) {
            command(hwnd, "set-interfaces " + (selection->empty() ? std::string("all") : joinInterfaceList(*selection)));
          }
          break;
        }
        case IDM_FORGET_MASTER:
          if (MessageBoxW(hwnd,
                          L"Forget the master this PC is paired with?\n\nIt will no longer control this PC until a "
                          L"master connects it again.",
                          kAppTitle, MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
            command(hwnd, "forget-master");
          }
          break;
        case IDM_CHANGE_ROLE:
          askRole(hwnd);
          break;
        case IDM_START_SERVICE:
          startService(hwnd);
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
      // The window's close button only hides it (like minimize); the app keeps working in the tray
      // and is closed from the tray icon's menu.
      ShowWindow(hwnd, SW_HIDE);
      if (!g.closeHintShown) {
        g.closeHintShown = true;
        showTrayBalloon(L"Net Mouse is still running",
                        L"To close it, right-click the mouse icon in the notification area and choose "
                        L"“Close Net Mouse”.");
      }
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
  wc.lpszClassName = kWindowClass;
  if (!RegisterClassExW(&wc)) return false;

  g.taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
  const UINT dpi = GetDpiForSystem();
  const int w = MulDiv(1040, static_cast<int>(dpi), 96);
  const int h = MulDiv(760, static_cast<int>(dpi), 96);
  const std::wstring title = std::format(L"{} – {}", kAppTitle, widen(win::computerName()));
  return CreateWindowExW(0, kWindowClass, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                         nullptr, nullptr, instance, nullptr) != nullptr;
}

// Posts "Exit" to every Net Mouse window in this session (also those of older versions).
int quitRunningInstances() {
  EnumWindows(
      [](HWND hwnd, LPARAM) -> BOOL {
        wchar_t cls[64];
        if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) && wcsncmp(cls, kWindowClassPrefix, wcslen(kWindowClassPrefix)) == 0 &&
            wcscmp(cls, L"NetMouseNetworkDialog") != 0) {
          PostMessageW(hwnd, WM_COMMAND, IDM_EXIT, 0);
        }
        return TRUE;
      },
      0);
  return 0;
}

int runControlApp(HINSTANCE instance) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);

  // One control app per session; starting it again opens the window of the running one.
  HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\NetMouseControl");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (instanceMutex) CloseHandle(instanceMutex);
    if (HWND existing = FindWindowW(kWindowClass, nullptr)) {
      AllowSetForegroundWindow(ASFW_ANY);
      PostMessageW(existing, WM_COMMAND, IDM_SHOW, 0);
    }
    return 0;
  }

  wchar_t appData[MAX_PATH];
  const DWORD n = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
  g.prefsPath = (n > 0 && n < MAX_PATH ? fs::path(appData) : fs::current_path()) / L"NetMouse" / L"control.ini";
  Config prefs;
  prefs.load(g.prefsPath);
  g.showTrayHint = prefs.getInt("tray_hint_shown", 0) == 0;
  prefs.setInt("tray_hint_shown", 1);
  prefs.save();

  loadIcons(instance);
  if (!createWindow(instance)) {
    CloseHandle(instanceMutex);
    MessageBoxW(nullptr, L"Could not create the main window.", kAppTitle, MB_OK | MB_ICONERROR);
    return 1;
  }
  g.poller = std::thread(pollLoop);
  if (g.show) showWindow();

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  g.polling = false;
  g.hwnd = nullptr;
  g.poller.join();
  CloseHandle(instanceMutex);
  if (g.font) DeleteObject(g.font);
  if (g.bigFont) DeleteObject(g.bigFont);
  return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  enum class Mode { Control, Agent, Service, Quit } mode = Mode::Control;
  win::AgentOptions agent;
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    const bool hasValue = i + 1 < argc;
    if (a == L"--service") {
      mode = Mode::Service;
    } else if (a == L"--agent") {
      mode = Mode::Agent;
    } else if (a == L"--quit") {
      mode = Mode::Quit;
    } else if (a == L"--show") {
      g.show = true;
    } else if (a == L"--dry-run") {
      agent.dryRun = true;
    } else if (a == L"--config" && hasValue) {
      agent.config = argv[++i];
    } else if (a == L"--pipe" && hasValue) {
      agent.pipe = argv[++i];
      g.pipe = agent.pipe;
    } else if (a == L"--quit-event" && hasValue) {
      agent.quitEvent = argv[++i];
    } else if (a == L"--role" && hasValue) {
      agent.role = narrow(argv[++i]);
    }
  }
  LocalFree(argv);
  switch (mode) {
    case Mode::Service:
      return win::runService();
    case Mode::Agent:
      return win::runAgent(agent);
    case Mode::Quit:
      return quitRunningInstances();
    case Mode::Control:
    default:
      return runControlApp(instance);
  }
}
