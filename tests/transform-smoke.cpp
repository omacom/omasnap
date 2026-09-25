/** @fileoverview Tests Wayland capture transforms and object cleanup. */
#include "transform-smoke.hpp"

#include "capture.hpp"
#include "surface-capture-smoke.hpp"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QVector>

#include <wayland-client-protocol.h>

namespace {
/** Writes an executable helper used to replace an external command. */
bool writeExecutable(const QString &path, const QByteArray &contents) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(contents) != contents.size())
    return false;
  file.close();
  return QFile::setPermissions(path, QFileDevice::ReadOwner |
                                         QFileDevice::WriteOwner |
                                         QFileDevice::ExeOwner);
}

/** Creates a small image whose red channel stores easy-to-check values. */
QImage indexedImage(const QVector<QVector<int>> &rows) {
  QImage image(rows.constFirst().size(), rows.size(), QImage::Format_RGB32);
  for (int y = 0; y < rows.size(); ++y) {
    for (int x = 0; x < rows.at(y).size(); ++x)
      image.setPixelColor(x, y, QColor(rows.at(y).at(x), 0, 0));
  }
  return image;
}
} // namespace

bool runTransformSmoke(QString &error) {
  if (!runWaylandCleanupChecks()) {
    error =
        QStringLiteral("Wayland capture objects were not released in order");
    return false;
  }
  QTemporaryDir fakeCommands;
  if (!fakeCommands.isValid()) {
    error = QStringLiteral("Could not create transform-test directory");
    return false;
  }
  const QString fakeHyprctl =
      QDir(fakeCommands.path()).filePath(QStringLiteral("hyprctl"));
  const QByteArray hyprctlScript = QByteArrayLiteral(
      "#!/usr/bin/env bash\n"
      "set -euo pipefail\n"
      "if [[ \"${1:-}\" == \"monitors\" ]]; then\n"
      "  printf '[{\"focused\":true,\"scale\":1.0,\"width\":300,"
      "\"height\":200,\"transform\":%s,\"name\":\"TEST-ROTATED\","
      "\"x\":0,\"y\":0,\"activeWorkspace\":{\"id\":7}}]\\n' "
      "\"$OMASNAP_TEST_TRANSFORM\"\n"
      "else\n"
      "  printf '[{\"workspace\":{\"id\":7},\"at\":[0,0],\"size\":[100,100],"
      "\"title\":\"Test window\",\"stableId\":\"w1\"}]\\n'\n"
      "fi\n");
  if (!writeExecutable(fakeHyprctl, hyprctlScript)) {
    error = QStringLiteral("Could not create transform-test commands");
    return false;
  }
  QImage rotatedSource(300, 200, QImage::Format_RGB32);
  rotatedSource.fill(QColor(QStringLiteral("#123456")));
  const QString capturePath =
      QDir(fakeCommands.path()).filePath(QStringLiteral("source.png"));
  if (!rotatedSource.save(capturePath, "PNG")) {
    error = QStringLiteral("Could not create transform-test capture");
    return false;
  }

  const QByteArray originalPath = qgetenv("PATH");
  qputenv("PATH", fakeCommands.path().toUtf8() + ':' + originalPath);
  qputenv("OMASNAP_TEST_CAPTURE", capturePath.toUtf8());
  const auto restoreEnvironment = [&originalPath] {
    qputenv("PATH", originalPath);
    qunsetenv("OMASNAP_TEST_CAPTURE");
    qunsetenv("OMASNAP_TEST_TRANSFORM");
  };
  for (const int transform : {1, 3, 5, 7}) {
    qputenv("OMASNAP_TEST_TRANSFORM", QByteArray::number(transform));
    CaptureData rotatedCapture;
    if (!captureFocusedMonitor(rotatedCapture, true, error) ||
        rotatedCapture.monitor.geometry.size() != QSize(200, 300) ||
        rotatedCapture.previewSize != QSize(200, 300) ||
        rotatedCapture.windows.size() != 1) {
      if (error.isEmpty())
        error = QStringLiteral("Quarter-turn monitor geometry was not swapped");
      restoreEnvironment();
      return false;
    }
  }

  // Callers that never show the overlay skip window discovery entirely.
  qputenv("OMASNAP_TEST_TRANSFORM", QByteArrayLiteral("0"));
  CaptureData withoutWindows;
  if (!captureFocusedMonitor(withoutWindows, false, error) ||
      withoutWindows.source.size() != QSize(300, 200) ||
      withoutWindows.previewSize != QSize(300, 200) ||
      !withoutWindows.windows.isEmpty()) {
    if (error.isEmpty())
      error = QStringLiteral("Capture without window discovery was incorrect");
    restoreEnvironment();
    return false;
  }

  // Without a test capture, missing output-capture protocol must fail clearly.
  qunsetenv("OMASNAP_TEST_CAPTURE");
  CaptureData missingProtocol;
  QString protocolError;
  const bool captured =
      captureFocusedMonitor(missingProtocol, false, protocolError);
  if (captured || protocolError.isEmpty()) {
    error = QStringLiteral("Missing output capture did not report an error");
    restoreEnvironment();
    return false;
  }

  // Every active monitor is discovered, focused or not, for the veils.
  if (!writeExecutable(
          fakeHyprctl,
          QByteArrayLiteral(
              "#!/usr/bin/env bash\n"
              "printf '[{\"focused\":false,\"scale\":1.25,\"width\":1920,"
              "\"height\":1080,\"transform\":0,\"name\":\"TEST-SIDE\","
              "\"x\":2048,\"y\":0,\"activeWorkspace\":{\"id\":2}},"
              "{\"focused\":true,\"scale\":1.0,\"width\":300,"
              "\"height\":200,\"transform\":1,\"name\":\"TEST-MAIN\","
              "\"x\":0,\"y\":0,\"activeWorkspace\":{\"id\":7}}]\\n'\n"))) {
    error = QStringLiteral("Could not create monitor-probe commands");
    restoreEnvironment();
    return false;
  }
  QString probeError;
  const QVector<MonitorInfo> monitors = probeMonitors(probeError);
  if (monitors.size() != 2 ||
      monitors.at(0).name != QStringLiteral("TEST-SIDE") ||
      monitors.at(0).geometry != QRect(2048, 0, 1536, 864) ||
      monitors.at(0).workspaceId != 2 ||
      monitors.at(1).geometry != QRect(0, 0, 200, 300)) {
    error = QStringLiteral("Monitor probe did not report every monitor: %1")
                .arg(probeError);
    restoreEnvironment();
    return false;
  }

  restoreEnvironment();

  const QImage upright = indexedImage({{1, 2}, {3, 4}, {5, 6}});
  const QVector<QPair<std::uint32_t, QImage>> transformedImages{
      {WL_OUTPUT_TRANSFORM_NORMAL, upright},
      {WL_OUTPUT_TRANSFORM_90, indexedImage({{2, 4, 6}, {1, 3, 5}})},
      {WL_OUTPUT_TRANSFORM_180, indexedImage({{6, 5}, {4, 3}, {2, 1}})},
      {WL_OUTPUT_TRANSFORM_270, indexedImage({{5, 3, 1}, {6, 4, 2}})},
      {WL_OUTPUT_TRANSFORM_FLIPPED, indexedImage({{2, 1}, {4, 3}, {6, 5}})},
      {WL_OUTPUT_TRANSFORM_FLIPPED_90, indexedImage({{1, 3, 5}, {2, 4, 6}})},
      {WL_OUTPUT_TRANSFORM_FLIPPED_180, indexedImage({{5, 6}, {3, 4}, {1, 2}})},
      {WL_OUTPUT_TRANSFORM_FLIPPED_270, indexedImage({{6, 4, 2}, {5, 3, 1}})},
  };
  for (const auto &[transform, transformed] : transformedImages) {
    if (normalizeWaylandCapture(transformed, transform) != upright) {
      error = QStringLiteral("Captured Wayland buffer was not upright");
      return false;
    }
  }
  return true;
}
