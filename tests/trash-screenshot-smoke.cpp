#include "trash-screenshot-smoke.hpp"
#include "capture.hpp"
#include "editor.hpp"
#include "recent-snaps.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QtTest/QTest>

bool runTrashScreenshotSmoke(QString &error) {
  // Put both the fixture and FreeDesktop Trash on the home filesystem, so
  // /tmp on another mount cannot turn this into a cross-device trash test.
  const QTemporaryDir directory(QDir::homePath() + QStringLiteral("/.omasnap-trash-test-XXXXXX"));
  if (!directory.isValid())
    return false;
  const QByteArray oldData = qgetenv("XDG_DATA_HOME");
  const QByteArray oldRecent = qgetenv("OMASNAP_RECENT_DIR");
  const auto restore = qScopeGuard([&] {
    if (oldData.isNull())
      qunsetenv("XDG_DATA_HOME");
    else
      qputenv("XDG_DATA_HOME", oldData);
    if (oldRecent.isNull())
      qunsetenv("OMASNAP_RECENT_DIR");
    else
      qputenv("OMASNAP_RECENT_DIR", oldRecent);
  });
  qputenv("XDG_DATA_HOME", directory.path().toUtf8());
  qputenv("OMASNAP_RECENT_DIR", directory.filePath(QStringLiteral("recent")).toUtf8());
  CaptureData capture;
  capture.source = QImage(200, 150, QImage::Format_RGB32);
  capture.source.fill(Qt::cyan);
  capture.previewSize = capture.source.size();
  capture.monitor.geometry = QRect(0, 0, 1000, 700);
  OperationLog log;
  log.recentId = QUuid::createUuid().toString(QUuid::Id128);
  log.outputPath = directory.filePath(QStringLiteral("it's a screenshot.png"));
  if (!capture.source.save(log.outputPath) ||
      !recordRecentSnap(capture.source, log, capture.source, error))
    return false;
  const auto recent = findRecentSnap(log.recentId);
  OperationLog restored;
  if (!recent || !loadOperationLog(recent->logPath, restored, error) ||
      restored.outputPath != log.outputPath) {
    error = QStringLiteral("Saved screenshot path did not survive the recent shelf");
    return false;
  }
  CaptureEditor editor(capture, CaptureEditor::CaptureMode::File,
                       QuickOutputMode::None, restored);
  editor.setSuppressSnapshots(true);
  editor.resize(1000, 700);
  editor.show();
  QCoreApplication::processEvents();
  const QString smokeOutput = QDir(QCoreApplication::applicationDirPath())
                                 .filePath(QStringLiteral("smoke-output"));
  QDir().mkpath(smokeOutput);
  editor.grab().save(QDir(smokeOutput).filePath(QStringLiteral("trash-toolbar.png")));
  const auto click = [&](const QString &action) {
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                      editor.toolbarButtonCenterForTest(action));
  };
  QTest::keyClick(&editor, Qt::Key_Delete);
  if (editor.confirmingTrash_ || !QFile::exists(log.outputPath)) {
    error = QStringLiteral("Plain Delete opened screenshot Trash confirmation");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Delete, Qt::ControlModifier);
  if (!editor.confirmingTrash_ || !QFile::exists(log.outputPath)) {
    error = QStringLiteral("Trash did not require confirmation");
    return false;
  }
  editor.grab().save(QDir(smokeOutput).filePath(QStringLiteral("trash-confirmation.png")));
  QTest::keyClick(&editor, Qt::Key_Escape);
  if (editor.confirmingTrash_ || !editor.isVisible() || !QFile::exists(log.outputPath)) {
    error = QStringLiteral("Cancelling Trash changed the screenshot");
    return false;
  }
  // The confirmation blocks canvas clicks and traps keyboard navigation.
  const QRectF originalSelection = editor.currentSelection();
  click(QStringLiteral("trash"));
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(10, 10));
  if (!editor.confirmingTrash_ || editor.currentSelection() != originalSelection) {
    error = QStringLiteral("Trash modal allowed a click through to the editor");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Tab);
  QTest::keyClick(&editor, Qt::Key_Return);
  if (editor.confirmingTrash_ || !QFile::exists(log.outputPath)) {
    error = QStringLiteral("Keyboard navigation did not cancel Trash safely");
    return false;
  }
  // Long names remain inside the dialog at the minimum window size.
  editor.resize(640, 420);
  const QString originalPath = editor.outputPath_;
  editor.outputPath_ = directory.filePath(
      QString(180, QLatin1Char('x')) + QStringLiteral(".png"));
  click(QStringLiteral("trash"));
  const QRectF panel = editor.trashDialogRect();
  for (const auto &button : editor.toolbarButtons()) {
    if (!panel.contains(button.rect)) {
      error = QStringLiteral("Trash modal controls escaped the dialog");
      return false;
    }
  }
  editor.grab().save(QDir(smokeOutput).filePath(QStringLiteral("trash-confirmation-small.png")));
  QTest::keyClick(&editor, Qt::Key_Escape);
  editor.outputPath_ = originalPath;
  editor.resize(1000, 700);
  // A missing/untrashable file leaves the editor and recent document intact.
  const QString savedPath = editor.outputPath_;
  editor.outputPath_ = directory.filePath(QStringLiteral("missing.png"));
  click(QStringLiteral("trash"));
  click(QStringLiteral("trash-confirm"));
  const auto waitForWorker = [&] {
    QElapsedTimer timer;
    timer.start();
    while (editor.busy_ && timer.elapsed() < 3000) {
      QCoreApplication::processEvents();
      QThread::msleep(1);
    }
    return !editor.busy_;
  };
  if (!waitForWorker() || !editor.isVisible() ||
      !editor.statusForTest().contains(QStringLiteral("Could not move")) ||
      !QFile::exists(savedPath) || !findRecentSnap(log.recentId)) {
    error = QStringLiteral("Failed Trash discarded the editor, file, or recent capture");
    return false;
  }
  editor.outputPath_ = savedPath;
  QTest::keyClick(&editor, Qt::Key_Delete, Qt::ControlModifier);
  QTest::keyClick(&editor, Qt::Key_Return);
  if (!waitForWorker() || editor.isVisible() || QFile::exists(savedPath) ||
      findRecentSnap(log.recentId)) {
    error = QStringLiteral("Confirmed Trash did not remove the file and its recent capture: %1")
                .arg(editor.statusForTest());
    return false;
  }
  const QDir trash(directory.filePath(QStringLiteral("Trash/files")));
  const QStringList files = trash.entryList(QDir::Files);
  if (files.size() != 1 || QImage(trash.filePath(files.first())) != capture.source) {
    error = QStringLiteral("Trashed screenshot could not be recovered intact");
    return false;
  }
  // A successful write must retarget Trash even when its preview cannot start.
  const QByteArray oldSaveDir = qgetenv("OMASNAP_SCREENSHOT_DIR");
  const QByteArray oldNotify = qgetenv("OMASNAP_NOTIFY");
  const auto restoreOutput = qScopeGuard([&] {
    oldSaveDir.isNull() ? qunsetenv("OMASNAP_SCREENSHOT_DIR")
                        : qputenv("OMASNAP_SCREENSHOT_DIR", oldSaveDir);
    oldNotify.isNull() ? qunsetenv("OMASNAP_NOTIFY")
                       : qputenv("OMASNAP_NOTIFY", oldNotify);
  });
  qputenv("OMASNAP_NOTIFY", "0");
  for (const bool saveAs : {false, true}) {
    const QString outputDir = directory.filePath(saveAs ? QStringLiteral("save-as")
                                                       : QStringLiteral("save"));
    QDir().mkpath(outputDir);
    qputenv("OMASNAP_SCREENSHOT_DIR", outputDir.toUtf8());
    log.recentId = QUuid::createUuid().toString(QUuid::Id128);
    if (!capture.source.save(log.outputPath))
      return false;
    CaptureEditor retry(capture, CaptureEditor::CaptureMode::File,
                        QuickOutputMode::None, log);
    retry.setSuppressSnapshots(true);
    retry.setProcessLauncherForTest([](const QString &, const QStringList &) { return false; });
    retry.show();
    if (saveAs)
      retry.saveAsToPath(QDir(outputDir).filePath(QStringLiteral("new.png")));
    else
      retry.finish(CaptureEditor::OutputMode::Save);
    const auto waitForRetry = [&] {
      QElapsedTimer timer;
      timer.start();
      while (retry.busy_ && timer.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
      }
      return !retry.busy_;
    };
    if (!waitForRetry() || !retry.isVisible() ||
        !retry.statusForTest().contains(QStringLiteral("could not show its preview")) ||
        retry.outputPath_ == log.outputPath || QImage(retry.outputPath_).isNull()) {
      error = QStringLiteral("Preview failure left Trash targeting the previous file (%1)")
                  .arg(saveAs ? QStringLiteral("Save As") : QStringLiteral("Save"));
      return false;
    }
    const QString latest = retry.outputPath_;
    retry.confirmTrash();
    retry.trashScreenshot();
    if (!waitForRetry() || retry.isVisible() || QFile::exists(latest) ||
        !QFile::exists(log.outputPath)) {
      error = QStringLiteral("Trash after preview failure removed the wrong export");
      return false;
    }
  }
  return true;
}
