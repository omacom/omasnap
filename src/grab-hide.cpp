/** @fileoverview Keeps omasnap's own timed preview cards out of the next
 *  screen grab. See the header for the protocol. */
#include "grab-hide.hpp"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileDevice>
#include <QIODevice>
#include <QRegularExpression>
#include <QSaveFile>
#include <QString>
#include <QThread>
#include <QtTypes>

#include <cerrno>
#include <signal.h>
#include <sys/types.h>
#include <utility>

namespace {
bool processAlive(qint64 pid) {
  return pid > 0 && (::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM);
}

qint64 readPid(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return 0;
  return file.read(32).trimmed().toLongLong();
}
} // namespace

QString timedPreviewMarkerPath(const QString &runtime, qint64 pid) {
  return QDir(runtime).filePath(QStringLiteral("timed-%1").arg(pid));
}

QString grabRequestPath(const QString &runtime) {
  return QDir(runtime).filePath(QStringLiteral("grab-request"));
}

QString grabAckPath(const QString &runtime, qint64 pid) {
  return QDir(runtime).filePath(QStringLiteral("grab-ack-%1").arg(pid));
}

qint64 activeGrabRequest(const QString &runtime) {
  if (runtime.isEmpty())
    return 0;
  const qint64 pid = readPid(grabRequestPath(runtime));
  return processAlive(pid) ? pid : 0;
}

TimedPreviewsHidden::TimedPreviewsHidden(QString runtime, int timeoutMs)
    : runtime_(std::move(runtime)) {
  if (runtime_.isEmpty())
    return;
  static const QRegularExpression marker(QStringLiteral("^timed-([0-9]+)$"));
  const QDir dir(runtime_);
  const auto markers = dir.entryList({QStringLiteral("timed-*")}, QDir::Files);
  for (const QString &name : markers) {
    const auto match = marker.match(name);
    if (!match.hasMatch())
      continue;
    const qint64 pid = match.captured(1).toLongLong();
    if (processAlive(pid))
      pids_.push_back(pid);
    else
      QFile::remove(dir.filePath(name)); // left behind by a card that crashed
  }
  if (pids_.isEmpty())
    return; // nothing on screen to hide: no wait at all
  const QByteArray self = QByteArray::number(QCoreApplication::applicationPid());
  for (const qint64 pid : std::as_const(pids_))
    QFile::remove(grabAckPath(runtime_, pid));
  QSaveFile request(grabRequestPath(runtime_));
  if (!request.open(QIODevice::WriteOnly) ||
      !request.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
      request.write(self) != self.size() || !request.commit())
    return;
  requested_ = true;
  // Each card paints itself transparent and round-trips the compositor
  // before it acknowledges, so the next composited frame is without it.
  // Events are processed while waiting: nothing of this process is on screen
  // yet, and it lets a card in this same process (the smoke suite) answer.
  const QDeadlineTimer deadline(timeoutMs);
  QVector<qint64> waiting = pids_;
  while (!waiting.isEmpty() && !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
    for (qsizetype index = waiting.size() - 1; index >= 0; --index) {
      if (readPid(grabAckPath(runtime_, waiting.at(index))) ==
          QCoreApplication::applicationPid())
        waiting.removeAt(index);
    }
    if (!waiting.isEmpty())
      QThread::msleep(1);
  }
}

TimedPreviewsHidden::~TimedPreviewsHidden() {
  if (!requested_)
    return;
  // Removing the request is the signal to come back.
  QFile::remove(grabRequestPath(runtime_));
  for (const qint64 pid : std::as_const(pids_))
    QFile::remove(grabAckPath(runtime_, pid));
}
