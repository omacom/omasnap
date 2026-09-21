#include "overlay-dismissal.hpp"
#include "capture.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QKeyEvent>
#include <QObject>
#include <QSocketNotifier>
#include <QString>
#include <QWidget>
#include <Qt>
#include <QtLogging>
#include <QtTypes>

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
QString socketPath() {
  const QString runtime = secureRuntimeDirectory();
  return runtime.isEmpty()
             ? QString()
             : QDir(runtime).filePath(QStringLiteral("overlay-close.sock"));
}

bool socketAddress(const QString &path, sockaddr_un &address) {
  const QByteArray encoded = QFile::encodeName(path);
  if (encoded.isEmpty() ||
      encoded.size() >= static_cast<qsizetype>(sizeof(address.sun_path)))
    return false;
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, encoded.constData(), encoded.size() + 1);
  return true;
}
} // namespace

OverlayDismissal::OverlayDismissal(QWidget &overlay)
    : QObject(&overlay), overlay_(overlay), path_(socketPath()) {
  overlay_.installEventFilter(this);
  if (overlay_.isVisible())
    listen();
}

OverlayDismissal::~OverlayDismissal() { stop(); }

bool OverlayDismissal::eventFilter(QObject *watched, QEvent *event) {
  if (watched == &overlay_) {
    if (event->type() == QEvent::Show)
      listen();
    else if (event->type() == QEvent::Hide)
      stop();
  }
  return QObject::eventFilter(watched, event);
}

void OverlayDismissal::listen() {
  if (fd_ >= 0)
    return;
  sockaddr_un address{};
  if (!socketAddress(path_, address))
    return;
  fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd_ < 0) {
    qWarning("Could not open the overlay close socket: %s", std::strerror(errno));
    return;
  }
  // The instance lock guarantees the preceding overlay has exited. Remove
  // its socket if it crashed before cleanup; the private directory owns it.
  ::unlink(address.sun_path);
  if (::bind(fd_, reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) != 0 ||
      ::listen(fd_, 8) != 0) {
    qWarning("Could not listen for overlay close requests: %s", std::strerror(errno));
    stop();
    return;
  }
  notifier_ = new QSocketNotifier(fd_, QSocketNotifier::Read, this);
  connect(notifier_, &QSocketNotifier::activated, this, [this] {
    // Coalesce requests already queued, so one key cannot cancel a selection
    // drag and then dismiss the overlay as a second Escape would.
    bool requested = false;
    for (;;) {
      const int client =
          ::accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (client < 0)
        break;
      ::close(client);
      requested = true;
    }
    if (requested && overlay_.isVisible()) {
      QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
      QCoreApplication::sendEvent(&overlay_, &escape);
    }
  });
}

void OverlayDismissal::stop() {
  if (notifier_) {
    notifier_->setEnabled(false);
    notifier_->deleteLater();
    notifier_ = nullptr;
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
    QFile::remove(path_);
  }
}

bool dismissActiveOverlay() {
  sockaddr_un address{};
  if (!socketAddress(socketPath(), address))
    return false;
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return false;
  const int result = ::connect(fd, reinterpret_cast<const sockaddr *>(&address),
                                sizeof(address));
  // A full accept queue still belongs to a live overlay. Keep the pin while
  // that overlay handles the requests already queued, including a slow save.
  const bool requested = result == 0 || errno == EAGAIN;
  ::close(fd);
  return requested;
}
