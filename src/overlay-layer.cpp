#include "overlay-layer.hpp"

#include "capture.hpp"

#include <LayerShellQt/Window>

#include <QColor>
#include <QGuiApplication>
#include <QObject>
#include <QPainter>
#include <QPaintEvent>
#include <QRectF>
#include <QScreen>
#include <QString>
#include <QWidget>
#include <QWindow>
#include <Qt>

#include <utility>

LayerShellQt::Window *configureOverlayLayer(QWidget &widget, QScreen *screen,
                                            bool takesKeyboard) {
  widget.setGeometry(screen->geometry());
  widget.winId();
  QWindow *window = widget.windowHandle();
  LayerShellQt::Window *layer =
      window ? LayerShellQt::Window::get(window) : nullptr;
  if (!layer)
    return nullptr;
  // Omarchy's layer rule matches this scope: no map animation, and kept out
  // of screen shares.
  layer->setScope(QStringLiteral("omasnap"));
  layer->setScreen(screen);
  layer->setLayer(LayerShellQt::Window::LayerOverlay);
  LayerShellQt::Window::Anchors anchors;
  anchors.setFlag(LayerShellQt::Window::AnchorTop);
  anchors.setFlag(LayerShellQt::Window::AnchorBottom);
  anchors.setFlag(LayerShellQt::Window::AnchorLeft);
  anchors.setFlag(LayerShellQt::Window::AnchorRight);
  layer->setAnchors(anchors);
  layer->setExclusiveZone(-1);
  layer->setKeyboardInteractivity(
      takesKeyboard ? LayerShellQt::Window::KeyboardInteractivityExclusive
                    : LayerShellQt::Window::KeyboardInteractivityNone);
  layer->setActivateOnShow(takesKeyboard);
  return layer;
}

MonitorVeil::MonitorVeil(CaptureData capture, QColor scrim)
    : capture_(std::move(capture)), scrim_(scrim) {
  setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
  // Only the overlay decides when the capture is over.
  setAttribute(Qt::WA_QuitOnClose, false);
  setAttribute(Qt::WA_OpaquePaintEvent);
}

bool MonitorVeil::present() {
  QScreen *target = nullptr;
  for (QScreen *screen : QGuiApplication::screens()) {
    if (screen->name() == capture_.monitor.name) {
      target = screen;
      break;
    }
  }
  if (!target)
    return false;
  setScreen(target);
  if (!configureOverlayLayer(*this, target, false))
    return false;
  show();
  return true;
}

void MonitorVeil::paintEvent(QPaintEvent *event) {
  QPainter painter(this);
  painter.setClipRegion(event->region());
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  painter.drawImage(QRectF(rect()), capture_.source);
  painter.fillRect(rect(), scrim_);
  if (!painted_) {
    painted_ = true;
    emit firstPainted();
  }
}
