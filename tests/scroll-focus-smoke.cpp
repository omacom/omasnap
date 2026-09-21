/** @fileoverview The scroll panel cannot reclaim focus after handing it back. */
#include "scroll-focus-smoke.hpp"
#include "scroll-capture.hpp"

#include <LayerShellQt/Window>
#include <QApplication>
#include <QEnterEvent>
#include <QEvent>
#include <QPointF>
#include <QRect>
#include <QString>
#include <QWidget>

bool runScrollFocusSmoke(QString &error) {
  QWidget host;
  host.resize(800, 600);
  static_cast<void>(host.winId());
  auto *layer = LayerShellQt::Window::get(host.windowHandle());
  if (!layer) {
    error = QStringLiteral("Could not create the scroll focus fixture");
    return false;
  }
  layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityExclusive);
  MonitorInfo monitor;
  monitor.geometry = QRect(0, 0, 800, 600);
  ScrollCapturePanel panel(monitor, layer, &host);
  panel.begin(QRect(100, 100, 400, 300));
  if (layer->keyboardInteractivity() != LayerShellQt::Window::KeyboardInteractivityOnDemand) {
    error = QStringLiteral("The live page did not release exclusive focus");
    return false;
  }
  // Hovering the chrome owns focus when Escape/Done dismisses the panel.
  const QPointF point(20, 20);
  QEnterEvent enter(point, point, point);
  QApplication::sendEvent(&panel, &enter);
  panel.release();
  // Hiding a hovered child can deliver Leave after the editor has already
  // reclaimed its surface. That old event must not release the editor's grab.
  QEvent leave(QEvent::Leave);
  QApplication::sendEvent(&panel, &leave);
  if (layer->keyboardInteractivity() != LayerShellQt::Window::KeyboardInteractivityExclusive) {
    error = QStringLiteral("A released scroll panel stole the editor's keyboard grab");
    return false;
  }
  return true;
}
