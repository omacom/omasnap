/** Rounded selection dashes must stay put when only a small area repaints. */
#include "selection-repaint-smoke.hpp"

#include "capture.hpp"
#include "editor.hpp"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QPainter>
#include <QRectF>
#include <QRegion>
#include <QRgb>
#include <QString>
#include <Qt>
#include <QtTypes>
#include <QtTest/qtestkeyboard.h>

#include <algorithm>
#include <cmath>

bool runSelectionRepaintSmoke(QApplication &application, QString &error) {
  for (const int width : {1, 8, 15, 40, 180}) {
    CaptureData capture;
    capture.previewSize = {900, 600};
    capture.source = QImage(capture.previewSize,
                            QImage::Format_ARGB32_Premultiplied);
    capture.source.fill(QColor(QStringLiteral("#566876")));
    Annotation box;
    box.id = 1;
    box.kind = Annotation::Kind::Rectangle;
    box.start = {350.3, 250.7};
    box.end = box.start + QPointF(width, 120);
    box.size = 24;
    box.cornerRadius = 24;
    box.color = Qt::red;
    Operation annotate;
    annotate.type = Operation::Type::Annotate;
    annotate.annotations = {box};
    OperationLog log;
    log.ops = {annotate};
    log.index = 1;
    log.nextId = 2;

    CaptureEditor editor(capture, CaptureEditor::CaptureMode::File,
                         QuickOutputMode::None, log);
    editor.setSuppressSnapshots(true);
    editor.setWindowedPresentation(false);
    editor.resize(1600, 1000);
    editor.show();
    application.processEvents();
    QTest::keyClick(&editor, Qt::Key_A, Qt::ControlModifier);
    application.processEvents();
    if (editor.selectedCountForTest() != 1) {
      error = QStringLiteral("Selection repaint fixture did not select its box");
      return false;
    }
    const auto history = editor.operationLog();
    const QImage output = editor.renderCurrentOutput();
    const QPointF corner = editor.annotationPointToWidgetForTest(box.start);

    // Render the actual editor into devices at several display scales. Passing
    // a source region to QWidget::render exercises the system paint clip, as
    // a pointer repaint does; a QPainter user clip alone misses this defect.
    for (const qreal ratio : {1.0, 1.5, 2.0}) {
      QImage full((QSizeF(editor.size()) * ratio).toSize(),
                   QImage::Format_ARGB32_Premultiplied);
      full.setDevicePixelRatio(ratio);
      full.fill(Qt::transparent);
      {
        QPainter painter(&full);
        editor.render(&painter);
      }
      for (const int offset : {-5, 0, 8, 20, 30, 45, 95, 110, 125}) {
        const QRegion clip(QRect(static_cast<int>(corner.x()) - 50,
                                 static_cast<int>(corner.y()) + offset, 320, 24));
        QImage partial = full.copy();
        {
          QPainter painter(&partial);
          editor.render(&painter, clip.boundingRect().topLeft(), clip);
        }
        const QRectF logical = clip.boundingRect();
        const QRect pixels =
            QRectF(logical.topLeft() * ratio, logical.size() * ratio)
                .adjusted(-1, -1, 1, 1)
                .toAlignedRect()
                .intersected(full.rect());
        for (int y = pixels.top(); y <= pixels.bottom(); ++y) {
          const auto *expected =
              reinterpret_cast<const QRgb *>(full.constScanLine(y));
          const auto *actual =
              reinterpret_cast<const QRgb *>(partial.constScanLine(y));
          for (int x = pixels.left(); x <= pixels.right(); ++x) {
            const int difference = std::max(
                {std::abs(qRed(expected[x]) - qRed(actual[x])),
                 std::abs(qGreen(expected[x]) - qGreen(actual[x])),
                 std::abs(qBlue(expected[x]) - qBlue(actual[x])),
                 std::abs(qAlpha(expected[x]) - qAlpha(actual[x]))});
            // Allow minor raster rounding, but not dashes changing position.
            if (difference > 12) {
              error = QStringLiteral("Rounded selection changed on partial "
                                     "repaint: width %1, scale %2, strip %3, "
                                     "pixel %4,%5, channel difference %6")
                          .arg(width).arg(ratio).arg(offset)
                          .arg(x).arg(y).arg(difference);
              return false;
            }
          }
        }
      }
    }
    if (editor.operationLog() != history ||
        editor.renderCurrentOutput() != output) {
      error = QStringLiteral("Selection repaint changed the document or export");
      return false;
    }
    editor.close();
  }
  return true;
}
