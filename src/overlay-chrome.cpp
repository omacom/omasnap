/** @fileoverview Shared overlay chrome (see overlay-chrome.hpp). */
#include "overlay-chrome.hpp"

#include <QFont>
#include <QFontMetricsF>
#include <QString>
#include <QStringList>
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

QString captureTabLabel(CaptureKind kind) {
  switch (kind) {
  case CaptureKind::Region:
    return QStringLiteral("REGION");
  case CaptureKind::Scroll:
    return QStringLiteral("SCROLLING REGION");
  case CaptureKind::Window:
    return QStringLiteral("WINDOW");
  case CaptureKind::Fullscreen:
    return QStringLiteral("FULLSCREEN");
  }
  return {};
}

namespace {
QFont captureTabFont() {
  QFont font(QStringLiteral("Noto Sans"));
  font.setBold(true);
  font.setPixelSize(11);
  return font;
}
QColor captureTabAccent(CaptureKind kind) {
  switch (kind) {
  case CaptureKind::Window:
    return QColor(QStringLiteral("#ffd60a"));
  case CaptureKind::Fullscreen:
    return QColor(QStringLiteral("#0a84ff"));
  case CaptureKind::Region:
  case CaptureKind::Scroll:
    break;
  }
  return QColor(QStringLiteral("#30d158"));
}
/// Gap between neighbouring tabs; a wider one means the strip is split.
constexpr qreal kTabGap = 2.0;
/// Physical width of the camera housing on the notched MacBook Pro panels.
constexpr int kMacBookNotchPhysicalWidth = 370;
} // namespace

qreal captureTabNotchWidth(const QSize &physicalMode, qreal scale) {
  const bool notched =
      physicalMode == QSize(3456, 2234) || physicalMode == QSize(3024, 1964);
  if (!notched)
    return 0.0;
  return kMacBookNotchPhysicalWidth / (scale > 0.0 ? scale : 1.0);
}

QVector<CaptureTab> captureTabLayout(const QRect &bounds, qreal notchWidth) {
  static const CaptureKind order[] = {CaptureKind::Region, CaptureKind::Window,
                                      CaptureKind::Scroll,
                                      CaptureKind::Fullscreen};
  const QFontMetricsF metrics(captureTabFont());
  constexpr qreal kPad = 14.0;
  constexpr qreal kHeight = 26.0;
  // Flush to the top edge (see drawCaptureTabs' -30 background extension):
  // derived from kCaptureTabBarBottom rather than a separate magic number,
  // so the two can't drift apart.
  constexpr qreal kTop = kCaptureTabBarBottom - kHeight - 5.0;
  QVector<CaptureTab> tabs;
  qreal total = 0.0;
  for (const CaptureKind kind : order) {
    const qreal w = metrics.horizontalAdvance(captureTabLabel(kind)) + 2 * kPad;
    tabs.push_back({kind, QRectF(total, kTop, w, kHeight)});
    total += w + kTabGap;
  }
  total -= kTabGap;
  const qreal left = bounds.left() + (bounds.width() - total) / 2.0;
  for (CaptureTab &tab : tabs)
    tab.rect.translate(left, 0);
  if (notchWidth <= 0.0)
    return tabs;
  // Notched panel: Region and Window sit left of the housing, Scrolling
  // Region and Fullscreen right of it. If either ear would run off the
  // surface, the centered strip stays.
  const qreal center = bounds.left() + bounds.width() / 2.0;
  const qreal leftShift = (center - notchWidth / 2.0) - tabs.at(1).rect.right();
  const qreal rightShift = (center + notchWidth / 2.0) - tabs.at(2).rect.left();
  if (tabs.at(0).rect.left() + leftShift < bounds.left() ||
      tabs.at(3).rect.right() + rightShift > bounds.right())
    return tabs;
  for (int index = 0; index < tabs.size(); ++index)
    tabs[index].rect.translate(index < 2 ? leftShift : rightShift, 0);
  return tabs;
}

int captureTabAt(const QVector<CaptureTab> &tabs, const QPointF &position) {
  for (int index = 0; index < tabs.size(); ++index) {
    if (tabs.at(index).rect.adjusted(-2, -6, 2, 6).contains(position))
      return index;
  }
  return -1;
}

void drawCaptureTabs(QPainter &painter, const QVector<CaptureTab> &tabs,
                     CaptureKind active, const QPointF &cursor) {
  if (tabs.isEmpty())
    return;
  // Hangs off the top edge like a tab strip: square at the top (drawn past
  // the edge so only the bottom corners round), not a floating pill. Around
  // a notch the strip is two such bars, one either side of the housing.
  const auto paintBar = [&](const CaptureTab &first, const CaptureTab &last) {
    const QRectF bar = first.rect.united(last.rect).adjusted(-5, -30, 5, 5);
    painter.setPen(QPen(QColor(255, 255, 255, 32), 1));
    painter.setBrush(QColor(18, 18, 22, 235));
    painter.drawRoundedRect(bar, 12, 12);
  };
  if (tabs.size() == 4 &&
      tabs.at(2).rect.left() - tabs.at(1).rect.right() > 2 * kTabGap) {
    paintBar(tabs.at(0), tabs.at(1));
    paintBar(tabs.at(2), tabs.at(3));
  } else {
    paintBar(tabs.constFirst(), tabs.constLast());
  }
  painter.setFont(captureTabFont());
  const int hovered = captureTabAt(tabs, cursor);
  for (int index = 0; index < tabs.size(); ++index) {
    const CaptureTab &tab = tabs.at(index);
    painter.setPen(Qt::NoPen);
    if (tab.kind == active) {
      painter.setBrush(captureTabAccent(tab.kind));
      painter.drawRoundedRect(tab.rect, 9, 9);
      painter.setPen(QColor(18, 18, 22));
    } else {
      if (index == hovered) {
        painter.setBrush(QColor(255, 255, 255, 28));
        painter.drawRoundedRect(tab.rect, 9, 9);
      }
      painter.setPen(QColor(255, 255, 255, index == hovered ? 255 : 190));
    }
    painter.drawText(tab.rect, Qt::AlignCenter, captureTabLabel(tab.kind));
  }
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

void drawStatusPill(QPainter &painter, const QRect &bounds,
                    const QString &text) {
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
