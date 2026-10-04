#pragma once

// Geometry of the master's "layout space": the master's own virtual desktop coordinates, with every
// slave's monitors placed in it at a user-chosen offset (like the Windows display arrangement).

#include <cstdint>
#include <optional>
#include <vector>

#include "core/geometry.h"

namespace nm::layout {

constexpr uint64_t kMasterOwner = 0;

struct Screen {
  Rect rect;       // in layout space
  uint64_t owner;  // kMasterOwner or a slave id
};

// First screen containing p (master screens are listed first, so they win on overlap).
const Screen* screenAt(const std::vector<Screen>& screens, Point p);

// Moves a pointer by (dx, dy) across the union of screens. Each axis is applied separately; a
// step that would leave every screen is clamped to the edge of the current one, so the pointer
// slides along edges and only crosses where screens touch.
Point movePointer(const std::vector<Screen>& screens, Point from, int dx, int dy);

// If pt (on a master screen) sits on an edge pixel and the neighbouring pixel belongs to a slave,
// returns that neighbouring pixel.
std::optional<Point> findEdgeCrossing(const std::vector<Screen>& screens, Point pt);

// Offset for a newly discovered slave: to the right of everything already placed, top-aligned
// with the master's primary monitor.
Point autoPlace(const std::vector<Rect>& occupied, const Rect& masterPrimary,
                const Rect& slaveLocalBounds);

bool overlapsAny(const std::vector<Rect>& a, const std::vector<Rect>& b);

}  // namespace nm::layout
