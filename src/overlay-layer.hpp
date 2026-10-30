/** @fileoverview Full-output layer surfaces for the capture overlay. */
#pragma once

#include "capture.hpp"

#include <QColor>
#include <QWidget>

class QPaintEvent;
class QMouseEvent;
class QEnterEvent;
class QScreen;
namespace LayerShellQt {
class Window;
}

/// Puts `widget` on a full-output layer surface on `screen`: Omasnap's
/// scope, the overlay layer, anchored to every edge and ignoring exclusive
/// zones, so a frozen frame painted into it lines up with the output. Only
/// the interactive overlay takes the keyboard. Null without a layer shell.
[[nodiscard]] LayerShellQt::Window *
configureOverlayLayer(QWidget &widget, QScreen *screen, bool takesKeyboard);

/**
 * A monitor the select overlay is not on, frozen when the capture started:
 * its frame under the same dim the overlay paints, so every screen reads as
 * waiting for a selection. Keyboard input stays with the picker. Pointer
 * arrival is forwarded too: newer Hyprland versions send motion to the veil
 * instead of keeping it on the picker's exclusive-keyboard surface.
 */
class MonitorVeil final : public QWidget {
  Q_OBJECT
public:
  MonitorVeil(CaptureData capture, QColor scrim);

  [[nodiscard]] const CaptureData &capture() const { return capture_; }
  /// Maps the veil over its monitor. False when that output is gone or
  /// there is no layer shell.
  [[nodiscard]] bool present();
  /// Whether the frozen frame has been drawn into the surface, so the
  /// overlay can leave this monitor without the live desktop flashing.
  [[nodiscard]] bool painted() const { return painted_; }

signals:
  void firstPainted();
  void pointerMoved(const QPointF &position);

protected:
  void paintEvent(QPaintEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void enterEvent(QEnterEvent *event) override;

private:
  CaptureData capture_;
  QColor scrim_;
  bool painted_ = false;
};
