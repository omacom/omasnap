#pragma once

/** @fileoverview Keeps omasnap's own timed preview cards out of the next
 *  screen grab.
 *
 *  A capture taken while the previous capture's preview card is still up
 *  (its 10 seconds) would otherwise contain that card. A timed card announces
 *  itself with a marker in the private runtime folder. Before grabbing, the
 *  capturing process writes a request; each card paints itself fully
 *  transparent, makes sure the compositor has that frame, and acknowledges.
 *  The grab runs, the request is removed, and the cards come back. Kept pins
 *  stay in the picture: the user pinned them on purpose. The pins already
 *  watch the runtime folder, so this needs no new IPC channel. */

#include <QString>
#include <QVector>

/// `timed-<pid>`: this pin process shows a timed (not kept) preview card.
[[nodiscard]] QString timedPreviewMarkerPath(const QString &runtime, qint64 pid);
/// `grab-request`: holds the pid of the process about to grab the screen.
[[nodiscard]] QString grabRequestPath(const QString &runtime);
/// `grab-ack-<pid>`: the card of pin process `pid` is hidden for that grab.
[[nodiscard]] QString grabAckPath(const QString &runtime, qint64 pid);

/// The requesting pid when a live process asks cards to hide, else 0.
[[nodiscard]] qint64 activeGrabRequest(const QString &runtime);

/// Capture side, scoped around the grab. Waits at most `timeoutMs` for the
/// cards to hide (with none on screen, no wait at all); the destructor brings
/// them back.
class TimedPreviewsHidden final {
public:
  explicit TimedPreviewsHidden(QString runtime, int timeoutMs = 150);
  ~TimedPreviewsHidden();
  TimedPreviewsHidden(const TimedPreviewsHidden &) = delete;
  TimedPreviewsHidden &operator=(const TimedPreviewsHidden &) = delete;

private:
  QString runtime_;
  QVector<qint64> pids_;
  bool requested_ = false;
};
