/** @fileoverview Save As preserves the editor and atomically exports PNG. */
#include "save-as-smoke.hpp"
#include "capture.hpp"
#include "editor.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QThread>
#include <QtTest/QTest>

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
  QTemporaryDir directory;
  if (!directory.isValid()) {
    error = QStringLiteral("Could not create Save As test directory");
    return false;
  }
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

  CaptureData capture;
  capture.monitor.geometry = QRect(0, 0, 800, 600);
  capture.monitor.pixelSize = QSize(800, 600);
  capture.monitor.scale = 1;
  capture.source = QImage(800, 600, QImage::Format_ARGB32);
  capture.source.fill(QColor(QStringLiteral("#354f68")));
  capture.previewSize = capture.source.size();
  for (const bool windowed : {false, true}) {
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::File);
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
    QSignalSpy closed(qApp, &QGuiApplication::lastWindowClosed);
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
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_S,
                     Qt::ControlModifier | Qt::ShiftModifier);
    if (!waitUntil([] { return saveDialog() != nullptr; })) {
      error = QStringLiteral("Second Save As did not open");
      return false;
    }
    dialog = saveDialog();
    const QString output = directory.filePath(
        windowed ? QStringLiteral("window.png") : QStringLiteral("overlay.png"));
    // Enter the path as a user does; selectFile() can leave the old name in
    // the visible chooser while its asynchronous directory model reloads.
    auto *filename = dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
    if (!filename) {
      error = QStringLiteral("Save chooser has no filename input");
      dialog->reject();
      return false;
    }
    filename->selectAll();
    QTest::keyClicks(filename, output);
    QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
    if (!waitUntil([&] { return !editor.busy_; }) || !editor.isVisible() ||
        editor.textEditing() || QImage(output).convertToFormat(QImage::Format_ARGB32) !=
            editor.renderCurrentOutput().convertToFormat(QImage::Format_ARGB32) ||
        editor.saveAsDirectory_ != directory.path()) {
      error = QStringLiteral("Save As did not export the draft and keep editing: "
                             "visible=%1 draft=%2 busy=%3 output=%4x%5 directory=%6 status=%7")
                  .arg(editor.isVisible()).arg(editor.textEditing()).arg(editor.busy_)
                  .arg(QImage(output).width()).arg(QImage(output).height())
                  .arg(editor.saveAsDirectory_, editor.statusForTest());
      return false;
    }
    const int beforeNudge = editor.operationIndex();
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
    editor.saveAsToPath(directory.filePath(QStringLiteral("missing/file.png")));
    if (!waitUntil([&] { return !editor.busy_; }) || !editor.isVisible() ||
        editor.operationIndex() != savedIndex ||
        !editor.statusForTest().contains(QStringLiteral("Could not"))) {
      error = QStringLiteral("Failed Save As changed or closed the editor");
      return false;
    }
    editor.close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  }
  return true;
}
