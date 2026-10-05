#include "ui/win/network_dialog.h"

namespace nm::ui {
namespace {

constexpr wchar_t kClass[] = L"NetMouseNetworkDialog";
constexpr int kIdAll = 100;
constexpr int kIdFirst = 200;

struct State {
  std::vector<UdpMulticast::Choice> choices;
  std::vector<HWND> boxes;
  HWND all = nullptr;
  bool done = false;
  bool accepted = false;
  std::vector<std::string> result;
};

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

void updateEnabled(State& st) {
  const bool all = SendMessageW(st.all, BM_GETCHECK, 0, 0) == BST_CHECKED;
  for (HWND box : st.boxes) EnableWindow(box, !all);
}

LRESULT CALLBACK dialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  auto* st = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_COMMAND:
      if (!st) break;
      if (LOWORD(wParam) == kIdAll) {
        updateEnabled(*st);
      } else if (LOWORD(wParam) == IDOK) {
        const bool all = SendMessageW(st->all, BM_GETCHECK, 0, 0) == BST_CHECKED;
        st->result.clear();
        for (size_t i = 0; i < st->boxes.size() && !all; ++i) {
          if (SendMessageW(st->boxes[i], BM_GETCHECK, 0, 0) == BST_CHECKED) {
            st->result.push_back(st->choices[i].iface.name);
          }
        }
        if (!all && st->result.empty()) {
          MessageBoxW(hwnd, L"Select at least one interface, or “All interfaces”.", L"Net Mouse",
                      MB_OK | MB_ICONWARNING);
          return 0;
        }
        st->accepted = true;
        st->done = true;
      } else if (LOWORD(wParam) == IDCANCEL) {
        st->done = true;
      }
      return 0;
    case WM_CLOSE:
      if (st) st->done = true;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

std::optional<std::vector<std::string>> showNetworkDialog(HWND owner, const std::vector<UdpMulticast::Choice>& choices,
                                                          bool allSelected, HFONT font, UINT dpi) {
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = dialogProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);  // fails harmlessly when already registered

  auto px = [dpi](int v) { return MulDiv(v, static_cast<int>(dpi), 96); };
  const int width = px(480);
  const int row = px(26);
  const int pad = px(14);
  const int intro = px(56);
  const int height = pad + intro + row * static_cast<int>(choices.size() + 1) + px(16) + px(30) + pad;

  RECT ownerRect;
  GetWindowRect(owner, &ownerRect);
  RECT frame{0, 0, width, height};
  AdjustWindowRectExForDpi(&frame, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, dpi);
  const int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
  const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - fw) / 2;
  const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - fh) / 2;
  HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, L"Net Mouse – Network interfaces",
                              WS_CAPTION | WS_SYSMENU | WS_POPUP, x, y, fw, fh, owner, nullptr, instance, nullptr);
  if (!hwnd) return std::nullopt;

  State st;
  st.choices = choices;
  SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
  auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int cx, int cy, int cw, int ch, int id) {
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, cx, cy, cw, ch, hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return h;
  };

  child(L"STATIC",
        L"Net Mouse finds other PCs and talks to them only on the selected interfaces. Leave out tunnels or "
        L"networks the other PCs should not be reached through.",
        0, pad, pad, width - 2 * pad, intro - px(6), 0);
  int cy = pad + intro;
  st.all = child(L"BUTTON", L"All interfaces (default)", BS_AUTOCHECKBOX | WS_TABSTOP, pad, cy, width - 2 * pad, row,
                 kIdAll);
  SendMessageW(st.all, BM_SETCHECK, allSelected ? BST_CHECKED : BST_UNCHECKED, 0);
  cy += row;
  for (size_t i = 0; i < choices.size(); ++i) {
    const auto& c = choices[i];
    const std::wstring label = widen(c.iface.name) + L"   —   " +
                               (c.iface.addr ? widen(ipToString(c.iface.addr)) : std::wstring(L"not connected"));
    HWND box = child(L"BUTTON", label.c_str(), BS_AUTOCHECKBOX | WS_TABSTOP, pad + px(20), cy,
                     width - 2 * pad - px(20), row, kIdFirst + static_cast<int>(i));
    SendMessageW(box, BM_SETCHECK, (allSelected || c.selected) ? BST_CHECKED : BST_UNCHECKED, 0);
    st.boxes.push_back(box);
    cy += row;
  }
  cy += px(16);
  child(L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, width - pad - px(180), cy, px(85), px(28), IDOK);
  child(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, width - pad - px(85), cy, px(85), px(28), IDCANCEL);
  updateEnabled(st);

  EnableWindow(owner, FALSE);
  ShowWindow(hwnd, SW_SHOW);
  SetForegroundWindow(hwnd);
  MSG msg;
  while (!st.done) {
    const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
    if (got <= 0) {
      if (got == 0) PostQuitMessage(static_cast<int>(msg.wParam));  // let the main loop see the quit too
      break;
    }
    if (!IsDialogMessageW(hwnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  EnableWindow(owner, TRUE);
  SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
  DestroyWindow(hwnd);
  SetForegroundWindow(owner);
  if (!st.accepted) return std::nullopt;
  return st.result;
}

}  // namespace nm::ui
