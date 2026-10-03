/** @fileoverview Fast, bounded PNG encoding for ordinary 8-bit captures. */
#include "png.hpp"

#include <QByteArray>
#include <QChar>
#include <QColorSpace>
#include <QImage>
#include <QImageWriter>
#include <QIODevice>
#include <QLatin1StringView>
#include <QPixelFormat>
#include <QSize>
#include <QString>
#include <QStringList>
#include <Qt>
#include <QtEndian>
#include <QtTypes>

#include <libdeflate.h>

#include <array>
#include <cstring>
#include <memory>

namespace {
constexpr QLatin1StringView kLogicalSizeKey("Omasnap logical size");

QString logicalSizeText(const QSize &size) {
  return QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
}

bool writeChunk(QIODevice &device, const char (&type)[5], const QByteArray &data) {
  std::array<char, 8> header{};
  qToBigEndian(static_cast<quint32>(data.size()), header.data());
  std::memcpy(header.data() + 4, type, 4);
  quint32 crc = libdeflate_crc32(0, type, 4);
  crc = libdeflate_crc32(crc, data.constData(), data.size());
  std::array<char, 4> checksum{};
  qToBigEndian(crc, checksum.data());
  return device.write(header.data(), header.size()) == qsizetype(header.size()) &&
         device.write(data) == data.size() &&
         device.write(checksum.data(), checksum.size()) == qsizetype(checksum.size());
}
} // namespace

void setPngLogicalSize(QImage &image, const QSize &size) {
  if (size.isEmpty() || size.width() > image.width() ||
      size.height() > image.height())
    return;
  const QString text = logicalSizeText(size);
  // Avoid detaching an already-tagged full-resolution render at output.
  if (image.text(kLogicalSizeKey) != text)
    image.setText(kLogicalSizeKey, text);
}

bool logicalScaleAllowed(const QSize &pixels, const QSize &logical) {
  if (logical.isEmpty() || pixels.isEmpty())
    return false;
  // Display scales run from 1 to about 3; 4 leaves headroom. A larger claim
  // can only come from a hand-made or altered file, and every frame margin
  // and the editor's view would be multiplied by it.
  constexpr qreal maximumScale = 4.0;
  const qreal horizontal = pixels.width() / qreal(logical.width());
  const qreal vertical = pixels.height() / qreal(logical.height());
  return horizontal >= 1.0 && vertical >= 1.0 && horizontal <= maximumScale &&
         vertical <= maximumScale;
}

QSize pngLogicalSize(const QImage &image) {
  const QString text = image.text(kLogicalSizeKey);
  const auto dimensions = text.split(QLatin1Char('x'));
  if (dimensions.size() != 2)
    return {};
  const QSize size(dimensions[0].toInt(), dimensions[1].toInt());
  if (text != logicalSizeText(size) || !logicalScaleAllowed(image.size(), size))
    return {};
  return size;
}

bool writePng(const QImage &image, QIODevice &device) {
  if (image.isNull() || !device.isWritable())
    return false;

  const bool alpha = image.hasAlphaChannel();
  const QPixelFormat format = image.pixelFormat();
  const qsizetype rowBytes = qsizetype(image.width()) * (alpha ? 4 : 3);
  const qsizetype stride = rowBytes + 1;
  const auto textKeys = image.textKeys();
  const bool captureSizeOnly = textKeys.size() == 1 &&
                               textKeys.first() == kLogicalSizeKey &&
                               !pngLogicalSize(image).isEmpty();
  // libdeflate compresses a whole buffer. Keep its extra working memory
  // bounded for long scroll captures, and let Qt preserve uncommon formats
  // and metadata without maintaining a second general-purpose PNG codec.
  constexpr qsizetype maxFilteredBytes = qsizetype(128) * 1024 * 1024;
  if (image.height() > maxFilteredBytes / stride || image.depth() > 32 ||
      format.redSize() > 8 || format.greenSize() > 8 ||
      format.blueSize() > 8 || format.alphaSize() > 8 ||
      image.colorSpace().isValid() || (!textKeys.isEmpty() && !captureSizeOnly) ||
      !image.offset().isNull()) {
    QImageWriter writer(&device, "PNG");
    writer.setCompression(11); // Qt's 0..100 scale maps this to zlib level 1.
    return writer.write(image);
  }

  const QImage pixels = image.convertToFormat(alpha ? QImage::Format_RGBA8888
                                                   : QImage::Format_RGB888);
  if (pixels.isNull())
    return false;
  QByteArray filtered(stride * pixels.height(), Qt::Uninitialized);
  for (int y = 0; y < pixels.height(); ++y) {
    char *row = filtered.data() + qsizetype(y) * stride;
    // No row prediction: screenshots compress well at level 1, and this
    // also avoids an expensive inverse filter in downstream PNG readers.
    *row = 0;
    std::memcpy(row + 1, pixels.constScanLine(y), rowBytes);
  }
  const std::unique_ptr<libdeflate_compressor, decltype(&libdeflate_free_compressor)>
      compressor(libdeflate_alloc_compressor(1), libdeflate_free_compressor);
  if (!compressor)
    return false;
  const auto capacity = static_cast<qsizetype>(
      libdeflate_zlib_compress_bound(compressor.get(), filtered.size()));
  QByteArray compressed(capacity, Qt::Uninitialized);
  const size_t bytes = libdeflate_zlib_compress(
      compressor.get(), filtered.constData(), filtered.size(),
      compressed.data(), compressed.size());
  if (bytes == 0)
    return false;
  compressed.truncate(static_cast<qsizetype>(bytes));

  QByteArray header(13, '\0');
  qToBigEndian(static_cast<quint32>(pixels.width()), header.data());
  qToBigEndian(static_cast<quint32>(pixels.height()), header.data() + 4);
  header[8] = 8; // bits per channel
  header[9] = alpha ? 6 : 2; // RGBA or RGB; standard PNG, non-interlaced
  if (device.write("\x89PNG\r\n\x1a\n", 8) != 8 ||
      !writeChunk(device, "IHDR", header))
    return false;
  if (image.dotsPerMeterX() > 0 && image.dotsPerMeterY() > 0) {
    QByteArray resolution(9, '\0');
    qToBigEndian(static_cast<quint32>(image.dotsPerMeterX()), resolution.data());
    qToBigEndian(static_cast<quint32>(image.dotsPerMeterY()), resolution.data() + 4);
    resolution[8] = 1; // metres
    if (!writeChunk(device, "pHYs", resolution))
      return false;
  }
  // Our bounded ASCII size tag must not send every capture through Qt's
  // slower encoder. Other text/profile metadata still uses Qt above.
  if (captureSizeOnly &&
      !writeChunk(device, "tEXt", QByteArray(kLogicalSizeKey.data(), kLogicalSizeKey.size()) + '\0' +
                                     image.text(kLogicalSizeKey).toLatin1()))
    return false;
  return writeChunk(device, "IDAT", compressed) && writeChunk(device, "IEND", {});
}
