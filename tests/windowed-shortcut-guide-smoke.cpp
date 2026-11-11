#include "windowed-shortcut-guide-smoke.hpp"

#include "capture.hpp"
#include "editor.hpp"
#include "shortcut-guide.hpp"
#include "state-marker.hpp"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QLineF>
#include <QPlainTextEdit>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QWheelEvent>
#include <Qt>
#include <QtNumeric>
#include <QtTypes>
#include <QtCore/qtestsupport_core.h>
#include <QtTest/qtestkeyboard.h>
#include <QtTest/qtestmouse.h>
#include <QtWidgets/qtestsupport_widgets.h>

bool runWindowedShortcutGuideSmoke(const QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 1200, 800};
  capture.monitor.pixelSize = {1200, 800};
  capture.previewSize = {1100, 650};
  capture.source = QImage(capture.previewSize, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#d8dde6")));

  for (const QSize size : {QSize(1200, 800), QSize(800, 600)}) {
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::File);
    editor.setWindowedPresentation(true);
    editor.setSuppressSnapshots(true);
    editor.resize(size);
    editor.show();
    if (!QTest::qWaitForWindowExposed(&editor)) {
      error = QStringLiteral("Windowed shortcut fixture was not exposed");
      return false;
    }
    const auto *guide = editor.findChild<ShortcutGuide *>();
    if (!guide || !guide->expanded() || guide->isVisible()) {
      error = QStringLiteral("Windowed guide did not use the anchored legend");
      return false;
    }
    const QRectF toolbar = editor.toolbarButtonRectForTest(QStringLiteral("tool-select"));
    const QRectF viewport = editor.editViewportRectForTest();
    const QRectF frame = editor.editImageRectForTest();
    const QImage shown = editor.grab().toImage();
    const qreal dpr = shown.devicePixelRatio();
    const QRect legendProbe(qRound((size.width() / 2.0 - 50) * dpr),
                             qRound(14 * dpr), qRound(100 * dpr), qRound(20 * dpr));
    const int operations = editor.operationIndex();

    QTest::keyClick(&editor, Qt::Key_Question, Qt::ShiftModifier);
    application.processEvents();
    const QImage hidden = editor.grab().toImage();
    if (guide->expanded() || guide->isVisible() ||
        editor.toolbarButtonRectForTest(QStringLiteral("tool-select")).top() >= toolbar.top() ||
        editor.editViewportRectForTest().height() <= viewport.height() ||
        editor.editImageRectForTest().height() <= frame.height() ||
        shown.copy(legendProbe) == hidden.copy(legendProbe)) {
      error = QStringLiteral("Question mark did not hide the windowed guide and reclaim its space");
      return false;
    }
    if (!QTest::qWaitFor([] { return stateMarkerSet(QStringLiteral("shortcuts-hidden")); })) {
      error = QStringLiteral("Windowed guide visibility was not persisted");
      return false;
    }
    for (const bool windowed : {false, true}) {
      CaptureEditor next(capture, CaptureEditor::CaptureMode::File);
      next.setWindowedPresentation(windowed);
      next.setSuppressSnapshots(true);
      next.resize(size);
      next.show();
      if (!QTest::qWaitForWindowExposed(&next)) {
        error = QStringLiteral("Restored shortcut fixture was not exposed");
        return false;
      }
      const auto *nextGuide = next.findChild<ShortcutGuide *>();
      if (!nextGuide || nextGuide->expanded() || nextGuide->isVisible() ||
          (windowed && next.toolbarButtonRectForTest(QStringLiteral("tool-select")).top() >= toolbar.top())) {
        error = QStringLiteral("A new editor did not restore the hidden guide");
        return false;
      }
      next.close();
    }
    QTest::keyClick(&editor, Qt::Key_Question, Qt::ShiftModifier);
    application.processEvents();
    const QImage restored = editor.grab().toImage();
    if (!guide->expanded() || guide->isVisible() ||
        editor.toolbarButtonRectForTest(QStringLiteral("tool-select")) != toolbar ||
        editor.editViewportRectForTest() != viewport ||
        editor.editImageRectForTest() != frame ||
        shown.copy(legendProbe) != restored.copy(legendProbe) ||
        !QTest::qWaitFor([] { return !stateMarkerSet(QStringLiteral("shortcuts-hidden")); })) {
      error = QStringLiteral("Question mark did not restore the windowed guide and layout");
      return false;
    }
    if (editor.operationIndex() != operations || editor.renderCurrentOutput() != capture.source) {
      error = QStringLiteral("Toggling the windowed guide changed the document");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_R);
    const QPoint start = editor.annotationPointToWidgetForTest({100, 100}).toPoint();
    const QPoint end = editor.annotationPointToWidgetForTest({250, 200}).toPoint();
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(&editor, end, 10);
    QTest::keyClick(&editor, Qt::Key_Question, Qt::ShiftModifier);
    if (!guide->expanded() || editor.editImageRectForTest() != frame) {
      error = QStringLiteral("Toggling help moved the canvas during drawing");
      return false;
    }
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, end);
    if (editor.annotationCountForTest() != 1 ||
        QLineF(editor.currentAnnotationsForTest().last().start, QPointF(100, 100)).length() > 2 ||
        QLineF(editor.currentAnnotationsForTest().last().end, QPointF(250, 200)).length() > 2) {
      error = QStringLiteral("A question mark during drawing changed the rectangle");
      return false;
    }
    const QPoint center = editor.editViewportRectForTest().center().toPoint();
    for (int step = 0; step < 30; ++step) {
      QWheelEvent zoom(center, editor.mapToGlobal(center), {}, {0, 120},
                       Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
      QApplication::sendEvent(&editor, &zoom);
    }
    QTest::mousePress(&editor, Qt::MiddleButton, Qt::NoModifier, center);
    QTest::mouseMove(&editor, center + QPoint(40, 40), 10);
    const QRectF panned = editor.editImageRectForTest();
    QTest::keyClick(&editor, Qt::Key_Question, Qt::ShiftModifier);
    if (!guide->expanded() || editor.editImageRectForTest() != panned) {
      error = QStringLiteral("Toggling help moved the canvas during panning");
      return false;
    }
    QTest::mouseRelease(&editor, Qt::MiddleButton, Qt::NoModifier, center + QPoint(40, 40));
    const QImage annotated = editor.renderCurrentOutput();
    for (const bool expanded : {false, true}) {
      QTest::keyClick(&editor, Qt::Key_Question, Qt::ShiftModifier);
      const QRectF image = editor.editImageRectForTest();
      const QRectF available = editor.editViewportRectForTest();
      if (guide->expanded() != expanded || editor.editScaleForTest() > 4.0001 ||
          (image.width() > available.width() &&
           (image.left() > available.left() + 1 || image.right() < available.right() - 1)) ||
          (image.height() > available.height() &&
           (image.top() > available.top() + 1 || image.bottom() < available.bottom() - 1)) ||
          editor.renderCurrentOutput() != annotated) {
        error = QStringLiteral("Toggling the zoomed guide left an invalid view or changed the document");
        return false;
      }
    }
    if (!QTest::qWaitFor([] { return !stateMarkerSet(QStringLiteral("shortcuts-hidden")); })) {
      error = QStringLiteral("Zoomed guide fixture did not restore visible help");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_T);
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                      editor.editViewportRectForTest().center().toPoint());
    auto *draft = editor.findChild<QPlainTextEdit *>();
    if (!draft || !draft->isVisible()) {
      error = QStringLiteral("Windowed shortcut fixture could not start a text draft");
      return false;
    }
    QTest::keyClicks(draft, QStringLiteral("?"));
    if (draft->toPlainText() != QStringLiteral("?") || !guide->expanded()) {
      error = QStringLiteral("Windowed guide toggle stole a typed question mark");
      return false;
    }
    editor.close();
  }
  return true;
}
