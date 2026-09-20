#include "recent-snaps.hpp"

#include "capture.hpp"
#include "startup-timing.hpp"

#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QLockFile>
#include <QRegularExpression>
#include <QUuid>
#include <QStandardPaths>
#include <QLatin1StringView>
#include <QStringList>

#include <algorithm>
#include <utility>

namespace {
// Entries are named by a zero-padded millisecond stamp so a plain name sort is
// a time sort. The capture identity groups repeated edits of the same shot.
constexpr QLatin1StringView kThumbSuffix(".thumb.png");

QString entryStem() {
  return QStringLiteral("%1").arg(QDateTime::currentMSecsSinceEpoch(), 16, 10,
                                  QChar('0'));
}

QString stemOf(const QString &thumbName) {
  return thumbName.chopped(kThumbSuffix.size());
}

RecentSnap snapForStem(const QDir &dir, const QString &stem) {
  RecentSnap snap;
  snap.sourcePath = dir.filePath(stem + QStringLiteral(".png"));
  snap.thumbPath = dir.filePath(stem + kThumbSuffix);
  snap.stampMs = stem.section(QLatin1Char('-'), 0, 0).toLongLong();
  const QString log = dir.filePath(stem + QStringLiteral(".json"));
  if (QFile::exists(log))
    snap.logPath = log;
  return snap;
}

bool validRecentId(const QString &id) {
  static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{32}$"));
  return pattern.match(id).hasMatch();
}

QStringList thumbNamesNewestFirst(const QDir &dir) {
  QStringList names =
      dir.entryList({QStringLiteral("*") + kThumbSuffix}, QDir::Files);
  std::ranges::sort(names, std::greater<>());
  return names;
}

bool waitForPendingCaptures(const QDir &dir, const QString &id = {}) {
  const QString pattern = QStringLiteral(".pending-%1.lock")
                              .arg(id.isEmpty() ? QStringLiteral("*") : id);
  QDeadlineTimer deadline(30000);
  for (const QString &name : dir.entryList({pattern}, QDir::Files | QDir::Hidden)) {
    QLockFile pending(dir.filePath(name));
    pending.setStaleLockTime(0);
    if (!pending.tryLock(static_cast<int>(deadline.remainingTime())))
      return false;
  }
  return true;
}
} // namespace

QString recentSnapsDirectory() {
  QString root = qEnvironmentVariable("OMASNAP_RECENT_DIR");
  if (root.isEmpty()) {
    const QString state =
        QStandardPaths::writableLocation(QStandardPaths::StateLocation);
    if (state.isEmpty())
      return {};
    root = QDir(state).filePath(QStringLiteral("recent"));
  }
  // Working documents hold whole-monitor pixels, so the shelf is as private
  // as the runtime directory those came from.
  return ensurePrivateDirectory(root) ? QDir::cleanPath(root) : QString();
}

QVector<RecentSnap> listRecentSnaps(bool loadThumbnails) {
  const QString root = recentSnapsDirectory();
  if (root.isEmpty())
    return {};
  const QDir dir(root);
  // The overlay itself is already available; only its shelf worker waits.
  static_cast<void>(waitForPendingCaptures(dir));
  QVector<RecentSnap> snaps;
  for (const QString &name : thumbNamesNewestFirst(dir)) {
    RecentSnap snap = snapForStem(dir, stemOf(name));
    if (!QFile::exists(snap.sourcePath)) {
      QFile::remove(snap.thumbPath); // orphaned by a failed move; tidy up
      continue;
    }
    if (loadThumbnails) {
      snap.thumbnail.load(snap.thumbPath);
      if (snap.thumbnail.isNull())
        continue;
    }
    snaps.push_back(std::move(snap));
    if (snaps.size() >= kRecentSnapLimit)
      break;
  }
  return snaps;
}

std::optional<RecentSnap> findRecentSnap(const QString &recentId, QString *error) {
  if (!validRecentId(recentId))
    return std::nullopt;
  const QString root = recentSnapsDirectory();
  if (root.isEmpty())
    return std::nullopt;
  const QDir dir(root);
  if (!waitForPendingCaptures(dir, recentId)) {
    if (error)
      *error = QStringLiteral("This capture is still being saved. Try editing it again shortly.");
    return std::nullopt;
  }
  for (const QString &name : thumbNamesNewestFirst(dir)) {
    if (name.endsWith(QLatin1Char('-') + recentId + kThumbSuffix)) {
      const RecentSnap snap = snapForStem(dir, stemOf(name));
      if (QFile::exists(snap.sourcePath))
        return snap;
    }
  }
  return std::nullopt;
}

RecentSnapWriter::RecentSnapWriter(QString recentId)
    : recentId_(recentId.isEmpty() ? QUuid::createUuid().toString(QUuid::Id128)
                                   : std::move(recentId)),
      root_(recentSnapsDirectory()), stem_(entryStem()) {
  if (!validRecentId(recentId_)) {
    error_ = QStringLiteral("Invalid recent capture identity");
    return;
  }
  if (root_.isEmpty()) {
    error_ = QStringLiteral("Could not create the recent captures directory");
    return;
  }
  pending_ = std::make_unique<QLockFile>(
      QDir(root_).filePath(QStringLiteral(".pending-%1.lock").arg(recentId_)));
  pending_->setStaleLockTime(0);
  if (!pending_->tryLock(30000))
    error_ = QStringLiteral("This capture is still being saved");
}

RecentSnapWriter::~RecentSnapWriter() = default;

bool RecentSnapWriter::record(const QImage &source, const OperationLog &log,
                              const QImage &rendered, QString &error,
                              const RecentSnap *replaced) {
  StartupTimingScope timing("record recent capture");
  if (!error_.isEmpty()) {
    error = error_;
    return false;
  }
  if (rendered.isNull() || source.isNull()) {
    error = QStringLiteral("Nothing to remember: no working document");
    return false;
  }
  OperationLog savedLog = log;
  savedLog.recentId = recentId_;
  const QDir dir(root_);
  QString stem = stem_;
  const QString suffix = QLatin1Char('-') + savedLog.recentId;
  while (QFile::exists(dir.filePath(stem + suffix + kThumbSuffix)) ||
         QFile::exists(dir.filePath(stem + suffix + QStringLiteral(".png"))) ||
         QFile::exists(dir.filePath(stem + suffix + QStringLiteral(".json"))))
    stem = QStringLiteral("%1").arg(stem.toLongLong() + 1, 16, 10, QChar('0'));
  const RecentSnap snap = snapForStem(dir, stem + suffix);

  // Publish the thumbnail last: readers never see a half-written document.
  // Keep the previous entry until its replacement is completely ready.
  QSaveFile sourceFile(snap.sourcePath);
  sourceFile.setDirectWriteFallback(false);
  if (!sourceFile.open(QIODevice::WriteOnly) ||
      !sourceFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
      !source.save(&sourceFile, "PNG") || !sourceFile.commit()) {
    error = QStringLiteral("Could not write recent capture source: %1")
                .arg(sourceFile.errorString());
    return false;
  }
  if (!saveOperationLog(operationLogPath(snap.sourcePath), savedLog, error)) {
    removeRecentSnap(snap);
    return false;
  }
  const QImage thumb = rendered.scaled(kRecentThumbEdge, kRecentThumbEdge,
                                       Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation);
  QSaveFile thumbFile(snap.thumbPath);
  thumbFile.setDirectWriteFallback(false);
  if (!thumbFile.open(QIODevice::WriteOnly) ||
      !thumbFile.setPermissions(QFileDevice::ReadOwner |
                                QFileDevice::WriteOwner) ||
      !thumb.save(&thumbFile, "PNG")) {
    error = QStringLiteral("Could not write recent capture thumbnail: %1")
                .arg(thumbFile.errorString());
    removeRecentSnap(snap);
    return false;
  }

  // Only publication and pruning serialize across captures. In particular,
  // a slow full-monitor PNG encode must not hold up another shot's preview.
  QLockFile lock(dir.filePath(QStringLiteral(".record.lock")));
  if (!lock.tryLock(5000) || !thumbFile.commit()) {
    error = QStringLiteral("Could not publish the recent capture");
    removeRecentSnap(snap);
    return false;
  }

  for (const QString &name : dir.entryList({QStringLiteral("*") + suffix + kThumbSuffix}, QDir::Files)) {
    if (dir.filePath(name) != snap.thumbPath)
      removeRecentSnap(snapForStem(dir, stemOf(name)));
  }

  // A reopened entry can have a different capture identity. Remove it before
  // pruning, so a full shelf does not evict an unrelated capture as well.
  if (replaced && replaced->sourcePath != snap.sourcePath)
    removeRecentSnap(*replaced);
  const QStringList names = thumbNamesNewestFirst(dir);
  for (qsizetype index = kRecentSnapLimit; index < names.size(); ++index)
    removeRecentSnap(snapForStem(dir, stemOf(names.at(index))));
  return true;
}

bool recordRecentSnap(const QImage &source, const OperationLog &log,
                      const QImage &rendered, QString &error) {
  RecentSnapWriter writer(log.recentId);
  return writer.record(source, log, rendered, error);
}

void removeRecentSnap(const RecentSnap &snap) {
  QFile::remove(snap.thumbPath);
  QFile::remove(snap.sourcePath);
  if (!snap.logPath.isEmpty())
    QFile::remove(snap.logPath);
  else if (!snap.sourcePath.isEmpty())
    QFile::remove(operationLogPath(snap.sourcePath));
}
