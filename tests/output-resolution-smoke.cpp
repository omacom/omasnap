/** @fileoverview Verifies the opt-in output-resolution downscale: a scaled
 *  monitor's export shrinks to its logical size only when requested, never
 *  for a loaded document, and never when there is nothing to shrink. */
#include "output-resolution-smoke.hpp"

#include "capture.hpp"

#include <QColor>
#include <QImage>
#include <QSize>

namespace {
QImage solidImage(const QSize &size, QRgb color) {
  QImage image(size, QImage::Format_ARGB32_Premultiplied);
  image.fill(color);
  return image;
}

bool sameSizeAndTopLeftPixel(const QImage &image, const QSize &size, QRgb color) {
  return image.size() == size && !image.isNull() &&
         image.pixel(0, 0) == color;
}
} // namespace

bool runOutputResolutionSmoke(QString &error) {
  constexpr QRgb kColor = qRgb(40, 90, 180);

  // A 2x monitor's render is twice the logical size on each axis; opting in
  // shrinks the export back down to what was visually selected.
  CaptureData scaled;
  scaled.monitor.scale = 2.0;
  scaled.previewSize = QSize(100, 60);
  scaled.source = solidImage(QSize(200, 120), kColor);
  const QImage render = solidImage(QSize(200, 120), kColor);

  const QImage optedOut = matchOutputToLogicalSize(scaled, render, false);
  if (!sameSizeAndTopLeftPixel(optedOut, QSize(200, 120), kColor)) {
    error = QStringLiteral("Disabled match_logical_size still resized the export");
    return false;
  }

  const QImage optedIn = matchOutputToLogicalSize(scaled, render, true);
  if (!sameSizeAndTopLeftPixel(optedIn, QSize(100, 60), kColor)) {
    error = QStringLiteral("Enabled match_logical_size did not shrink a 2x export, got %1x%2")
                .arg(optedIn.width())
                .arg(optedIn.height());
    return false;
  }

  // A loaded document (preserveSourceResolution) keeps its exact pixels,
  // regardless of the config: it is the file's final resolution already.
  CaptureData loaded = scaled;
  loaded.preserveSourceResolution = true;
  const QImage keptLoaded = matchOutputToLogicalSize(loaded, render, true);
  if (!sameSizeAndTopLeftPixel(keptLoaded, QSize(200, 120), kColor)) {
    error = QStringLiteral("A loaded document was resized despite preserveSourceResolution");
    return false;
  }

  // A 1x (unscaled) monitor's render already matches its logical size: there
  // is nothing to shrink, with the flag on or off.
  CaptureData unscaled;
  unscaled.monitor.scale = 1.0;
  unscaled.previewSize = QSize(50, 40);
  unscaled.source = solidImage(QSize(50, 40), kColor);
  const QImage unscaledRender = solidImage(QSize(50, 40), kColor);
  if (!sameSizeAndTopLeftPixel(
          matchOutputToLogicalSize(unscaled, unscaledRender, true), QSize(50, 40),
          kColor) ||
      !sameSizeAndTopLeftPixel(
          matchOutputToLogicalSize(unscaled, unscaledRender, false), QSize(50, 40),
          kColor)) {
    error = QStringLiteral("An unscaled monitor's export changed size");
    return false;
  }

  // A null image and an incomplete CaptureData (no source) both fall back to
  // returning the input unchanged instead of crashing on a bad target size.
  if (!matchOutputToLogicalSize(scaled, QImage(), true).isNull()) {
    error = QStringLiteral("A null export was not returned unchanged");
    return false;
  }
  if (matchOutputToLogicalSize(CaptureData{}, render, true).size() != render.size()) {
    error = QStringLiteral("An incomplete CaptureData resized instead of passing through");
    return false;
  }

  return true;
}
