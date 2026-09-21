#include "output-image.hpp"
#include "output-config.hpp"

#include <QImage>
#include <QSize>
#include <QtCore/qnumeric.h>
#include <QtCore/qtypes.h>
#include <QtCore/qnamespace.h>

#include <algorithm>
#include <cmath>

QImage prepareOutputImage(const QImage &image, qreal scale) {
  if (image.isNull() || !std::isfinite(scale) || scale <= 1.0 ||
      !loadOutputConfig(defaultConfigPath()).logicalSize)
    return image;

  // Scale the completed canvas, including backdrops and annotations. Never
  // resample source pixels before redaction has destroyed sensitive content.
  const QSize size(std::max(1, qRound(image.width() / scale)),
                   std::max(1, qRound(image.height() / scale)));
  QImage output = image.scaled(size, Qt::IgnoreAspectRatio,
                               Qt::SmoothTransformation);
  output.setDevicePixelRatio(1.0);
  return output;
}
