#include "pin-expiry.hpp"

#include <QAbstractAnimation>
#include <QObject>
#include <Qt>

#include <algorithm>

PinExpiry::PinExpiry(bool kept, QObject *parent) : QObject(parent), kept_(kept) {
  timer_.setSingleShot(true);
  timer_.setTimerType(Qt::PreciseTimer);
  fade_.setDuration(250);
  fade_.setStartValue(1.0);
  fade_.setEndValue(0.0);
  connect(&timer_, &QTimer::timeout, this, [this] {
    remainingMs_ = 0;
    fade_.start();
  });
  connect(&fade_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
    emit opacityChanged(value.toReal());
  });
  connect(&fade_, &QVariantAnimation::finished, this, [this] {
    if (!kept_ && !paused_)
      emit expired();
  });
}

void PinExpiry::start() {
  if (started_)
    return;
  started_ = true;
  updateCountdown();
}

void PinExpiry::setKept(bool kept) {
  if (kept_ == kept)
    return;
  kept_ = kept;
  timer_.stop();
  fade_.stop();
  remainingMs_ = 10000;
  emit opacityChanged(1.0);
  updateCountdown();
}

void PinExpiry::setPaused(bool paused) {
  if (paused_ == paused)
    return;
  paused_ = paused;
  updateCountdown();
}

void PinExpiry::updateCountdown() {
  if (!started_ || kept_ || paused_) {
    if (timer_.isActive())
      remainingMs_ = std::max(0, timer_.remainingTime());
    timer_.stop();
    fade_.stop();
    emit opacityChanged(1.0);
  } else if (!timer_.isActive() && fade_.state() != QAbstractAnimation::Running) {
    timer_.start(remainingMs_);
  }
}
