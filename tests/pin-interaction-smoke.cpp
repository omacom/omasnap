// Exercise the private window's real event handlers without adding a public
// window API or test hooks to the production binary. pin.cpp is not otherwise
// linked into the smoke executable.
#include "../src/pin.cpp"
#include "pin-interaction-smoke.hpp"

#include <QHelpEvent>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

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
  QToolTip::hideText();
  return true;
}
