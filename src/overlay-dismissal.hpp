/** @fileoverview Routes a pin's compositor close to the active overlay. */
#pragma once

#include <QObject>
#include <QString>

class QSocketNotifier;
class QWidget;

/** Own only in the overlay process, after acquiring the instance lock. The
 * private socket exists while the overlay is shown, never for a normal editor
 * window. Connecting requests the same action as Escape. */
class OverlayDismissal final : public QObject {
public:
  explicit OverlayDismissal(QWidget &overlay);
  ~OverlayDismissal() override;

  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  void listen();
  void stop();

  QWidget &overlay_;
  QString path_;
  int fd_ = -1;
  QSocketNotifier *notifier_ = nullptr;
};

/** Nonblocking local IPC. False means no overlay accepted the close request. */
[[nodiscard]] bool dismissActiveOverlay();
