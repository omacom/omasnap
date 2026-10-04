#include "clipboard-image.hpp"
#include "capture.hpp"
#include "ext-data-control-v1-client-protocol.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QUrl>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <wayland-client.h>

QString retainClipboardImage(const QByteArray &png, QString &error) {
  if (png.isEmpty()) {
    error = QStringLiteral("Screenshot PNG is empty");
    return {};
  }
  const QString state = QStandardPaths::writableLocation(QStandardPaths::StateLocation);
  const QString directory = QDir(state).filePath(QStringLiteral("clipboard"));
  if (state.isEmpty() || !ensurePrivateDirectory(directory)) {
    error = QStringLiteral("Could not create clipboard image directory");
    return {};
  }
  const QString hash = QString::fromLatin1(
      QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex());
  const QString path = QDir(directory).absoluteFilePath(hash + QStringLiteral(".png"));
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) ||
      !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
      file.write(png) != png.size() || !file.commit()) {
    error = QStringLiteral("Could not retain clipboard PNG: %1").arg(file.errorString());
    return {};
  }
  return path;
}

QMap<QByteArray, QByteArray> clipboardImagePayloads(const QString &path,
                                                  const QByteArray &png) {
  return {{"image/png", png},
          {"text/plain", path.toUtf8()},
          {"text/plain;charset=utf-8", path.toUtf8()},
          {"text/uri-list", QUrl::fromLocalFile(path).toEncoded() + "\r\n"}};
}

namespace {
// A stalled or closed paste target must not block selection replacement. Each
// transfer drains independently; image bytes are implicitly shared, not copied.
class Transfer final : public QObject {
public:
  Transfer(int fd, QByteArray bytes, QObject *parent)
      : QObject(parent), fd_(fd), bytes_(std::move(bytes)),
        writable_(fd, QSocketNotifier::Write, this) {
    connect(&writable_, &QSocketNotifier::activated, this, [this] {
      while (offset_ < bytes_.size()) {
        const ssize_t written = ::write(fd_, bytes_.constData() + offset_,
                                         bytes_.size() - offset_);
        if (written > 0) {
          offset_ += written;
        } else if (written < 0 && errno == EINTR) {
          continue;
        } else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          return;
        } else {
          break;
        }
      }
      writable_.setEnabled(false);
      deleteLater();
    });
  }
  ~Transfer() override { ::close(fd_); }

private:
  int fd_;
  QByteArray bytes_;
  qsizetype offset_ = 0;
  QSocketNotifier writable_;
};

class ClipboardOwner final : public QObject {
public:
  explicit ClipboardOwner(QMap<QByteArray, QByteArray> payloads)
      : payloads_(std::move(payloads)) {}

  ~ClipboardOwner() override {
    if (source_) ext_data_control_source_v1_destroy(source_);
    if (device_) ext_data_control_device_v1_destroy(device_);
    if (manager_) ext_data_control_manager_v1_destroy(manager_);
    if (seat_) wl_seat_destroy(seat_);
    if (registry_) wl_registry_destroy(registry_);
    if (display_) wl_display_disconnect(display_);
  }

  bool claim() {
    display_ = wl_display_connect(nullptr);
    if (!display_) return false;
    registry_ = wl_display_get_registry(display_);
    static const wl_registry_listener registryListener{
        [](void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t) {
          auto &self = *static_cast<ClipboardOwner *>(data);
          if (std::strcmp(interface, ext_data_control_manager_v1_interface.name) == 0) {
            self.manager_ = static_cast<ext_data_control_manager_v1 *>(
                wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, 1));
          } else if (!self.seat_ && std::strcmp(interface, wl_seat_interface.name) == 0) {
            self.seat_ = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
          }
        },
        [](void *, wl_registry *, uint32_t) {}};
    wl_registry_add_listener(registry_, &registryListener, this);
    if (wl_display_roundtrip(display_) < 0 || !manager_ || !seat_) return false;

    device_ = ext_data_control_manager_v1_get_data_device(manager_, seat_);
    static const ext_data_control_device_v1_listener deviceListener{
        [](void *, ext_data_control_device_v1 *, ext_data_control_offer_v1 *offer) {
          // This process only publishes; it never reads other selections.
          ext_data_control_offer_v1_destroy(offer);
        },
        [](void *, ext_data_control_device_v1 *, ext_data_control_offer_v1 *) {},
        [](void *data, ext_data_control_device_v1 *) { static_cast<ClipboardOwner *>(data)->stop(); },
        [](void *, ext_data_control_device_v1 *, ext_data_control_offer_v1 *) {}};
    ext_data_control_device_v1_add_listener(device_, &deviceListener, this);
    source_ = ext_data_control_manager_v1_create_data_source(manager_);
    static const ext_data_control_source_v1_listener sourceListener{
        [](void *data, ext_data_control_source_v1 *, const char *mime, int32_t fd) {
          auto &self = *static_cast<ClipboardOwner *>(data);
          const auto payload = self.payloads_.constFind(QByteArray(mime));
          const int flags = fcntl(fd, F_GETFL);
          if (payload == self.payloads_.cend() || flags < 0 ||
              fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            ::close(fd);
          } else {
            new Transfer(fd, *payload, &self);
          }
        },
        [](void *data, ext_data_control_source_v1 *) { static_cast<ClipboardOwner *>(data)->stop(); }};
    ext_data_control_source_v1_add_listener(source_, &sourceListener, this);
    for (auto it = payloads_.cbegin(); it != payloads_.cend(); ++it)
      ext_data_control_source_v1_offer(source_, it.key().constData());
    active_ = true;
    ext_data_control_device_v1_set_selection(device_, source_);
    if (wl_display_roundtrip(display_) < 0 || !active_) return false;

    auto *readable = new QSocketNotifier(wl_display_get_fd(display_), QSocketNotifier::Read, this);
    auto *writable = new QSocketNotifier(wl_display_get_fd(display_), QSocketNotifier::Write, this);
    writable->setEnabled(false);
    const auto flush = [this, writable] {
      if (wl_display_flush(display_) < 0) {
        if (errno == EAGAIN) writable->setEnabled(true);
        else stop();
      } else {
        writable->setEnabled(false);
      }
    };
    connect(readable, &QSocketNotifier::activated, this, [this, flush] {
      if (wl_display_dispatch(display_) < 0) stop();
      else flush();
    });
    connect(writable, &QSocketNotifier::activated, this, flush);
    flush();
    return active_;
  }

private:
  void stop() {
    active_ = false;
    QCoreApplication::quit();
  }
  QMap<QByteArray, QByteArray> payloads_;
  wl_display *display_ = nullptr;
  wl_registry *registry_ = nullptr;
  wl_seat *seat_ = nullptr;
  ext_data_control_manager_v1 *manager_ = nullptr;
  ext_data_control_device_v1 *device_ = nullptr;
  ext_data_control_source_v1 *source_ = nullptr;
  bool active_ = false;
};
} // namespace

int runClipboardImageOwner(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    qCritical() << "Could not read clipboard PNG:" << file.errorString();
    return 1;
  }
  const QByteArray png = file.readAll();
  if (png.isEmpty()) return 1;
  std::signal(SIGPIPE, SIG_IGN);
  ClipboardOwner owner(clipboardImagePayloads(path, png));
  if (!owner.claim()) {
    qCritical() << "Could not claim the Wayland clipboard (ext-data-control required)";
    return 1;
  }
  // This is a dedicated, single-threaded QCoreApplication, not the GUI process.
  // Only detach after the compositor acknowledges ownership. The parent exits
  // successfully; the child serves transfers until cancelled or disconnected.
  if (::daemon(0, 0) < 0) return 1;
  return QCoreApplication::exec();
}
