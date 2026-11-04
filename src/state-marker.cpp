#include "state-marker.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureSynchronizer>
#include <QHash>
#include <QIODevice>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QString>
#include <QtConcurrent/QtConcurrentRun>

namespace {
QString markerPath(const QString &name) {
  const QString state =
      QStandardPaths::writableLocation(QStandardPaths::StateLocation);
  return state.isEmpty() ? QString() : QDir(state).filePath(name);
}

QMutex wantedLock;
QHash<QString, bool> wanted;
QMutex writing;

QFutureSynchronizer<void> &saving() {
  static QFutureSynchronizer<void> futures;
  return futures;
}
} // namespace

bool stateMarkerSet(const QString &name) {
  const QString path = markerPath(name);
  return !path.isEmpty() && QFile::exists(path);
}

void setStateMarker(const QString &name, bool set) {
  const QString path = markerPath(name);
  if (path.isEmpty())
    return;
  {
    const QMutexLocker lock(&wantedLock);
    wanted.insert(path, set);
  }
  saving().addFuture(QtConcurrent::run([path] {
    const QMutexLocker lock(&writing);
    bool want = false;
    {
      const QMutexLocker wantedGuard(&wantedLock);
      want = wanted.value(path);
    }
    if (!want) {
      QFile::remove(path);
    } else if (QDir().mkpath(QFileInfo(path).path())) {
      QFile file(path);
      (void)file.open(QIODevice::WriteOnly);
    }
  }));
}

void finishSavingStateMarkers() { saving().waitForFinished(); }
