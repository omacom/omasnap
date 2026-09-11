/** @fileoverview Capture tab strip on notched and plain panels. */
#include "capture-tab-cutout-smoke.hpp"

#include "overlay-chrome.hpp"

bool runCaptureTabCutoutSmoke(QString &error) {
  const QRect bounds(0, 0, 1728, 1117);

  // A plain monitor has no notch and the layout is the centered one.
  if (captureTabNotchWidth(QSize(1920, 1080), 1.0) != 0.0) {
    error = QStringLiteral("1920x1080 should have no notch");
    return false;
  }
  const QVector<CaptureTab> centered = captureTabLayout(bounds);
  const QVector<CaptureTab> zero = captureTabLayout(bounds, 0.0);
  for (int index = 0; index < centered.size(); ++index) {
    if (zero.at(index).rect != centered.at(index).rect) {
      error = QStringLiteral("zero notch width changed the centered layout");
      return false;
    }
  }

  // The 16" MacBook Pro mode: two tabs either side of the housing.
  const qreal notch = captureTabNotchWidth(QSize(3456, 2234), 2.0);
  if (notch <= 0.0) {
    error = QStringLiteral("3456x2234 should have a notch");
    return false;
  }
  const QVector<CaptureTab> split = captureTabLayout(bounds, notch);
  const qreal center = bounds.width() / 2.0;
  if (split.size() != 4 || split.at(1).rect.right() > center - notch / 2.0 ||
      split.at(2).rect.left() < center + notch / 2.0) {
    error = QStringLiteral("split tabs overlap the notch");
    return false;
  }
  return true;
}
