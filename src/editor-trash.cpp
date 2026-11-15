/** @fileoverview Trash the current capture's saved image without blocking input. */
#include "editor.hpp"
#include "pin-file.hpp"
#include "recent-snaps.hpp"

#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

void CaptureEditor::confirmTrash() {
  if (busy_ || dragging_ || panning_ || configuredCustomDefaultPending_ || outputPath_.isEmpty())
    return;
  endNudgeRun();
  confirmingTrash_ = true;
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
