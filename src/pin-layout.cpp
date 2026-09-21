/** @fileoverview Implements pinned-window stacking and dispatch helpers. */
#include "pin-layout.hpp"

#include <QJsonArray>
#include <QTransform>
#include <QtNumeric>
#include <QtTypes>
#include <QtMath>

#include <algorithm>
#include <cmath>

QSize pinFrameSize(const QSize &screenSize) {
  constexpr int width = 200;
  const double aspect =
      screenSize.width() > 0 && screenSize.height() > 0
          ? static_cast<double>(screenSize.height()) / screenSize.width()
          : 9.0 / 16.0;
  const int height = std::clamp(static_cast<int>(std::lround(width * aspect)),
                                width / 4, width * 2);
  return {width, height};
}

QRect pinVisibleRect(const QRect &rect, const QRect &screen, int margin) {
  if (rect.isEmpty() || screen.isEmpty())
    return rect;
  const int insetX = std::clamp(margin, 0, std::max(0, (screen.width() - rect.width()) / 2));
  const int insetY = std::clamp(margin, 0, std::max(0, (screen.height() - rect.height()) / 2));
  const int left = screen.left() + insetX;
  const int top = screen.top() + insetY;
  const int right = std::max(left, screen.right() - rect.width() + 1 - insetX);
  const int bottom = std::max(top, screen.bottom() - rect.height() + 1 - insetY);
  return {QPoint(std::clamp(rect.x(), left, right),
                 std::clamp(rect.y(), top, bottom)), rect.size()};
}

std::optional<QPoint> pinPackedPosition(const QVector<QRect> &blockers,
                         const QSize &screenSize, const QSize &frame, int gap,
                         int margin) {
  int x = screenSize.width() - margin - frame.width();
  if (frame.isEmpty() || gap < 0 || margin < 0)
    return std::nullopt;
  while (x >= margin) {
    int y = screenSize.height() - margin - frame.height();
    while (y >= margin) {
      const QRect candidate(x, y, frame.width(), frame.height());
      std::optional<int> lowestTop;
      for (const QRect &blocker : blockers) {
        if (candidate.intersects(blocker))
          lowestTop = lowestTop ? std::max(*lowestTop, blocker.top())
                                : blocker.top();
      }
      if (!lowestTop)
        return QPoint(x, y);
      // Climb to one gap above the lowest pin in the way, then look again:
      // the spot up there may graze another one.
      y = *lowestTop - gap - frame.height();
    }
    x -= frame.width() + gap;
  }
  return std::nullopt;
}

QVector<QPair<QString, QRect>>
pinStackLayout(const QVector<QPair<QString, QRect>> &ordered,
               const QVector<QRect> &blockers, const QSize &screenSize,
               int gap, int margin, bool expanded) {
  QVector<QRect> occupied = blockers;
  QVector<QPair<QString, QRect>> layout;
  for (const auto &[title, rect] : ordered) {
    const auto at = pinPackedPosition(occupied, screenSize, rect.size(), gap, margin);
    if (!at)
      return {};
    const QRect seat(*at, rect.size());
    layout.push_back({title, seat});
    occupied.push_back(seat);
  }
  if (expanded)
    return layout;

  // Compress each column independently, preserving order and the front
  // card's position. A free pin in the deck's footprint keeps that column
  // exposed instead of covering the free pin when it folds.
  for (qsizetype first = 0; first < layout.size();) {
    qsizetype end = first + 1;
    while (end < layout.size() &&
           layout.at(end).second.right() == layout.at(first).second.right())
      ++end;
    QVector<QRect> deck;
    bool blocked = false;
    for (qsizetype index = first; index < end; ++index) {
      QRect card = layout.at(index).second;
      card.moveBottom(layout.at(first).second.bottom() -
                       static_cast<int>(12 * (index - first)));
      for (const QRect &blocker : blockers)
        blocked = blocked || card.intersects(blocker);
      deck.push_back(card);
    }
    if (!blocked) {
      for (qsizetype index = first; index < end; ++index)
        layout[index].second = deck.at(index - first);
    }
    first = end;
  }
  return layout;
}

QTransform pinCardTransform(const QSize &frame, qreal degrees) {
  if (frame.isEmpty() || qFuzzyIsNull(degrees))
    return {};
  const qreal radians = qDegreesToRadians(degrees);
  const qreal sine = std::abs(std::sin(radians));
  const qreal cosine = std::abs(std::cos(radians));
  const qreal width = std::max(1, frame.width() - 2);
  const qreal height = std::max(1, frame.height() - 2);
  const qreal scale = std::min(width / (width * cosine + height * sine),
                              height / (height * cosine + width * sine));
  QTransform transform;
  transform.translate(frame.width() / 2.0, frame.height() / 2.0);
  transform.rotate(degrees);
  transform.scale(scale, scale);
  transform.translate(-frame.width() / 2.0, -frame.height() / 2.0);
  return transform;
}

QRect pinStackHotZone(const QVector<QRect> &cards, const QRect &screen) {
  QRect zone;
  for (const QRect &card : cards)
    zone |= card;
  return zone.isEmpty() ? QRect() : zone.adjusted(-12, -12, 12, 12).intersected(screen);
}

PinInsertionPlan pinInsertionPlan(QVector<QPair<QString, QRect>> column,
                                  const QVector<QRect> &blockers,
                                  const QRect &dragged,
                                  const QSize &screenSize, int gap,
                                  int margin) {
  PinInsertionPlan plan;
  std::sort(column.begin(), column.end(),
            [screenSize, gap, margin](const auto &a, const auto &b) {
              const auto columnIndex = [screenSize, gap, margin](const QRect &rect) {
                return qRound(qreal(screenSize.width() - margin - rect.right() - 1) /
                                (rect.width() + gap));
              };
              const int aColumn = columnIndex(a.second), bColumn = columnIndex(b.second);
              if (aColumn != bColumn)
                return aColumn < bColumn;
              return a.second.y() > b.second.y();
            });
  // The dragged pin's place in the order comes from its center against the
  // column as it would pack, not against the possibly already-spread live
  // positions, so the preview does not chase its own moves.
  QVector<QRect> seed = blockers;
  QVector<QRect> packed;
  for (const auto &pair : column) {
    const auto at =
        pinPackedPosition(seed, screenSize, pair.second.size(), gap, margin);
    if (!at)
      return {};
    seed.push_back(QRect(*at, pair.second.size()));
    packed.push_back(QRect(*at, pair.second.size()));
  }
  // Touching any part of the stack joins it; fully outside stays out. The
  // stack includes the open spot on top, which is where a pin dragged off
  // the top of the stack came from: it snaps back until it has been
  // dragged fully past where it would sit. For an empty column that spot
  // is the corner itself.
  QVector<QRect> stack = packed;
  const auto vacant = pinPackedPosition(seed, screenSize, dragged.size(), gap, margin);
  if (vacant)
    stack.push_back(QRect(*vacant, dragged.size()));
  // The pins' live positions count too: a stack that has not packed down
  // yet is still the stack the user sees and aims for.
  for (const auto &pair : column)
    stack.push_back(pair.second);
  // The region a drag folds back into is the whole column band: the
  // bounding box of every pin and seat, which always reaches the bottom
  // corner because packing anchors there. Anywhere inside it is where some
  // pin would live, not just the dragged pin's own former spot; only fully
  // outside it stays out.
  QRect band;
  for (const QRect &rect : stack)
    band |= rect;
  if (!dragged.intersects(band))
    return plan;
  // Choose the column with the largest horizontal overlap, then order
  // vertically within it. Earlier columns remain ahead of the insertion.
  int columnRight = screenSize.width() - margin - 1;
  int overlap = 0;
  for (const QRect &seat : stack) {
    const int width = std::max(0, std::min(seat.right(), dragged.right()) -
                                     std::max(seat.left(), dragged.left()) + 1);
    if (width > overlap) {
      overlap = width;
      columnRight = seat.right();
    }
  }
  int index = 0;
  for (const QRect &seat : packed)
    if (seat.right() > columnRight + 6 ||
        (std::abs(seat.right() - columnRight) <= 6 &&
         seat.center().y() > dragged.center().y()))
      ++index;
  plan.index = index;

  // Pack again with a dragged-sized hole at the insertion point.
  seed = blockers;
  for (int position = 0; position < column.size(); ++position) {
    if (position == index) {
      const auto at =
          pinPackedPosition(seed, screenSize, dragged.size(), gap, margin);
      if (!at)
        return {};
      plan.spot = QRect(*at, dragged.size());
      seed.push_back(plan.spot);
    }
    const auto &pair = column.at(position);
    const auto at =
        pinPackedPosition(seed, screenSize, pair.second.size(), gap, margin);
    if (!at)
      return {};
    seed.push_back(QRect(*at, pair.second.size()));
    plan.spread.push_back({pair.first, QRect(*at, pair.second.size())});
  }
  if (index == column.size()) {
    const auto at =
        pinPackedPosition(seed, screenSize, dragged.size(), gap, margin);
    if (!at)
      return {};
    plan.spot = QRect(*at, dragged.size());
  }
  return plan;
}

bool pinInColumn(const QRect &rect, const QSize &screenSize, int margin, int gap) {
  constexpr int tolerance = 6;
  const int stride = rect.width() + gap;
  if (stride <= 0 || rect.left() < margin - tolerance)
    return false;
  const int offset = screenSize.width() - margin - rect.right() - 1;
  const int column = std::max(0, qRound(qreal(offset) / stride));
  return std::abs(offset - column * stride) <= tolerance;
}

namespace {
// Address selectors identify the exact client already filtered by app class.
QString windowSelector(const QString &address) {
  return QStringLiteral("window = \"address:%1\"").arg(address);
}
} // namespace

QString pinFloatDispatch(const QString &address) {
  return QStringLiteral("hl.dsp.window.float({ %1 })")
      .arg(windowSelector(address));
}

QString pinPinDispatch(const QString &address) {
  return QStringLiteral("hl.dsp.window.pin({ %1 })").arg(windowSelector(address));
}

QString pinMoveDispatch(const QString &address, int x, int y) {
  return QStringLiteral(
             "hl.dsp.window.move({ x = %1, y = %2, relative = false, %3 })")
      .arg(x)
      .arg(y)
      .arg(windowSelector(address));
}

QString pinRaiseDispatch(const QString &address) {
  return QStringLiteral("hl.dsp.window.alter_zorder({ mode = \"top\", %1 })")
      .arg(windowSelector(address));
}

QString pinFocusDispatch(const QString &address) {
  return QStringLiteral("hl.dsp.focus({ %1 })").arg(windowSelector(address));
}

QRect pinMonitorGeometry(const QJsonObject &monitor) {
  const qreal scale = std::max<qreal>(0.0001, monitor.value(QStringLiteral("scale")).toDouble(1));
  QSize pixels(monitor.value(QStringLiteral("width")).toInt(),
               monitor.value(QStringLiteral("height")).toInt());
  if (monitor.value(QStringLiteral("transform")).toInt() % 2 != 0)
    pixels.transpose();
  return {monitor.value(QStringLiteral("x")).toInt(),
          monitor.value(QStringLiteral("y")).toInt(),
          qRound(pixels.width() / scale), qRound(pixels.height() / scale)};
}

QRect pinMonitorWorkArea(const QJsonObject &monitor) {
  const QRect geometry = pinMonitorGeometry(monitor);
  const QJsonArray reserved = monitor.value(QStringLiteral("reserved")).toArray();
  if (reserved.size() != 4)
    return geometry;
  // Hyprland reports left, top, right, bottom in logical coordinates,
  // already accounting for the output's scale and transform.
  return geometry.adjusted(std::max(0, reserved.at(0).toInt()),
                           std::max(0, reserved.at(1).toInt()),
                           -std::max(0, reserved.at(2).toInt()),
                           -std::max(0, reserved.at(3).toInt()));
}

QRectF pinControlRect(const QSize &frame, int index) {
  if (frame.isEmpty())
    return {};
  // Very wide displays produce previews only 50 pixels tall. Keep two
  // separate rows there so the centered actions never cover a corner control.
  const bool compact = frame.height() < 76;
  const qreal size = compact ? 19 : 23;
  const qreal inset = compact ? 4 : 7;
  const qreal gap = compact ? 3 : 6;
  constexpr qreal dragWidth = 18;
  constexpr qreal actionWidth = 52;
  constexpr qreal actionGap = 6;
  const qreal actionHeight = compact ? 20 : 26;
  const qreal actionY = std::max((frame.height() - actionHeight) / 2,
                                 inset + size + gap);
  const qreal right = frame.width() - inset - size;
  switch (index) {
  case 0: // Close
    return {right, inset, size, size};
  case 1: // Copy
    return {(frame.width() + actionGap) / 2, actionY, actionWidth, actionHeight};
  case 2: // Copy path
    return {inset + dragWidth + gap, inset, size, size};
  case 3: // Edit
    return {(frame.width() - actionGap) / 2 - actionWidth, actionY,
            actionWidth, actionHeight};
  case 4: // Drag out
    return {inset, inset, dragWidth, size};
  case 5: // Pin
    return {right - size - gap, inset, size, size};
  default:
    return {};
  }
}

QString pinControlTip(int index, bool kept) {
  switch (index) {
  case 0:
    return QStringLiteral("Close · X / Super+W / Esc / middle-click");
  case 2:
    return QStringLiteral("Copy saved file path · L / F");
  case 4:
    return QStringLiteral("Drag this image out");
  case 5:
    return kept ? QStringLiteral("Unpin · T / Ctrl+P · fade after 10 seconds")
                 : QStringLiteral("Keep on screen · T / Ctrl+P");
  default:
    return {};
  }
}
