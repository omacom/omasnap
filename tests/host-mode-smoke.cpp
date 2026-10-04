/** @fileoverview Host mode: argument helpers, the JSON result contract, exit
 *  codes, and a hosted capture that writes only the host's output. */
#include "host-mode-smoke.hpp"

#include "capture.hpp"
#include "editor.hpp"
#include "host-mode.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

namespace {
QJsonObject readJson(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return {};
  return QJsonDocument::fromJson(file.readAll()).object();
}

bool checkHelpers(QString &error) {
  QRect region;
  if (!parseHostRegion(QStringLiteral("10, 20,300,400"), region) ||
      region != QRect(10, 20, 300, 400) ||
      !parseHostRegion(QStringLiteral("-1920,0,5,5"), region) ||
      region != QRect(-1920, 0, 5, 5) ||
      parseHostRegion(QStringLiteral("1,2,3"), region) ||
      parseHostRegion(QStringLiteral("1,2,0,4"), region) ||
      parseHostRegion(QStringLiteral("a,2,3,4"), region)) {
    error = QStringLiteral("--region parsing accepted or rejected the wrong input");
    return false;
  }
  const QStringList argv = splitHostCommand(
      QStringLiteral("/usr/lib/xerahs/omaxerahs upload --quiet \"a b\" 'c d' e\\ f"));
  if (argv != QStringList{QStringLiteral("/usr/lib/xerahs/omaxerahs"),
                          QStringLiteral("upload"), QStringLiteral("--quiet"),
                          QStringLiteral("a b"), QStringLiteral("c d"),
                          QStringLiteral("e f")} ||
      !splitHostCommand(QStringLiteral("   ")).isEmpty()) {
    error = QStringLiteral("Host upload command splitting changed argv");
    return false;
  }
  if (hostUploadUrl(QStringLiteral(" https://i.example/a.png ")) !=
          QStringLiteral("https://i.example/a.png") ||
      hostUploadUrl(QStringLiteral(
          R"({"schemaVersion":1,"ok":true,"url":"https://i.example/b.png"})")) !=
          QStringLiteral("https://i.example/b.png") ||
      !hostUploadUrl(QStringLiteral(R"({"ok":false,"url":"file:///etc"})")).isEmpty() ||
      !hostUploadUrl(QStringLiteral("Uploading...")).isEmpty()) {
    error = QStringLiteral("Host upload output parsing returned the wrong link");
    return false;
  }

  HostResult ok;
  ok.status = QStringLiteral("ok");
  ok.target = QStringLiteral("region");
  ok.path = QStringLiteral("/run/user/1000/xerahs/omasnap/cap.png");
  ok.pixelSize = {2006, 1600};
  ok.logicalSize = {1003, 800};
  ok.scale = 2.0;
  ok.monitor = QStringLiteral("eDP-1");
  ok.region = {120, 80, 1003, 800};
  ok.window = HostWindowInfo{QStringLiteral("firefox"), QStringLiteral("Title"),
                             QStringLiteral("0x5559")};
  const QJsonObject json = hostResultJson(ok, QStringLiteral("1.22.0"));
  const QJsonObject jsonRegion = json.value(QStringLiteral("region")).toObject();
  if (json.value(QStringLiteral("schemaVersion")).toInt() != 1 ||
      json.value(QStringLiteral("status")).toString() != QStringLiteral("ok") ||
      json.value(QStringLiteral("target")).toString() != QStringLiteral("region") ||
      json.value(QStringLiteral("pixelWidth")).toInt() != 2006 ||
      json.value(QStringLiteral("logicalHeight")).toInt() != 800 ||
      json.value(QStringLiteral("scale")).toDouble() != 2.0 ||
      json.value(QStringLiteral("monitor")).toString() != QStringLiteral("eDP-1") ||
      jsonRegion.value(QStringLiteral("x")).toInt() != 120 ||
      jsonRegion.value(QStringLiteral("width")).toInt() != 1003 ||
      json.value(QStringLiteral("window")).toObject().value(QStringLiteral("class")).toString() !=
          QStringLiteral("firefox") ||
      json.value(QStringLiteral("annotated")).toBool() ||
      !json.contains(QStringLiteral("documentPath")) ||
      json.value(QStringLiteral("omasnapVersion")).toString() != QStringLiteral("1.22.0")) {
    error = QStringLiteral("Host result JSON lost a schemaVersion 1 field");
    return false;
  }

  HostCapabilities caps;
  if (hostCapabilitiesJson(caps, QStringLiteral("1.22.0")).value(QStringLiteral("ok")).toBool()) {
    error = QStringLiteral("An empty capability probe reported ok");
    return false;
  }
  caps = {true, true, true, true};
  const QJsonObject capsJson = hostCapabilitiesJson(caps, QStringLiteral("1.22.0"));
  if (!capsJson.value(QStringLiteral("ok")).toBool() ||
      capsJson.value(QStringLiteral("hostMode")).toInt() != kHostModeVersion) {
    error = QStringLiteral("A passing capability probe did not report host mode");
    return false;
  }
  return true;
}

bool checkExitCodes(const QString &directory, QString &error) {
  const QString resultPath = QDir(directory).filePath(QStringLiteral("exit.json"));
  const auto run = [&](int code) {
    QFile::remove(resultPath);
    HostSession session;
    session.outputPath = QDir(directory).filePath(QStringLiteral("exit.png"));
    session.resultPath = resultPath;
    session.target = QStringLiteral("smart");
    setHostSession(session);
    return finishHostRun(code, session.target);
  };
  if (run(0) != kHostExitCancelled ||
      readJson(resultPath).value(QStringLiteral("status")).toString() !=
          QStringLiteral("cancelled") ||
      run(1) != kHostExitFailure ||
      readJson(resultPath).value(QStringLiteral("status")).toString() !=
          QStringLiteral("error") ||
      run(2) != kHostExitUsage) {
    error = QStringLiteral("Host exit codes do not match 0 ok, 1 failure, 2 usage, 3 cancelled");
    return false;
  }
  // One result per process: a later report never replaces the first.
  run(0);
  HostResult late;
  late.status = QStringLiteral("ok");
  reportHostResult(late);
  if (readJson(resultPath).value(QStringLiteral("status")).toString() !=
          QStringLiteral("cancelled") ||
      hostExitCodeForReported(0) != kHostExitCancelled) {
    error = QStringLiteral("A second host result replaced the first");
    return false;
  }
  clearHostSession();
  return true;
}

bool checkHostedCapture(QTemporaryDir &directory, QString &error) {
  // Every side effect has a visible trap: clipboard, notification, the
  // screenshots directory, and launched pins or previews.
  const QString clipboard = directory.filePath(QStringLiteral("clipboard.png"));
  const QString notified = directory.filePath(QStringLiteral("notified"));
  const auto executable = [&](const QString &name, const QByteArray &script) {
    QFile file(directory.filePath(name));
    if (!file.open(QIODevice::WriteOnly) || file.write(script) != script.size())
      return false;
    file.close();
    return file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                               QFileDevice::ExeOwner);
  };
  if (!executable(QStringLiteral("wl-copy"),
                  "#!/bin/sh\ncat > \"$OMASNAP_TEST_HOST_CLIPBOARD\"\n") ||
      !executable(QStringLiteral("wl-paste"),
                  "#!/bin/sh\ncat \"$OMASNAP_TEST_HOST_CLIPBOARD\"\n") ||
      !executable(QStringLiteral("omarchy-notification-send"),
                  "#!/bin/sh\ntouch \"$OMASNAP_TEST_HOST_NOTIFIED\"\n"))
    return false;
  const QString saved = directory.filePath(QStringLiteral("saved"));
  QDir().mkpath(saved);
  const QByteArray oldPath = qgetenv("PATH");
  const QByteArray oldSaveDir = qgetenv("OMASNAP_SCREENSHOT_DIR");
  const auto restore = qScopeGuard([&] {
    qputenv("PATH", oldPath);
    oldSaveDir.isNull() ? qunsetenv("OMASNAP_SCREENSHOT_DIR")
                        : qputenv("OMASNAP_SCREENSHOT_DIR", oldSaveDir);
    qunsetenv("OMASNAP_TEST_HOST_CLIPBOARD");
    qunsetenv("OMASNAP_TEST_HOST_NOTIFIED");
    clearHostSession();
  });
  qputenv("PATH", directory.path().toUtf8() + ':' + oldPath);
  qputenv("OMASNAP_SCREENSHOT_DIR", saved.toUtf8());
  qputenv("OMASNAP_TEST_HOST_CLIPBOARD", clipboard.toUtf8());
  qputenv("OMASNAP_TEST_HOST_NOTIFIED", notified.toUtf8());

  CaptureData capture;
  capture.monitor.name = QStringLiteral("eDP-1");
  capture.monitor.geometry = {1920, 0, 800, 600};
  capture.monitor.pixelSize = {1600, 1200};
  capture.monitor.scale = 2;
  capture.source = QImage(1600, 1200, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#345678")));
  capture.previewSize = {800, 600};
  capture.windows = {{QRect(100, 100, 300, 200), QStringLiteral("0xabc"),
                      QStringLiteral("Fixture title"), QStringLiteral("fixture")}};

  const QString output = directory.filePath(QStringLiteral("host/out.png"));
  const QString resultPath = directory.filePath(QStringLiteral("result.json"));
  HostSession session;
  session.name = QStringLiteral("smoke");
  session.outputPath = output;
  session.resultPath = resultPath;
  session.recents = false;
  session.version = QStringLiteral("test");
  session.target = QStringLiteral("region");
  setHostSession(session);

  bool launched = false;
  {
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::Region,
                         QuickOutputMode::Save);
    editor.setProcessLauncherForTest([&](const QString &, const QStringList &) {
      launched = true;
      return true;
    });
    editor.resize(800, 600);
    editor.show();
    QApplication::processEvents();
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, {150, 150});
    QTest::mouseMove(&editor, {350, 250}, 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, {350, 250});
    const QImage expected = editor.renderCurrentOutput();
    editor.waitForExport();
    const QJsonObject json = readJson(resultPath);
    const QJsonObject region = json.value(QStringLiteral("region")).toObject();
    const QImage written(output);
    if (editor.isVisible() || written.isNull() ||
        written.convertToFormat(expected.format()) != expected ||
        json.value(QStringLiteral("status")).toString() != QStringLiteral("ok") ||
        json.value(QStringLiteral("path")).toString() != output ||
        json.value(QStringLiteral("pixelWidth")).toInt() != expected.width() ||
        json.value(QStringLiteral("logicalWidth")).toInt() != 200 ||
        json.value(QStringLiteral("monitor")).toString() != QStringLiteral("eDP-1") ||
        region.value(QStringLiteral("x")).toInt() != 2070 ||
        region.value(QStringLiteral("width")).toInt() != 200 ||
        json.value(QStringLiteral("window")).toObject().value(QStringLiteral("class")).toString() !=
            QStringLiteral("fixture")) {
      error = QStringLiteral("A hosted region capture did not write its output and ok result");
      return false;
    }
  }
  QApplication::processEvents();
  if (QFile::exists(clipboard) || QFile::exists(notified) || launched ||
      !QDir(saved).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty()) {
    error = QStringLiteral("A hosted capture copied, notified, saved, or launched a preview");
    return false;
  }

  // Esc reports nothing itself; the process exit resolves it to cancelled.
  QFile::remove(resultPath);
  setHostSession(session);
  {
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::Region,
                         QuickOutputMode::Save);
    editor.resize(800, 600);
    editor.show();
    QApplication::processEvents();
    QTest::keyClick(&editor, Qt::Key_Escape);
    QApplication::processEvents();
    if (editor.isVisible() || hostResultReported() ||
        finishHostRun(0, session.target) != kHostExitCancelled ||
        readJson(resultPath).value(QStringLiteral("status")).toString() !=
            QStringLiteral("cancelled")) {
      error = QStringLiteral("Esc in a hosted overlay did not resolve to cancelled");
      return false;
    }
  }
  return true;
}
} // namespace

bool runHostModeSmoke(QString &error) {
  QTemporaryDir directory;
  if (!directory.isValid()) {
    error = QStringLiteral("Could not create a host-mode fixture directory");
    return false;
  }
  return checkHelpers(error) && checkExitCodes(directory.path(), error) &&
         checkHostedCapture(directory, error);
}
