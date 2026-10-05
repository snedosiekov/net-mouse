#include "ui/win/layout_view.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

// GDI+ uses the min/max macros that NOMINMAX removes.
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <objidl.h>
#include <gdiplus.h>

#include "core/udp.h"

namespace nm::ui {
namespace {

namespace gp = Gdiplus;

struct Palette {
  gp::Color top;
  gp::Color bottom;
};

const Palette kMaster{gp::Color(255, 78, 140, 232), gp::Color(255, 38, 84, 168)};
const Palette kSlave{gp::Color(255, 66, 196, 136), gp::Color(255, 26, 124, 84)};
const Palette kSlaveDragging{gp::Color(255, 118, 220, 166), gp::Color(255, 50, 160, 110)};
const Palette kSlaveOffline{gp::Color(255, 168, 174, 184), gp::Color(255, 118, 124, 134)};
const Palette kAvailable{gp::Color(255, 132, 146, 168), gp::Color(255, 96, 108, 128)};
const Palette kWarning{gp::Color(255, 226, 96, 88), gp::Color(255, 168, 48, 44)};

// Slaves shown in the arrangement: connected ones (or being connected) with a trusted key.
bool inLayout(const SlaveView& s) { return s.paired && !s.keyMismatch; }
// Slaves shown as cards in the "Available" strip.
bool isAvailable(const SlaveView& s) { return s.online && !inLayout(s); }
const gp::Color kBezel(255, 34, 38, 46);
const gp::Color kActive(255, 255, 140, 0);
const gp::Color kCanvasTop(255, 246, 248, 251);
const gp::Color kCanvasBottom(255, 226, 231, 239);

void ensureGdiplus() {
  static ULONG_PTR token = [] {
    ULONG_PTR t = 0;
    gp::GdiplusStartupInput input;
    gp::GdiplusStartup(&t, &input, nullptr);
    return t;
  }();
  (void)token;  // GDI+ stays up for the lifetime of the process
}

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::wstring resolution(const Rect& r) {
  return std::to_wstring(r.width()) + L" × " + std::to_wstring(r.height());
}

void addRoundRect(gp::GraphicsPath& path, const gp::RectF& r, float radius) {
  const float d = std::min({radius * 2, r.Width, r.Height});
  if (d <= 1) {
    path.AddRectangle(r);
    return;
  }
  path.AddArc(r.X, r.Y, d, d, 180, 90);
  path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
  path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
  path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
  path.CloseFigure();
}

void fillRoundRect(gp::Graphics& g, const gp::Brush& brush, const gp::RectF& r, float radius) {
  gp::GraphicsPath path;
  addRoundRect(path, r, radius);
  g.FillPath(&brush, &path);
}

gp::RectF inflated(gp::RectF r, float by) {
  r.Inflate(by, by);
  return r;
}

struct Fonts {
  gp::Font* title;
  gp::Font* body;
};

// A monitor: soft shadow, dark bezel, gradient "screen", text lines (most important first; lines
// that do not fit are dropped, lines that are too wide end in an ellipsis).
void drawMonitor(gp::Graphics& g, gp::RectF r, const Palette& palette, bool glow,
                 const std::vector<std::wstring>& lines, const Fonts& fonts, float s) {
  r.Inflate(-1.5f * s, -1.5f * s);  // a small gap keeps touching monitors distinguishable
  if (r.Width < 4 || r.Height < 4) return;
  const float radius = std::clamp(std::min(r.Width, r.Height) * 0.08f, 3 * s, 10 * s);

  for (int i = 3; i >= 1; --i) {  // shadow
    gp::SolidBrush shade(gp::Color(static_cast<BYTE>(18 * (4 - i)), 20, 30, 50));
    gp::RectF sr = inflated(r, i * 1.2f * s);
    sr.Offset(0, 2.5f * s);
    fillRoundRect(g, shade, sr, radius + i * s);
  }
  if (glow) {
    for (int i = 4; i >= 1; --i) {
      gp::SolidBrush halo(gp::Color(static_cast<BYTE>(45 * (5 - i)), kActive.GetR(), kActive.GetG(), kActive.GetB()));
      fillRoundRect(g, halo, inflated(r, i * 1.6f * s), radius + i * s);
    }
  }
  gp::SolidBrush bezel(kBezel);
  fillRoundRect(g, bezel, r, radius);

  const float bezelWidth = std::max(2.0f, 3 * s);
  const gp::RectF screen = inflated(r, -bezelWidth);
  if (screen.Width < 2 || screen.Height < 2) return;
  const float innerRadius = std::max(1.0f, radius - bezelWidth * 0.6f);
  gp::LinearGradientBrush fill(screen, palette.top, palette.bottom, gp::LinearGradientModeVertical);
  fillRoundRect(g, fill, screen, innerRadius);
  gp::RectF gloss = screen;  // subtle glass highlight on the upper part
  gloss.Height *= 0.45f;
  gp::LinearGradientBrush shine(gloss, gp::Color(46, 255, 255, 255), gp::Color(0, 255, 255, 255),
                                gp::LinearGradientModeVertical);
  fillRoundRect(g, shine, gloss, innerRadius);

  // Text block, vertically centered.
  gp::StringFormat format;
  format.SetAlignment(gp::StringAlignmentCenter);
  format.SetLineAlignment(gp::StringAlignmentCenter);
  format.SetTrimming(gp::StringTrimmingEllipsisCharacter);
  format.SetFormatFlags(gp::StringFormatFlagsNoWrap);
  const float titleHeight = fonts.title->GetHeight(&g);
  const float bodyHeight = fonts.body->GetHeight(&g);
  const gp::RectF text = inflated(screen, -3 * s);
  float needed = 0;
  size_t count = 0;
  for (size_t i = 0; i < lines.size(); ++i) {
    const float h = i == 0 ? titleHeight : bodyHeight;
    if (needed + h > text.Height && count > 0) break;
    needed += h;
    ++count;
  }
  float y = text.Y + (text.Height - needed) / 2;
  gp::SolidBrush titleBrush(gp::Color(255, 255, 255, 255));
  gp::SolidBrush bodyBrush(gp::Color(215, 255, 255, 255));
  for (size_t i = 0; i < count; ++i) {
    const bool title = i == 0;
    const float h = title ? titleHeight : bodyHeight;
    const gp::RectF line(text.X, y, text.Width, h);
    g.DrawString(lines[i].c_str(), -1, title ? fonts.title : fonts.body, line, &format,
                 title ? &titleBrush : &bodyBrush);
    y += h;
  }
}

}  // namespace

void LayoutView::paint(HDC dc, const RECT& area, const MasterSnapshot& snapshot, HFONT font, UINT dpi) {
  ensureGdiplus();
  snap_ = snapshot;
  dpi_ = dpi;
  const float s = static_cast<float>(dpi) / 96.0f;
  // Discovered PCs that are not connected get a strip of cards at the bottom.
  std::vector<const SlaveView*> available;
  for (const SlaveView& sv : snap_.slaves) {
    if (isAvailable(sv)) available.push_back(&sv);
  }
  const int stripHeight = available.empty() ? 0 : MulDiv(108, static_cast<int>(dpi), 96);
  area_ = area;
  area_.bottom = std::max(area_.top + 1, area_.bottom - stripHeight);
  if (!drag_) computeTransform();

  gp::Graphics g(dc);
  g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
  g.SetTextRenderingHint(gp::TextRenderingHintClearTypeGridFit);
  g.SetClip(gp::Rect(area.left, area.top, area.right - area.left, area.bottom - area.top));

  // Background: soft vertical gradient with a faint dot grid.
  const gp::RectF bg(static_cast<float>(area.left), static_cast<float>(area.top),
                     static_cast<float>(area.right - area.left), static_cast<float>(area.bottom - area.top));
  gp::LinearGradientBrush bgBrush(inflated(bg, 1), kCanvasTop, kCanvasBottom, gp::LinearGradientModeVertical);
  g.FillRectangle(&bgBrush, bg);
  gp::SolidBrush dot(gp::Color(34, 40, 60, 90));
  const float step = 22 * s;
  for (float y = bg.Y + step / 2; y < bg.Y + bg.Height; y += step) {
    for (float x = bg.X + step / 2; x < bg.X + bg.Width; x += step) g.FillEllipse(&dot, x - s, y - s, 2 * s, 2 * s);
  }

  LOGFONTW lf{};
  GetObjectW(font, sizeof(lf), &lf);
  gp::Font body(dc, &lf);
  lf.lfWeight = FW_BOLD;
  lf.lfHeight = MulDiv(lf.lfHeight, 115, 100);
  gp::Font title(dc, &lf);
  const Fonts fonts{&title, &body};

  auto toRectF = [this](const Rect& r) {
    const RECT rc = toScreen(r);
    return gp::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top), static_cast<float>(rc.right - rc.left),
                     static_cast<float>(rc.bottom - rc.top));
  };

  for (size_t i = 0; i < snap_.masterMonitors.size(); ++i) {
    const MonitorInfo& m = snap_.masterMonitors[i];
    std::vector<std::wstring> lines{L"MASTER " + std::to_wstring(i + 1), resolution(m.rect)};
    if (m.primary) lines.push_back(L"primary");
    drawMonitor(g, toRectF(m.rect), kMaster, false, lines, fonts, s);
  }

  for (const SlaveView& sv : snap_.slaves) {
    const bool isDragged = drag_ && drag_->id == sv.id;
    if (!inLayout(sv) || (!sv.online && !isDragged)) continue;
    const Point off = offsetOf(sv);
    const bool active = snap_.remote && snap_.activeSlave == sv.id;
    const Palette& palette = isDragged ? kSlaveDragging : (sv.connected ? kSlave : kSlaveOffline);
    for (size_t i = 0; i < sv.monitors.size(); ++i) {
      const Rect r = sv.monitors[i].translated(off);
      std::vector<std::wstring> lines{widen(sv.name) + (sv.monitors.size() > 1 ? L"  #" + std::to_wstring(i + 1) : L"")};
      if (sv.ip) lines.push_back(widen(ipToString(sv.ip)));
      lines.push_back(resolution(r));
      if (!sv.connected) {
        lines.push_back(L"connecting…");
      } else if (!sv.synced) {
        lines.push_back(L"syncing…");
      }
      drawMonitor(g, toRectF(r), palette, active, lines, fonts, s);
    }
  }

  // "Available" strip: one card per discovered PC that is not connected.
  availableCards_.clear();
  if (!available.empty()) {
    const float top = static_cast<float>(area_.bottom);
    gp::SolidBrush sep(gp::Color(40, 40, 60, 90));
    g.FillRectangle(&sep, bg.X + 12 * s, top, bg.Width - 24 * s, 1 * s);
    gp::SolidBrush caption(gp::Color(255, 70, 80, 96));
    g.DrawString(L"Available — click a PC to connect it", -1, &body,
                 gp::PointF(bg.X + 16 * s, top + 6 * s), &caption);
    const float cardW = 230 * s, cardH = 70 * s, gap = 12 * s;
    float x = bg.X + 16 * s;
    const float y = top + 28 * s;
    for (const SlaveView* sv : available) {
      if (x + cardW > bg.X + bg.Width) break;
      std::vector<std::wstring> lines{widen(sv->name), widen(ipToString(sv->ip)), widen(sv->fingerprint)};
      const Palette* palette = &kAvailable;
      if (sv->keyMismatch) {
        lines.push_back(L"key changed — click to trust it");
        palette = &kWarning;
      } else if (sv->otherMaster) {
        lines.push_back(L"paired with another master");
        palette = &kSlaveOffline;
      }
      drawMonitor(g, gp::RectF(x, y, cardW, cardH), *palette, false, lines, fonts, s);
      availableCards_.push_back({RECT{static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + cardW),
                                      static_cast<LONG>(y + cardH)},
                                 sv->id});
      x += cardW + gap;
    }
  }

  if (snap_.remote) {  // where the pointer is right now
    const POINT p = toScreen(snap_.virtualPos);
    const float rad = 6 * s;
    gp::SolidBrush ring(gp::Color(255, 255, 255, 255));
    gp::SolidBrush core(kActive);
    g.FillEllipse(&ring, p.x - rad - 2 * s, p.y - rad - 2 * s, 2 * (rad + 2 * s), 2 * (rad + 2 * s));
    g.FillEllipse(&core, p.x - rad, p.y - rad, 2 * rad, 2 * rad);
  }

  if (std::none_of(snap_.slaves.begin(), snap_.slaves.end(), [](const SlaveView& v) { return v.online; })) {
    gp::StringFormat format;
    format.SetAlignment(gp::StringAlignmentCenter);
    gp::SolidBrush hintBrush(gp::Color(255, 90, 98, 110));
    const gp::RectF hint(bg.X, bg.Y + bg.Height - 36 * s, bg.Width, 24 * s);
    g.DrawString(L"Waiting for slaves… Start Net Mouse on another PC and choose “Slave”.", -1, &body,
                 hint, &format, &hintBrush);
  }
}

void LayoutView::computeTransform() {
  std::vector<Rect> all;
  for (const MonitorInfo& m : snap_.masterMonitors) all.push_back(m.rect);
  for (const SlaveView& s : snap_.slaves) {
    if (!s.online || !inLayout(s)) continue;
    for (const Rect& r : s.monitors) all.push_back(r.translated(s.offset));
  }
  const double aw = std::max<LONG>(1, area_.right - area_.left);
  const double ah = std::max<LONG>(1, area_.bottom - area_.top);
  if (all.empty()) {
    scale_ = 0.1;
    originX_ = originY_ = 0;
    return;
  }
  // Fill the canvas, keeping a margin for shadows and for dragging a slave past the outer edges.
  const Rect b = boundingRect(all);
  const double margin = MulDiv(36, static_cast<int>(dpi_), 96);
  scale_ = std::min((aw - 2 * margin) / b.width(), (ah - 2 * margin) / b.height());
  scale_ = std::clamp(scale_, 0.01, 0.25 * dpi_ / 96.0);  // keep a single small screen from filling the view
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

std::optional<uint64_t> LayoutView::availableAt(POINT pt) const {
  for (const auto& [rc, id] : availableCards_) {
    if (PtInRect(&rc, pt)) return id;
  }
  return std::nullopt;
}

std::optional<uint64_t> LayoutView::slaveAt(POINT pt) const {
  for (auto it = snap_.slaves.rbegin(); it != snap_.slaves.rend(); ++it) {
    if (!it->online || !inLayout(*it)) continue;
    for (const Rect& r : it->monitors) {
      const RECT rc = toScreen(r.translated(it->offset));
      if (PtInRect(&rc, pt)) return it->id;
    }
  }
  return std::nullopt;
}

bool LayoutView::beginDrag(POINT pt) {
  for (auto it = snap_.slaves.rbegin(); it != snap_.slaves.rend(); ++it) {
    if (!it->online || !inLayout(*it)) continue;
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
    if (s.id == moving.id || !s.online || !inLayout(s)) continue;
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
