/** @fileoverview Nonblocking Save As for overlay and windowed editors. */
#include "editor.hpp"
#include "capture.hpp"
#include "output-config.hpp"
#include "overlay-chrome.hpp"
#include "pin.hpp"
#include "recent-snaps.hpp"
#include "pin-file.hpp"

#include <LayerShellQt/Window>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QObject>
#include <QProcess>
#include <QPromise>
#include <QString>
#include <QTimer>
#include <Qt>
#include <QtTypes>
#include <QtLogging>
#include <QtConcurrent/QtConcurrentRun>

namespace {
void prepareSaveAsWindow() {
  // The overlay has no xdg-shell parent for its chooser. Without a rule,
  // Hyprland tiles the independent dialog and rearranges the user's windows.
  // Register before mapping, on the same worker that prepares the filename.
  QProcess process;
  process.start(QStringLiteral("hyprctl"),
                {QStringLiteral("eval"),
                 QStringLiteral("hl.window_rule({ name = \"omasnap-save-as\", "
                                "match = { class = \"^omasnap$\", "
                                "title = \"^Save screenshot as$\" }, "
                                "float = true, center = true, opacity = 1 })")});
  if (!process.waitForFinished(500)) {
    process.kill();
    process.waitForFinished(500);
    qWarning("Could not configure the floating Save As window");
  } else if (process.exitStatus() != QProcess::NormalExit ||
             process.exitCode() != 0 ||
             process.readAllStandardOutput().contains("error")) {
    qWarning("Could not configure the floating Save As window");
  }
}
} // namespace

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
  const bool wayland = QGuiApplication::platformName() == QStringLiteral("wayland");
  watcher->setFuture(QtConcurrent::run([appSlug, directory = saveAsDirectory_, wayland] {
    if (wayland)
      prepareSaveAsWindow();
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
  dialog->setScreen(screen());
  dialog->setFont(chromeFont(13));
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
  connect(watcher, &QFutureWatcher<QString>::resultReadyAt, this,
          [this, watcher, request] {
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
    close();
  });
  saveAsDirectory_ = QFileInfo(path).absolutePath();
  saveAsFuture_ = QtConcurrent::run(
      [capture = capture_, selection = selection_, annotations = annotations_,
       background = backgroundStyle_, shadow = imageShadow_,
       boundary = canvasBoundaryMode_, backdrop = customBackdrop_,
       source = pristineSource_, log = currentOperationLog(),
       previous = editingRecent_, document = pinDocument_,
       launcher = processLauncher_, path](QPromise<QString> &completion) {
    RecentSnapWriter recent(log.recentId);
    const QImage image = matchOutputToLogicalSize(
        capture,
        renderCapture(capture, selection, annotations, background, shadow,
                     boundary, backdrop),
        loadOutputConfig(defaultConfigPath()).matchLogicalSize);
    QString error;
    if (savePngFile(image, path, error)) {
      const QString preview = launchPinnedCapture(
          image, renderedCaptureLogicalSize(capture, image.size()), false,
          PinLifetime::Timed, error, launcher, log.recentId, path);
      if (!preview.isEmpty()) {
        if (document)
          document->finishSavedPreview(log, path);
      } else {
        error = QStringLiteral("Saved to %1, but could not show its preview: %2")
                    .arg(path, error);
      }
    }
    // Close as soon as the saved preview is ready; retain the editable source
    // and log afterward, just as other completed captures do.
    completion.addResult(error);
    QString recentError;
    if (!recent.record(source, log, image, recentError,
                       previous ? &*previous : nullptr))
      qWarning("Could not retain Save As capture: %s", qPrintable(recentError));
  });
  watcher->setFuture(saveAsFuture_);
}
