/** @fileoverview Shared card tilt for the recents shelf and pinned captures. */
#pragma once

#include <QtTypes>

/// Keep the front card straight and alternate a growing three-degree lean
/// behind it. Fan progress runs from a folded deck (0) to upright cards (1).
[[nodiscard]] constexpr qreal stackCardTilt(int depth, qreal fan = 0.0) {
  if (depth <= 0)
    return 0.0;
  return (depth % 2 == 0 ? 1.0 : -1.0) * depth * 3.0 * (1.0 - fan);
}
