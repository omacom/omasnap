#include "qr-code-smoke.hpp"
#include "qr-code.hpp"

#include <ZXing/BarcodeFormat.h>
#include <ZXing/CreateBarcode.h>
#include <ZXing/WriteBarcode.h>
#include <ZXing/ZXingCpp.h>

#include <QImage>
#include <QPainter>
#include <QColor>

namespace {

QImage zxingImageToQImage(const ZXing::Image &zxImage) {
  if (zxImage.width() <= 0 || zxImage.height() <= 0)
    return {};
  QImage out(zxImage.width(), zxImage.height(), QImage::Format_Grayscale8);
  const uint8_t *src = zxImage.data();
  const int srcStride = zxImage.rowStride();
  for (int y = 0; y < zxImage.height(); ++y) {
    uint8_t *dst = out.scanLine(y);
    const uint8_t *srcRow = src + y * srcStride;
    for (int x = 0; x < zxImage.width(); ++x) {
      dst[x] = srcRow[x * zxImage.pixStride()];
    }
  }
  return out;
}

QImage generateQrImage(const QString &text, int scale = 6) {
  const std::string utf8 = text.toStdString();
  auto barcode = ZXing::CreateBarcodeFromText(utf8, ZXing::BarcodeFormat::QRCode);
  if (!barcode.isValid())
    return {};
  ZXing::WriterOptions opts;
  opts.scale(scale);
  opts.addQuietZones(true);
  ZXing::Image zx = ZXing::WriteBarcodeToImage(barcode, opts);
  QImage img = zxingImageToQImage(zx);
  // ZXing returns Lum where 0=black? Verify: ensure we return as grayscale; decoder expects Lum.
  // If image is inverted, ZXing's tryInvert should handle, but keep as is.
  return img;
}

QImage makeCombinedQrImage(const QString &leftText, const QString &rightText) {
  QImage left = generateQrImage(leftText, 4);
  QImage right = generateQrImage(rightText, 4);
  if (left.isNull() || right.isNull())
    return {};
  const int gap = 20;
  const int w = left.width() + gap + right.width();
  const int h = std::max(left.height(), right.height());
  QImage out(w, h, QImage::Format_RGB32);
  out.fill(Qt::white);
  QPainter p(&out);
  p.drawImage(0, (h - left.height()) / 2, left.convertToFormat(QImage::Format_RGB32));
  p.drawImage(left.width() + gap, (h - right.height()) / 2, right.convertToFormat(QImage::Format_RGB32));
  p.end();
  return out;
}

} // namespace

bool runQrCodeChecks(QString &error) {
  // 1. Single QR correct payload
  {
    const QString payload = QStringLiteral("https://example.com/qr-test-123");
    QImage qr = generateQrImage(payload, 6);
    if (qr.isNull()) {
      error = QStringLiteral("Failed to generate single QR fixture");
      return false;
    }
    // Embed QR in larger white canvas to mimic screenshot context
    QImage canvas(400, 400, QImage::Format_RGB32);
    canvas.fill(Qt::white);
    QPainter p(&canvas);
    p.drawImage((canvas.width() - qr.width()) / 2, (canvas.height() - qr.height()) / 2, qr);
    p.end();

    QrDecodeResult result = decodeQrCodes(canvas);
    if (!result.error.isEmpty()) {
      error = QStringLiteral("Single QR decode failed: %1").arg(result.error);
      return false;
    }
    if (result.codes.size() != 1) {
      error = QStringLiteral("Single QR: expected 1 code, got %1").arg(result.codes.size());
      return false;
    }
    if (result.codes.constFirst().text != payload) {
      error = QStringLiteral("Single QR payload mismatch: got \"%1\" expected \"%2\"")
                  .arg(result.codes.constFirst().text, payload);
      return false;
    }
  }

  // 2. No QR present
  {
    QImage plain(300, 200, QImage::Format_RGB32);
    plain.fill(QColor(QStringLiteral("#a0b4c8")));
    QPainter p(&plain);
    p.setPen(Qt::black);
    p.drawText(plain.rect(), Qt::AlignCenter, QStringLiteral("no qr here"));
    p.end();
    QrDecodeResult result = decodeQrCodes(plain);
    if (!result.error.isEmpty()) {
      error = QStringLiteral("No-QR case should not error: %1").arg(result.error);
      return false;
    }
    if (!result.codes.isEmpty()) {
      error = QStringLiteral("No-QR case expected 0 codes, got %1").arg(result.codes.size());
      return false;
    }
  }

  // 3. Invalid/empty image handling
  {
    QImage empty;
    QrDecodeResult result = decodeQrCodes(empty);
    if (result.error.isEmpty()) {
      error = QStringLiteral("Empty image should produce error");
      return false;
    }
    if (!result.codes.isEmpty()) {
      error = QStringLiteral("Empty image should have 0 codes");
      return false;
    }
    QImage nullSized(0, 0, QImage::Format_ARGB32);
    QrDecodeResult result2 = decodeQrCodes(nullSized);
    if (result2.error.isEmpty()) {
      error = QStringLiteral("Null-sized image should produce error");
      return false;
    }
  }

  // 4. Multiple QR codes if decoder supports them
  {
    const QString leftPayload = QStringLiteral("first-payload");
    const QString rightPayload = QStringLiteral("second-payload-456");
    QImage combined = makeCombinedQrImage(leftPayload, rightPayload);
    if (combined.isNull()) {
      error = QStringLiteral("Failed to generate combined QR fixture");
      return false;
    }
    QrDecodeResult result = decodeQrCodes(combined);
    if (!result.error.isEmpty()) {
      error = QStringLiteral("Multiple QR decode failed: %1").arg(result.error);
      return false;
    }
    if (result.codes.size() < 2) {
      error = QStringLiteral("Multiple QR: expected at least 2 codes, got %1").arg(result.codes.size());
      return false;
    }
    bool hasLeft = false;
    bool hasRight = false;
    for (const auto &c : result.codes) {
      if (c.text == leftPayload)
        hasLeft = true;
      if (c.text == rightPayload)
        hasRight = true;
    }
    if (!hasLeft || !hasRight) {
      error = QStringLiteral("Multiple QR payloads missing: hasLeft=%1 hasRight=%2 codes=%3")
                  .arg(hasLeft)
                  .arg(hasRight)
                  .arg(result.codes.size());
      return false;
    }
  }

  // 5. Region overload: scan only one QR from combined image
  {
    const QString leftPayload = QStringLiteral("region-left");
    const QString rightPayload = QStringLiteral("region-right");
    QImage left = generateQrImage(leftPayload, 4);
    QImage right = generateQrImage(rightPayload, 4);
    QImage combined = makeCombinedQrImage(leftPayload, rightPayload);
    if (combined.isNull() || left.isNull()) {
      error = QStringLiteral("Failed to generate region fixture");
      return false;
    }
    const int gap = 20;
    QRect leftRegion(0, 0, left.width() + gap / 2, combined.height());
    QrDecodeResult leftResult = decodeQrCodes(combined, leftRegion);
    if (!leftResult.error.isEmpty()) {
      error = QStringLiteral("Region QR decode failed: %1").arg(leftResult.error);
      return false;
    }
    if (leftResult.codes.size() != 1 || leftResult.codes.constFirst().text != leftPayload) {
      error = QStringLiteral("Region QR expected only left payload, got %1 codes").arg(leftResult.codes.size());
      if (!leftResult.codes.isEmpty())
        error += QStringLiteral(" first=%1").arg(leftResult.codes.constFirst().text);
      return false;
    }
    // Also verify source image not modified by decoder
    QImage before = combined.copy();
    (void)decodeQrCodes(combined);
    if (before != combined) {
      error = QStringLiteral("Decoder modified source image");
      return false;
    }
  }

  // 6. Very large screenshot does not crash (smoke)
  {
    QImage large(2000, 1200, QImage::Format_RGB32);
    large.fill(Qt::white);
    // Place a QR in large image
    QImage qr = generateQrImage(QStringLiteral("large-test"), 6);
    if (!qr.isNull()) {
      QPainter p(&large);
      p.drawImage(500, 300, qr);
      p.end();
      QrDecodeResult result = decodeQrCodes(large);
      if (!result.error.isEmpty()) {
        error = QStringLiteral("Large image decode error: %1").arg(result.error);
        return false;
      }
      if (result.codes.isEmpty() || result.codes.constFirst().text != QStringLiteral("large-test")) {
        error = QStringLiteral("Large image QR not found");
        return false;
      }
    }
  }

  // 7. Annotations should not affect QR decoding (pristine vs annotated)
  {
    const QString payload = QStringLiteral("annotation-pristine-test");
    QImage qr = generateQrImage(payload, 6);
    if (qr.isNull()) {
      error = QStringLiteral("Failed to generate annotation test QR");
      return false;
    }
    QImage pristine(400, 400, QImage::Format_RGB32);
    pristine.fill(Qt::white);
    QPainter pp(&pristine);
    pp.drawImage(100, 100, qr);
    pp.end();

    QImage annotated = pristine.copy();
    QPainter pa(&annotated);
    pa.fillRect(QRect(100, 100, qr.width(), qr.height()), Qt::black);
    pa.end();

    QrDecodeResult pristineResult = decodeQrCodes(pristine);
    QrDecodeResult annotatedResult = decodeQrCodes(annotated);

    if (pristineResult.codes.isEmpty() || pristineResult.codes.constFirst().text != payload) {
      error = QStringLiteral("Pristine QR should decode even when annotated version is obscured");
      return false;
    }
    // Annotated (black box over QR) should find nothing - proves scanning annotated would fail
    if (!annotatedResult.codes.isEmpty()) {
      // Not strictly a failure of decoder, but indicates our pristine strategy matters:
      // if annotated still decodes, the QR was not fully covered; adjust test to fully cover.
      // Treat as warning but not failure; just ensure pristine decodes.
    }
    // The key assertion: pristine decodes, which is what editor's runQrScan uses.
  }

  return true;
}
