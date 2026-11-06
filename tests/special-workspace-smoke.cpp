#include "special-workspace-smoke.hpp"

#include "capture.hpp"
#include "editor.hpp"

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QImage>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QPoint>
#include <QRect>
#include <QRectF>
#include <QScopeGuard>
#include <QString>
#include <QTemporaryDir>
#include <Qt>
#include <QtEnvironmentVariables>
#include <QtTypes>
#include <QtTest/qtestmouse.h>
#include <QtWidgets/qtestsupport_widgets.h>

namespace {
bool writeFixture(const QString &path, const QByteArray &data) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
} // namespace

bool runSpecialWorkspaceSmoke(const QApplication &application, QString &error) {
  const QTemporaryDir fixtures;
  if (!fixtures.isValid()) {
    error = QStringLiteral("Could not create special-workspace fixtures");
    return false;
  }
  const QString command = fixtures.filePath(QStringLiteral("hyprctl"));
  if (!writeFixture(command, QByteArrayLiteral("#!/bin/sh\ncat \"$OMASNAP_WORKSPACE_SMOKE_DIR/$1.json\"\n")) ||
      !QFile::setPermissions(command, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
    error = QStringLiteral("Could not create special-workspace hyprctl");
    return false;
  }
  QImage image(800, 600, QImage::Format_ARGB32_Premultiplied);
  image.fill(QColor(QStringLiteral("#406080")));
  const QString source = fixtures.filePath(QStringLiteral("source.png"));
  if (!image.save(source, "PNG")) {
    error = QStringLiteral("Could not write special-workspace pixels");
    return false;
  }
  const QByteArray oldPath = qgetenv("PATH");
  const QByteArray oldCapture = qgetenv("OMASNAP_TEST_CAPTURE");
  const QByteArray oldFixtures = qgetenv("OMASNAP_WORKSPACE_SMOKE_DIR");
  const auto restore = qScopeGuard([&] {
    qputenv("PATH", oldPath);
    if (oldCapture.isNull())
      qunsetenv("OMASNAP_TEST_CAPTURE");
    else
      qputenv("OMASNAP_TEST_CAPTURE", oldCapture);
    if (oldFixtures.isNull())
      qunsetenv("OMASNAP_WORKSPACE_SMOKE_DIR");
    else
      qputenv("OMASNAP_WORKSPACE_SMOKE_DIR", oldFixtures);
  });
  qputenv("PATH", fixtures.path().toUtf8() + ':' + oldPath);
  qputenv("OMASNAP_TEST_CAPTURE", source.toUtf8());
  qputenv("OMASNAP_WORKSPACE_SMOKE_DIR", fixtures.path().toUtf8());

  QJsonObject monitor{{QStringLiteral("focused"), true},
                       {QStringLiteral("name"), QStringLiteral("TEST")},
                       {QStringLiteral("width"), 800}, {QStringLiteral("height"), 600},
                       {QStringLiteral("scale"), 1.0},
                       {QStringLiteral("x"), 1000}, {QStringLiteral("y"), 200},
                       {QStringLiteral("activeWorkspace"), QJsonObject{{QStringLiteral("id"), 7}}}};
  QJsonObject otherMonitor = monitor;
  otherMonitor.insert(QStringLiteral("focused"), false);
  otherMonitor.insert(QStringLiteral("name"), QStringLiteral("OTHER"));
  otherMonitor.insert(QStringLiteral("x"), 0);
  otherMonitor.insert(QStringLiteral("specialWorkspace"), QJsonObject{{QStringLiteral("id"), -43}});
  const auto client = [](int workspace, const QString &id, const QRect &rect) {
    return QJsonObject{{QStringLiteral("workspace"), QJsonObject{{QStringLiteral("id"), workspace}}},
                        {QStringLiteral("stableId"), id}, {QStringLiteral("title"), id},
                        {QStringLiteral("class"), id + QStringLiteral("-app")},
                        {QStringLiteral("at"), QJsonArray{rect.x() + 1000, rect.y() + 200}},
                        {QStringLiteral("size"), QJsonArray{rect.width(), rect.height()}}};
  };
  const QRect regularRect(100, 100, 500, 400);
  const QRect specialRect(250, 200, 200, 150);
  const QJsonObject regular = client(7, QStringLiteral("regular"), regularRect);
  const QJsonObject special = client(-42, QStringLiteral("special"), specialRect);
  const QJsonObject inactive = client(-43, QStringLiteral("inactive"), regularRect);
  const QJsonObject hidden = client(8, QStringLiteral("hidden"), regularRect);
  MonitorInfo reusedMonitor;
  const QJsonValue activeWorkspace(QJsonObject{{QStringLiteral("id"), -42}});
  for (const QJsonValue &workspace : {
           activeWorkspace, QJsonValue(QJsonValue::Undefined),
           activeWorkspace, QJsonValue(), activeWorkspace,
           QJsonValue(QStringLiteral("malformed")),
           activeWorkspace,
           QJsonValue(QJsonObject{{QStringLiteral("id"), 0}})}) {
    monitor.insert(QStringLiteral("specialWorkspace"), workspace);
    const bool active = workspace.isObject() &&
                        workspace.toObject().value(QStringLiteral("id")).toInt() == -42;
    if (!writeFixture(fixtures.filePath(QStringLiteral("monitors.json")),
                       QJsonDocument(QJsonArray{otherMonitor, monitor}).toJson())) {
      error = QStringLiteral("Could not write special-workspace monitor data");
      return false;
    }
    for (const bool reversed : {false, true}) {
      const QJsonArray clients = reversed ? QJsonArray{special, regular, inactive, hidden}
                                           : QJsonArray{hidden, inactive, regular, special};
      if (!writeFixture(fixtures.filePath(QStringLiteral("clients.json")), QJsonDocument(clients).toJson())) {
        error = QStringLiteral("Could not write special-workspace client data");
        return false;
      }
      CaptureData capture;
      if (!probeFocusedMonitor(reusedMonitor, error) ||
          !captureMonitorPixels(reusedMonitor, capture, true, error))
        return false;
      if (capture.monitor.workspaceId != 7 || capture.monitor.geometry != QRect(1000, 200, 800, 600) ||
          capture.windows.size() != (active ? 2 : 1) ||
          capture.windows.constFirst().stableId != QStringLiteral("regular") ||
          (active && capture.windows.constLast().stableId != QStringLiteral("special"))) {
        error = QStringLiteral("Special-workspace window discovery included hidden clients, lost regular windows, or ordered the special window underneath");
        return false;
      }
      for (const QPoint point : {QPoint(300, 250), QPoint(150, 150)}) {
        CaptureEditor editor(capture, CaptureEditor::CaptureMode::Smart);
        editor.setSuppressSnapshots(true);
        editor.resize(800, 600);
        editor.show();
        if (!QTest::qWaitForWindowExposed(&editor)) {
          error = QStringLiteral("Special-workspace editor was not exposed");
          return false;
        }
        QTest::mouseMove(&editor, point, 10);
        QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, point);
        application.processEvents();
        const QRect expected = active && specialRect.contains(point) ? specialRect : regularRect;
        if (!editor.editingForTest() || editor.currentSelection() != QRectF(expected)) {
          error = QStringLiteral("Smart selection chose the wrong regular or special-workspace window");
          return false;
        }
        const QString expectedApp = expected == specialRect ? QStringLiteral("special-app")
                                                             : QStringLiteral("regular-app");
        if (dominantAppClass(capture.windows, editor.currentSelection()) != expectedApp) {
          error = QStringLiteral("The selected window inherited the underlying app's filename");
          return false;
        }
        editor.close();
      }
    }
  }
  return true;
}
