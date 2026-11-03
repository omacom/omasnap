/** @fileoverview Tests the Wayland display check run before QApplication. */
#include "wayland-display-smoke.hpp"

#include "capture.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QTemporaryDir>

#include <cstddef>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
/** Listens on a Unix socket at `path`, as a compositor does; -1 on failure. */
int listenOnSocket(const QString &path) {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const QByteArray encoded = QFile::encodeName(path);
  if (encoded.size() >= static_cast<qsizetype>(sizeof(address.sun_path)))
    return -1;
  std::memcpy(address.sun_path, encoded.constData(),
              static_cast<std::size_t>(encoded.size()));
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  if (::bind(fd, reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) != 0 ||
      ::listen(fd, 4) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

/** Runs omasnap with no platform chosen and `display` as WAYLAND_DISPLAY
 *  (unset when empty); it must exit 1 with a message, not abort. */
bool runOmasnapWithoutDisplay(const QString &runtime, const QString &display,
                              QString &error) {
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  for (const char *name :
       {"WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DISPLAY", "QT_QPA_PLATFORM"})
    environment.remove(QString::fromLatin1(name));
  if (!display.isEmpty())
    environment.insert(QStringLiteral("WAYLAND_DISPLAY"), display);
  environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime);
  // Qt logs to the journal when stderr is not a terminal; keep it readable.
  environment.insert(QStringLiteral("QT_FORCE_STDERR_LOGGING"),
                     QStringLiteral("1"));
  QProcess process;
  process.setProcessEnvironment(environment);
  process.start(
      QDir(QCoreApplication::applicationDirPath())
          .filePath(QStringLiteral("omasnap")),
      {QStringLiteral("--capture-fullscreen"), QStringLiteral("--save")});
  const QString label =
      display.isEmpty() ? QStringLiteral("unset WAYLAND_DISPLAY") : display;
  if (!process.waitForStarted(5000)) {
    error = QStringLiteral("Could not start omasnap for %1").arg(label);
    return false;
  }
  if (!process.waitForFinished(10000)) {
    process.kill();
    process.waitForFinished(2000);
    error = QStringLiteral("omasnap did not exit with %1").arg(label);
    return false;
  }
  const QString output = QString::fromLocal8Bit(process.readAllStandardError());
  if (process.exitStatus() != QProcess::NormalExit ||
      process.exitCode() != 1 ||
      !output.contains(QStringLiteral("Wayland display"))) {
    error = QStringLiteral("omasnap with %1 did not fail cleanly (status %2, "
                           "code %3): %4")
                .arg(label)
                .arg(process.exitStatus() == QProcess::NormalExit
                         ? QStringLiteral("normal")
                         : QStringLiteral("crashed"))
                .arg(process.exitCode())
                .arg(output.trimmed());
    return false;
  }
  return true;
}
} // namespace

bool runWaylandDisplaySmoke(QString &error) {
  QTemporaryDir runtime;
  if (!runtime.isValid()) {
    error = QStringLiteral("Could not create Wayland runtime directory");
    return false;
  }
  const QByteArray oldDisplay = qgetenv("WAYLAND_DISPLAY");
  const QByteArray oldSocket = qgetenv("WAYLAND_SOCKET");
  const QByteArray oldRuntime = qgetenv("XDG_RUNTIME_DIR");
  const auto restore = qScopeGuard([&] {
    oldDisplay.isNull() ? qunsetenv("WAYLAND_DISPLAY")
                        : qputenv("WAYLAND_DISPLAY", oldDisplay);
    oldSocket.isNull() ? qunsetenv("WAYLAND_SOCKET")
                       : qputenv("WAYLAND_SOCKET", oldSocket);
    oldRuntime.isNull() ? qunsetenv("XDG_RUNTIME_DIR")
                        : qputenv("XDG_RUNTIME_DIR", oldRuntime);
  });
  qunsetenv("WAYLAND_SOCKET");
  qputenv("XDG_RUNTIME_DIR", QFile::encodeName(runtime.path()));

  qunsetenv("WAYLAND_DISPLAY");
  if (waylandDisplayError().isEmpty()) {
    error = QStringLiteral("Unset WAYLAND_DISPLAY passed the display check");
    return false;
  }
  qputenv("WAYLAND_DISPLAY", "wayland-gone");
  if (!waylandDisplayError().contains(QStringLiteral("wayland-gone"))) {
    error = QStringLiteral("Missing Wayland socket passed the display check");
    return false;
  }
  // A compositor that died leaves its socket file behind, refusing connects.
  const int staleFd =
      listenOnSocket(runtime.filePath(QStringLiteral("wayland-stale")));
  if (staleFd < 0) {
    error = QStringLiteral("Could not create the stale Wayland socket");
    return false;
  }
  ::close(staleFd);
  qputenv("WAYLAND_DISPLAY", "wayland-stale");
  if (waylandDisplayError().isEmpty()) {
    error = QStringLiteral("Stale Wayland socket passed the display check");
    return false;
  }
  const int liveFd =
      listenOnSocket(runtime.filePath(QStringLiteral("wayland-live")));
  if (liveFd < 0) {
    error = QStringLiteral("Could not create the live Wayland socket");
    return false;
  }
  const auto closeLive = qScopeGuard([liveFd] { ::close(liveFd); });
  qputenv("WAYLAND_DISPLAY", "wayland-live");
  const QString liveError = waylandDisplayError();
  if (!liveError.isEmpty()) {
    error = QStringLiteral("Live Wayland socket failed the display check: %1")
                .arg(liveError);
    return false;
  }

  return runOmasnapWithoutDisplay(runtime.path(), QString(), error) &&
         runOmasnapWithoutDisplay(runtime.path(), QStringLiteral("wayland-gone"),
                                  error) &&
         runOmasnapWithoutDisplay(runtime.path(),
                                  QStringLiteral("wayland-stale"), error);
}
