#pragma once

// Display-arrangement canvas for the master window: master monitors are fixed, slave screens can
// be dragged around them (with edge snapping), like the Windows "Display settings" page.

#include <windows.h>

#include <optional>
#include <utility>
#include <vector>

#include "app/master_engine.h"

namespace nm::ui {

class LayoutView {
 public:
  void paint(HDC dc, const RECT& area, const MasterSnapshot& snapshot, HFONT font, UINT dpi);

  bool beginDrag(POINT pt);
  void dragTo(POINT pt);
  struct Drop {
    uint64_t slaveId;
    Point offset;
  };
  std::optional<Drop> endDrag();
  bool dragging() const { return drag_.has_value(); }

  // A card in the "Available" strip (discovered PCs that are not connected yet).
  std::optional<uint64_t> availableAt(POINT pt) const;
  // A connected (paired) slave's screen.
  std::optional<uint64_t> slaveAt(POINT pt) const;

 private:
  void computeTransform();
  RECT toScreen(const Rect& r) const;
  POINT toScreen(Point p) const;
  Point offsetOf(const SlaveView& s) const;
  Point snap(const SlaveView& moving, Point offset) const;

  MasterSnapshot snap_;
  RECT area_{};
  UINT dpi_ = 96;
  double scale_ = 0.1;
  double originX_ = 0;
  double originY_ = 0;

  struct Drag {
    uint64_t id;
    POINT start;
    Point startOffset;
    Point offset;
  };
  std::optional<Drag> drag_;
  std::vector<std::pair<RECT, uint64_t>> availableCards_;  // from the last paint
};

}  // namespace nm::ui
