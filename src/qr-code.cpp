#include "qr-code.hpp"

#include <ZXing/BarcodeFormat.h>
#include <ZXing/ImageView.h>
#include <ZXing/ReadBarcode.h>
#include <ZXing/ReaderOptions.h>

#include <QImage>

namespace {

QImage toGrayscale(const QImage &image) {
  if (image.isNull())
    return {};
  if (image.format() == QImage::Format_Grayscale8)
    return image;
  return image.convertToFormat(QImage::Format_Grayscale8);
}

QrDecodeResult decodeInternal(const QImage &image, const QRect &region) {
  QrDecodeResult result;
  if (image.isNull() || image.width() < 1 || image.height() < 1) {
    result.error = QStringLiteral("Image is empty");
    return result;
  }

  QImage gray = toGrayscale(image);
  if (gray.isNull()) {
    result.error = QStringLiteral("Could not prepare image for QR decoding");
    return result;
  }

  QRect clip = region;
  if (clip.isNull() || clip.isEmpty()) {
    clip = QRect(QPoint(0, 0), gray.size());
  } else {
    clip = clip.intersected(QRect(QPoint(0, 0), gray.size()));
    if (clip.isEmpty()) {
      result.error = QStringLiteral("QR region is empty");
      return result;
    }
  }

  // Crop without copying if possible, otherwise copy.
  QImage cropped;
  if (clip == QRect(QPoint(0, 0), gray.size())) {
    cropped = gray;
  } else {
    cropped = gray.copy(clip);
    if (cropped.isNull()) {
      result.error = QStringLiteral("Could not crop image for QR decoding");
      return result;
    }
  }

  // ZXing expects a contiguous buffer. QImage scanlines may have padding
  // beyond width, so we pass rowStride explicitly.
  const int width = cropped.width();
  const int height = cropped.height();
  const int bytesPerLine = static_cast<int>(cropped.bytesPerLine());
  const uint8_t *data = cropped.constBits();

  try {
    ZXing::ImageView view(data, width, height, ZXing::ImageFormat::Lum,
                          bytesPerLine, 1);
    ZXing::ReaderOptions options;
    options.setFormats(ZXing::BarcodeFormat::QRCode);
    options.setTryRotate(true);
    options.setTryInvert(true);
    options.setBinarizer(ZXing::Binarizer::LocalAverage);

    ZXing::Barcodes barcodes = ZXing::ReadBarcodes(view, options);
    for (const auto &barcode : barcodes) {
      if (!barcode.isValid())
        continue;
      const std::string utf8 = barcode.text();
      if (utf8.empty())
        continue;
      QrCodeResult entry;
      entry.text = QString::fromUtf8(utf8.c_str(), static_cast<qsizetype>(utf8.size()));
      if (!entry.text.isEmpty())
        result.codes.push_back(std::move(entry));
    }
  } catch (const std::exception &e) {
    result.error = QString::fromUtf8(e.what());
    result.codes.clear();
    return result;
  } catch (...) {
    result.error = QStringLiteral("QR decoding failed");
    result.codes.clear();
    return result;
  }

  return result;
}

} // namespace

QrDecodeResult decodeQrCodes(const QImage &image) {
  return decodeInternal(image, QRect());
}

QrDecodeResult decodeQrCodes(const QImage &image, const QRect &region) {
  return decodeInternal(image, region);
}
