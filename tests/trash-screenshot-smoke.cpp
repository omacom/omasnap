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
  click(QStringLiteral("trash"));
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
  click(QStringLiteral("trash"));
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
  return true;
}
