#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace nm {

struct Point {
  int32_t x = 0;
  int32_t y = 0;
  friend bool operator==(const Point&, const Point&) = default;
};

// Axis-aligned rectangle; right and bottom are exclusive (same convention as Win32 RECT).
struct Rect {
  int32_t left = 0;
  int32_t top = 0;
  int32_t right = 0;
  int32_t bottom = 0;

  int32_t width() const { return right - left; }
  int32_t height() const { return bottom - top; }
  bool empty() const { return width() <= 0 || height() <= 0; }
  bool contains(Point p) const { return p.x >= left && p.x < right && p.y >= top && p.y < bottom; }
  bool intersects(const Rect& o) const {
    return left < o.right && o.left < right && top < o.bottom && o.top < bottom;
  }
  Rect translated(Point d) const { return {left + d.x, top + d.y, right + d.x, bottom + d.y}; }
  Point clamp(Point p) const {
    return {std::clamp(p.x, left, right - 1), std::clamp(p.y, top, bottom - 1)};
  }
  Point center() const { return {left + width() / 2, top + height() / 2}; }
  friend bool operator==(const Rect&, const Rect&) = default;
};

inline Rect boundingRect(const std::vector<Rect>& rects) {
  if (rects.empty()) return {};
  Rect b = rects.front();
  for (const Rect& r : rects) {
    b.left = std::min(b.left, r.left);
    b.top = std::min(b.top, r.top);
    b.right = std::max(b.right, r.right);
    b.bottom = std::max(b.bottom, r.bottom);
  }
  return b;
}

}  // namespace nm
