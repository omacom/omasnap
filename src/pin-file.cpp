/** @fileoverview Implements pinned snapshot and slot locking. */
#include "pin-file.hpp"

#include "capture.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <memory>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

PinSnapshotFile::PinSnapshotFile(QString path)
    : path_(QFileInfo(path).absoluteFilePath()) {
  fd_ = ::open(QFile::encodeName(path_).constData(),
               O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd_ < 0 || ::flock(fd_, LOCK_SH | LOCK_NB) != 0) {
    if (fd_ >= 0)
      ::close(fd_);
    fd_ = -1;
  }
}

PinSnapshotFile::~PinSnapshotFile() {
  if (fd_ < 0)
    return;
  // Only the last holder may unlink. Upgrade SH -> EX; if another pin still
  // holds SH, leave the file for that process.
  const bool lastOwner = !preserve_ && ::flock(fd_, LOCK_EX | LOCK_NB) == 0;
  if (lastOwner && isOwnedPath(path_)) {
    QFile::remove(path_);
    QFile::remove(operationLogPath(path_));
    QFile::remove(previewPath());
    QFile::remove(operationLogPath(previewPath()));
  }
  ::close(fd_);
}

bool PinSnapshotFile::isLocked() const { return fd_ >= 0; }

bool PinSnapshotFile::isOwnedPath(const QString &path) {
  static const QRegularExpression internalName(
      QStringLiteral("^pin-[1-9][0-9]*-[1-9][0-9]*-[0-9a-f]{16}\\.png$"));
  const QFileInfo file(path);
  const QString runtime = secureRuntimeDirectory();
  return !runtime.isEmpty() && file.absolutePath() == runtime &&
         internalName.match(file.fileName()).hasMatch() && !file.isSymLink();
}

void PinSnapshotFile::preserveForEditor() { preserve_ = true; }

std::shared_ptr<PinSnapshotFile> copyPinDocument(const QString &path, QString &error) {
  const QString copy = pinnedSnapshotPath(1);
  if (copy.isEmpty() || !QFile::copy(path, copy)) {
    error = QStringLiteral("Could not retain the pinned capture for editing");
    return {};
  }
  auto document = std::make_shared<PinSnapshotFile>(copy);
  if (!document->isLocked()) {
    QFile::remove(copy);
    error = QStringLiteral("Could not lock the pinned document");
    return {};
  }
  if (!QFile::setPermissions(copy, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
    error = QStringLiteral("Could not make the pinned document private");
    return {};
  }
  OperationLog log;
  const QString sidecar = operationLogPath(path);
  if ((QFile::exists(sidecar) && !loadOperationLog(sidecar, log, error)) ||
      !saveOperationLog(operationLogPath(copy), log, error))
    return {};
  return document;
}
