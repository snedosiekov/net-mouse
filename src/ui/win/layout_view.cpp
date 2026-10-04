#include "ui/win/layout_view.h"

#include <cmath>
#include <cstdlib>
#include <string>

namespace nm::ui {
namespace {

constexpr COLORREF kMasterFill = RGB(52, 101, 164);
constexpr COLORREF kSlaveOnlineFill = RGB(46, 139, 87);
constexpr COLORREF kSlaveOfflineFill = RGB(150, 150, 150);
constexpr COLORREF kSlaveDragFill = RGB(90, 180, 130);
constexpr COLORREF kBorder = RGB(30, 30, 30);
constexpr COLORREF kActiveBorder = RGB(255, 140, 0);
constexpr COLORREF kCanvas = RGB(236, 238, 242);

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

void drawScreen(HDC dc, RECT rc, COLORREF fill, COLORREF border, int borderWidth, const std::wstring& label) {
  HBRUSH brush = CreateSolidBrush(fill);
  FillRect(dc, &rc, brush);
  DeleteObject(brush);

  HPEN pen = CreatePen(PS_INSIDEFRAME, borderWidth, border);
  HGDIOBJ oldPen = SelectObject(dc, pen);
  HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
  Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
  SelectObject(dc, oldBrush);
  SelectObject(dc, oldPen);
  DeleteObject(pen);

  RECT text = rc;
  InflateRect(&text, -4, -4);
  RECT measure = text;
  DrawTextW(dc, label.c_str(), -1, &measure, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
  const int free = (text.bottom - text.top) - (measure.bottom - measure.top);
  if (free > 0) text.top += free / 2;
  SetTextColor(dc, RGB(255, 255, 255));
  DrawTextW(dc, label.c_str(), -1, &text, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
}

}  // namespace

void LayoutView::paint(HDC dc, const RECT& area, const MasterSnapshot& snapshot, HFONT font, UINT dpi) {
  snap_ = snapshot;
  area_ = area;
  dpi_ = dpi;
  if (!drag_) computeTransform();

  HBRUSH bg = CreateSolidBrush(kCanvas);
  FillRect(dc, &area, bg);
  DeleteObject(bg);

  HGDIOBJ oldFont = SelectObject(dc, font);
  SetBkMode(dc, TRANSPARENT);
  const int border = std::max(1, MulDiv(2, static_cast<int>(dpi), 96));

  for (size_t i = 0; i < snap_.masterMonitors.size(); ++i) {
    const MonitorInfo& m = snap_.masterMonitors[i];
    std::wstring label = L"MASTER " + std::to_wstring(i + 1) + (m.primary ? L" (primary)" : L"") + L"\n" +
                         std::to_wstring(m.rect.width()) + L"×" + std::to_wstring(m.rect.height());
    drawScreen(dc, toScreen(m.rect), kMasterFill, kBorder, border, label);
  }

  for (const SlaveView& s : snap_.slaves) {
    const bool isDragged = drag_ && drag_->id == s.id;
    if (!s.online && !isDragged) continue;
    const Point off = offsetOf(s);
    const bool active = snap_.remote && snap_.activeSlave == s.id;
    for (size_t i = 0; i < s.monitors.size(); ++i) {
      const Rect r = s.monitors[i].translated(off);
      std::wstring label = widen(s.name);
      if (s.monitors.size() > 1) label += L" #" + std::to_wstring(i + 1);
      label += L"\n" + std::to_wstring(r.width()) + L"×" + std::to_wstring(r.height());
      if (!s.synced) label += L"\n(syncing…)";
      const COLORREF fill = isDragged ? kSlaveDragFill : (s.online ? kSlaveOnlineFill : kSlaveOfflineFill);
      drawScreen(dc, toScreen(r), fill, active ? kActiveBorder : kBorder, active ? border * 2 : border, label);
    }
  }

  if (snap_.remote) {
    const POINT p = toScreen(snap_.virtualPos);
    const int rad = MulDiv(5, static_cast<int>(dpi), 96);
    HBRUSH dot = CreateSolidBrush(kActiveBorder);
    HGDIOBJ oldBrush = SelectObject(dc, dot);
    Ellipse(dc, p.x - rad, p.y - rad, p.x + rad, p.y + rad);
    SelectObject(dc, oldBrush);
    DeleteObject(dot);
  }

  if (snap_.slaves.empty()) {
    RECT hint = area;
    hint.top = hint.bottom - MulDiv(40, static_cast<int>(dpi), 96);
    SetTextColor(dc, RGB(90, 90, 90));
    DrawTextW(dc, L"Waiting for slaves… Start net-mouse on another PC and choose \"Slave\".", -1, &hint,
              DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
  }
  SelectObject(dc, oldFont);
}

void LayoutView::computeTransform() {
  std::vector<Rect> all;
  for (const MonitorInfo& m : snap_.masterMonitors) all.push_back(m.rect);
  for (const SlaveView& s : snap_.slaves) {
    if (!s.online) continue;
    for (const Rect& r : s.monitors) all.push_back(r.translated(s.offset));
  }
  const double aw = std::max<LONG>(1, area_.right - area_.left);
  const double ah = std::max<LONG>(1, area_.bottom - area_.top);
  if (all.empty()) {
    scale_ = 0.1;
    originX_ = originY_ = 0;
    return;
  }
  const Rect b = boundingRect(all);
  // Leave room around the arrangement so slaves can be dragged to any side.
  const double pad = std::max(b.width(), b.height()) * 0.35;
  scale_ = std::min(aw / (b.width() + 2 * pad), ah / (b.height() + 2 * pad));
  originX_ = b.left + b.width() / 2.0 - aw / 2.0 / scale_;
  originY_ = b.top + b.height() / 2.0 - ah / 2.0 / scale_;
}

POINT LayoutView::toScreen(Point p) const {
  return {area_.left + static_cast<LONG>(std::lround((p.x - originX_) * scale_)),
          area_.top + static_cast<LONG>(std::lround((p.y - originY_) * scale_))};
}

RECT LayoutView::toScreen(const Rect& r) const {
  const POINT a = toScreen(Point{r.left, r.top});
  const POINT b = toScreen(Point{r.right, r.bottom});
  return {a.x, a.y, b.x, b.y};
}

Point LayoutView::offsetOf(const SlaveView& s) const {
  return drag_ && drag_->id == s.id ? drag_->offset : s.offset;
}

bool LayoutView::beginDrag(POINT pt) {
  for (auto it = snap_.slaves.rbegin(); it != snap_.slaves.rend(); ++it) {
    if (!it->online) continue;
    for (const Rect& r : it->monitors) {
      const RECT rc = toScreen(r.translated(it->offset));
      if (PtInRect(&rc, pt)) {
        drag_ = Drag{it->id, pt, it->offset, it->offset};
        return true;
      }
    }
  }
  return false;
}

void LayoutView::dragTo(POINT pt) {
  if (!drag_) return;
  for (const SlaveView& s : snap_.slaves) {
    if (s.id != drag_->id) continue;
    const Point raw{drag_->startOffset.x + static_cast<int32_t>(std::lround((pt.x - drag_->start.x) / scale_)),
                    drag_->startOffset.y + static_cast<int32_t>(std::lround((pt.y - drag_->start.y) / scale_))};
    drag_->offset = snap(s, raw);
  }
}

std::optional<LayoutView::Drop> LayoutView::endDrag() {
  if (!drag_) return std::nullopt;
  Drop drop{drag_->id, drag_->offset};
  const bool moved = !(drag_->offset == drag_->startOffset);
  drag_.reset();
  if (!moved) return std::nullopt;
  return drop;
}

// Snaps the dragged slave so its edges line up with nearby edges of other screens; that is what
// makes screens touch, and touching edges are where the pointer can cross.
Point LayoutView::snap(const SlaveView& moving, Point offset) const {
  std::vector<Rect> others;
  for (const MonitorInfo& m : snap_.masterMonitors) others.push_back(m.rect);
  for (const SlaveView& s : snap_.slaves) {
    if (s.id == moving.id || !s.online) continue;
    for (const Rect& r : s.monitors) others.push_back(r.translated(s.offset));
  }
  const double threshold = MulDiv(12, static_cast<int>(dpi_), 96) / scale_;
  double bestX = threshold + 1, bestY = threshold + 1;
  int32_t dx = 0, dy = 0;
  for (const Rect& local : moving.monitors) {
    const Rect m = local.translated(offset);
    for (const Rect& o : others) {
      for (int32_t d : {o.right - m.left, o.left - m.right, o.left - m.left, o.right - m.right}) {
        if (std::abs(d) < bestX) {
          bestX = std::abs(d);
          dx = d;
        }
      }
      for (int32_t d : {o.bottom - m.top, o.top - m.bottom, o.top - m.top, o.bottom - m.bottom}) {
        if (std::abs(d) < bestY) {
          bestY = std::abs(d);
          dy = d;
        }
      }
    }
  }
  if (bestX <= threshold) offset.x += dx;
  if (bestY <= threshold) offset.y += dy;
  return offset;
}

}  // namespace nm::ui
