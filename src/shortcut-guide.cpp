#include "shortcut-guide.hpp"
#include "chrome-theme.hpp"
#include "overlay-chrome.hpp"

#include <QColor>
#include <QCursor>
#include <QEvent>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPair>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>
#include <QWheelEvent>
#include <QWidget>
#include <Qt>
#include <QtTypes>

#include <algorithm>

namespace {
constexpr int kPadding = 12;
constexpr int kRowHeight = 24;
constexpr int kToggleHeight = 36;
constexpr int kKeyGap = 12;
constexpr int kMargin = 14;
} // namespace

ShortcutGuide::ShortcutGuide(QWidget *parent) : QWidget(parent) {
  setObjectName(QStringLiteral("shortcut-guide"));
  setAccessibleName(QStringLiteral("Keyboard shortcuts"));
  setFocusPolicy(Qt::NoFocus);
  setMouseTracking(true);
  parent->installEventFilter(this);
  hide();
}

void ShortcutGuide::setEntries(
    const QVector<QPair<QString, QString>> &entries) {
  if (entries_ == entries)
    return;
  entries_ = entries;
  firstRow_ = 0;
  keyWidth_ = 0;
  int actionWidth = 0;
  const QFontMetrics keys(chromeMonoFont(12, true));
  const QFontMetrics actions(chromeFont(13));
  for (const auto &entry : entries_) {
    keyWidth_ = std::max(keyWidth_, keys.horizontalAdvance(entry.first) + 12);
    actionWidth = std::max(actionWidth, actions.horizontalAdvance(entry.second));
  }
  contentWidth_ = kPadding * 2 + keyWidth_ + kKeyGap + actionWidth + 6;
  reflow();
  update();
}

void ShortcutGuide::setExpanded(bool expanded) {
  if (expanded_ == expanded)
    return;
  expanded_ = expanded;
  hovered_ = false;
  reflow();
  // Expanding or collapsing uncovers a large part of the canvas at once.
  parentWidget()->update();
  update();
}

QRect ShortcutGuide::toggleRect() const {
  return {0, height() - kToggleHeight, width(), kToggleHeight};
}

void ShortcutGuide::reflow() {
  const QSize available = parentWidget()->size();
  const int rowSpace = available.height() - kMargin * 2 - 64 -
                       kToggleHeight - kPadding * 2;
  visibleRows_ = std::min(static_cast<int>(entries_.size()),
                          std::max(1, rowSpace / kRowHeight));
  firstRow_ = std::clamp(firstRow_, 0,
                         std::max(0, static_cast<int>(entries_.size()) - visibleRows_));
  const int width = std::min(expanded_ ? std::max(160, contentWidth_) : 146,
                             std::max(1, available.width() - kMargin * 2));
  const int height = kToggleHeight +
                     (expanded_ ? kPadding * 2 + visibleRows_ * kRowHeight : 0);
  setGeometry(kMargin, std::max(kMargin, available.height() - kMargin - height),
               width, height);
}

bool ShortcutGuide::eventFilter(QObject *watched, QEvent *event) {
  if (watched == parentWidget() && event->type() == QEvent::Resize)
    reflow();
  return QWidget::eventFilter(watched, event);
}

void ShortcutGuide::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
  const auto &theme = chromeTheme();
  QColor surface = theme.surface;
  surface.setAlpha(255);
  const QRectF panel = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
  painter.setPen(QPen(theme.panelBorder.brush(panel), 1));
  painter.setBrush(surface);
  painter.drawRoundedRect(panel, 10, 10);

  if (expanded_) {
    const QFontMetrics keys(chromeMonoFont(12, true));
    const QFontMetrics actions(chromeFont(13));
    for (int row = 0; row < visibleRows_; ++row) {
      const auto &entry = entries_.at(firstRow_ + row);
      const int y = kPadding + row * kRowHeight;
      const QRectF key(kPadding, y + 2, keys.horizontalAdvance(entry.first) + 12,
                        kRowHeight - 4);
      painter.setPen(Qt::NoPen);
      painter.setBrush(theme.button);
      painter.drawRoundedRect(key, 4, 4);
      painter.setFont(chromeMonoFont(12, true));
      painter.setPen(theme.buttonText);
      painter.drawText(key, Qt::AlignCenter, entry.first);
      painter.setFont(chromeFont(13));
      painter.setPen(theme.foreground);
      const QRect action(kPadding + keyWidth_ + kKeyGap, y,
                          width() - kPadding * 2 - keyWidth_ - kKeyGap - 6,
                          kRowHeight);
      painter.drawText(action, Qt::AlignLeft | Qt::AlignVCenter,
                       actions.elidedText(entry.second, Qt::ElideRight,
                                          action.width()));
    }
    if (visibleRows_ < entries_.size()) {
      const qreal track = visibleRows_ * kRowHeight;
      const qreal count = static_cast<qreal>(entries_.size());
      const qreal thumb = track * visibleRows_ / count;
      const qreal y = kPadding + track * firstRow_ / count;
      painter.setPen(Qt::NoPen);
      painter.setBrush(theme.muted);
      painter.drawRoundedRect(QRectF(width() - 6, y, 3, thumb), 1.5, 1.5);
    }
    painter.setPen(QPen(theme.panelBorder.brush(panel), 1));
    painter.drawLine(kPadding, toggleRect().top(), width() - kPadding,
                     toggleRect().top());
  }
  const QRect toggle = toggleRect().adjusted(5, 4, -5, -4);
  if (hovered_) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme.buttonHover);
    painter.drawRoundedRect(toggle, 6, 6);
  }
  painter.setFont(chromeFont(13, true));
  painter.setPen(hovered_ ? theme.buttonHoverText : theme.foreground);
  painter.drawText(toggle.adjusted(7, 0, -28, 0), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("?  Shortcuts"));
  const QPointF center(width() - 22, toggle.center().y());
  const qreal direction = expanded_ ? 1.0 : -1.0;
  painter.setPen(QPen(hovered_ ? theme.buttonHoverText : theme.muted, 1.5));
  painter.drawLine(center + QPointF(-4, -2 * direction),
                   center + QPointF(0, 2 * direction));
  painter.drawLine(center + QPointF(0, 2 * direction),
                   center + QPointF(4, -2 * direction));
}

void ShortcutGuide::mousePressEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton &&
      toggleRect().contains(event->position().toPoint()))
    setExpanded(!expanded_);
  // Reading the card must not draw, crop, pan, or select a layer underneath.
  event->accept();
}

void ShortcutGuide::mouseMoveEvent(QMouseEvent *event) {
  const bool hovered = toggleRect().contains(event->position().toPoint());
  if (hovered != hovered_) {
    hovered_ = hovered;
    update(toggleRect());
  }
  const Qt::CursorShape shape = hovered ? Qt::PointingHandCursor : Qt::ArrowCursor;
  if (cursor().shape() != shape)
    setCursor(shape);
  event->accept();
}

void ShortcutGuide::leaveEvent(QEvent *event) {
  hovered_ = false;
  update(toggleRect());
  QWidget::leaveEvent(event);
}

void ShortcutGuide::wheelEvent(QWheelEvent *event) {
  const int delta = event->angleDelta().y() != 0 ? event->angleDelta().y()
                                               : event->pixelDelta().y();
  if (expanded_ && delta != 0) {
    firstRow_ = std::clamp(firstRow_ + (delta > 0 ? -3 : 3), 0,
                           static_cast<int>(entries_.size()) - visibleRows_);
    update();
  }
  event->accept();
}
