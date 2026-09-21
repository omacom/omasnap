// Exercise the private window's real event handlers without adding a public
// window API or test hooks to the production binary. pin.cpp is not otherwise
// linked into the smoke executable.
#include "../src/pin.cpp"
#include "chrome-theme.hpp"
#include "pin-layout.hpp"
#include "pin-interaction-smoke.hpp"

#include <QByteArray>
#include <QHelpEvent>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPoint>
#include <QRectF>
#include <QSaveFile>
#include <QScopeGuard>
#include <QString>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <Qt>
#include <QtEnvironmentVariables>
#include <QtMath>

bool runPinThemeRenderingSmoke(const QString &path, QString &error) {
  const QTemporaryDir runtime;
  if (!runtime.isValid()) {
    error = QStringLiteral("Could not isolate the pin theme fixture");
    return false;
  }
  const QByteArray previousRuntime = qgetenv("XDG_RUNTIME_DIR");
  const auto restore = qScopeGuard([&] {
    pinPool().waitForDone();
    QThreadPool::globalInstance()->waitForDone();
    if (previousRuntime.isNull())
      qunsetenv("XDG_RUNTIME_DIR");
    else
      qputenv("XDG_RUNTIME_DIR", previousRuntime);
  });
  qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
  QImage source(320, 200, QImage::Format_ARGB32_Premultiplied);
  source.fill(Qt::transparent);
  const QString sourcePath = runtime.filePath(QStringLiteral("source.png"));
  if (!source.save(sourcePath)) {
    error = QStringLiteral("Could not save the pin theme fixture");
    return false;
  }
  // No show/event loop: no compositor placement or interaction is requested.
  PinWindow pin(source, sourcePath, source.size());
  QImage card(source.size(), QImage::Format_ARGB32_Premultiplied);
  card.fill(Qt::transparent);
  pin.render(&card);
  const QRectF button = pinControlRect(pin.size(), 5);
  const QPoint fill(qRound(button.left() + 2), qRound(button.center().y()));
  const bool saved = card.save(path);
  if (card.pixelColor(160, 100).rgba() != chromeTheme().surface.rgba() ||
      card.pixelColor(fill).rgba() != chromeTheme().button.rgba() || !saved) {
    error = QStringLiteral("Pin chrome: surface=%1 expected=%2, button=%3 expected=%4 at %5,%6")
                .arg(card.pixelColor(160, 100).name(), chromeTheme().surface.name(),
                     card.pixelColor(fill).name(), chromeTheme().button.name())
                .arg(fill.x()).arg(fill.y());
    return false;
  }
  return true;
}

bool runPinInteractionSmoke(QString &error) {
  QTemporaryDir runtime;
  if (!runtime.isValid()) {
    error = QStringLiteral("Could not create the pin interaction fixture");
    return false;
  }
  // Clipboard and compositor calls must not touch the developer's session.
  // An editor child exits in the smoke entry point before creating any UI.
  for (const QString &name : {QStringLiteral("hyprctl"), QStringLiteral("wl-copy"),
                              QStringLiteral("wl-paste")}) {
    QFile command(runtime.filePath(name));
    if (!command.open(QIODevice::WriteOnly) ||
        command.write("#!/bin/sh\nexit 1\n") < 0 ||
        !command.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                  QFileDevice::ExeOwner)) {
      error = QStringLiteral("Could not isolate pin interaction commands");
      return false;
    }
  }
  const QByteArray previousPath = qgetenv("PATH");
  const QByteArray previousRuntime = qgetenv("XDG_RUNTIME_DIR");
  const QByteArray previousChild = qgetenv(kPinSmokeEditorChild);
  const auto restore = qScopeGuard([&] {
    pinPool().waitForDone();
    QThreadPool::globalInstance()->waitForDone();
    for (const auto &variable : {
             qMakePair("PATH", previousPath),
             qMakePair("XDG_RUNTIME_DIR", previousRuntime),
             qMakePair(kPinSmokeEditorChild, previousChild)}) {
      if (variable.second.isNull())
        qunsetenv(variable.first);
      else
        qputenv(variable.first, variable.second);
    }
  });
  qputenv("PATH", runtime.path().toUtf8());
  qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
  qputenv(kPinSmokeEditorChild, "1");

  QImage image(200, 113, QImage::Format_RGB32);
  image.fill(Qt::darkGray);
  const QString path = runtime.filePath(QStringLiteral("capture.png"));
  if (!image.save(path)) {
    error = QStringLiteral("Could not save the pin interaction fixture");
    return false;
  }
  PinWindow window(image, path, image.size(), PinLifetime::Timed);
  window.show();
  const QPoint background(100, 95);
  QEnterEvent enter(background, background, window.mapToGlobal(background));
  QApplication::sendEvent(&window, &enter);
  const auto drainActions = [] {
    // Completion handlers can queue document/stack reads of their own.
    for (int pass = 0; pass < 3; ++pass) {
      pinPool().waitForDone();
      QThreadPool::globalInstance()->waitForDone();
      QCoreApplication::sendPostedEvents();
    }
  };
  const auto isKept = [&] {
    const QPoint point = pinControlRect(window.size(), 5).center().toPoint();
    QHelpEvent tip(QEvent::ToolTip, point, window.mapToGlobal(point));
    QApplication::sendEvent(&window, &tip);
    return QToolTip::text().startsWith(QStringLiteral("Unpin"));
  };
  const auto expectTimed = [&](const QString &action) {
    drainActions();
    if (!isKept())
      return true;
    error = QStringLiteral("%1 implicitly pinned a timed preview").arg(action);
    return false;
  };
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, background);
  if (!expectTimed(QStringLiteral("Clicking the image")))
    return false;
  QWheelEvent wheel(background, window.mapToGlobal(background), {}, QPoint(0, 120),
                     Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
  QApplication::sendEvent(&window, &wheel);
  if (!expectTimed(QStringLiteral("Scrolling")))
    return false;
  for (const int control : {1, 2, 3, 4}) {
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                       pinControlRect(window.size(), control).center().toPoint());
    if (!expectTimed(QStringLiteral("Using control %1").arg(control)))
      return false;
  }
  for (const Qt::Key key : {Qt::Key_C, Qt::Key_L, Qt::Key_E}) {
    QApplication::sendEvent(&window, &enter);
    QTest::keyClick(&window, key);
    if (!expectTimed(QStringLiteral("Using shortcut %1").arg(static_cast<int>(key))))
      return false;
  }
  QTest::keyClick(&window, Qt::Key_C, Qt::ControlModifier);
  if (!expectTimed(QStringLiteral("Ctrl+C")))
    return false;

  // Feed the real drag watcher compositor snapshots without moving any window
  // in the developer's session. Dispatches still fail inside this fixture.
  QFile hyprctl(runtime.filePath(QStringLiteral("hyprctl")));
  if (!hyprctl.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
      hyprctl.write("#!/bin/sh\ncase \"$*\" in\n"
                    "  '-j clients') /bin/cat \"${0%/*}/clients.json\";;\n"
                    "  '-j monitors') /bin/cat \"${0%/*}/monitors.json\";;\n"
                    "  *) exit 1;;\nesac\n") < 0) {
    error = QStringLiteral("Could not prepare the drag compositor fixture");
    return false;
  }
  hyprctl.close();
  QFile monitors(runtime.filePath(QStringLiteral("monitors.json")));
  if (!monitors.open(QIODevice::WriteOnly) ||
      monitors.write("[{\"x\":0,\"y\":0,\"width\":1200,\"height\":900,"
                     "\"scale\":1,\"focused\":true,\"reserved\":[0,0,0,0]}]") < 0) {
    error = QStringLiteral("Could not prepare the drag monitor fixture");
    return false;
  }
  monitors.close();
  const QRect screen(0, 0, 1200, 900);
  const QRect origin(986, 773, 200, 113);
  const CompositorPin older{QStringLiteral("omasnap-pin older"),
                            QStringLiteral("0x222"), QRect(986, 640, 200, 113),
                            true, true};
  const auto pinsAt = [&](const QRect &rect) {
    return QVector<CompositorPin>{{window.windowTitle(), QStringLiteral("0x111"),
                                  rect, true, true}, older};
  };
  const auto writePosition = [&](const QRect &rect) {
    QJsonArray clients;
    for (const CompositorPin &pin : pinsAt(rect))
      clients.push_back(QJsonObject{
          {QStringLiteral("class"), QStringLiteral("omasnap")},
          {QStringLiteral("title"), pin.title},
          {QStringLiteral("address"), pin.address},
          {QStringLiteral("at"), QJsonArray{pin.rect.x(), pin.rect.y()}},
          {QStringLiteral("size"), QJsonArray{pin.rect.width(), pin.rect.height()}},
          {QStringLiteral("floating"), true}, {QStringLiteral("pinned"), true}});
    QSaveFile file(runtime.filePath(QStringLiteral("clients.json")));
    return file.open(QIODevice::WriteOnly) &&
           file.write(QJsonDocument(clients).toJson(QJsonDocument::Compact)) >= 0 &&
           file.commit();
  };
  const struct {
    const char *name;
    QPoint movement;
    bool waitForPoll;
    bool superDrag;
    bool returnToOrigin;
  } drags[] = {{"Clicking without moving", {}, false, false, false},
                {"Holding Super without moving", {}, false, true, false},
                {"Reordering the stack", {0, -160}, true, false, false},
                {"Dragging away from the stack", {-360, -170}, true, false, false},
                {"Dropping between polls", {-300, -220}, false, false, false},
                {"Reordering with Super", {0, -160}, false, true, false},
                {"Dragging back to the starting spot", {-300, -220}, true, false, true}};
  for (const auto &drag : drags) {
    if (!writePosition(origin))
      return false;
    window.setPlacementSnapshot(screen, pinsAt(origin));
    QApplication::sendEvent(&window, &enter);
    drainActions();
    if (drag.superDrag)
      QTest::keyPress(&window, Qt::Key_Meta);
    else
      QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, background);
    if (!writePosition(origin.translated(drag.movement)))
      return false;
    if (drag.waitForPoll && !QTest::qWaitFor(isKept, 1500)) {
      error = QStringLiteral("%1 did not pin the preview during movement")
                  .arg(QString::fromLatin1(drag.name));
      return false;
    }
    if (drag.returnToOrigin && !writePosition(origin))
      return false;
    if (drag.superDrag) {
      QTest::keyRelease(&window, Qt::Key_Meta);
    } else {
      QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, background);
      // The first pointer event after the compositor's move grab ends requests
      // a final snapshot, including drags shorter than one polling interval.
      QMouseEvent wake(QEvent::MouseMove, background, window.mapToGlobal(background),
                       Qt::NoButton, Qt::NoButton, Qt::NoModifier);
      QApplication::sendEvent(&window, &wake);
    }
    drainActions();
    // Let any failed fixture snap finish its existing placement retries.
    QTest::qWait(200);
    drainActions();
    const bool moved = !drag.movement.isNull();
    if (isKept() != moved) {
      error = QStringLiteral("%1 left the preview %2")
                  .arg(QString::fromLatin1(drag.name),
                       moved ? QStringLiteral("timed") : QStringLiteral("pinned"));
      return false;
    }
    if (moved) {
      QTest::keyClick(&window, Qt::Key_P, Qt::ControlModifier);
      if (!expectTimed(QStringLiteral("Unpinning after a drag")))
        return false;
    }
  }
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                     pinControlRect(window.size(), 5).center().toPoint());
  if (!isKept()) {
    error = QStringLiteral("The explicit pin button did not keep the preview");
    return false;
  }
  QTest::keyClick(&window, Qt::Key_P, Qt::ControlModifier);
  if (!expectTimed(QStringLiteral("Unpinning with Ctrl+P")))
    return false;
  QTest::keyClick(&window, Qt::Key_P, Qt::ControlModifier);
  if (!isKept()) {
    error = QStringLiteral("Ctrl+P did not keep the preview");
    return false;
  }
  QApplication::sendEvent(&window, &enter);
  QTest::keyClick(&window, Qt::Key_T);
  if (!expectTimed(QStringLiteral("Unpinning with T")))
    return false;
  QTest::keyClick(&window, Qt::Key_T, Qt::ControlModifier);
  if (!expectTimed(QStringLiteral("Ctrl+T")))
    return false;
  QEvent leave(QEvent::Leave);
  QApplication::sendEvent(&window, &leave);
  QTest::keyClick(&window, Qt::Key_T);
  if (!expectTimed(QStringLiteral("T without hovering")))
    return false;
  QApplication::sendEvent(&window, &enter);
  QTest::keyClick(&window, Qt::Key_T);
  if (!isKept()) {
    error = QStringLiteral("T did not keep the hovered preview");
    return false;
  }
  QKeyEvent repeat(QEvent::KeyPress, Qt::Key_T, Qt::NoModifier,
                   QStringLiteral("t"), true);
  QApplication::sendEvent(&window, &repeat);
  if (!isKept()) {
    error = QStringLiteral("Holding T repeatedly toggled the preview pin");
    return false;
  }
  QToolTip::hideText();
  return true;
}
