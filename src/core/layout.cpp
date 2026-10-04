#include "core/layout.h"

#include <climits>

namespace nm::layout {

const Screen* screenAt(const std::vector<Screen>& screens, Point p) {
  for (const Screen& s : screens) {
    if (s.rect.contains(p)) return &s;
  }
  return nullptr;
}

Point movePointer(const std::vector<Screen>& screens, Point from, int dx, int dy) {
  Point p = from;
  auto step = [&](bool horizontal, int d) {
    if (d == 0) return;
    const Point candidate = horizontal ? Point{p.x + d, p.y} : Point{p.x, p.y + d};
    if (screenAt(screens, candidate)) {
      p = candidate;
    } else if (const Screen* current = screenAt(screens, p)) {
      p = current->rect.clamp(candidate);
    }
  };
  step(true, dx);
  step(false, dy);
  return p;
}

std::optional<Point> findEdgeCrossing(const std::vector<Screen>& screens, Point pt) {
  static constexpr Point kDirs[] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
  for (const Point d : kDirs) {
    const Point q{pt.x + d.x, pt.y + d.y};
    const Screen* s = screenAt(screens, q);
    if (s && s->owner != kMasterOwner) return q;
  }
  return std::nullopt;
}

Point autoPlace(const std::vector<Rect>& occupied, const Rect& masterPrimary,
                const Rect& slaveLocalBounds) {
  int32_t maxRight = masterPrimary.right;
  for (const Rect& r : occupied) maxRight = std::max(maxRight, r.right);
  return {maxRight - slaveLocalBounds.left, masterPrimary.top - slaveLocalBounds.top};
}

bool overlapsAny(const std::vector<Rect>& a, const std::vector<Rect>& b) {
  for (const Rect& x : a) {
    for (const Rect& y : b) {
      if (x.intersects(y)) return true;
    }
  }
  return false;
}

}  // namespace nm::layout
