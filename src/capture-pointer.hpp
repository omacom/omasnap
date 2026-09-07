#pragma once

#include <QPointF>
#include <QString>
#include <memory>

// Reuse the output-bound Wayland pointer used by scrolling capture.
class CapturePointer {
public:
  explicit CapturePointer(const QString &outputName);
  ~CapturePointer();
  CapturePointer(const CapturePointer &) = delete;
  CapturePointer &operator=(const CapturePointer &) = delete;
  void moveTo(const QPointF &fraction);

private:
  struct State;
  std::shared_ptr<State> state_;
};
