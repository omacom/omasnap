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
#include <QTransform>

/// The frame a pin fills: the display's aspect at a fixed width, clamped
/// so a tall pivot or an ultrawide still yields a pin rather than a line;
/// 16:9 when the display cannot be asked.
[[nodiscard]] QSize pinFrameSize(const QSize &screenSize);

/// Keep a freely dropped pin inside a monitor's work area with the stack's
/// edge inset. If it is smaller than the pin, keep the top-left controls visible.
[[nodiscard]] QRect pinVisibleRect(const QRect &rect, const QRect &screen,
                                    int margin);

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

/// Lay out a bottom-to-top ordered deck. Idle cards overlap by all but a
/// twelve-pixel lip; hovering exposes every card. Both layouts keep the same
/// front card anchored, wrap into columns and leave freely placed pins alone.
/// Empty means the complete deck cannot fit; callers must not move a subset.
[[nodiscard]] QVector<QPair<QString, QRect>>
pinStackLayout(const QVector<QPair<QString, QRect>> &ordered,
               const QVector<QRect> &blockers, const QSize &screenSize,
               int gap, int margin, bool expanded);

/// A restrained, alternating lean beneath a straight front card.
[[nodiscard]] qreal pinStackTilt(int depth, bool expanded);
/// Rotate the painted card inside its existing window, fitting every corner.
[[nodiscard]] QTransform pinCardTransform(const QSize &frame, qreal degrees);

/// Includes the gaps between exposed cards so crossing a gap keeps them open.
[[nodiscard]] QRect pinStackHotZone(const QVector<QRect> &cards,
                                   const QRect &screen);

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
[[nodiscard]] QString pinRaiseDispatch(const QString &address);
[[nodiscard]] QString pinFocusDispatch(const QString &address);
/// Global logical geometry, including scale and quarter-turn transforms.
[[nodiscard]] QRect pinMonitorGeometry(const QJsonObject &monitor);
/// Global logical geometry excluding reserved space for bars on any edge.
[[nodiscard]] QRect pinMonitorWorkArea(const QJsonObject &monitor);

/** The hover tip for a pin control, empty outside the known controls. */
[[nodiscard]] QString pinControlTip(int index, bool kept = false);
