#include "text-band.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

#include <QImage>
#include <QPointF>
#include <QSizeF>
#include <QtCore/qnumeric.h>
#include <QtCore/qtypes.h>
#include <QtGui/qrgb.h>

namespace {
constexpr qreal kHorizontalRadius = 96.0;
constexpr qreal kSnapDistance = 20.0;
constexpr qreal kMaximumTextHeight = 64.0;
constexpr qreal kMinimumTextHeight = 5.0;
// Bowl-shaped glyphs can have quiet antialiased rows between edge runs. Merge
// gaps up to six logical pixels so one text line stays one band.
constexpr qreal kVerticalMergeGap = 6.0;
constexpr qreal kEdgeDensity = 0.035;

int scaledPixels(qreal logicalPixels, qreal scale) {
  return std::max(1, qRound(logicalPixels * std::max<qreal>(scale, 0.01)));
}

int colorDistance(QRgb first, QRgb second) {
  // Luminance-only edges miss colored text whose perceived brightness is
  // close to its background. The strongest RGB channel remains cheap and
  // catches those glyph edges without caring whether the theme is light or
  // dark.
  return std::max({std::abs(qRed(first) - qRed(second)),
                   std::abs(qGreen(first) - qGreen(second)),
                   std::abs(qBlue(first) - qBlue(second))});
}
} // namespace

std::optional<TextBand>
detectTextBand(const QImage &source, const QPointF &sourcePoint,
               const QSizeF &sourcePixelsPerLogicalPixel) {
  if (source.isNull() ||
      !QRectF(QPointF(), QSizeF(source.size())).contains(sourcePoint))
    return std::nullopt;

  const qreal scaleX =
      std::max<qreal>(sourcePixelsPerLogicalPixel.width(), 0.01);
  const qreal scaleY =
      std::max<qreal>(sourcePixelsPerLogicalPixel.height(), 0.01);
  const int edgeReach = scaledPixels(2.0, scaleX);
  const int columnStep = scaledPixels(2.0, scaleX);
  const int halfWidth = scaledPixels(kHorizontalRadius, scaleX);
  const int snapRows = scaledPixels(kSnapDistance, scaleY);
  const int minimumBandHeight = scaledPixels(kMinimumTextHeight, scaleY);
  const int maximumBandHeight = scaledPixels(kMaximumTextHeight, scaleY);
  const int mergeGap = scaledPixels(kVerticalMergeGap, scaleY);
  const int halfHeight = snapRows + maximumBandHeight;

  // QRectF accepts sub-pixel points just inside the bottom/right edge, while
  // rounding those can land exactly one pixel past the image.
  const int centerX =
      std::clamp(qRound(sourcePoint.x()), 0, source.width() - 1);
  const int centerY =
      std::clamp(qRound(sourcePoint.y()), 0, source.height() - 1);
  const int x0 = std::max(0, centerX - halfWidth);
  const int x1 = std::min(source.width() - 1 - edgeReach, centerX + halfWidth);
  const int y0 = std::max(0, centerY - halfHeight);
  const int y1 = std::min(source.height() - 1, centerY + halfHeight);
  if (x1 < x0 || y1 < y0)
    return std::nullopt;

  std::vector<int> columns;
  const auto columnCapacity =
      static_cast<std::size_t>(x1 - x0) / static_cast<std::size_t>(columnStep) +
      1;
  columns.reserve(columnCapacity);
  for (int x = x0; x <= x1; x += columnStep)
    columns.push_back(x);
  if (columns.size() < 8)
    return std::nullopt;

  std::vector<bool> textRows(static_cast<std::size_t>(y1 - y0 + 1), false);
  std::vector<int> deltas(columns.size());
  const int minimumEdges =
      std::max(3, static_cast<int>(std::ceil(
                      static_cast<qreal>(columns.size()) * kEdgeDensity)));
  for (int y = y0; y <= y1; ++y) {
    for (std::size_t index = 0; index < columns.size(); ++index) {
      const int x = columns[index];
      deltas[index] =
          colorDistance(source.pixel(x, y), source.pixel(x + edgeReach, y));
    }

    // Screenshot pixels are normally lossless, but images opened from files
    // can carry compression noise. Estimate that row's noise floor from the
    // median edge magnitude, then demand a visibly stronger transition. The
    // cap still recognizes subdued UI text; a lone panel/button boundary
    // cannot satisfy the density requirement by itself.
    const auto middle =
        deltas.begin() + static_cast<std::ptrdiff_t>(deltas.size() / 2);
    std::nth_element(deltas.begin(), middle, deltas.end());
    const int edgeThreshold = std::clamp(*middle + 10, 12, 24);
    const int edgeCount = static_cast<int>(std::count_if(
        deltas.cbegin(), deltas.cend(),
        [edgeThreshold](int delta) { return delta >= edgeThreshold; }));
    textRows[static_cast<std::size_t>(y - y0)] = edgeCount >= minimumEdges;
  }

  const int centerIndex = centerY - y0;
  std::optional<int> anchor;
  if (textRows[static_cast<std::size_t>(centerIndex)]) {
    anchor = centerIndex;
  } else {
    for (int distance = 1; distance <= snapRows; ++distance) {
      const int above = centerIndex - distance;
      const int below = centerIndex + distance;
      if (above >= 0 && textRows[static_cast<std::size_t>(above)]) {
        anchor = above;
        break;
      }
      if (below < static_cast<int>(textRows.size()) &&
          textRows[static_cast<std::size_t>(below)]) {
        anchor = below;
        break;
      }
    }
  }
  if (!anchor)
    return std::nullopt;

  int top = *anchor;
  int gap = 0;
  while (top > 0) {
    const int next = top - 1;
    if (textRows[static_cast<std::size_t>(next)]) {
      top = next;
      gap = 0;
    } else if (gap < mergeGap) {
      top = next;
      ++gap;
    } else {
      break;
    }
  }
  while (top < static_cast<int>(textRows.size()) &&
         !textRows[static_cast<std::size_t>(top)])
    ++top;

  int bottom = *anchor;
  gap = 0;
  while (bottom + 1 < static_cast<int>(textRows.size())) {
    const int next = bottom + 1;
    if (textRows[static_cast<std::size_t>(next)]) {
      bottom = next;
      gap = 0;
    } else if (gap < mergeGap) {
      bottom = next;
      ++gap;
    } else {
      break;
    }
  }
  while (bottom >= 0 && !textRows[static_cast<std::size_t>(bottom)])
    --bottom;

  const int height = bottom - top + 1;
  if (height < minimumBandHeight || height > maximumBandHeight)
    return std::nullopt;
  return TextBand{static_cast<qreal>(y0 + top),
                  static_cast<qreal>(y0 + bottom + 1)};
}
