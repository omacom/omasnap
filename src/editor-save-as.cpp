/** @fileoverview Nonblocking Save As for overlay and windowed editors. */
#include "editor.hpp"
#include "overlay-chrome.hpp"

#include <LayerShellQt/Window>
#include <QCloseEvent>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

void CaptureEditor::cancelSaveAs() {
  // QObject stays alive after close(), and destruction can drain queued events.
  // Invalidate every continuation before closing the independent chooser.
  ++saveAsRequest_;
  if (saveAsActive_) {
    saveAsActive_ = false;
    busy_ = false;
  }
  if (saveAsDialog_) {
    saveAsDialog_->reject();
    saveAsDialog_->deleteLater();
    saveAsDialog_.clear();
  }
}

void CaptureEditor::closeEvent(QCloseEvent *event) {
  cancelSaveAs();
  QWidget::closeEvent(event);
}

void CaptureEditor::saveAs() {
  if (busy_ || dragging_ || panning_ || configuredCustomDefaultPending_ ||
      phase_ != Phase::Edit || selection_.isEmpty())
    return;
  busy_ = true;
  saveAsActive_ = true;
  const quint64 request = ++saveAsRequest_;
  setStatus(QStringLiteral("Choosing a save destination…"));
  const QString appSlug =
      appFilenameSlug(dominantAppClass(capture_.windows, selection_));
  auto *watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, request] {
    const QString suggested = watcher->result();
    watcher->deleteLater();
    if (request == saveAsRequest_)
      showSaveAsDialog(suggested);
  });
  watcher->setFuture(QtConcurrent::run([appSlug, directory = saveAsDirectory_] {
    const QString suggested = suggestedScreenshotPath(appSlug);
    return directory.isEmpty()
               ? suggested
               : QDir(directory).filePath(QFileInfo(suggested).fileName());
  }));
}

void CaptureEditor::showSaveAsDialog(const QString &suggested) {
  // A normal dialog cannot appear above an exclusive layer-shell overlay.
  // Keep it independent of that hidden widget so Wayland can map/focus it.
  auto *dialog = new QFileDialog(windowedPresentation_ ? this : nullptr,
                                 QStringLiteral("Save screenshot as"), suggested,
                                 QStringLiteral("PNG image (*.png)"));
  saveAsDialog_ = dialog;
  const quint64 request = saveAsRequest_;
  dialog->setObjectName(QStringLiteral("omasnap-save-as"));
  dialog->setFont(chromeFont(13));
  dialog->setStyleSheet(QStringLiteral(
      "QWidget { background: #18181c; color: #f5f5f7; }"
      "QLineEdit, QAbstractItemView { background: #25252b; "
      "selection-background-color: #3b82f6; selection-color: #ffffff; }"
      "QPushButton, QToolButton, QComboBox { background: #303038; "
      "border: 1px solid #56565f; border-radius: 4px; padding: 5px; }"
      "QPushButton:hover, QToolButton:hover { background: #41414b; }"
      "QPushButton:default { border-color: #60a5fa; }"
      "QWidget:disabled { color: #858590; }"));
  dialog->resize(760, 520);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  // The overlay is hidden until the chooser has finished closing.
  dialog->setAttribute(Qt::WA_QuitOnClose, false);
  dialog->setOption(QFileDialog::DontUseNativeDialog);
  dialog->setAcceptMode(QFileDialog::AcceptSave);
  dialog->setFileMode(QFileDialog::AnyFile);
  dialog->setDefaultSuffix(QStringLiteral("png"));
  dialog->setWindowModality(Qt::ApplicationModal);
  connect(this, &QObject::destroyed, dialog, &QObject::deleteLater);
  connect(dialog, &QDialog::finished, this, [this, dialog, request](int result) {
    if (request != saveAsRequest_)
      return;
    const QString path = result == QDialog::Accepted
                             ? dialog->selectedFiles().value(0)
                             : QString();
    // The chooser's xdg-shell unmap and modal focus teardown must complete
    // before the exclusive layer returns, otherwise Hyprland can hand focus
    // back to the underlying client after the overlay has already mapped.
    connect(dialog, &QObject::destroyed, this, [this, path, request] {
      QTimer::singleShot(0, this, [this, path, request] {
        if (request != saveAsRequest_)
          return;
        if (!windowedPresentation_) {
          if (layer_)
            layer_->setKeyboardInteractivity(
                LayerShellQt::Window::KeyboardInteractivityExclusive);
          show();
        }
        raise();
        activateWindow();
        restoreSaveAsFocus();
        if (path.isEmpty()) {
          saveAsActive_ = false;
          busy_ = false;
          setStatus(QStringLiteral("Save As cancelled"));
          return;
        }
        // The chooser's filter/default suffix does not prevent an explicit
        // .jpg name. Never write PNG bytes under another format's extension.
        if (QFileInfo(path).suffix().compare(QStringLiteral("png"),
                                            Qt::CaseInsensitive) != 0) {
          saveAsActive_ = false;
          busy_ = false;
          setStatus(QStringLiteral("Save As only writes PNG; choose a .png filename"));
          return;
        }
        // Only accepting output commits a text draft; cancelling the chooser
        // preserves both its content and the operation-log cursor.
        if (textEditing())
          acceptText();
        saveAsToPath(path);
      });
    });
    dialog->deleteLater();
  });
  if (!windowedPresentation_) {
    if (layer_)
      layer_->setKeyboardInteractivity(
          LayerShellQt::Window::KeyboardInteractivityNone);
    hide();
  }
  dialog->open();
}

void CaptureEditor::saveAsToPath(const QString &path) {
  if (dragging_ || panning_ || configuredCustomDefaultPending_)
    return;
  endNudgeRun();
  busy_ = true;
  saveAsActive_ = true;
  const quint64 request = saveAsRequest_;
  setStatus(QStringLiteral("Saving screenshot…"));
  auto *watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcher<QString>::finished, this,
          [this, watcher, path, request] {
    const QString error = watcher->result();
    watcher->deleteLater();
    if (request != saveAsRequest_)
      return;
    saveAsActive_ = false;
    busy_ = false;
    if (!error.isEmpty()) {
      setStatus(error);
      return;
    }
    saveAsDirectory_ = QFileInfo(path).absolutePath();
    setStatus(QStringLiteral("Saved to %1").arg(path));
  });
  watcher->setFuture(QtConcurrent::run(
      [capture = capture_, selection = selection_, annotations = annotations_,
       background = backgroundStyle_, shadow = imageShadow_,
       boundary = canvasBoundaryMode_, backdrop = customBackdrop_, path] {
    const QImage image = renderCapture(capture, selection, annotations,
                                       background, shadow, boundary, backdrop);
    QString error;
    static_cast<void>(savePngFile(image, path, error));
    return error;
  }));
}
