/** @fileoverview Trash the current capture's saved image without blocking input. */
#include "editor.hpp"
#include "chrome-theme.hpp"
#include "icons.hpp"
#include "overlay-chrome.hpp"
#include <QPainter>
#include <QFontMetrics>
#include <algorithm>
#include "pin-file.hpp"
#include "recent-snaps.hpp"

#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

void CaptureEditor::confirmTrash() {
  if (busy_ || dragging_ || panning_ || configuredCustomDefaultPending_ || outputPath_.isEmpty())
    return;
  if (textEditing())
    acceptText();
  endNudgeRun();
  confirmingTrash_ = true;
  trashCancelFocused_ = false;
  textSizeMenuOpen_ = false;
  colorPaletteOpen_ = false;
  shapeMenuOpen_ = false;
  customColorPickerOpen_ = false;
  setStatus(QStringLiteral("Move %1 to Trash? Enter confirms · Esc cancels · Unsaved edits will be discarded")
                .arg(outputPath_));
  update();
}

void CaptureEditor::trashScreenshot() {
  if (busy_ || !confirmingTrash_ || outputPath_.isEmpty())
    return;
  confirmingTrash_ = false;
  busy_ = true;
  setEnabled(false);
  snapshotDirty_ = false;
  setStatus(QStringLiteral("Moving screenshot to Trash…"));
  auto *watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
    const QString error = watcher->result();
    watcher->deleteLater();
    busy_ = false;
    setEnabled(true);
    if (error.isEmpty())
      close(); // Do not put the deleted capture back on the recents shelf.
    else {
      scheduleSnapshot();
      setStatus(error);
    }
  });
  // Reuse the dismissal future so destruction drains this worker, too.
  dismissFuture_ = QtConcurrent::run(
      [path = outputPath_, id = recentId_, document = pinDocument_,
       log = currentOperationLog()] {
    // Resolve the pending shelf write before touching the saved file. Failure
    // keeps both the file and editable document available for a retry.
    QString error;
    const auto recent = findRecentSnap(id, &error);
    if (!error.isEmpty())
      return error;
    QFile file(path);
    if (!QFileInfo(path).isFile() || !file.moveToTrash())
      return QStringLiteral("Could not move %1 to Trash. The editor is still open; check the file and Trash permissions.")
          .arg(path);
    if (recent)
      removeRecentSnap(*recent);
    // Retire an originating preview through its existing document watcher.
    if (document)
      document->finishSavedPreview(log, path);
    return QString();
  });
  watcher->setFuture(dismissFuture_);
}

QRectF CaptureEditor::trashDialogRect() const {
  const qreal dialogWidth = std::min<qreal>(480, width() - 32);
  constexpr qreal dialogHeight = 236;
  return QRectF((width() - dialogWidth) / 2.0,
                (height() - dialogHeight) / 2.0, dialogWidth, dialogHeight);
}

void CaptureEditor::paintTrashConfirmation(QPainter &painter) {
  painter.save();
  const ChromeTheme &theme = chromeTheme();
  const QRectF panel = trashDialogRect();
  painter.fillRect(rect(), chromeAlpha(theme.scrim, 180));
  painter.setPen(QPen(theme.panelBorder.brush(panel), 1));
  painter.setBrush(theme.surface);
  painter.drawRoundedRect(panel, 11, 11);

  const qreal left = panel.left() + 24;
  const qreal contentWidth = panel.width() - 48;
  painter.setFont(chromeFont(20, true));
  painter.setPen(theme.foreground);
  painter.drawText(QRectF(left, panel.top() + 24, contentWidth, 28),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("Move screenshot to Trash?"));

  painter.setFont(chromeFont(13, true));
  const QString filename = QFileInfo(outputPath_).fileName();
  const QString visibleName = QFontMetrics(painter.font()).elidedText(
      filename, Qt::ElideMiddle, static_cast<int>(contentWidth));
  painter.drawText(QRectF(left, panel.top() + 64, contentWidth, 22),
                   Qt::AlignLeft | Qt::AlignVCenter, visibleName);
  painter.setFont(chromeFont(14));
  painter.setPen(theme.muted);
  painter.drawText(QRectF(left, panel.top() + 94, contentWidth, 22),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("Unsaved edits will be discarded."));

  // The theme's own red, darkened on hover, so the action follows Omarchy.
  const QColor destructive = theme.destructive;
  const QColor destructiveHover = theme.destructive.darker(115);
  const QColor destructiveText = theme.destructiveText;
  painter.setFont(chromeFont(14, true));
  for (const ToolbarButton &button : toolbarButtons()) {
    const bool confirm = button.action == QStringLiteral("trash-confirm");
    const bool hover = button.rect.contains(cursor_);
    const bool focused = confirm != trashCancelFocused_;
    painter.setBrush(confirm ? (hover ? destructiveHover : destructive)
                             : (hover ? theme.buttonHover : theme.button));
    painter.setPen(QPen((hover ? theme.buttonHoverBorder : theme.buttonBorder)
                            .brush(button.rect), 1));
    painter.drawRoundedRect(button.rect, 9, 9);
    if (focused) {
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(theme.foreground, 1));
      painter.drawRoundedRect(button.rect.adjusted(-3, -3, 3, 3), 12, 12);
    }
    const QColor text = confirm ? destructiveText
                               : (hover ? theme.buttonHoverText : theme.buttonText);
    painter.setPen(text);
    if (confirm) {
      const qreal textWidth = QFontMetrics(painter.font()).horizontalAdvance(button.label);
      const qreal groupLeft = button.rect.center().x() - (textWidth + 28) / 2;
      drawToolbarIcon(painter, QRectF(groupLeft, button.rect.center().y() - 10, 20, 20),
                      QStringLiteral("trash"), {}, text);
      painter.drawText(QRectF(groupLeft + 28, button.rect.top(), textWidth, button.rect.height()),
                       Qt::AlignCenter, button.label);
    } else {
      painter.drawText(button.rect, Qt::AlignCenter, button.label);
    }
  }
  painter.setFont(chromeFont(12));
  painter.setPen(theme.muted);
  painter.drawText(QRectF(left, panel.bottom() - 38, contentWidth, 18),
                   Qt::AlignCenter, QStringLiteral("Enter to confirm · Esc to cancel"));
  painter.restore();
}
