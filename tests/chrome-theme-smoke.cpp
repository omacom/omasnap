#include "chrome-theme-smoke.hpp"
#include "capture.hpp"
#include "chrome-theme.hpp"
#include "editor.hpp"
#include "pin-interaction-smoke.hpp"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QImage>
#include <QMouseEvent>
#include <QPointF>
#include <QSaveFile>
#include <QString>
#include <QTemporaryDir>
#include <Qt>
#include <QtCore/qtestsupport_core.h>
#include <QtMath>

#include <cmath>
#include <functional>

namespace {
bool writeFile(const QString &path, const QByteArray &text) {
  QSaveFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(text) == text.size() &&
         file.commit();
}

bool waitFor(const std::function<bool()> &ready) {
  QElapsedTimer timer;
  timer.start();
  while (!ready() && timer.elapsed() < 3000)
    QTest::qWait(10);
  return ready();
}

QColor pixelAt(const QImage &image, const QPointF &point) {
  return image.pixelColor((point * image.devicePixelRatio()).toPoint());
}

bool closeColor(const QColor &a, const QColor &b) {
  return std::abs(a.red() - b.red()) <= 2 &&
         std::abs(a.green() - b.green()) <= 2 &&
         std::abs(a.blue() - b.blue()) <= 2;
}
} // namespace

bool runChromeThemeSmoke(const QString &outputRoot, QString &error) {
  const QTemporaryDir root;
  if (!root.isValid()) {
    error = QStringLiteral("Could not create theme fixtures");
    return false;
  }
  const QString themeDir = root.filePath(QStringLiteral("current/theme"));
  QDir().mkpath(themeDir);
  const QString colors = QDir(themeDir).filePath(QStringLiteral("colors.toml"));
  const QString shell = QDir(themeDir).filePath(QStringLiteral("shell.toml"));
  const QByteArray dark =
      "background = '#203040'\nforeground = '#edf2f7'\naccent = '#20c0a0'\n";
  const QByteArray light =
      "background = '#f7f0e5'\nforeground = '#252018'\naccent = '#a02065'\n";
  if (loadChromeTheme(themeDir) != ChromeTheme() || !writeFile(colors, dark)) {
    error =
        QStringLiteral("Missing theme files did not use the neutral defaults");
    return false;
  }
  const ChromeTheme simple = loadChromeTheme(themeDir);
  if (simple.surface != QColor("#203040") ||
      simple.foreground != QColor("#edf2f7") ||
      simple.accent != QColor("#20c0a0") ||
      simple.accentText != simple.surface ||
      simple.selectionBorder.colors != QVector<QColor>{simple.accent}) {
    error =
        QStringLiteral("A colors-only theme did not supply the chrome palette");
    return false;
  }
  const QByteArray overrides =
      "[hyprland]\nactive-border = 'rgba(11223380) rgb(445566) 45deg'\n"
      "[popups]\nbackground = 'background' # palette reference\n"
      "text = 'foreground'\nborder = 'hyprland.active-border'\nborder-alpha = "
      "0.5\n"
      "[controls]\nnormal-color = 'foreground'\nnormal-fill-alpha = 0\n"
      "hover-cursor-fill-alpha = 1\nselected-fill-alpha = 0.5\n"
      "[tooltip]\nbackground = '#aabbcc80'\nbackground-alpha = 0.5\n"
      "text = 'foreground'\n";
  if (!writeFile(shell, overrides)) {
    error = QStringLiteral("Could not write shell theme overrides");
    return false;
  }
  const ChromeTheme overridden = loadChromeTheme(themeDir);
  const auto stops = overridden.activeBorder.brush(QRectF(0, 0, 100, 80));
  if (overridden.button != overridden.surface ||
      overridden.buttonHover != overridden.foreground ||
      overridden.activeBorder.colors.size() != 2 ||
      overridden.activeBorder.colors[0].rgba() !=
          QColor(17, 34, 51, 64).rgba() ||
      overridden.activeBorder.colors[1].rgba() !=
          QColor(68, 85, 102, 128).rgba() ||
      overridden.activeBorder.angle != 45 || !stops.gradient() ||
      overridden.tooltipBackground.rgba() != QColor(170, 187, 204, 64).rgba() ||
      overridden.selectionBorder != overridden.activeBorder) {
    error = QStringLiteral(
        "Shell references, RGBA, alpha, or gradient borders failed");
    return false;
  }
  if (!writeFile(
          shell,
          "[popups]\nbackground = 'popups.cycle'\ncycle = 'popups.background'\n"
          "[controls]\nnormal-fill-alpha = -1\n") ||
      loadChromeTheme(themeDir).surface != simple.surface) {
    error = QStringLiteral(
        "Malformed theme overrides lost the usable base palette");
    return false;
  }
  if (!writeFile(colors,
                 "background = 'not-a-color'\nforeground = '#eeeeee'\n") ||
      loadChromeTheme(themeDir) != ChromeTheme() || !writeFile(colors, dark) ||
      !QFile::remove(shell)) {
    error = QStringLiteral("An invalid palette did not fall back safely");
    return false;
  }

  // Main starts this loader once. UI paint calls only read its completed value.
  initializeChromeTheme(themeDir);
  if (!waitFor([&] { return chromeTheme() == simple; })) {
    error =
        QStringLiteral("The asynchronous initial theme load did not finish");
    return false;
  }
  CaptureData capture;
  capture.monitor.scale = 1.0;
  capture.source = QImage(500, 300, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor("#395571"));
  capture.previewSize = capture.source.size();
  OperationLog log;
  Annotation rectangle;
  rectangle.id = 1;
  rectangle.kind = Annotation::Kind::Rectangle;
  rectangle.start = {70, 60};
  rectangle.end = {260, 180};
  rectangle.color = QColor("#ff375f");
  rectangle.size = 4;
  Operation op;
  op.type = Operation::Type::Annotate;
  op.annotations = {rectangle};
  log.ops = {op};
  log.index = 1;
  log.nextId = 2;
  CaptureEditor editor(capture, CaptureEditor::CaptureMode::File,
                       QuickOutputMode::None, log);
  editor.setSuppressSnapshots(true);
  editor.setWindowedPresentation(true);
  editor.resize(1100, 850);
  editor.show();
  QMouseEvent leaveToolbar(QEvent::MouseMove, QPointF(1090, 600),
                           editor.mapToGlobal(QPoint(1090, 600)), Qt::NoButton,
                           Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(&editor, &leaveToolbar);
  QApplication::processEvents();
  const QImage exported = editor.renderCurrentOutput();
  const int history = editor.operationIndex();
  capture.windows = {{QRect(100, 80, 250, 150), QStringLiteral("test"),
                      QStringLiteral("Theme fixture"),
                      QStringLiteral("fixture")}};
  CaptureEditor selector(capture, CaptureEditor::CaptureMode::Smart);
  selector.setSuppressSnapshots(true);
  selector.resize(capture.previewSize);
  selector.show();
  QApplication::processEvents();
  // Deliver directly: several smoke windows share the offscreen display, so
  // native cursor motion can otherwise be routed to another top-level widget.
  const QPoint hover(200, 150);
  QMouseEvent move(QEvent::MouseMove, hover, selector.mapToGlobal(hover),
                   Qt::NoButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(&selector, &move);
  QApplication::processEvents();
  const auto checkEditor = [&](const QString &name) {
    const QImage ui = editor.grab().toImage();
    if (!ui.save(outputRoot + QStringLiteral("-theme-%1.png").arg(name))) {
      error = QStringLiteral("Could not save the theme rendering fixture");
      return false;
    }
    const QRectF button =
        editor.toolbarButtonRectForTest(QStringLiteral("tool-arrow-standard"));
    const QRectF frame = editor.sourceFrameWidgetRectForTest();
    int accentPixels = 0;
    for (int x = qRound(frame.left()) + 30; x < qRound(frame.left()) + 90; ++x)
      for (int y = qRound(frame.top()) - 2; y <= qRound(frame.top()); ++y) {
        // The one-pixel dashed stroke is antialiased over its shadow. Look for
        // each fixture's green/magenta tint, not an opaque exact-color pixel.
        const QColor pixel = pixelAt(ui, QPointF(x, y));
        accentPixels += name == QStringLiteral("dark")
                            ? pixel.green() > pixel.red() + 25 &&
                                  pixel.green() > pixel.blue() + 5
                            : pixel.red() > pixel.green() + 25 &&
                                  pixel.blue() > pixel.green() + 10;
      }
    if (button.isEmpty() || accentPixels < 15 ||
        !closeColor(pixelAt(ui, {20, 450}), chromeTheme().canvasSurface) ||
        !closeColor(pixelAt(ui, {button.center().x(), button.top() + 4}),
                    chromeTheme().button) ||
        editor.renderCurrentOutput() != exported ||
        editor.operationIndex() != history) {
      error =
          QStringLiteral("Theme %1: crop pixels=%2, toolbar width=%3, fill=%4 "
                         "expected=%5, canvas=%6, output unchanged=%7")
              .arg(name)
              .arg(accentPixels)
              .arg(button.width())
              .arg(pixelAt(ui, {button.center().x(), button.top() + 4}).name(),
                   chromeTheme().button.name(), pixelAt(ui, {20, 450}).name())
              .arg(editor.renderCurrentOutput() == exported &&
                   editor.operationIndex() == history);
      return false;
    }
    const QImage selectionUi = selector.grab().toImage();
    const bool saved = selectionUi.save(
        outputRoot + QStringLiteral("-selection-theme-%1.png").arg(name));
    if (!closeColor(pixelAt(selectionUi, {160, 80}), chromeTheme().accent) ||
        !saved) {
      error =
          QStringLiteral("Theme %1 did not reach the window-selection outline: "
                         "pixel=%2, widget=%3x%4, measurement=%5")
              .arg(name, pixelAt(selectionUi, {160, 80}).name())
              .arg(selector.width())
              .arg(selector.height())
              .arg(selector.measurementText());
      return false;
    }
    return runPinThemeRenderingSmoke(
        outputRoot + QStringLiteral("-pin-theme-%1.png").arg(name), error);
  };
  if (!checkEditor(QStringLiteral("dark")))
    return false;

  // Atomic file replacement (QSaveFile) must reattach the watch to the new
  // inode.
  if (!writeFile(colors, light) ||
      !waitFor([] { return chromeTheme().surface == QColor("#f7f0e5"); }) ||
      !checkEditor(QStringLiteral("light"))) {
    if (error.isEmpty())
      error = QStringLiteral(
          "Atomic palette replacement did not update the live UI");
    return false;
  }
  if (!writeFile(colors, dark) ||
      !waitFor([&] { return chromeTheme() == simple; })) {
    error = QStringLiteral("The replacement color file was no longer watched");
    return false;
  }

  // Theme switches replace the directory itself; the parent watch must survive.
  const QString retired = root.filePath(QStringLiteral("retired"));
  if (!QDir().rename(themeDir, retired) || !QDir().mkpath(themeDir) ||
      !writeFile(colors, light) ||
      !waitFor([] { return chromeTheme().surface == QColor("#f7f0e5"); }) ||
      !writeFile(colors, dark) ||
      !waitFor([&] { return chromeTheme() == simple; })) {
    error = QStringLiteral("Theme-directory replacement lost live reload");
    return false;
  }
  // A late shell file, deletion, and recreation all have to be observable too.
  if (!writeFile(shell, "[popups]\nbackground = '#304050'\n") ||
      !waitFor([] { return chromeTheme().surface == QColor("#304050"); }) ||
      !QFile::remove(shell) ||
      !waitFor([&] { return chromeTheme() == simple; }) ||
      !QFile::remove(colors) ||
      !waitFor([] { return chromeTheme() == ChromeTheme(); }) ||
      !writeFile(colors, dark) ||
      !waitFor([&] { return chromeTheme() == simple; })) {
    error = QStringLiteral("Missing/recreated theme files did not recover");
    return false;
  }
  editor.close();
  selector.close();
  return true;
}
