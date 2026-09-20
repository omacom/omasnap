/** @fileoverview Provides pure pinned-window packing and dispatch helpers. */
#pragma once

#include <QPair>
#include <QJsonObject>
#include <optional>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>

/// The frame a pin fills: the display's aspect at a fixed width, clamped
/// so a tall pivot or an ultrawide still yields a pin rather than a line;
/// 16:9 when the display cannot be asked.
[[nodiscard]] QSize pinFrameSize(const QSize &screenSize);

/// Where a frame of `frame` size lands so it covers none of `blockers`:
/// snug in the bottom-right corner, or one gap above whatever occupies it,
/// climbing the column and starting a new column to the left when this one
/// is full. Blockers can be any size; the pin packs against what is
/// actually there rather than onto a grid that wastes a slot for every
/// straddled boundary.
// Returns no position when the output is full or smaller than the frame.
[[nodiscard]] std::optional<QPoint> pinPackedPosition(const QVector<QRect> &blockers,
                                       const QSize &screenSize,
                                       const QSize &frame, int gap,
                                       int margin);

/// What inserting a dragged pin into the column would look like right now.
/// `index` is -1 while the drag touches no part of the stack; any overlap
/// with a stacked pin (or with the empty corner spot) is enough to join,
/// and fully outside is what keeps a pin out. While joined, the column
/// pins in `spread` step aside around a dragged-sized hole at `spot`, and
/// releasing snaps the pin into it.
struct PinInsertionPlan {
  int index = -1;
  QRect spot;
  QVector<QPair<QString, QRect>> spread;
};
[[nodiscard]] PinInsertionPlan
pinInsertionPlan(QVector<QPair<QString, QRect>> column,
                 const QVector<QRect> &blockers, const QRect &dragged,
                 const QSize &screenSize, int gap, int margin);

/// Whether a pin hugs one of the packed columns. Freely placed pins are
/// left alone during compaction.
[[nodiscard]] bool pinInColumn(const QRect &rect, const QSize &screenSize,
                               int margin, int gap);

/// Dispatch expressions for a Lua-configured Hyprland, which evaluates the
/// dispatch argument as Lua; the classic dispatcher grammar parses as an
/// expression there and fails while reporting success.
[[nodiscard]] QString pinFloatDispatch(const QString &address);
[[nodiscard]] QString pinPinDispatch(const QString &address);
[[nodiscard]] QString pinMoveDispatch(const QString &address, int x, int y);
/// Global logical geometry, including scale and quarter-turn transforms.
[[nodiscard]] QRect pinMonitorGeometry(const QJsonObject &monitor);

/** The hover tip for a pin control, empty outside the known controls. */
[[nodiscard]] QString pinControlTip(int index);

/** Remaining-time fraction for the pin auto-dismiss bar, clamped to 0..1.
 *  A non-positive total yields 0. */
[[nodiscard]] qreal pinDismissFraction(qint64 remainingMs, qint64 totalMs);
