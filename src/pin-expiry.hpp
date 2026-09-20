#pragma once

#include <QObject>
#include <QTimer>
#include <QVariantAnimation>

/// A preview gets ten seconds of idle time, then a short fade. Keeping it
/// cancels expiry; browsing the stack pauses the remaining time.
class PinExpiry final : public QObject {
  Q_OBJECT
public:
  explicit PinExpiry(bool kept, QObject *parent = nullptr);
  [[nodiscard]] bool kept() const { return kept_; }
  void start();
  void setKept(bool kept);
  void setPaused(bool paused);

signals:
  void opacityChanged(qreal opacity);
  void expired();

private:
  void updateCountdown();
  QTimer timer_;
  QVariantAnimation fade_;
  int remainingMs_ = 10000;
  bool kept_;
  bool started_ = false;
  bool paused_ = false;
};
