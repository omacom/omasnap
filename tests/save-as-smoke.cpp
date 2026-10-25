/** @fileoverview Save As atomically exports PNG and returns a saved preview. */
#include "save-as-smoke.hpp"
#include "capture.hpp"
#include "editor.hpp"
#include "recent-snaps.hpp"
#include "pin-file.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QColor>
#include <QCoreApplication>
#include <QEvent>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QImage>
#include <QMetaObject>
#include <QObject>
#include <QRectF>
#include <QScopeGuard>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <QWindow>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QThread>
#include <Qt>
#include <QtTypes>
#include <QtCore/qtenvironmentvariables.h>
#include <QtTest/qtestkeyboard.h>

#include <memory>

namespace {
template <typename Predicate> bool waitUntil(Predicate ready) {
  QElapsedTimer timer;
  timer.start();
  while (!ready() && timer.elapsed() < 3000) {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QThread::msleep(1);
  }
  return ready();
}
QFileDialog *saveDialog() {
  for (QWidget *widget : QApplication::topLevelWidgets()) {
    if (widget->objectName() == QStringLiteral("omasnap-save-as"))
      return qobject_cast<QFileDialog *>(widget);
  }
  return nullptr;
}
} // namespace

bool runSaveAsSmoke(QString &error) {
  const QTemporaryDir directory;
  if (!directory.isValid()) {
    error = QStringLiteral("Could not create Save As test directory");
    return false;
  }
  const QByteArray previousRecents = qgetenv("OMASNAP_RECENT_DIR");
  const auto restoreRecents = qScopeGuard([&] {
    if (previousRecents.isNull())
      qunsetenv("OMASNAP_RECENT_DIR");
    else
      qputenv("OMASNAP_RECENT_DIR", previousRecents);
  });
  qputenv("OMASNAP_RECENT_DIR", directory.filePath(QStringLiteral("recent")).toUtf8());
  const QString target = directory.filePath(QStringLiteral("chosen.png"));
  QImage original(100, 80, QImage::Format_ARGB32);
  original.fill(Qt::red);
  if (!savePngFile(original, target, error))
    return false;
  QImage replacement(100, 80, QImage::Format_ARGB32);
  replacement.fill(Qt::blue);
  if (!savePngFile(replacement, target, error) || QImage(target).convertToFormat(QImage::Format_ARGB32) != replacement) {
    error = QStringLiteral("Save As did not atomically replace existing PNG");
    return false;
  }
  if (savePngFile({}, target, error) || error.isEmpty() ||
      QImage(target).convertToFormat(QImage::Format_ARGB32) != replacement) {
    error = QStringLiteral("Failed Save As damaged the existing file");
    return false;
  }
  if (savePngFile(original, directory.path(), error) || error.isEmpty()) {
    error = QStringLiteral("Save As accepted a directory as its destination");
    return false;
  }
  error.clear();

  QStringList previews;
  const auto cleanupPreviews = qScopeGuard([&] {
    for (const QString &path : previews) {
      const PinSnapshotFile owned(path);
    }
  });
  const auto preparePreview = [&](CaptureEditor &editor) {
    editor.setProcessLauncherForTest([&](const QString &, const QStringList &args) {
      if (args.size() != 2 || args.first() != QStringLiteral("--preview"))
        return false;
      previews.push_back(args.last());
      return true;
    });
  };
  CaptureData capture;
  capture.monitor.geometry = QRect(0, 0, 800, 600);
  capture.monitor.pixelSize = QSize(1600, 1200);
  capture.monitor.scale = 2;
  capture.source = QImage(1600, 1200, QImage::Format_ARGB32);
  capture.source.fill(QColor(QStringLiteral("#354f68")));
  capture.previewSize = capture.monitor.geometry.size();
  for (const bool windowed : {false, true}) {
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::File);
    preparePreview(editor);
    editor.setWindowedPresentation(windowed);
    editor.resize(800, 600);
    editor.show();
    QCoreApplication::processEvents();
    editor.beginText(QPointF(120, 160));
    auto *textEditor = editor.findChild<QPlainTextEdit *>();
    if (!textEditor) {
      error = QStringLiteral("Save As fixture has no text editor");
      return false;
    }
    textEditor->setPlainText(QStringLiteral("Keep this draft"));
    textEditor->setFocus();
    const int before = editor.operationIndex();
    const QRectF selection = editor.currentSelection();
    const qreal zoom = editor.viewZoom_;
    const QSignalSpy closed(qApp, &QGuiApplication::lastWindowClosed);
    QTest::keyClick(textEditor, Qt::Key_S,
                     Qt::ControlModifier | Qt::ShiftModifier);
    if (!waitUntil([] { return saveDialog() != nullptr; })) {
      error = QStringLiteral("Ctrl+Shift+S did not open the save chooser");
      return false;
    }
    auto *dialog = saveDialog();
    if (editor.isVisible() != windowed || !editor.busy_) {
      error = QStringLiteral("Save As did not suspend the correct presentation");
      dialog->reject();
      return false;
    }
    dialog->reject();
    if (!waitUntil([&] {
          return !editor.busy_ && editor.isVisible() && textEditor->hasFocus();
        }) ||
        closed.count() != 0 || !editor.textEditing() ||
        textEditor->toPlainText() != QStringLiteral("Keep this draft") ||
        editor.operationIndex() != before || editor.currentSelection() != selection ||
        editor.viewZoom_ != zoom) {
      error = QStringLiteral("Cancelling Save As changed the draft or document");
      return false;
    }
    // Drain deletion before looking for the next dialog.
    if (!waitUntil([] { return saveDialog() == nullptr; })) {
      error = QStringLiteral("Cancelled save chooser was not disposed");
      return false;
    }
    // QFileDialog accepts explicit non-PNG suffixes despite its name filter.
    // Even an approved overwrite must not replace that file with PNG bytes.
    const QString wrongFormat = directory.filePath(QStringLiteral("keep.jpg"));
    QFile existing(wrongFormat);
    if (!existing.open(QIODevice::WriteOnly) || existing.write("keep original") != 13) {
      error = QStringLiteral("Could not prepare Save As format fixture");
      return false;
    }
    existing.close();
    editor.saveAs();
    if (!waitUntil([] { return saveDialog() != nullptr; })) {
      error = QStringLiteral("Format fixture could not open Save As");
      return false;
    }
    dialog = saveDialog();
    dialog->setOption(QFileDialog::DontConfirmOverwrite);
    dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"))->setText(wrongFormat);
    QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
    if (!waitUntil([&] { return !editor.busy_; }) || !editor.textEditing() ||
        textEditor->toPlainText() != QStringLiteral("Keep this draft") ||
        editor.operationIndex() != before ||
        !editor.statusForTest().contains(QStringLiteral("choose a .png")) ||
        !existing.open(QIODevice::ReadOnly) || existing.readAll() != "keep original") {
      error = QStringLiteral("Non-PNG Save As changed its destination or draft");
      return false;
    }
    existing.close();
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_S,
                     Qt::ControlModifier | Qt::ShiftModifier);
    if (!waitUntil([] { return saveDialog() != nullptr; })) {
      error = QStringLiteral("Second Save As did not open");
      return false;
    }
    dialog = saveDialog();
    const QString output = directory.filePath(
        windowed ? QStringLiteral("window.png") : QStringLiteral("overlay.PNG"));
    // Enter the path as a user does; selectFile() can leave the old name in
    // the visible chooser while its asynchronous directory model reloads.
    auto *filename = dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
    if (!filename) {
      error = QStringLiteral("Save chooser has no filename input");
      dialog->reject();
      return false;
    }
    filename->selectAll();
    QTest::keyClicks(filename, windowed ? output.chopped(4) : output);
    QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
    if (!waitUntil([&] { return !editor.busy_; }) || editor.isVisible() ||
        editor.textEditing() || QImage(output).convertToFormat(QImage::Format_ARGB32) !=
            editor.renderCurrentOutput().convertToFormat(QImage::Format_ARGB32) ||
        editor.saveAsDirectory_ != directory.path()) {
      error = QStringLiteral("Save As did not export the draft and close: "
                             "visible=%1 draft=%2 busy=%3 output=%4x%5 directory=%6 status=%7")
                  .arg(editor.isVisible()).arg(editor.textEditing()).arg(editor.busy_)
                  .arg(QImage(output).width()).arg(QImage(output).height())
                  .arg(editor.saveAsDirectory_, editor.statusForTest());
      return false;
    }
    const int beforeNudge = editor.operationIndex();
    CaptureData reopened;
    describeFileCapture(reopened, QImage(output), {});
    if (reopened.previewSize != (QSizeF(reopened.source.size()) / 2).toSize()) {
      error = QStringLiteral("Save As lost the capture's logical size in its standalone PNG");
      return false;
    }
    const auto recent = findRecentSnap(editor.recentId_, &error);
    const qsizetype recentCount = listRecentSnaps(false).size();
    OperationLog savedLog;
    if (!recent || !loadOperationLog(recent->logPath, savedLog, error) ||
        savedLog.index != beforeNudge || savedLog.ops.size() != beforeNudge ||
        QImage(recent->sourcePath).convertToFormat(QImage::Format_ARGB32) != capture.source) {
      error = QStringLiteral("Save As did not retain its editable capture");
      return false;
    }
    OperationLog previewLog;
    if (previews.isEmpty() ||
        !loadOperationLog(operationLogPath(previews.last()), previewLog, error) ||
        previewLog.savedPath != output || previewLog.recentId != savedLog.recentId ||
        QImage(previews.last()) != QImage(output)) {
      error = QStringLiteral("Saved preview lost its file, pixels, or capture identity");
      return false;
    }
    editor.show();
    const auto document = copyPinDocument(previews.last(), error);
    if (!document)
      return false;
    editor.setPinDocument(document);
    editor.selectedAnnotation_ = 0;
    editor.selectedAnnotations_ = {0};
    editor.nudgeSelectedAnnotation(QPointF(3, 0));
    const QString nudged = directory.filePath(
        windowed ? QStringLiteral("window-nudged.png") : QStringLiteral("overlay-nudged.png"));
    editor.saveAsToPath(nudged);
    if (!waitUntil([&] { return !editor.busy_; }) ||
        editor.operationIndex() != beforeNudge + 1 ||
        QImage(nudged).convertToFormat(QImage::Format_ARGB32) !=
            editor.renderCurrentOutput().convertToFormat(QImage::Format_ARGB32)) {
      error = QStringLiteral("Save As did not commit an immediate nudge");
      return false;
    }
    const int savedIndex = editor.operationIndex();
    OperationLog completedDocument;
    if (!loadOperationLog(operationLogPath(document->path()), completedDocument, error) ||
        completedDocument.savedPath != nudged || editor.isVisible()) {
      error = QStringLiteral("Save As did not retire the originating preview and close");
      return false;
    }
    const auto updatedRecent = findRecentSnap(editor.recentId_, &error);
    if (!updatedRecent ||
        !loadOperationLog(updatedRecent->logPath, savedLog, error) ||
        savedLog.index != savedIndex || savedLog.recentId != editor.recentId_ ||
        listRecentSnaps(false).size() != recentCount || QFile::exists(recent->sourcePath)) {
      error = QStringLiteral("Repeated Save As did not update the same recent capture");
      return false;
    }
    editor.show();
    editor.setPinDocument({});
    editor.saveAsToPath(directory.filePath(QStringLiteral("missing/file.png")));
    if (!waitUntil([&] { return !editor.busy_; }) || !editor.isVisible() ||
        editor.operationIndex() != savedIndex ||
        !editor.statusForTest().contains(QStringLiteral("Could not"))) {
      error = QStringLiteral("Failed Save As changed or closed the editor");
      return false;
    }
    const QString failedPreview = directory.filePath(QStringLiteral("preview-failed.png"));
    editor.setProcessLauncherForTest([](const QString &, const QStringList &) { return false; });
    editor.saveAsToPath(failedPreview);
    if (!waitUntil([&] { return !editor.busy_; }) || !editor.isVisible() ||
        QImage(failedPreview).isNull() ||
        !editor.statusForTest().contains(QStringLiteral("could not show its preview"))) {
      error = QStringLiteral("A failed preview launch lost its saved PNG or closed the editor");
      return false;
    }
    editor.close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  }
  // Under --notify, Save As announces the chosen file instead of previewing it.
  {
    qputenv("OMASNAP_NOTIFY", "1");
    const auto restoreNotify = qScopeGuard([] { qunsetenv("OMASNAP_NOTIFY"); });
    // Keep the wording English whatever this desktop's language is.
    QFile i18n(directory.filePath(QStringLiteral("omarchy-i18n")));
    if (!i18n.open(QIODevice::WriteOnly) ||
        i18n.write("#!/bin/sh\nprintf '%s' \"$1\"\n") < 0 ||
        !i18n.setPermissions(QFileDevice::ReadOwner | QFileDevice::ExeOwner))
      return false;
    i18n.close();
    const QByteArray oldPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", oldPath); });
    qputenv("PATH", directory.path().toUtf8() + ':' + oldPath);
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::File);
    QStringList toast;
    bool previewed = false;
    editor.setProcessLauncherForTest([&](const QString &program, const QStringList &args) {
      if (program == QStringLiteral("omarchy-notification-send"))
        toast = args;
      else
        previewed = true;
      return true;
    });
    editor.show();
    const QString output = directory.filePath(QStringLiteral("notified.png"));
    editor.saveAsToPath(output);
    if (!waitUntil([&] { return !editor.busy_; }) || editor.isVisible() || previewed ||
        toast.isEmpty() || toast.last() != output || QImage(output).isNull()) {
      error = QStringLiteral("Save As under --notify did not announce its file: %1")
                  .arg(toast.join(QStringLiteral(" | ")));
      return false;
    }
  }
  {
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::File);
    editor.show();
    // A chooser must not steal the mouse release that commits a live gesture,
    // or export before the configured backdrop has established the document.
    for (bool *pending : {&editor.dragging_, &editor.panning_,
                          &editor.configuredCustomDefaultPending_}) {
      *pending = true;
      editor.saveAs();
      if (editor.busy_ || editor.saveAsActive_) {
        error = QStringLiteral("Save As interrupted an unfinished edit");
        return false;
      }
      *pending = false;
    }
    editor.close();
  }
  // A finished worker may still have its GUI completion queued when close
  // arrives. Closing must invalidate that completion, even without destruction.
  for (const bool windowed : {false, true}) {
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::File);
    preparePreview(editor);
    editor.setWindowedPresentation(windowed);
    editor.show();
    editor.saveAs();
    for (auto *watcher : editor.findChildren<QFutureWatcherBase *>())
      watcher->waitForFinished();
    editor.close();
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    if (saveDialog() || editor.isVisible()) {
      if (auto *dialog = saveDialog())
        dialog->reject();
      error = QStringLiteral("Closing during Save As preparation reopened UI");
      return false;
    }
    // Close while the chooser is visible, then after its rejection but before
    // deferred deletion/restoration. Neither route may resurrect the editor.
    for (const bool rejectFirst : {false, true}) {
      editor.show();
      editor.saveAs();
      if (!waitUntil([] { return saveDialog() != nullptr; })) {
        error = QStringLiteral("Close-race fixture could not open Save As");
        return false;
      }
      if (rejectFirst)
        saveDialog()->reject();
      editor.close();
      if (!waitUntil([] { return saveDialog() == nullptr; })) {
        error = QStringLiteral("Closing editor left an orphan save chooser");
        return false;
      }
      QCoreApplication::processEvents();
      if (editor.isVisible()) {
        error = QStringLiteral("Closing during Save As cancellation reopened editor");
        return false;
      }
    }
  }
  // Compositor close now routes through the editor's dismissal path. It must
  // still cancel Save As before that path checks whether the editor is busy.
  {
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::File);
    editor.setWindowedPresentation(true);
    editor.show();
    editor.saveAs();
    if (!waitUntil([] { return saveDialog() != nullptr; })) {
      error = QStringLiteral("Compositor-close fixture could not open Save As");
      return false;
    }
    QCloseEvent close;
    QCoreApplication::sendEvent(editor.windowHandle(), &close);
    if (!waitUntil([&] { return !editor.isVisible() && !saveDialog(); })) {
      error = QStringLiteral("Compositor close left Save As or its editor open");
      return false;
    }
  }
  {
    auto editor = std::make_unique<CaptureEditor>(capture, CaptureEditor::CaptureMode::File);
    editor->show();
    editor->saveAs();
    for (auto *watcher : editor->findChildren<QFutureWatcherBase *>())
      watcher->waitForFinished();
    editor.reset();
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    if (saveDialog()) {
      saveDialog()->reject();
      error = QStringLiteral("Destroyed editor left a save chooser");
      return false;
    }
    // Accepted output owns copied pixels/state. Destruction drains it before
    // application teardown, even with its GUI completion still pending.
    editor = std::make_unique<CaptureEditor>(capture, CaptureEditor::CaptureMode::File);
    const QString output = directory.filePath(QStringLiteral("destroyed.png"));
    preparePreview(*editor);
    editor->saveAsToPath(output);
    editor.reset();
    if (QImage(output).isNull()) {
      error = QStringLiteral("Destroying editor interrupted accepted Save As output");
      return false;
    }
  }
  return true;
}
