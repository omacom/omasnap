/** @fileoverview Tests the lifetime rules for pinned screenshot files. */
#include "pin-lifecycle-smoke.hpp"

#include "capture.hpp"
#include "pin-file.hpp"
#include "pin-expiry.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QTest>
#include <QStringList>
#include <algorithm>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

bool runPinExpirySmoke(QString &error) {
  PinExpiry timed(false), kept(true), pinnedLater(false), paused(false),
      unpinned(true), keptDuringFade(false), hoveredDuringFade(false);
  QSignalSpy timedClosed(&timed, &PinExpiry::expired);
  QSignalSpy keptClosed(&kept, &PinExpiry::expired);
  QSignalSpy pinnedClosed(&pinnedLater, &PinExpiry::expired);
  QSignalSpy pausedClosed(&paused, &PinExpiry::expired);
  QSignalSpy unpinnedClosed(&unpinned, &PinExpiry::expired);
  QSignalSpy fadedKeptClosed(&keptDuringFade, &PinExpiry::expired);
  QSignalSpy fadedHoveredClosed(&hoveredDuringFade, &PinExpiry::expired);
  bool faded = false, pinnedInFade = false, pausedInFade = false;
  qreal keptOpacity = 1.0, hoveredOpacity = 1.0;
  QElapsedTimer elapsed, unpinnedElapsed;
  qint64 closedAt = 0, unpinnedAt = 0;
  QObject::connect(&timed, &PinExpiry::opacityChanged, &timed, [&](qreal opacity) {
    faded = faded || (opacity > 0.0 && opacity < 1.0);
  });
  QObject::connect(&timed, &PinExpiry::expired, &timed, [&] { closedAt = elapsed.elapsed(); });
  QObject::connect(&unpinned, &PinExpiry::expired, &unpinned, [&] { unpinnedAt = unpinnedElapsed.elapsed(); });
  QObject::connect(&keptDuringFade, &PinExpiry::opacityChanged, &keptDuringFade, [&](qreal opacity) {
    keptOpacity = opacity;
    if (!pinnedInFade && opacity < 1.0) {
      pinnedInFade = true;
      keptDuringFade.setKept(true);
    }
  });
  QObject::connect(&hoveredDuringFade, &PinExpiry::opacityChanged, &hoveredDuringFade, [&](qreal opacity) {
    hoveredOpacity = opacity;
    if (!pausedInFade && opacity < 1.0) {
      pausedInFade = true;
      hoveredDuringFade.setPaused(true);
    }
  });
  elapsed.start();
  for (PinExpiry *expiry : {&timed, &kept, &pinnedLater, &paused, &unpinned,
                            &keptDuringFade, &hoveredDuringFade})
    expiry->start();
  QTest::qWait(200);
  pinnedLater.setKept(true);
  paused.setPaused(true);
  unpinnedElapsed.start();
  unpinned.setKept(false);
  while ((timedClosed.isEmpty() || unpinnedClosed.isEmpty()) && elapsed.elapsed() < 13000)
    QTest::qWait(20);
  if (timedClosed.size() != 1 || unpinnedClosed.size() != 1 || !faded ||
      closedAt < 10000 || unpinnedAt < 10000) {
    error = QStringLiteral("A preview did not fade after ten seconds, or unpinning did not restart its lifetime");
    return false;
  }
  if (!keptClosed.isEmpty() || !pinnedClosed.isEmpty() || !pausedClosed.isEmpty() ||
      !fadedKeptClosed.isEmpty() || !fadedHoveredClosed.isEmpty() ||
      !pinnedInFade || !pausedInFade || keptOpacity != 1.0 || hoveredOpacity != 1.0) {
    error = QStringLiteral("Pinning or hovering failed to keep a preview visible, including during its fade");
    return false;
  }
  hoveredDuringFade.setPaused(false);
  if (!fadedHoveredClosed.wait(1000) || fadedHoveredClosed.size() != 1) {
    error = QStringLiteral("Leaving a fading preview did not resume its expiry");
    return false;
  }
  return true;
}

bool runPinLifecycleSmoke(QString &error) {
  const QString firstPath = pinnedSnapshotPath(987654);
  const QString secondPath = pinnedSnapshotPath(987654);
  if (firstPath.isEmpty() || secondPath.isEmpty() || firstPath == secondPath) {
    error = QStringLiteral("Pinned snapshot paths are not unique");
    return false;
  }

  QImage image(8, 8, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::white);

  // Copying a temporary pin's path exports a durable file; closing the pin
  // must still clean up its runtime document without invalidating that path.
  QTemporaryDir screenshots;
  if (!screenshots.isValid()) {
    error = QStringLiteral("Could not create pin path export directory");
    return false;
  }
  const QByteArray oldDirectory = qgetenv("OMASNAP_SCREENSHOT_DIR");
  const auto restoreDirectory = qScopeGuard([&] {
    oldDirectory.isNull() ? qunsetenv("OMASNAP_SCREENSHOT_DIR")
                          : qputenv("OMASNAP_SCREENSHOT_DIR", oldDirectory);
  });
  qputenv("OMASNAP_SCREENSHOT_DIR", screenshots.path().toUtf8());
  const QString sourcePath = pinnedSnapshotPath(987661);
  QString exportedPath;
  if (!savePinnedSnapshot(image, sourcePath, QSize(4, 4), error))
    return false;
  {
    PinSnapshotFile source(sourcePath);
    exportedPath = copySnapshotToScreenshots(sourcePath, error);
    if (!source.isLocked() || exportedPath.isEmpty() ||
        !QFile::exists(sourcePath) || !QFile::exists(operationLogPath(sourcePath)) ||
        QFileInfo(exportedPath).absolutePath() != screenshots.path()) {
      error = QStringLiteral("Sharing a pin path consumed its source or ignored the output directory");
      return false;
    }
  }
  if (QFile::exists(sourcePath) || QFile::exists(operationLogPath(sourcePath)) ||
      QImage(exportedPath).convertToFormat(image.format()) != image) {
    error = QStringLiteral("Closing a pin invalidated the shared file or leaked its runtime document");
    return false;
  }

  // Editing a pinned snapshot reopens at the captured scale: the pin save
  // records the logical size in a sidecar the file editor reads back.
  const QString scaledPath = pinnedSnapshotPath(987660);
  if (!savePinnedSnapshot(image, scaledPath, QSize(4, 4), error))
    return false;
  OperationLog sidecar;
  if (!loadOperationLog(operationLogPath(scaledPath), sidecar, error)) {
    QFile::remove(scaledPath);
    return false;
  }
  CaptureData reopened;
  describeFileCapture(reopened, QImage(scaledPath), sidecar);
  const bool scaleRestored = qFuzzyCompare(reopened.monitor.scale, 2.0) &&
                             reopened.previewSize == QSize(4, 4);
  QFile::remove(scaledPath);
  QFile::remove(operationLogPath(scaledPath));
  if (!scaleRestored) {
    error = QStringLiteral(
        "Pinned snapshot sidecar did not restore the captured scale");
    return false;
  }

  const QString closePath = pinnedSnapshotPath(987656);
  if (!savePinnedSnapshot(image, closePath, QSize(4, 4), error))
    return false;
  {
    PinSnapshotFile file(closePath);
    if (!file.isLocked()) {
      error = QStringLiteral("Normal pin snapshot was not locked");
      QFile::remove(closePath);
      return false;
    }
  }
  if (QFileInfo::exists(closePath)) {
    error = QStringLiteral("Normal pin close did not remove its snapshot");
    QFile::remove(closePath);
    return false;
  }
  if (QFileInfo::exists(operationLogPath(closePath))) {
    error = QStringLiteral("Pin close left its scale sidecar behind");
    QFile::remove(operationLogPath(closePath));
    return false;
  }

  const QString sharedPath = pinnedSnapshotPath(987659);
  if (!saveTemporarySnapshot(image, sharedPath, error))
    return false;
  {
    PinSnapshotFile first(sharedPath);
    if (!first.isLocked()) {
      error = QStringLiteral("Shared pin snapshot locks were not acquired");
      QFile::remove(sharedPath);
      return false;
    }
    {
      PinSnapshotFile second(sharedPath);
      if (!second.isLocked()) {
        error = QStringLiteral("Shared pin snapshot locks were not acquired");
        QFile::remove(sharedPath);
        return false;
      }
    }
    if (!QFileInfo::exists(sharedPath)) {
      error = QStringLiteral("Shared pin snapshot vanished before last close");
      return false;
    }
  }
  if (QFileInfo::exists(sharedPath)) {
    error = QStringLiteral("Last pin close did not remove the shared snapshot");
    QFile::remove(sharedPath);
    return false;
  }

  const QString lockedPath = pinnedSnapshotPath(987658);
  if (!saveTemporarySnapshot(image, lockedPath, error))
    return false;
  const int lockedFd =
      ::open(QFile::encodeName(lockedPath).constData(), O_RDONLY | O_CLOEXEC);
  if (lockedFd < 0 || ::flock(lockedFd, LOCK_EX | LOCK_NB) != 0) {
    error = QStringLiteral("Could not hold exclusive pin snapshot lock");
    if (lockedFd >= 0)
      ::close(lockedFd);
    QFile::remove(lockedPath);
    return false;
  }
  {
    PinSnapshotFile file(lockedPath);
    if (file.isLocked()) {
      error = QStringLiteral("Contended pin snapshot unexpectedly locked");
      ::flock(lockedFd, LOCK_UN);
      ::close(lockedFd);
      QFile::remove(lockedPath);
      return false;
    }
  }
  ::flock(lockedFd, LOCK_UN);
  ::close(lockedFd);
  if (!QFileInfo::exists(lockedPath)) {
    error = QStringLiteral("Unowned pin snapshot was removed");
    return false;
  }
  QFile::remove(lockedPath);

  const QString reopenPath = pinnedSnapshotPath(987657);
  if (!savePinnedSnapshot(image, reopenPath, QSize(4, 4), error))
    return false;
  {
    PinSnapshotFile file(reopenPath);
    if (!file.isLocked()) {
      error = QStringLiteral("Reopened pin snapshot was not locked");
      QFile::remove(reopenPath);
      return false;
    }
    file.preserveForEditor();
  }
  if (!QFileInfo::exists(reopenPath) ||
      !QFileInfo::exists(operationLogPath(reopenPath))) {
    error = QStringLiteral("Reopened pin snapshot was not fully preserved");
    QFile::remove(reopenPath);
    QFile::remove(operationLogPath(reopenPath));
    return false;
  }
  QFile::remove(reopenPath);
  QFile::remove(operationLogPath(reopenPath));

  const QString unrelatedPath =
      QDir(secureRuntimeDirectory()).filePath(QStringLiteral("manual.png"));
  if (!saveTemporarySnapshot(image, unrelatedPath, error))
    return false;
  {
    PinSnapshotFile file(unrelatedPath);
    if (!file.isLocked()) {
      error = QStringLiteral("Unrelated runtime file was not locked");
      QFile::remove(unrelatedPath);
      return false;
    }
  }
  if (!QFileInfo::exists(unrelatedPath)) {
    error = QStringLiteral("Unrelated runtime file was removed");
    return false;
  }
  QFile::remove(unrelatedPath);

  const QString path = pinnedSnapshotPath(987654);
  const QString preview = path + QStringLiteral(".preview.png");
  if (!savePinnedSnapshot(image, path, QSize(4, 4), error) ||
      !savePinnedSnapshot(image, preview, QSize(4, 4), error))
    return false;
  const QStringList documentFiles{path, operationLogPath(path), preview,
                                   operationLogPath(preview)};
  for (const QString &filePath : documentFiles) {
    QFile agedFile(filePath);
    if (!agedFile.open(QIODevice::ReadOnly) ||
        !agedFile.setFileTime(QDateTime::currentDateTime().addDays(-2),
                              QFileDevice::FileModificationTime)) {
      error = QStringLiteral("Could not age pin document");
      return false;
    }
  }

  bool survived = false;
  {
    PinSnapshotFile file(path);
    if (!file.isLocked()) {
      error = QStringLiteral("Could not hold pin snapshot lock");
      QFile::remove(path);
      return false;
    }
    prunePinnedSnapshots();
    survived = std::all_of(documentFiles.cbegin(), documentFiles.cend(),
                            [](const QString &filePath) { return QFileInfo::exists(filePath); });
  }
  if (!survived) {
    error = QStringLiteral("An active pin lost its source, log, or preview during pruning");
    return false;
  }
  if (std::any_of(documentFiles.cbegin(), documentFiles.cend(),
                   [](const QString &filePath) { return QFileInfo::exists(filePath); })) {
    error = QStringLiteral("Closing a pin left part of its editable document behind");
    return false;
  }

  const QString abandonedPath = pinnedSnapshotPath(987655);
  if (!saveTemporarySnapshot(image, abandonedPath, error))
    return false;
  QFile abandonedFile(abandonedPath);
  if (!abandonedFile.open(QIODevice::ReadOnly) ||
      !abandonedFile.setFileTime(QDateTime::currentDateTime().addDays(-2),
                                 QFileDevice::FileModificationTime)) {
    error = QStringLiteral("Could not age abandoned pin snapshot");
    QFile::remove(abandonedPath);
    return false;
  }
  abandonedFile.close();
  prunePinnedSnapshots();
  if (QFileInfo::exists(abandonedPath)) {
    error = QStringLiteral("Abandoned pin snapshot was not pruned");
    QFile::remove(abandonedPath);
    return false;
  }
  return true;
}
