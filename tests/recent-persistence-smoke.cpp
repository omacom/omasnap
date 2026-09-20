#include "recent-persistence-smoke.hpp"

#include "capture.hpp"
#include "cli-path.hpp"
#include "editor.hpp"
#include "recent-snaps.hpp"

#include <QApplication>
#include <QByteArray>
#include <QCommandLineParser>
#include <QChar>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QImage>
#include <QObject>
#include <QPromise>
#include <QScopeGuard>
#include <QSemaphore>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>
#include <QtCore/qnamespace.h>
#include <QtCore/qtenvironmentvariables.h>
#include <QtCore/qtestsupport_core.h>
#include <QtCore/qtypes.h>
#include <QtConcurrentRun>
#include <QtTest/QTest>

#include <memory>
#include <QtTest/qtestkeyboard.h>
#include <QtTest/qtestmouse.h>

namespace {
bool checkPublication(const QImage &image, const QDir &, QString &error) {
  QString failure;
  if (recordRecentSnap({}, {}, image, failure) || failure.isEmpty() ||
      !listRecentSnaps().isEmpty()) {
    error = QStringLiteral("Invalid source published a partial recent");
    return false;
  }
  return true;
}

bool checkReplacement(const QImage &image, const QDir &, QString &error) {
  for (int index = 0; index < kRecentSnapLimit; ++index) {
    if (!recordRecentSnap(image, {}, image, error))
      return false;
  }
  const auto original = listRecentSnaps();
  const RecentSnap replaced = original.at(2);
  QString failure;
  {
    RecentSnapWriter writer({});
    if (writer.record({}, {}, image, failure, &replaced) ||
        !QFile::exists(replaced.sourcePath)) {
      error = QStringLiteral("Failed replacement removed a published recent");
      return false;
    }
  }
  {
    RecentSnapWriter writer({});
    if (!writer.record(image, {}, image, error, &replaced))
      return false;
  }
  if (QFile::exists(replaced.sourcePath) ||
      listRecentSnaps().size() != kRecentSnapLimit) {
    error = QStringLiteral("Replacement pruned an extra recent capture");
    return false;
  }
  for (const auto &snap : original) {
    if (snap.sourcePath != replaced.sourcePath &&
        (!QFile::exists(snap.sourcePath) || !QFile::exists(snap.thumbPath))) {
      error = QStringLiteral("Replacement removed an unrelated recent capture");
      return false;
    }
  }
  for (const auto &snap : listRecentSnaps())
    removeRecentSnap(snap);
  return true;
}

bool checkBackdrop(QApplication &application, const CaptureData &capture,
                   QString &error) {
  CaptureEditor editor(capture, CaptureEditor::CaptureMode::Fullscreen);
  editor.resize(800, 600);
  editor.show();
  if (!editor.waitForSnapshot()) {
    error = QStringLiteral("Could not persist backdrop fixture");
    return false;
  }
  QPromise<QImage> pending;
  pending.start();
  editor.setBackdropFutureForTest(pending.future());
  const auto initialLog = editor.operationLog();
  QTest::keyClick(&editor, Qt::Key_S, Qt::ControlModifier);
  const bool started =
      editor.statusForTest().contains(QStringLiteral("Saving screenshot"));
  // Finish the held decode before any failure return destroys its promise.
  pending.addResult(capture.source);
  pending.finish();
  application.processEvents();
  if (started || !editor.isVisible() || editor.operationLog() == initialLog) {
    editor.waitForExport();
    error = QStringLiteral("Output did not wait for the configured backdrop");
    return false;
  }
  const auto ready = editor.operationLog();
  if (ready.isEmpty() || ready.first().type != Operation::Type::Background ||
      ready.first().background != BackgroundStyle::Custom) {
    error = QStringLiteral("Ready backdrop was not included in document history");
    return false;
  }
  return true;
}

bool checkDestruction(const CaptureData &capture, QString &error) {
  auto editor = std::make_unique<CaptureEditor>(
      capture, CaptureEditor::CaptureMode::Fullscreen);
  if (!editor->waitForSnapshot()) {
    error = QStringLiteral("Could not persist destruction fixture");
    return false;
  }
  const QString source = temporarySnapshotPath();
  const QString log = operationLogPath(source);
  QSemaphore started;
  QSemaphore destroyed;
  // The old destructor unlinks the files and wakes this worker. A destructor
  // that drains output must wait for the bounded worker instead, retaining
  // both files throughout that interval. No GUI event loop releases the wait.
  auto pending = QtConcurrent::run([&] {
    started.release();
    static_cast<void>(destroyed.tryAcquire(1, 100));
    return QFile::exists(source) && QFile::exists(log);
  });
  QObject::connect(editor.get(), &QObject::destroyed,
                    [&] { destroyed.release(); });
  editor->setFinishFutureForTest(pending);
  started.acquire();
  editor.reset();
  pending.waitForFinished();
  if (!pending.result()) {
    error = QStringLiteral("Destruction removed files still owned by output");
    return false;
  }
  if (QFile::exists(source) || QFile::exists(log)) {
    error = QStringLiteral("Destruction did not clean drained working files");
    return false;
  }
  return true;
}
bool checkRecentHandoff(QApplication &application, const CaptureData &capture,
                        const QDir &, QString &error) {
  const auto original = listRecentSnaps(false);
  if (original.size() != kRecentSnapLimit) {
    error = QStringLiteral("Recent handoff fixture did not fill the shelf");
    return false;
  }
  const QString replaced = original.at(2).sourcePath;
  const auto launchArguments = std::make_shared<QStringList>();
  {
    CaptureEditor editor(capture);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    if (!editor.waitForRecents())
      return false;
    // Move away first: the preceding editor may have left the system pointer
    // at this same stack position, in which case Qt sends no move event.
    QTest::mouseMove(&editor, QPoint(100, 300), 20);
    QTest::mouseMove(&editor, editor.recentCardRectForTest(0).center().toPoint(), 20);
    if (!editor.recentsOpenForTest()) {
      error = QStringLiteral("Could not fan the recent handoff fixture shelf");
      return false;
    }
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                      editor.recentCardRectForTest(2).center().toPoint());
    editor.waitForReopen();
    if (!editor.editingForTest()) {
      error = QStringLiteral("Could not reopen the handoff fixture");
      return false;
    }
    editor.setProcessLauncherForTest(
        [launchArguments](const QString &, const QStringList &arguments) {
          *launchArguments = arguments;
          return false;
        });
    QTest::keyClick(&editor, Qt::Key_W);
    for (int attempt = 0; attempt < 500 && !editor.isEnabled(); ++attempt)
      QTest::qWait(10);
    if (!editor.isEnabled() || !editor.isVisible() ||
        !QFile::exists(replaced) ||
        QFile::exists(launchArguments->value(1)) ||
        listRecentSnaps(false).size() != kRecentSnapLimit) {
      error = QStringLiteral("Failed presentation handoff lost its recent capture");
      return false;
    }
    editor.setProcessLauncherForTest(
        [launchArguments](const QString &, const QStringList &arguments) {
          *launchArguments = arguments;
          return true;
        });
    QTest::keyClick(&editor, Qt::Key_W);
    for (int attempt = 0; attempt < 500 && editor.isVisible(); ++attempt)
      QTest::qWait(10);
    if (editor.isVisible()) {
      error = QStringLiteral("Recent presentation handoff did not finish");
      return false;
    }
  }
  QCommandLineParser parser;
  configureCaptureCommandLine(parser);
  if (!parser.parse(QStringList{QStringLiteral("omasnap")} + *launchArguments)) {
    error = parser.errorText();
    return false;
  }
  const QString path = parser.value(QStringLiteral("file"));
  const QString token = parser.value(QStringLiteral("handoff-token"));
  const QImage source(path);
  OperationLog log;
  OperationLog originalLog;
  if (source.isNull() || !loadOperationLog(operationLogPath(path), log, error) ||
      !loadOperationLog(original.at(2).logPath, originalLog, error))
    return false;
  if (log.recentId.isEmpty() || log.recentId != originalLog.recentId) {
    error = QStringLiteral("Presentation handoff lost its capture identity");
    return false;
  }
  if (removeEditorHandoff(path, QString(32, QLatin1Char('0'))) ||
      !QFile::exists(path) || !QFile::exists(replaced) ||
      !removeEditorHandoff(path, token)) {
    error = QStringLiteral("Presentation handoff ownership validation failed");
    return false;
  }
  CaptureData reopened;
  describeFileCapture(reopened, source, log);
  CaptureEditor receiver(reopened, CaptureEditor::CaptureMode::File,
                         QuickOutputMode::None, log);
  receiver.setWindowedPresentation(true);
  receiver.resize(800, 600);
  receiver.show();
  QTest::keyClick(&receiver, Qt::Key_S, Qt::ControlModifier);
  receiver.waitForExport();
  if (receiver.isVisible() || QFile::exists(replaced) ||
      listRecentSnaps(false).size() != kRecentSnapLimit) {
    error = QStringLiteral("Saving after handoff failed to replace the recent entry");
    return false;
  }
  for (const auto &snap : original) {
    if (snap.sourcePath != replaced && !QFile::exists(snap.sourcePath)) {
      error = QStringLiteral("Saving after handoff evicted an unrelated recent");
      return false;
    }
  }
  return true;
}
bool checkFreshSelection(QApplication &application, const CaptureData &capture,
                         QString &error) {
  const auto shelf = listRecentSnaps(false);
  if (shelf.size() < 3)
    return false;
  const RecentSnap previous = shelf.at(1);
  OperationLog previousLog;
  if (!loadOperationLog(previous.logPath, previousLog, error))
    return false;
  CaptureEditor editor(capture, CaptureEditor::CaptureMode::Fullscreen,
                        QuickOutputMode::None, previousLog);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  editor.returnToSelectForTest();
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(150, 150));
  QTest::mouseMove(&editor, QPoint(400, 350), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(400, 350));
  QTest::keyClick(&editor, Qt::Key_S, Qt::ControlModifier);
  editor.waitForExport();
  const auto after = listRecentSnaps(false);
  OperationLog freshLog;
  if (editor.isVisible() || !QFile::exists(previous.sourcePath) || after.isEmpty() ||
      !loadOperationLog(after.first().logPath, freshLog, error) ||
      freshLog.recentId.isEmpty() || freshLog.recentId == previousLog.recentId) {
    error = QStringLiteral("Fresh selection reused the previous capture identity");
    return false;
  }
  return true;
}
} // namespace

bool runRecentPersistenceSmoke(QApplication &application, QString &error) {
  const QTemporaryDir directory;
  if (!directory.isValid()) {
    error = QStringLiteral("Could not create recent persistence directory");
    return false;
  }
  const QDir root(directory.path());
  const QByteArray previousShelf = qgetenv("OMASNAP_RECENT_DIR");
  const QByteArray previousOutput = qgetenv("OMASNAP_SCREENSHOT_DIR");
  const auto restore = qScopeGuard([&] {
    qputenv("OMASNAP_RECENT_DIR", previousShelf);
    qputenv("OMASNAP_SCREENSHOT_DIR", previousOutput);
  });
  const QString shelf = root.filePath(QStringLiteral("recent"));
  const QString output = root.filePath(QStringLiteral("output"));
  qputenv("OMASNAP_RECENT_DIR", shelf.toUtf8());
  qputenv("OMASNAP_SCREENSHOT_DIR", output.toUtf8());

  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(Qt::white);
  capture.previewSize = capture.source.size();
  if (!checkPublication(capture.source, root, error) ||
      !checkReplacement(capture.source, root, error) ||
      !checkDestruction(capture, error) ||
      !checkBackdrop(application, capture, error))
    return false;

  auto editorOwner = std::make_unique<CaptureEditor>(
      capture, CaptureEditor::CaptureMode::Fullscreen);
  CaptureEditor &editor = *editorOwner;
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  if (!editor.waitForSnapshot()) {
    error = QStringLiteral("Could not persist recent editor fixture");
    return false;
  }

  QPromise<bool> pending;
  pending.start();
  bool released = false;
  const auto release = [&] {
    if (!released) {
      released = true;
      pending.addResult(true);
      pending.finish();
    }
  };
  const auto finishPending = qScopeGuard(release);
  editor.setSnapshotFutureForTest(pending.future());
  // Coalesce an edit behind the held autosave. The final shelf must include it.
  QTest::keyClick(&editor, Qt::Key_A);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 200));
  QTest::mouseMove(&editor, QPoint(400, 320), 20);
  const auto unfinishedLog = editor.operationLog();
  QTest::keyClick(&editor, Qt::Key_S, Qt::ControlModifier);
  QTest::qWait(30);
  if (editor.operationLog() != unfinishedLog ||
      editor.statusForTest().contains(QStringLiteral("Saving screenshot"))) {
    release();
    editor.waitForExport();
    error = QStringLiteral("Output started before the drawing gesture committed");
    return false;
  }
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(400, 320));
  if (editor.annotationCountForTest() != 1) {
    error = QStringLiteral("Could not draw recent persistence fixture arrow");
    return false;
  }

  QTest::keyClick(&editor, Qt::Key_V);
  const Annotation beforeNudge =
      editor.currentAnnotationsForTest().constFirst();
  const QPoint hit =
      editor.annotationPointToWidgetForTest(beforeNudge.start).toPoint();
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, hit);
  QTest::keyClick(&editor, Qt::Key_Right);
  if (editor.currentAnnotationsForTest().constFirst().start ==
      beforeNudge.start) {
    error = QStringLiteral("Could not nudge recent persistence fixture arrow");
    return false;
  }

  // A file where the shelf directory should be makes publication fail without
  // relying on Unix permissions (which behave differently when run as root).
  const QString blockedShelf = root.filePath(QStringLiteral("blocked"));
  QFile blocker(blockedShelf);
  if (!blocker.open(QIODevice::WriteOnly)) {
    error = QStringLiteral("Could not block the recent shelf directory");
    return false;
  }
  blocker.close();
  qputenv("OMASNAP_RECENT_DIR", blockedShelf.toUtf8());
  QTimer watchdog;
  watchdog.setSingleShot(true);
  QObject::connect(&watchdog, &QTimer::timeout, &application, release);
  watchdog.start(1500);
  QTest::keyClick(&editor, Qt::Key_S, Qt::ControlModifier);
  const auto lockedAnnotations = editor.currentAnnotationsForTest();
  const auto lockedLog = editor.operationLog();
  QWheelEvent wheel(QPointF(hit), QPointF(editor.mapToGlobal(hit)), {},
                    {0, 120}, Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
                    false);
  application.sendEvent(&editor, &wheel);
  if (editor.currentAnnotationsForTest() != lockedAnnotations ||
      editor.operationLog() != lockedLog) {
    release();
    editor.waitForExport();
    error =
        QStringLiteral("Wheel input changed layers during a pending export");
    return false;
  }
  QTest::qWait(200);
  // The old completion handler entered a nested event loop until the watchdog
  // fired. This wait must return while persistence is still held on the worker.
  if (released) {
    error = QStringLiteral(
        "Export completion blocked the GUI waiting for autosave");
    return false;
  }
  watchdog.stop();
  release();
  editor.waitForExport();
  const QString source = temporarySnapshotPath();
  const QString logPath = operationLogPath(source);
  if (editor.isVisible() || !QFile::exists(source) ||
      !QFile::exists(logPath) ||
      QDir(output).entryList({QStringLiteral("*.png")}, QDir::Files).size() !=
          1) {
    error = QStringLiteral("Failed shelving discarded the editable document or "
                           "hid successful output");
    return false;
  }
  OperationLog log;
  if (!loadOperationLog(logPath, log, error))
    return false;
  const qsizetype operations = log.ops.size();
  if (operations < 2 || log.ops.last().annotations.size() != 1 ||
      log.ops.last().annotations.first().start !=
          editor.currentAnnotationsForTest().first().start) {
    error =
        QStringLiteral("Final persistence dropped the coalesced annotation");
    return false;
  }
  editorOwner.reset();
  if (!QFile::exists(source) || !QFile::exists(logPath)) {
    error = QStringLiteral("Closed editor removed the retained recovery document");
    return false;
  }
  qputenv("OMASNAP_RECENT_DIR", shelf.toUtf8());
  CaptureData recoveryCapture;
  describeFileCapture(recoveryCapture, QImage(source), log);
  CaptureEditor recovered(recoveryCapture, CaptureEditor::CaptureMode::File,
                          QuickOutputMode::None, log);
  recovered.resize(800, 600);
  recovered.show();
  const QImage beforeUndo = recovered.renderCurrentOutput();
  QTest::keyClick(&recovered, Qt::Key_Z, Qt::ControlModifier);
  const QImage afterUndo = recovered.renderCurrentOutput();
  QTest::keyClick(&recovered, Qt::Key_Z,
                  Qt::ControlModifier | Qt::ShiftModifier);
  if (afterUndo == beforeUndo || recovered.renderCurrentOutput() != beforeUndo ||
      recovered.operationLog() != log.ops || recovered.operationIndex() != log.index) {
    error = QStringLiteral("Recovery did not preserve undo and redo of final edits");
    return false;
  }
  QTest::keyClick(&recovered, Qt::Key_S, Qt::ControlModifier);
  recovered.waitForExport();
  const auto recent = listRecentSnaps();
  if (recovered.isVisible() || recent.size() != 1 ||
      recent.first().logPath.isEmpty() || QFile::exists(source) ||
      QFile::exists(logPath) ||
      !loadOperationLog(recent.first().logPath, log, error) ||
      log.ops.size() != operations) {
    error = QStringLiteral(
        "Retry did not publish the complete editable recent capture");
    return false;
  }
  // If the source needs to be recreated after cutting, it must still hold
  // pristine pixels. Saving the composed image would apply the cut twice when
  // this same operation log is reopened.
  Operation cut;
  cut.type = Operation::Type::Cut;
  cut.cut = {Qt::Horizontal, 100, 200, 100, 200};
  OperationLog cutLog;
  cutLog.ops = {cut};
  cutLog.index = 1;
  cutLog.previewSize = capture.previewSize;
  const QString cutLogPath = root.filePath(QStringLiteral("cut.json"));
  if (!saveOperationLog(cutLogPath, cutLog, error))
    return false;
  CaptureEditor cutEditor(capture, CaptureEditor::CaptureMode::Fullscreen);
  cutEditor.resize(800, 600);
  cutEditor.show();
  application.processEvents();
  if (!cutEditor.waitForSnapshot() || !QFile::remove(source) ||
      !cutEditor.restoreOperationLog(cutLogPath, error) ||
      !cutEditor.waitForSnapshot()) {
    error = QStringLiteral("Could not recreate a working source after cutting");
    return false;
  }
  if (QImage(source).size() != capture.source.size()) {
    error = QStringLiteral(
        "Working snapshot baked in a cut still present in history");
    return false;
  }
  QTest::keyClick(&cutEditor, Qt::Key_S, Qt::ControlModifier);
  cutEditor.waitForExport();
  const auto withCut = listRecentSnaps();
  if (withCut.size() != 2 || cutEditor.isVisible() ||
      QImage(withCut.first().sourcePath).size() != capture.source.size() ||
      !loadOperationLog(withCut.first().logPath, log, error) ||
      log.ops != cutLog.ops || log.index != cutLog.index ||
      log.previewSize != cutLog.previewSize) {
    error = QStringLiteral(
        "Shelving a cut document lost its pristine source or history");
    return false;
  }
  // A successful pending save can still belong to an earlier document. Cover
  // both coalesced autosave and immediate output after adopting a stitched
  // page.
  for (const bool immediateSave : {false, true}) {
    CaptureEditor replaced(capture, CaptureEditor::CaptureMode::Fullscreen);
    replaced.resize(800, 600);
    replaced.show();
    application.processEvents();
    if (!replaced.waitForSnapshot()) {
      error = QStringLiteral("Could not persist the old document fixture");
      return false;
    }
    QPromise<bool> oldSave;
    oldSave.start();
    bool oldReleased = false;
    const auto releaseOld = [&] {
      if (!oldReleased) {
        oldReleased = true;
        oldSave.addResult(true);
        oldSave.finish();
      }
    };
    const auto finishOld = qScopeGuard(releaseOld);
    replaced.setSnapshotFutureForTest(oldSave.future());
    QImage stitched(520, 920, QImage::Format_RGB32);
    stitched.fill(Qt::cyan);
    replaced.adoptStitchedForTest(stitched);
    if (QImage(source).size() != capture.source.size()) {
      error =
          QStringLiteral("Old document fixture was not held before adoption");
      return false;
    }
    if (immediateSave)
      QTest::keyClick(&replaced, Qt::Key_S, Qt::ControlModifier);
    releaseOld();
    if (!immediateSave) {
      if (!replaced.waitForSnapshot() ||
          QImage(source).convertToFormat(QImage::Format_RGB32) != stitched) {
        error =
            QStringLiteral("Old autosave completion hid the adopted source");
        return false;
      }
      QTest::keyClick(&replaced, Qt::Key_S, Qt::ControlModifier);
    }
    replaced.waitForExport();
    const auto adopted = listRecentSnaps();
    if (replaced.isVisible() || adopted.isEmpty() ||
        QImage(adopted.first().sourcePath)
                .convertToFormat(QImage::Format_RGB32) != stitched ||
        !loadOperationLog(adopted.first().logPath, log, error) ||
        log.previewSize != stitched.size()) {
      error = QStringLiteral(
          "Old autosave completion shelved pixels from another document");
      return false;
    }
  }
  // Reopening a shelf document makes Save replace that entry. Adopting a new
  // stitched document ends that ownership, even in the same editor window.
  const auto beforeFresh = listRecentSnaps();
  const QString reopenedSource = beforeFresh.first().sourcePath;
  CaptureEditor fresh(capture, CaptureEditor::CaptureMode::Region,
                      QuickOutputMode::CopyAndPreview);
  fresh.resize(800, 600);
  fresh.show();
  application.processEvents();
  if (!fresh.waitForRecents()) {
    error = QStringLiteral("Could not list recents for fresh document fixture");
    return false;
  }
  QTest::mouseMove(&fresh, fresh.recentCardRectForTest(0).center().toPoint(), 20);
  QTest::mouseClick(&fresh, Qt::LeftButton, Qt::NoModifier,
                    fresh.recentCardRectForTest(0).center().toPoint());
  fresh.waitForReopen();
  if (!fresh.editingForTest() || !fresh.isVisible()) {
    error = QStringLiteral("Reopening a recent inherited fresh-capture quick output");
    return false;
  }
  QImage newDocument(300, 400, QImage::Format_RGB32);
  newDocument.fill(Qt::yellow);
  fresh.adoptStitchedForTest(newDocument);
  QTest::keyClick(&fresh, Qt::Key_S, Qt::ControlModifier);
  fresh.waitForExport();
  if (fresh.isVisible() || !QFile::exists(reopenedSource) ||
      listRecentSnaps().size() != beforeFresh.size() + 1) {
    error = QStringLiteral("A fresh document replaced a previously reopened recent");
    return false;
  }
  return checkRecentHandoff(application, capture, root, error) &&
         checkFreshSelection(application, capture, error);
}
