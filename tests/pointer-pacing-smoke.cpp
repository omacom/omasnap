/** Pointer repaints wait for the window's next frame and cover only damage.
 *
 *  A 1000 Hz mouse delivers several samples per display refresh. The editor
 *  must merge their damage into one paint when the frame arrives (on Wayland,
 *  the surface's frame callback; offscreen, Qt's short update timer), and that
 *  paint must not grow to the whole overlay. Moves here are sent with posted
 *  events flushed after each one, as the event loop would between two pointer
 *  samples, but without running timers: an unpaced update() paints on every
 *  move, while a paced requestUpdate() paints only once the frame arrives. */
#include "pointer-pacing-smoke.hpp"

#include "capture.hpp"
#include "editor.hpp"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QEvent>
#include <QImage>
#include <QList>
#include <QMouseEvent>
#include <QObject>
#include <QPaintEvent>
#include <QPointF>
#include <QRect>
#include <QRegion>
#include <QString>
#include <Qt>
#include <QtCore/qtestsupport_core.h>
#include <QtTest/qtestmouse.h>
#include <QtTypes>
#include <QtWidgets/qtestsupport_widgets.h>

namespace {

/// Records every paint the editor widget receives, and what it covered.
class PaintObserver : public QObject {
public:
  int paints = 0;
  QRegion painted;

  void reset() {
    paints = 0;
    painted = {};
  }

protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (event->type() == QEvent::Paint) {
      ++paints;
      painted |= static_cast<QPaintEvent *>(event)->region();
    }
    return QObject::eventFilter(watched, event);
  }
};

qint64 regionArea(const QRegion &region) {
  qint64 area = 0;
  for (const QRect &rect : region)
    area += static_cast<qint64>(rect.width()) * rect.height();
  return area;
}

void sendMove(CaptureEditor &editor, const QPointF &at,
              Qt::MouseButtons buttons) {
  QMouseEvent event(QEvent::MouseMove, at, editor.mapToGlobal(at),
                    Qt::NoButton, buttons, Qt::NoModifier);
  QApplication::sendEvent(&editor, &event);
  QCoreApplication::sendPostedEvents();
}

/// Sends one burst of moves along `path` and checks it produced exactly one
/// bounded paint, after the burst rather than during it.
bool checkBurst(CaptureEditor &editor, PaintObserver &observer,
                const QList<QPointF> &path, Qt::MouseButtons buttons,
                const QString &name, QString &error) {
  observer.reset();
  for (const QPointF &point : path)
    sendMove(editor, point, buttons);
  if (observer.paints != 0) {
    error = QStringLiteral("%1: %2 pointer moves painted %3 times before the "
                           "next frame; pointer repaints are not paced")
                .arg(name)
                .arg(path.size())
                .arg(observer.paints);
    return false;
  }
  if (!QTest::qWaitFor([&] { return observer.paints > 0; }, 1000)) {
    error = QStringLiteral("%1: the next frame never painted the pointer "
                           "damage")
                .arg(name);
    return false;
  }
  // Give a second, unwanted paint the chance to land before counting.
  QTest::qWait(40);
  if (observer.paints != 1) {
    error = QStringLiteral("%1: one frame of pointer moves painted %2 times")
                .arg(name)
                .arg(observer.paints);
    return false;
  }
  const qint64 widgetArea =
      static_cast<qint64>(editor.width()) * editor.height();
  const qint64 paintedArea = regionArea(observer.painted);
  if (paintedArea * 4 >= widgetArea) {
    error = QStringLiteral("%1: the pointer frame painted %2 of %3 pixels "
                           "instead of only its damage")
                .arg(name)
                .arg(paintedArea)
                .arg(widgetArea);
    return false;
  }
  return true;
}

} // namespace

bool runPointerPacingSmoke(QApplication &application, QString &error) {
  constexpr int width = 2400;
  constexpr int height = 1600;
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = QRect(0, 0, width, height);
  capture.monitor.pixelSize = QSize(width, height);
  capture.monitor.scale = 1.0;
  capture.source = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#406080")));
  capture.previewSize = capture.source.size();

  CaptureEditor editor(capture, CaptureEditor::CaptureMode::Region);
  editor.setSuppressSnapshots(true);
  editor.resize(width, height);
  editor.show();
  if (!QTest::qWaitForWindowExposed(&editor)) {
    error = QStringLiteral("Pointer pacing window was never exposed");
    return false;
  }
  // The select overlay loads the recents shelf on a worker and repaints the
  // whole overlay when it lands. Let that happen before counting paints, or
  // it could arrive mid-burst and look like an unpaced or full pointer frame.
  editor.waitForRecents();
  PaintObserver observer;
  editor.installEventFilter(&observer);

  // Settle the opening full paint and the first pointer position.
  const QPointF start(900, 700);
  sendMove(editor, start, Qt::NoButton);
  QTest::qWait(80);
  application.processEvents();

  // Hover: the crosshair and badge of every sample in the frame are repainted
  // together, including the position the pointer left.
  QList<QPointF> hover;
  for (int step = 1; step <= 6; ++step)
    hover.push_back(start + QPointF(step * 9, step * 5));
  if (!checkBurst(editor, observer, hover, Qt::NoButton,
                  QStringLiteral("Crosshair hover"), error))
    return false;
  QRegion expected = editor.pointerMotionRegionForTest(start);
  for (const QPointF &point : hover)
    expected |= editor.pointerMotionRegionForTest(point);
  const QRegion missed = expected.subtracted(observer.painted);
  if (!missed.isEmpty()) {
    error = QStringLiteral("Crosshair hover: the paced frame missed %1 damaged "
                           "pixels from earlier samples (first at %2,%3)")
                .arg(regionArea(missed))
                .arg(missed.boundingRect().x())
                .arg(missed.boundingRect().y());
    return false;
  }

  // A second frame of hover still paints once: the request is re-armed.
  QList<QPointF> moreHover;
  for (int step = 1; step <= 6; ++step)
    moreHover.push_back(hover.last() + QPointF(step * 7, -step * 4));
  if (!checkBurst(editor, observer, moreHover, Qt::NoButton,
                  QStringLiteral("Second hover frame"), error))
    return false;

  // Dragging out a region is paced the same way.
  const QPointF anchor = moreHover.last();
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, anchor.toPoint());
  QTest::qWait(80);
  application.processEvents();
  QList<QPointF> drag;
  for (int step = 1; step <= 6; ++step)
    drag.push_back(anchor + QPointF(step * 30, step * 20));
  if (!checkBurst(editor, observer, drag, Qt::LeftButton,
                  QStringLiteral("Region drag"), error))
    return false;

  editor.removeEventFilter(&observer);
  editor.close();
  return true;
}
