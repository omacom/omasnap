/** @fileoverview Tests [pin] dismiss_after_seconds loading: missing file,
 *  valid value, and invalid/negative values falling back to 0 (never). */
#include "pin-dismiss-smoke.hpp"

#include "output-config.hpp"
#include "pin-layout.hpp"

#include <QFile>
#include <QTemporaryDir>

namespace {
bool writeFile(const QString &path, const QByteArray &contents) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) &&
         file.write(contents) == contents.size();
}
} // namespace

bool runPinDismissConfigSmoke(QString &error) {
  QTemporaryDir dir;
  if (!dir.isValid()) {
    error = QStringLiteral("could not create temporary directory");
    return false;
  }

  // Missing file -> 0 (pins stay until closed).
  if (loadPinDismissAfterSeconds(
          dir.filePath(QStringLiteral("absent.conf"))) != 0) {
    error = QStringLiteral("missing file did not default to 0");
    return false;
  }

  // Valid value is honored.
  const QString full = dir.filePath(QStringLiteral("full.conf"));
  if (!writeFile(full, "[pin]\ndismiss_after_seconds=300\n")) {
    error = QStringLiteral("could not write full config");
    return false;
  }
  if (loadPinDismissAfterSeconds(full) != 300) {
    error = QStringLiteral("valid dismiss_after_seconds not applied");
    return false;
  }

  // Non-numeric value falls back to 0.
  const QString invalid = dir.filePath(QStringLiteral("invalid.conf"));
  if (!writeFile(invalid, "[pin]\ndismiss_after_seconds=soon\n")) {
    error = QStringLiteral("could not write invalid config");
    return false;
  }
  if (loadPinDismissAfterSeconds(invalid) != 0) {
    error = QStringLiteral("invalid dismiss_after_seconds not rejected");
    return false;
  }

  // Negative value falls back to 0.
  const QString negative = dir.filePath(QStringLiteral("negative.conf"));
  if (!writeFile(negative, "[pin]\ndismiss_after_seconds=-5\n")) {
    error = QStringLiteral("could not write negative config");
    return false;
  }
  if (loadPinDismissAfterSeconds(negative) != 0) {
    error = QStringLiteral("negative dismiss_after_seconds not rejected");
    return false;
  }

  // Dismiss-bar fraction: remaining/total clamped to 0..1.
  if (!qFuzzyCompare(pinDismissFraction(300, 600), 0.5) ||
      !qFuzzyCompare(pinDismissFraction(600, 600) + 1, 2.0) ||
      pinDismissFraction(0, 600) != 0 ||
      pinDismissFraction(900, 600) != 1 ||
      pinDismissFraction(-100, 600) != 0 || pinDismissFraction(100, 0) != 0 ||
      pinDismissFraction(100, -5) != 0) {
    error = QStringLiteral("dismiss fraction math wrong");
    return false;
  }

  return true;
}
