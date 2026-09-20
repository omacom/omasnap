#include "capture-delay.hpp"

#include <QChar>
#include <QCoreApplication>
#include <QObject>
#include <QString>
#include <QTimer>
#include <Qt>

#include <algorithm>

bool parseCaptureDelay(const QString &value, int &seconds) {
  if (value.isEmpty() ||
      !std::ranges::all_of(value, [](QChar character) {
        return character >= QLatin1Char('0') && character <= QLatin1Char('9');
      }))
    return false;
  bool ok = false;
  const int parsed = value.toInt(&ok);
  if (!ok || parsed > 3600)
    return false;
  seconds = parsed;
  return true;
}

bool runCaptureDelay(int seconds) {
  if (seconds == 0)
    return true;
  bool completed = false;
  QTimer timer;
  timer.setSingleShot(true);
  // A coarse timer can expire early; the requested interval is a minimum.
  timer.setTimerType(Qt::PreciseTimer);
  QObject::connect(&timer, &QTimer::timeout, &timer, [&completed] {
    completed = true;
    QCoreApplication::quit();
  });
  timer.start(seconds * 1000);
  // No window exists during the wait, so menus retain keyboard/pointer focus
  // and there is no countdown surface to remove from the captured pixels.
  QCoreApplication::exec();
  return completed;
}
