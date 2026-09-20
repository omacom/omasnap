/** @fileoverview Delayed sampling, CLI validation, and real process cancellation. */
#include "capture-delay-smoke.hpp"

#include "capture-delay.hpp"

#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDevice>
#include <QImage>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <Qt>

#include <utility>
#include <signal.h>
#include <sys/types.h>

namespace {
bool writeCommand(const QString &path, const QByteArray &contents) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) &&
         file.write(contents) == contents.size() &&
         file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                             QFileDevice::ExeOwner);
}

bool waitForCountdown(QProcess &process) {
  QElapsedTimer deadline;
  deadline.start();
  QByteArray output;
  while (deadline.elapsed() < 5000 && process.state() != QProcess::NotRunning) {
    process.waitForReadyRead(100);
    output += process.readAll();
    if (output.contains("Capturing in "))
      return true;
  }
  return false;
}

bool finishedSuccessfully(QProcess &process) {
  return (process.state() == QProcess::NotRunning || process.waitForFinished(5000)) &&
         process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}
} // namespace

bool runCaptureDelaySmoke(QString &error) {
  for (const auto &[text, expected] :
       {std::pair{"0", 0}, {"1", 1}, {"3600", 3600}, {"0005", 5}}) {
    int seconds = -1;
    if (!parseCaptureDelay(QString::fromLatin1(text), seconds) || seconds != expected) {
      error = QStringLiteral("Valid capture delay was rejected");
      return false;
    }
  }
  for (const char *text : {"", "-1", "+1", "1.5", "3601", " 1", "1 ",
                           "forever", "99999999999999999999"}) {
    int seconds = -1;
    if (parseCaptureDelay(QString::fromLatin1(text), seconds)) {
      error = QStringLiteral("Invalid capture delay was accepted");
      return false;
    }
  }

  const QTemporaryDir directory;
  const QDir root(directory.path());
  if (!directory.isValid() || !root.mkdir(QStringLiteral("runtime")) ||
      !root.mkdir(QStringLiteral("screenshots")) ||
      !QFile::setPermissions(root.filePath(QStringLiteral("runtime")),
                            QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                QFileDevice::ExeOwner) ||
      !writeCommand(root.filePath(QStringLiteral("hyprctl")),
                    "#!/bin/sh\nprintf '%s\\n' '[{\"name\":\"TEST\","
                    "\"focused\":true,\"width\":64,\"height\":48,\"scale\":1}]'\n") ||
      !writeCommand(root.filePath(QStringLiteral("omarchy-notification-send")),
                    "#!/bin/sh\nexit 0\n")) {
    error = QStringLiteral("Could not prepare delayed capture fixture");
    return false;
  }
  const QString sourcePath = root.filePath(QStringLiteral("source.png"));
  QImage source(64, 48, QImage::Format_ARGB32);
  source.fill(Qt::red);
  if (!source.save(sourcePath)) {
    error = QStringLiteral("Could not write delayed capture source");
    return false;
  }
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert(QStringLiteral("PATH"), directory.path() + QLatin1Char(':') +
                                                environment.value(QStringLiteral("PATH")));
  environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), root.filePath(QStringLiteral("runtime")));
  environment.insert(QStringLiteral("XDG_CONFIG_HOME"), directory.path());
  environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
  environment.insert(QStringLiteral("QT_FORCE_STDERR_LOGGING"), QStringLiteral("1"));
  environment.insert(QStringLiteral("OMASNAP_TEST_CAPTURE"), sourcePath);
  environment.insert(QStringLiteral("OMASNAP_SCREENSHOT_DIR"), root.filePath(QStringLiteral("screenshots")));
  const QString executable = QDir(QCoreApplication::applicationDirPath())
                                 .filePath(QStringLiteral("omasnap"));
  const auto start = [&](QProcess &process, const QStringList &arguments) {
    process.setProcessEnvironment(environment);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(executable, arguments);
    return process.waitForStarted(5000);
  };

  for (const QStringList &arguments : {
           QStringList{"--delay", "-1"}, {"--delay", "1.5"}, {"--delay", "3601"},
           {"--delay", "1", "--clipboard"}, {"--delay", "0", "--file", sourcePath},
           {"--delay", "1", "--file", ""},
           {"--delay", "1", sourcePath}, {"--delay", "1", "--pin", sourcePath},
           {"--delay", "1", "--preview", sourcePath}}) {
    QProcess process;
    if (!start(process, arguments) || !process.waitForFinished(5000) ||
        process.exitStatus() != QProcess::NormalExit || process.exitCode() != 2) {
      error = QStringLiteral("Delayed capture CLI accepted invalid options: %1")
                  .arg(arguments.join(QLatin1Char(' ')));
      return false;
    }
  }

  // The fixture starts red and becomes green during the wait. Exporting green
  // proves main samples after the delay, preserving the fullscreen save path.
  QProcess capture;
  QElapsedTimer elapsed;
  elapsed.start();
  if (!start(capture, {"fullscreen", "--save", "--delay", "1"}) ||
      !waitForCountdown(capture)) {
    error = QStringLiteral("Delayed capture did not reach its wait");
    return false;
  }
  source.fill(Qt::green);
  if (!source.save(sourcePath) || !finishedSuccessfully(capture) || elapsed.elapsed() < 1000) {
    error = QStringLiteral("Delayed fullscreen save failed or captured too early: %1")
                .arg(QString::fromUtf8(capture.readAll()));
    return false;
  }
  const QDir screenshots(root.filePath(QStringLiteral("screenshots")));
  const QStringList saved = screenshots.entryList({QStringLiteral("*.png")}, QDir::Files);
  if (saved.size() != 1 || QImage(screenshots.filePath(saved.first())) != source) {
    error = QStringLiteral("Delayed capture sampled pixels before the wait ended");
    return false;
  }
  if (!QFile::remove(screenshots.filePath(saved.first()))) {
    error = QStringLiteral("Could not clear delayed capture output");
    return false;
  }

  for (const int cancellation : {SIGTERM, SIGINT, 0}) {
    QProcess waiting;
    if (!start(waiting, {"fullscreen", "--save", "--delay", "30"}) ||
        !waitForCountdown(waiting)) {
      error = QStringLiteral("Cancellation fixture did not reach its wait");
      return false;
    }
    if (cancellation == 0) {
      QProcess dismiss;
      if (!start(dismiss, {"fullscreen", "--save"}) || !finishedSuccessfully(dismiss)) {
        error = QStringLiteral("Second invocation failed to cancel delayed capture");
        return false;
      }
    } else {
      if (::kill(static_cast<pid_t>(waiting.processId()), cancellation) != 0) {
        error = QStringLiteral("Could not signal the delayed capture");
        return false;
      }
    }
    if (!finishedSuccessfully(waiting) ||
        !screenshots.entryList({QStringLiteral("*.png")}, QDir::Files).isEmpty() ||
        QFile::exists(root.filePath(QStringLiteral("runtime/omasnap/omasnap.instance")))) {
      error = QStringLiteral("Cancelled delay captured pixels or retained its instance lock");
      return false;
    }
  }
  return true;
}
