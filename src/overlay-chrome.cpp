/** @fileoverview Shared overlay chrome (see overlay-chrome.hpp). */
#include "overlay-chrome.hpp"

#include <QFont>
#include <QFontMetricsF>
#include <QString>
#include <QPainter>

#include <algorithm>

QFont chromeFont(int pixelSize, bool bold) {
  static const QFont base = [] {
    QFont font;
    font.setFamilies({QStringLiteral("Adwaita Sans"),
                      QStringLiteral("Noto Sans")});
    return font;
  }();
  QFont font = base;
  font.setPixelSize(pixelSize);
  font.setBold(bold);
  return font;
}

QFont chromeDefaultFont() {
  QFont font;
  font.setFamilies({QStringLiteral("Adwaita Sans"),
                    QStringLiteral("Noto Sans")});
  font.setPointSize(11);
  return font;
}

QFont chromeMonoFont(int pixelSize, bool bold) {
  static const QFont base = [] {
    QFont font;
    font.setFamilies({QStringLiteral("monospace")});
    return font;
  }();
  QFont font = base;
  font.setPixelSize(pixelSize);
  font.setBold(bold);
  return font;
}

QRectF drawModeBadge(QPainter &painter, const QRect &bounds,
                     const QString &label, const QColor &accent,
                     QRectF *closeRect) {
  QFont badgeFont(QStringLiteral("Noto Sans"));
  badgeFont.setBold(true);
  badgeFont.setPixelSize(11);
  painter.setFont(badgeFont);
  const QString badge = label + QStringLiteral("  ×");
  const int badgeWidth = painter.fontMetrics().horizontalAdvance(badge) + 24;
  const QRectF badgeRect((bounds.width() - badgeWidth) / 2.0, 12, badgeWidth,
                         32);
  painter.setPen(QPen(QColor(255, 255, 255, 32), 1));
  painter.setBrush(QColor(18, 18, 22, 235));
  painter.drawRoundedRect(badgeRect, 10, 10);
  painter.setPen(accent);
  painter.drawText(badgeRect, Qt::AlignCenter, badge);
  if (closeRect) {
    // The × and a little around it, so the click that closes has a target
    // rather than a pixel.
    const qreal closeWidth =
        painter.fontMetrics().horizontalAdvance(QStringLiteral("×")) + 18;
    *closeRect = QRectF(badgeRect.right() - closeWidth, badgeRect.top(),
                        closeWidth, badgeRect.height());
  }
  return badgeRect;
}

void drawHotkeyLegend(QPainter &painter, const QRect &bounds,
                      const QVector<QPair<QString, QString>> &entries) {
  if (entries.isEmpty())
    return;
  const QFont font = chromeFont(11);
  const QFontMetricsF metrics(font);
  constexpr qreal keyGap = 10;    // between a key and what it does
  constexpr qreal marginLeft = 14;
  constexpr qreal marginBottom = 14;
  constexpr qreal rowHeight = 17;
  qreal keyWidth = 0.0;
  for (const auto &entry : entries)
    keyWidth = std::max(keyWidth, metrics.horizontalAdvance(entry.first));
  painter.setFont(font);
  // Bottom-left, growing upward: entry 0 is the bottom-most row. No card, no
  // border, no dodging the pointer or anything else — the caller draws this
  // early, so real chrome painted afterward simply covers it where the two
  // overlap, and the low opacity keeps it out of the way where nothing does.
  for (int index = 0; index < entries.size(); ++index) {
    const qreal y =
        bounds.height() - marginBottom - (index + 1) * rowHeight;
    painter.setPen(QColor(169, 182, 203, 165));
    painter.drawText(QRectF(marginLeft, y, keyWidth, rowHeight - 2),
                     Qt::AlignLeft | Qt::AlignVCenter, entries.at(index).first);
    painter.setPen(QColor(199, 204, 214, 130));
    painter.drawText(
        QRectF(marginLeft + keyWidth + keyGap, y,
               bounds.width() - marginLeft - keyWidth - keyGap - 14,
               rowHeight - 2),
        Qt::AlignLeft | Qt::AlignVCenter, entries.at(index).second);
  }
}

namespace {
constexpr qreal kLegendKeyGapAnchored = 8;
constexpr qreal kLegendColumnGapAnchored = 14;
constexpr qreal kLegendPaddingAnchored = 12;

struct AnchoredLegendLayout {
  int columns = 2;
  int rows = 0;
  QVector<qreal> keyWidth;
  QVector<qreal> textWidth;
  QVector<qreal> columnWidth;
  qreal width = 0;
};

QFont anchoredLegendFont() {
  QFont font = chromeFont(11);
  return font;
}

AnchoredLegendLayout measureAnchoredLegend(
    const QVector<QPair<QString, QString>> &entries, int columns) {
  const QFontMetricsF metrics{anchoredLegendFont()};
  AnchoredLegendLayout layout;
  layout.columns = columns;
  layout.rows = (entries.size() + columns - 1) / columns;
  layout.keyWidth = QVector<qreal>(columns, 0.0);
  layout.textWidth = QVector<qreal>(columns, 0.0);
  for (int index = 0; index < entries.size(); ++index) {
    const int column = std::min(index / layout.rows, columns - 1);
    layout.keyWidth[column] =
        std::max(layout.keyWidth[column],
                 metrics.horizontalAdvance(entries.at(index).first));
    layout.textWidth[column] =
        std::max(layout.textWidth[column],
                 metrics.horizontalAdvance(entries.at(index).second));
  }
  layout.columnWidth = QVector<qreal>(columns, 0.0);
  layout.width = 2 * kLegendPaddingAnchored;
  for (int column = 0; column < columns; ++column) {
    if (layout.keyWidth[column] <= 0 && layout.textWidth[column] <= 0)
      continue;
    layout.columnWidth[column] = layout.keyWidth[column] +
                                 kLegendKeyGapAnchored +
                                 layout.textWidth[column];
    layout.width += layout.columnWidth[column];
    if (column > 0)
      layout.width += kLegendColumnGapAnchored;
  }
  return layout;
}

AnchoredLegendLayout fitAnchoredLegend(
    const QVector<QPair<QString, QString>> &entries, qreal maxWidth) {
  AnchoredLegendLayout layout;
  for (int tryRows = 4;; ++tryRows) {
    layout = measureAnchoredLegend(
        entries, std::max(1, static_cast<int>((entries.size() + tryRows - 1) /
                                              tryRows)));
    if (layout.width <= maxWidth || layout.columns == 1)
      return layout;
  }
}
} // namespace

QSize hotkeyLegendAnchoredSize(const QVector<QPair<QString, QString>> &entries,
                               qreal maxWidth) {
  if (entries.isEmpty())
    return {};
  const AnchoredLegendLayout layout = fitAnchoredLegend(entries, maxWidth);
  return {qRound(std::min(layout.width, maxWidth)), layout.rows * 19 + 24};
}

void drawAnchoredHotkeyLegend(QPainter &painter, const QRect &bounds,
                              const QVector<QPair<QString, QString>> &entries,
                              const QRectF &anchorAbove) {
  if (entries.isEmpty())
    return;
  const QFont font = anchoredLegendFont();
  const AnchoredLegendLayout layout =
      fitAnchoredLegend(entries, bounds.width() - 28.0);
  const qreal width = std::min(layout.width, bounds.width() - 28.0);
  const qreal height = layout.rows * 19 + 24;
  // Pinned to the window's top, centered over the anchor: the windowed
  // layout reserves the room, so the guide never covers the toolbar or the
  // canvas.
  const qreal x =
      std::clamp(anchorAbove.center().x() - width / 2.0, 14.0,
                 std::max(14.0, bounds.width() - width - 14.0));
  const QRectF panel(x, 14.0, width, height);
  painter.setPen(QPen(QColor(255, 255, 255, 34), 1));
  painter.setBrush(QColor(13, 15, 20, 224));
  painter.drawRoundedRect(panel, 11, 11);
  painter.setFont(font);
  for (int index = 0; index < entries.size(); ++index) {
    const int column = std::min(index / layout.rows, layout.columns - 1);
    const int row = index % layout.rows;
    qreal cell = panel.left() + kLegendPaddingAnchored;
    for (int before = 0; before < column; ++before)
      cell += layout.columnWidth[before] + kLegendColumnGapAnchored;
    const qreal top = panel.top() + 12 + row * 19;
    painter.setPen(QColor(QStringLiteral("#a9b6cb")));
    painter.drawText(QRectF(cell, top, layout.keyWidth[column], 18),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     entries.at(index).first);
    painter.setPen(QColor(QStringLiteral("#f5f5f7")));
    painter.drawText(QRectF(cell + layout.keyWidth[column] +
                                kLegendKeyGapAnchored,
                            top, layout.textWidth[column], 18),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     entries.at(index).second);
  }
}

void drawStatusPill(QPainter &painter, const QRect &bounds,
                    const QString &text) {
  if (text.isEmpty())
    return;

  QFont font(QStringLiteral("Noto Sans"));
  font.setPixelSize(13);
  painter.setFont(font);
  const int width = painter.fontMetrics().horizontalAdvance(text) + 28;
  const QRectF pill((bounds.width() - width) / 2.0, bounds.height() - 42.0,
                    width, 30);
  painter.setPen(QPen(QColor(255, 255, 255, 32), 1));
  painter.setBrush(QColor(18, 18, 22, 232));
  painter.drawRoundedRect(pill, 10, 10);
  painter.setPen(Qt::white);
  painter.drawText(pill, Qt::AlignCenter, text);
}
