/** @fileoverview Independently decode PNG output and verify lossless pixels. */
#include "png-smoke.hpp"

#include "capture.hpp"
#include "png.hpp"

#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QColorSpace>
#include <QImage>
#include <QIODevice>
#include <QPoint>
#include <QRandomGenerator>
#include <QSize>
#include <QString>
#include <Qt>
#include <QtTypes>
#include <QtEndian>
#include <QtMinMax>

namespace {
QByteArray imageData(const QByteArray &png) {
  QByteArray data;
  for (qsizetype offset = 8; offset + 12 <= png.size();) {
    const quint32 size = qFromBigEndian<quint32>(png.constData() + offset);
    if (size > png.size() - offset - 12)
      return {};
    if (png.mid(offset + 4, 4) == "IDAT")
      data.append(png.constData() + offset + 8, size);
    offset += size + 12;
  }
  return data;
}

bool roundTrip(const QImage &source, QString &error) {
  QByteArray bytes;
  QBuffer buffer(&bytes);
  if (!buffer.open(QIODevice::WriteOnly) || !writePng(source, buffer)) {
    error = QStringLiteral("Could not encode PNG format %1").arg(source.format());
    return false;
  }
  // Qt/libpng is an independent reader. Compare with Qt's own PNG writer
  // too, including premultiplied-alpha rounding and high-bit-depth pixels.
  QByteArray reference;
  QBuffer referenceBuffer(&reference);
  if (!referenceBuffer.open(QIODevice::WriteOnly) || !source.save(&referenceBuffer, "PNG"))
    return false;
  const QImage decoded = QImage::fromData(bytes, "PNG");
  const QImage expected = QImage::fromData(reference, "PNG");
  if (decoded.isNull() || expected.isNull() ||
      decoded.convertToFormat(QImage::Format_RGBA64) !=
          expected.convertToFormat(QImage::Format_RGBA64) ||
      decoded.dotsPerMeterX() != expected.dotsPerMeterX() ||
      decoded.dotsPerMeterY() != expected.dotsPerMeterY() ||
      decoded.offset() != expected.offset() ||
      decoded.colorSpace() != expected.colorSpace() ||
      decoded.text() != expected.text()) {
    error = QStringLiteral("PNG pixels or metadata changed for format %1").arg(source.format());
    return false;
  }
  return true;
}

class FailingDevice : public QIODevice {
public:
  explicit FailingDevice(qint64 budget) : remaining_(budget) { open(QIODevice::WriteOnly); }
protected:
  qint64 readData(char *, qint64) override { return -1; }
  qint64 writeData(const char *, qint64 size) override {
    if (size > remaining_)
      return -1;
    remaining_ -= size;
    return size;
  }
private:
  qint64 remaining_;
};
} // namespace

bool runPngSmoke(QString &error) {
  QRandomGenerator random(71);
  for (const QSize size : {QSize(1, 1), QSize(3, 7), QSize(127, 65)}) {
    QImage source(size, QImage::Format_ARGB32);
    for (int y = 0; y < size.height(); ++y)
      for (int x = 0; x < size.width(); ++x)
        source.setPixel(x, y, random.generate());
    source.setDotsPerMeterX(3780);
    source.setDotsPerMeterY(7560);
    for (const auto format : {QImage::Format_RGB32, QImage::Format_RGB888,
                              QImage::Format_ARGB32, QImage::Format_ARGB32_Premultiplied,
                              QImage::Format_RGBA8888, QImage::Format_RGBA8888_Premultiplied,
                              QImage::Format_Grayscale8, QImage::Format_Indexed8,
                              QImage::Format_Mono, QImage::Format_Grayscale16,
                              QImage::Format_RGBA64}) {
      if (!roundTrip(source.convertToFormat(format), error))
        return false;
    }
    QByteArray ordinary;
    QBuffer ordinaryBuffer(&ordinary);
    ordinaryBuffer.open(QIODevice::WriteOnly);
    if (!writePng(source, ordinaryBuffer))
      return false;
    const QSize logical(qMax(1, size.width() / 2), qMax(1, size.height() / 2));
    setPngLogicalSize(source, logical);
    QByteArray tagged;
    QBuffer taggedBuffer(&tagged);
    taggedBuffer.open(QIODevice::WriteOnly);
    if (!writePng(source, taggedBuffer) || !roundTrip(source, error) ||
        pngLogicalSize(QImage::fromData(tagged, "PNG")) != logical ||
        imageData(ordinary).isEmpty() || imageData(ordinary) != imageData(tagged)) {
      error = QStringLiteral("Logical size metadata changed PNG pixels, scale, or fast compression");
      return false;
    }
    source.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    source.setOffset(QPoint(-12, 43));
    source.setText(QStringLiteral("Description"), QStringLiteral("Screenshot α with metadata"));
    if (!roundTrip(source, error))
      return false;
  }

  QImage precise(7, 3, QImage::Format_RGBA64);
  precise.fill(QColor::fromRgba64(111, 2222, 33333, 44444));
  for (const auto format : {QImage::Format_RGBA64, QImage::Format_RGB30,
                            QImage::Format_A2RGB30_Premultiplied,
                            QImage::Format_Grayscale16}) {
    if (!roundTrip(precise.convertToFormat(format), error))
      return false;
  }

  QImage image(13, 9, QImage::Format_RGBA8888);
  image.fill(Qt::transparent);
  for (const QString &invalid : {QString(), QStringLiteral("0x4"),
                                 QStringLiteral("4x0"), QStringLiteral("-1x4"),
                                 QStringLiteral("14x4"), QStringLiteral("4x10"),
                                 QStringLiteral("4x4x4"), QStringLiteral("4.5x4"),
                                 QStringLiteral("999999999999999999x4"),
                                 QStringLiteral("4x4α")}) {
    image.setText(QStringLiteral("Omasnap logical size"), invalid);
    if (!pngLogicalSize(image).isEmpty() || !roundTrip(image, error)) {
      error = QStringLiteral("Malformed logical size was accepted or damaged PNG metadata");
      return false;
    }
  }
  setPngLogicalSize(image, QSize(6, 4));
  QBuffer closed;
  if (writePng(image, closed)) {
    error = QStringLiteral("PNG output accepted a closed device");
    return false;
  }
  for (const int budget : {0, 8, 33, 55, 70, 90}) {
    FailingDevice failed(budget);
    if (writePng(image, failed) || writePng({}, failed)) {
      error = QStringLiteral("PNG output ignored a write failure or empty image");
      return false;
    }
  }
  // A long scroll exercises the streaming path, above the bounded encoder's
  // scratch budget. Sparse contents keep this regression inexpensive.
  QImage scroll(1024, 33000, QImage::Format_ARGB32);
  scroll.fill(Qt::white);
  scroll.setPixelColor(23, 32999, Qt::red);
  setPngLogicalSize(scroll, QSize(512, 16500));
  QByteArray bytes;
  QBuffer buffer(&bytes);
  if (!buffer.open(QIODevice::WriteOnly) || !writePng(scroll, buffer) ||
      QImage::fromData(bytes, "PNG").convertToFormat(scroll.format()) != scroll ||
      pngLogicalSize(QImage::fromData(bytes, "PNG")) != QSize(512, 16500)) {
    error = QStringLiteral("A long scrolling capture lost pixels in PNG output");
    return false;
  }
  return true;
}

bool runPngLogicalSizeBoundsSmoke(QString &error) {
  // Real display scales, a long scroll capture included, are believed.
  struct Kept {
    QSize pixels;
    QSize logical;
  };
  for (const Kept &kept : {Kept{{2000, 2000}, {2000, 2000}},
                           Kept{{2000, 2000}, {1000, 1000}},
                           Kept{{3200, 2000}, {2560, 1600}},
                           Kept{{2000, 2000}, {500, 500}},
                           Kept{{1024, 33000}, {512, 16500}}}) {
    QImage image(kept.pixels, QImage::Format_RGB32);
    setPngLogicalSize(image, kept.logical);
    if (pngLogicalSize(image) != kept.logical) {
      error = QStringLiteral("A %1x%2 logical size on %3x%4 pixels was refused")
                  .arg(kept.logical.width())
                  .arg(kept.logical.height())
                  .arg(kept.pixels.width())
                  .arg(kept.pixels.height());
      return false;
    }
  }
  // A tag from a PNG anyone can hand to --file is not believed when it
  // claims an impossible scale: it would multiply the frame drawn around
  // the export (and the editor's view) by that factor.
  // An opened file keeps its pixels, so its output scale per axis is the
  // source size over the logical size (what renderCapture multiplies by).
  const auto outputScale = [](const CaptureData &capture) {
    return QSizeF(capture.source.width() / qreal(capture.previewSize.width()),
                  capture.source.height() / qreal(capture.previewSize.height()));
  };
  QImage small(400, 400, QImage::Format_RGB32);
  small.fill(Qt::white);
  for (const char *tag : {"40x40", "99x99", "1x1", "400x10"}) {
    small.setText(QStringLiteral("Omasnap logical size"), QString::fromLatin1(tag));
    CaptureData capture;
    describeFileCapture(capture, small, {});
    const QSizeF scale = outputScale(capture);
    if (scale.width() > 4.0 || scale.height() > 4.0) {
      const QImage framed =
          renderCapture(capture, QRectF(QPointF(), capture.previewSize), {},
                        BackgroundStyle::Slate, false, CanvasBoundaryMode::Framed);
      error = QStringLiteral("Logical size tag %1 on a 400x400 PNG was believed: "
                             "scale %2x%3, framed export %4x%5")
                  .arg(QString::fromLatin1(tag))
                  .arg(scale.width())
                  .arg(scale.height())
                  .arg(framed.width())
                  .arg(framed.height());
      return false;
    }
  }
  // An edit log's preview size sets the same scale and obeys the same bound.
  small.setText(QStringLiteral("Omasnap logical size"), QString());
  OperationLog log;
  log.previewSize = {40, 40};
  CaptureData capture;
  describeFileCapture(capture, small, log);
  const QSizeF scale = outputScale(capture);
  if (scale.width() > 4.0 || scale.height() > 4.0) {
    error = QStringLiteral("An operation log's 40x40 preview size on a 400x400 "
                           "image was believed: scale %1x%2")
                .arg(scale.width())
                .arg(scale.height());
    return false;
  }
  return true;
}
