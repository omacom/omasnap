#pragma once

#include <QImage>
#include <QString>
#include <QVector>

struct QrCodeResult {
  QString text;
  bool operator==(const QrCodeResult &) const = default;
};

struct QrDecodeResult {
  QVector<QrCodeResult> codes;
  QString error;
  bool operator==(const QrDecodeResult &) const = default;
};

[[nodiscard]] QrDecodeResult decodeQrCodes(const QImage &image);
[[nodiscard]] QrDecodeResult decodeQrCodes(const QImage &image, const QRect &region);
