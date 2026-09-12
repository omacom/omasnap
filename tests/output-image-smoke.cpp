/** @fileoverview Logical export sizing and real editor Copy/Save regression. */
#include "output-image-smoke.hpp"

#include "capture.hpp"
#include "editor.hpp"
#include "output-config.hpp"
#include "output-image.hpp"

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QImage>
#include <QScopeGuard>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QtCore/qtenvironmentvariables.h>
#include <QtCore/qnumeric.h>
#include <QtCore/qtypes.h>
#include <QtCore/qnamespace.h>
#include <QtTest/QTest>
#include <QtTest/qtestkeyboard.h>
#include <QtTest/qtestmouse.h>

#include <array>
#include <cstddef>
#include <limits>
#include <utility>

namespace {
bool writeFile(const QString &path, const QByteArray &contents) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) &&
         file.write(contents) == contents.size();
}
} // namespace

bool runOutputImageSmoke(QApplication &application, QString &error) {
  error = QStringLiteral("Logical output smoke failed");
  const QTemporaryDir directory;
  if (!directory.isValid())
    return false;
  const std::array variables = {"XDG_CONFIG_HOME", "OMASNAP_SCREENSHOT_DIR",
                               "OMASNAP_TEST_OUTPUT_PNG", "PATH"};
  std::array<QByteArray, variables.size()> previous;
  for (std::size_t i = 0; i < variables.size(); ++i)
    previous[i] = qgetenv(variables[i]);
  const auto restore = qScopeGuard([&] {
    for (std::size_t i = 0; i < variables.size(); ++i) {
      if (previous[i].isNull())
        qunsetenv(variables[i]);
      else
        qputenv(variables[i], previous[i]);
    }
  });
  qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
  if (!defaultConfigPath().startsWith(directory.path() + '/'))
    return false;
  QDir().mkpath(directory.filePath(QStringLiteral("omasnap")));
  QImage image(601, 303, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  image.setPixelColor(200, 100, Qt::red);
  if (prepareOutputImage(image, 2.0) != image)
    return false;
  for (const QByteArray &value : {QByteArray("false"), QByteArray("invalid")}) {
    if (!writeFile(defaultConfigPath(), "[output]\nlogical_size=" + value + '\n') ||
        loadOutputConfig(defaultConfigPath()).logicalSize ||
        prepareOutputImage(image, 2.0) != image)
      return false;
  }
  if (!writeFile(defaultConfigPath(), "[output]\nlogical_size=true\n") ||
      !loadOutputConfig(defaultConfigPath()).logicalSize)
    return false;
  for (const auto &[scale, size] :
       std::array{std::pair{1.0, QSize(601, 303)},
                  std::pair{1.5, QSize(401, 202)},
                  std::pair{2.0, QSize(301, 152)}}) {
    const QImage output = prepareOutputImage(image, scale);
    if (output.size() != size || output.pixelColor(0, 0).alpha() != 0)
      return false;
  }
  for (const qreal scale : {0.0, -1.0, 0.75,
                      std::numeric_limits<qreal>::infinity(),
                      std::numeric_limits<qreal>::quiet_NaN()}) {
    if (prepareOutputImage(image, scale) != image)
      return false;
  }
  QImage tiny(1, 1, QImage::Format_RGB32);
  tiny.fill(Qt::red);
  if (!prepareOutputImage({}, 2.0).isNull() ||
      prepareOutputImage(tiny, 2.0).size() !=
          QSize(1, 1))
    return false;

  // Exercise both destinations through the editor's actual asynchronous export.
  // Stubs keep the test independent of the user's clipboard and notifications.
  for (const auto &[name, script] : std::array{
           std::pair{"wl-copy", "#!/bin/sh\ncat > \"$OMASNAP_TEST_OUTPUT_PNG\"\n"},
           std::pair{"wl-paste", "#!/bin/sh\ncat \"$OMASNAP_TEST_OUTPUT_PNG\"\n"},
           std::pair{"omarchy-notification-send", "#!/bin/sh\nexit 0\n"}}) {
    const QString path = directory.filePath(QString::fromLatin1(name));
    if (!writeFile(path, script) ||
        !QFile::setPermissions(path, QFileDevice::ReadOwner |
                                       QFileDevice::WriteOwner |
                                       QFileDevice::ExeOwner))
      return false;
  }
  qputenv("PATH", directory.path().toUtf8() + ':' + qgetenv("PATH"));
  const QString clipboard = directory.filePath(QStringLiteral("clipboard.png"));
  qputenv("OMASNAP_TEST_OUTPUT_PNG", clipboard.toUtf8());
  for (const qreal scale : {1.0, 1.5, 2.0}) {
    const QString savedDir = directory.filePath(QString::number(scale));
    qputenv("OMASNAP_SCREENSHOT_DIR", savedDir.toUtf8());
    CaptureData capture;
    capture.previewSize = QSize(400, 300);
    capture.monitor.geometry = QRect(QPoint(), capture.previewSize);
    capture.monitor.scale = scale;
    capture.source = QImage(qRound(400 * scale), qRound(300 * scale),
                            QImage::Format_ARGB32_Premultiplied);
    capture.source.fill(QColor(QStringLiteral("#345678")));
    capture.monitor.pixelSize = capture.source.size();
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::File);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    QTest::keyClick(&editor, Qt::Key_R);
    const QPoint start = editor.annotationPointToWidgetForTest(QPointF(80, 80)).toPoint();
    const QPoint end = editor.annotationPointToWidgetForTest(QPointF(180, 160)).toPoint();
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(&editor, end);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, end);
    if (editor.currentAnnotationsForTest().size() != 1)
      return false;
    const QImage native = editor.renderCurrentOutput();
    const QImage expected = native.scaled(
        qRound(native.width() / scale), qRound(native.height() / scale),
        Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QTest::keyClick(&editor, Qt::Key_Return);
    editor.waitForExport();
    const QStringList files = QDir(savedDir).entryList({QStringLiteral("*.png")},
                                                      QDir::Files);
    if (editor.isVisible() || files.size() != 1 ||
        editor.captureData().source != capture.source) {
      error = QStringLiteral("Logical export failed or changed the working source");
      return false;
    }
    const QImage saved(QDir(savedDir).filePath(files.constFirst()));
    if (saved.convertToFormat(QImage::Format_ARGB32) !=
            expected.convertToFormat(QImage::Format_ARGB32) ||
        saved != QImage(clipboard)) {
      error = QStringLiteral("Logical Copy/Save pixels differ from rendered output");
      return false;
    }
  }
  return true;
}
