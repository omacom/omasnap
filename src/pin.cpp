#include <QThreadPool>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QLockFile>
#include <QSaveFile>
#include <QDateTime>
#include "pin.hpp"
#include "capture.hpp"
#include "output-config.hpp"
#include "pin-file.hpp"
#include "pin-layout.hpp"
#include "icons.hpp"

#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QDrag>
#include <QElapsedTimer>
#include <QFile>
#include <QEnterEvent>
#include <QFontMetrics>
#include <QHash>
#include <QImage>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainterPath>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QSocketNotifier>
#include <QCloseEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QToolTip>

#include <QUrl>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>

#include <fcntl.h>
#include <linux/input.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <memory>
#include <functional>
#include <utility>

namespace {

constexpr qreal kCloseButtonSize = 18;
constexpr qreal kCloseButtonInset = 7;
constexpr qreal kControlGap = 6;
constexpr qreal kDragButtonWidth = kCloseButtonSize * 2 + kControlGap;
constexpr qreal kCornerMargin = 14;
constexpr int kPinGap = 10;
constexpr int kToastMs = 1200;

// Every pin's title starts with this, followed by the process id, so pins
// can recognize each other in the compositor's client list and a dispatcher
// can name exactly one of them. Without the unique half a title pattern
// matches every pin and the compositor acts on whichever it finds first.
const QString kPinTitlePrefix = QStringLiteral("omasnap-pin");

QString pinTitle() {
  return QStringLiteral("%1 %2").arg(kPinTitlePrefix).arg(
      QCoreApplication::applicationPid());
}

// A single worker preserves each process's dispatch order. Cross-process
// placement is protected separately by the runtime transaction below.
QThreadPool &pinPool() {
  static QThreadPool pool;
  pool.setMaxThreadCount(1);
  return pool;
}

QString runForOutput(const QString &program, const QStringList &arguments,
                     bool *ok = nullptr) {
  if (ok)
    *ok = false;
  QProcess process;
  process.start(program, arguments);
  if (!process.waitForFinished(500)) {
    process.kill();
    process.waitForFinished(500);
    return {};
  }
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    return {};
  if (ok)
    *ok = true;
  return QString::fromUtf8(process.readAllStandardOutput());
}

bool hyprDispatch(const QString &expression) {
  bool ok = false;
  const QString output = runForOutput(QStringLiteral("hyprctl"),
                                      {QStringLiteral("dispatch"), expression}, &ok);
  return ok && output.trimmed() == QStringLiteral("ok");
}

QRect compositorScreenRect(const QPoint &point = {}, bool usePoint = false) {
  const QJsonArray monitors = QJsonDocument::fromJson(
      runForOutput(QStringLiteral("hyprctl"),
                   {QStringLiteral("-j"), QStringLiteral("monitors")}).toUtf8()).array();
  QRect focused;
  for (const QJsonValue &value : monitors) {
    const QJsonObject monitor = value.toObject();
    const QRect geometry = pinMonitorGeometry(monitor);
    if (usePoint && geometry.contains(point))
      return geometry;
    if (monitor.value(QStringLiteral("focused")).toBool())
      focused = geometry;
  }
  return focused;
}

struct CompositorPin {
  QString title;
  QString address;
  QRect rect;
  bool floating = false;
  bool pinned = false;
};

QVector<CompositorPin> compositorPinRects() {
  QVector<CompositorPin> pins;
  const QJsonArray clients = QJsonDocument::fromJson(
      runForOutput(QStringLiteral("hyprctl"),
                   {QStringLiteral("-j"), QStringLiteral("clients")}).toUtf8()).array();
  for (const QJsonValue &value : clients) {
    const QJsonObject client = value.toObject();
    const QString title = client.value(QStringLiteral("title")).toString();
    if (client.value(QStringLiteral("class")).toString() != QStringLiteral("omasnap") ||
        !title.startsWith(kPinTitlePrefix + QLatin1Char(' ')) ||
        client.value(QStringLiteral("address")).toString().isEmpty())
      continue;
    const QJsonArray at = client.value(QStringLiteral("at")).toArray();
    const QJsonArray size = client.value(QStringLiteral("size")).toArray();
    if (at.size() == 2 && size.size() == 2)
      pins.push_back({title, client.value(QStringLiteral("address")).toString(), QRect(at.at(0).toInt(), at.at(1).toInt(),
                                   size.at(0).toInt(), size.at(1).toInt()),
                      client.value(QStringLiteral("floating")).toBool(),
                      client.value(QStringLiteral("pinned")).toBool()});
  }
  return pins;
}

// Dispatch completion precedes the compositor's animation. Reserve targets
// under one lock so another pin cannot claim the same corner in that gap.
// Reservations disappear once reached, on drag, or after a bounded timeout;
// they are never a persistent substitute for actual compositor geometry.
class PinPlacement {
public:
  PinPlacement() : root_(secureRuntimeDirectory()),
                   lock_(QDir(root_).filePath(QStringLiteral("pin-placement.lock"))) {
    ready_ = !root_.isEmpty() && lock_.tryLock(1000);
    if (!ready_)
      return;
    QFile file(QDir(root_).filePath(QStringLiteral("pin-targets.json")));
    if (file.open(QIODevice::ReadOnly))
      targets_ = QJsonDocument::fromJson(file.readAll()).object();
    pins = compositorPinRects();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const QString &title : targets_.keys()) {
      const QJsonObject target = targets_.value(title).toObject();
      const QRect rect(target.value(QStringLiteral("x")).toInt(),
                        target.value(QStringLiteral("y")).toInt(),
                        target.value(QStringLiteral("w")).toInt(),
                        target.value(QStringLiteral("h")).toInt());
      auto pin = std::find_if(pins.begin(), pins.end(), [&](const CompositorPin &p) {
        return p.title == title;
      });
      if (now - target.value(QStringLiteral("time")).toInteger() > 5000 ||
          pin == pins.end() || pin->rect == rect) {
        targets_.remove(title);
      } else {
        pin->rect = rect;
      }
    }
  }
  bool ready() const { return ready_; }
  void release(const QString &title) {
    targets_.remove(title);
    save();
  }
  bool move(const QString &title, const QRect &rect) {
    const auto pin = std::find_if(pins.cbegin(), pins.cend(), [&](const CompositorPin &p) {
      return p.title == title;
    });
    if (pin == pins.cend())
      return false;
    targets_.insert(title, QJsonObject{{QStringLiteral("x"), rect.x()},
                                      {QStringLiteral("y"), rect.y()},
                                      {QStringLiteral("w"), rect.width()},
                                      {QStringLiteral("h"), rect.height()},
                                      {QStringLiteral("time"), QDateTime::currentMSecsSinceEpoch()}});
    if (!save())
      return false;
    if (hyprDispatch(pinMoveDispatch(pin->address, rect.x(), rect.y())))
      return true;
    targets_.remove(title);
    save();
    return false;
  }
  QVector<CompositorPin> pins;
private:
  bool save() {
    QSaveFile file(QDir(root_).filePath(QStringLiteral("pin-targets.json")));
    const QByteArray data = QJsonDocument(targets_).toJson(QJsonDocument::Compact);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
  }
  QString root_;
  QLockFile lock_;
  QJsonObject targets_;
  bool ready_ = false;
};

QFuture<bool> movePin(const QString &title, const QRect &target) {
  return QtConcurrent::run(&pinPool(), [title, target] {
    PinPlacement placement;
    return placement.ready() && placement.move(title, target);
  });
}

void compactPinColumn(const QString &excludedTitle, const QRect &screen) {
  static_cast<void>(QtConcurrent::run(&pinPool(), [excludedTitle, screen] {
    PinPlacement placement;
    if (!placement.ready() || screen.isEmpty())
      return;
    QVector<CompositorPin> column;
    QVector<QRect> blockers;
    for (CompositorPin pin : placement.pins) {
      if (pin.title == excludedTitle || !screen.intersects(pin.rect))
        continue;
      pin.rect.translate(-screen.topLeft());
      if (pinInColumn(pin.rect, screen.size(), qRound(kCornerMargin), kPinGap))
        column.push_back(pin);
      else
        blockers.push_back(pin.rect);
    }
    std::sort(column.begin(), column.end(), [screen](const CompositorPin &a, const CompositorPin &b) {
      const auto columnIndex = [screen](const QRect &rect) {
        return qRound(qreal(screen.width() - qRound(kCornerMargin) - rect.right() - 1) /
                        (rect.width() + kPinGap));
      };
      const int aColumn = columnIndex(a.rect), bColumn = columnIndex(b.rect);
      if (aColumn != bColumn)
        return aColumn < bColumn;
      return a.rect.y() > b.rect.y();
    });
    for (const CompositorPin &pin : column) {
      const auto at = pinPackedPosition(blockers, screen.size(), pin.rect.size(),
                                        kPinGap, qRound(kCornerMargin));
      if (!at)
        return;
      const QRect target(*at, pin.rect.size());
      if ((*at - pin.rect.topLeft()).manhattanLength() > 4 &&
          !placement.move(pin.title, target.translated(screen.topLeft())))
        return;
      blockers.push_back(target);
    }
  }));
}

class PinWindow final : public QWidget {
public:
  explicit PinWindow(QImage image, QString path, const QSize &frame)
      : image_(std::move(image)), path_(std::move(path)), snapshotFile_(path_) {
    setWindowTitle(pinTitle());
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_ShowWithoutActivating);
    // Fixed, not merely sized: min equal to max is the hint a compositor
    // honors when floating, and Hyprland floats an unresizable window on
    // its own instead of first stretching it into a tile.
    setFixedSize(frame);
    setAttribute(Qt::WA_AlwaysShowToolTips, true);
    dragWatchTimer_.setInterval(80);
    connect(&dragWatchTimer_, &QTimer::timeout, this,
            [this] { requestDragSnapshot(); });
    using DragPayload = QPair<QByteArray, QImage>;
    auto *payload = new QFutureWatcher<DragPayload>(this);
    connect(payload, &QFutureWatcher<DragPayload>::finished, this, [this, payload] {
      auto result = payload->result();
      dragPng_ = std::move(result.first);
      dragPreview_ = std::move(result.second);
      payload->deleteLater();
    });
    payload->setFuture(QtConcurrent::run([image = image_, path = path_] {
      QFile file(path);
      QByteArray png;
      if (file.open(QIODevice::ReadOnly))
        png = file.readAll();
      return DragPayload{png, image.scaled(256, 256, Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation)};
    }));
  }

  void setPlacementSnapshot(const QRect &screen, const QVector<CompositorPin> &pins) {
    dragScreen_ = screen;
    cachedPins_ = pins;
  }

  // Arms idle auto-dismiss: without interaction the pin closes itself
  // after `seconds`. The first hover, press, or key disarms it for good:
  // a pin the user touched is theirs to close.
  void armIdleDismiss(qint64 seconds) {
    if (seconds <= 0)
      return;
    dismissTotalMs_ = std::min<qint64>(seconds, 86400) * 1000;
    dismissTimer_.setSingleShot(true);
    connect(&dismissTimer_, &QTimer::timeout, this, [this] { close(); });
    dismissTimer_.start(dismissTotalMs_);
    dismissTick_.setInterval(250);
    connect(&dismissTick_, &QTimer::timeout, this, [this] { update(); });
    dismissTick_.start();
    dismissClock_.start();
  }

  void disarmIdleDismiss() {
    dismissTimer_.stop();
    dismissTick_.stop();
    dismissTotalMs_ = 0;
    update();
  }

  void requestDragSnapshot() {
    if (queryPending_)
      return;
    queryPending_ = true;
    using Snapshot = QPair<QRect, QVector<CompositorPin>>;
    auto *watcher = new QFutureWatcher<Snapshot>(this);
    connect(watcher, &QFutureWatcher<Snapshot>::finished, this, [this, watcher] {
      const auto snapshot = watcher->result();
      cachedPins_ = snapshot.second;
      if (!snapshot.first.isEmpty() && snapshot.first != dragScreen_) {
        compactPinColumn(windowTitle(), dragScreen_);
        dragScreen_ = snapshot.first;
        commandedTargets_.clear();
      }
      watcher->deleteLater();
      queryPending_ = false;
      if (closing_)
        return;
      if (finishRequested_) {
        finishRequested_ = false;
        finishDragFromSnapshot();
      } else if (dragWatchTimer_.isActive()) {
        observeDrag();
      }
    });
    const QString title = windowTitle();
    watcher->setFuture(QtConcurrent::run(&pinPool(), [title]() -> Snapshot {
      const auto pins = compositorPinRects();
      for (const CompositorPin &pin : pins) {
        if (pin.title == title)
          return {compositorScreenRect(pin.rect.center(), true), pins};
      }
      return {{}, pins};
    }));
  }

  ~PinWindow() override { closeButtonWatch(); }

  [[nodiscard]] bool hasPinLock() const { return snapshotFile_.isLocked(); }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // The compositor draws the border and the shadow around a floating
    // window, so the picture is the whole window: a mat of our own would be
    // a second frame inside the first. Cover-cropped and anchored to the
    // top rather than the middle, because a capture's top is its title bar,
    // its tabs, its heading; a tall page cropped to its middle is a slab of
    // body text that could be any of them.
    painter.fillRect(rect(), QColor(18, 18, 22));
    if (!image_.isNull()) {
      const qreal scale =
          std::max(static_cast<qreal>(width()) / image_.width(),
                   static_cast<qreal>(height()) / image_.height());
      const QRectF source((image_.width() - width() / scale) / 2.0, 0.0,
                          width() / scale, height() / scale);
      painter.drawImage(QRectF(rect()), image_, source);
    }
    if (!toast_.isEmpty())
      paintToast(painter);
    if (dismissTimer_.isActive())
      paintDismissCountdown(painter);
    if (!hovered_)
      return;

    drawControlButton(painter, dragButtonRect(), QStringLiteral("drag-handle"));
    drawControlButton(painter, editButtonRect(), QStringLiteral("edit"));
    drawControlButton(painter, pathButtonRect(), QStringLiteral("path"));
    drawControlButton(painter, copyButtonRect(), QStringLiteral("copy"));
    drawControlButton(painter, closeButtonRect(), QStringLiteral("close"));
  }

  void drawControlButton(QPainter &painter, const QRectF &rect,
                         const QString &action) const {
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(12, 12, 16, 190));
    painter.drawRoundedRect(rect, 6, 6);
    drawToolbarIcon(painter, rect, action, {}, QColor(245, 245, 247));
  }

  void paintToast(QPainter &painter) const {
    const QFontMetrics metrics(painter.font());
    const QRectF pill((width() - metrics.horizontalAdvance(toast_) - 28) / 2.0,
                      height() - 42, metrics.horizontalAdvance(toast_) + 28,
                      26);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(12, 12, 16, 205));
    painter.drawRoundedRect(pill, 13, 13);
    painter.setPen(QColor(240, 240, 245));
    painter.drawText(pill, Qt::AlignCenter, toast_);
  }

  void paintDismissCountdown(QPainter &painter) const {
    const qint64 remainingMs =
        std::max<qint64>(0, dismissTotalMs_ - dismissClock_.elapsed());
    // Thin remaining-time bar along the bottom edge, shrinking away.
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(12, 12, 16, 160));
    painter.drawRect(0, height() - 3, width(), 3);
    painter.setBrush(QColor(245, 245, 247, 220));
    painter.drawRect(0, height() - 3,
                      qRound(width() * pinDismissFraction(remainingMs, dismissTotalMs_)), 3);
    // Numeric countdown, last 10 seconds only: a bottom-left pill in the
    // same style as the toast.
    if (remainingMs > 10000)
      return;
    const QString text =
        QStringLiteral("%1s").arg((remainingMs + 999) / 1000);
    const QFontMetrics metrics(painter.font());
    const QRectF pill(12, height() - 42,
                      metrics.horizontalAdvance(text) + 28, 26);
    painter.setBrush(QColor(12, 12, 16, 205));
    painter.drawRoundedRect(pill, 13, 13);
    painter.setPen(QColor(240, 240, 245));
    painter.drawText(pill, Qt::AlignCenter, text);
  }

  void mousePressEvent(QMouseEvent *event) override {
    disarmIdleDismiss();
    if (event->button() == Qt::MiddleButton) {
      close();
      return;
    }
    const QPointF position = event->position();
    if (event->button() == Qt::LeftButton) {
      if (closeButtonRect().contains(position)) {
        close();
        return;
      }
      if (dragButtonRect().contains(position)) {
        beginFileDrag();
        return;
      }
      if (copyButtonRect().contains(position)) {
        copyImage();
        return;
      }
      if (pathButtonRect().contains(position)) {
        runAction([path = path_] {
          QString error;
          static_cast<void>(copyTextToClipboard(path, error));
          return error;
        }, QStringLiteral("Copied path"));
        return;
      }
      if (editButtonRect().contains(position)) {
        reopenInEditor();
        return;
      }
      if (QWindow *handle = windowHandle())
        handle->startSystemMove();
      beginDragWatch();
      event->accept();
    }
  }

  // startSystemMove hands the drag to the compositor and this side never
  // hears when it ends, so the end is read off the effect: the window left
  // its starting position and then held one spot for a few polls. A press
  // that never moves the window was a click and times out instead. Either
  // way the column closes the gap behind a pin that was dragged away.
  void beginDragWatch() {
    ++snapGeneration_;
    const QString title = windowTitle();
    static_cast<void>(QtConcurrent::run(&pinPool(), [title] {
      PinPlacement placement;
      if (placement.ready())
        placement.release(title);
    }));
    dragStartRect_ = ownCompositorRect();
    if (dragStartRect_.isNull())
      return;

    dragPreviousRect_ = {};
    commandedTargets_.clear();
    dragMoved_ = false;
    dragPolls_ = 0;
    dragStablePolls_ = 0;
    spreadActive_ = false;
    snapSpot_ = {};
    openButtonWatch();
    dragWatchTimer_.start();
  }

  void observeDrag() {
    const QRect rect = ownCompositorRect();
    ++dragPolls_;
    const bool clickTimeout = !dragMoved_ && dragPolls_ >= 12;
    if (rect.isNull() || clickTimeout || dragPolls_ >= 750) {
      dragWatchTimer_.stop();
      closeButtonWatch();
      if (spreadActive_)
        compactPinColumn(windowTitle(), dragScreen_);
      spreadActive_ = false;
      return;
    }
    dragMoved_ = dragMoved_ || rect != dragStartRect_;
    if (dragMoved_)
      previewInsertion(rect);
    const bool still = rect == dragPreviousRect_;
    dragStablePolls_ = dragMoved_ && still ? dragStablePolls_ + 1 : 0;
    dragPreviousRect_ = rect;
    // The release normally arrives from the input device watch or as a
    // pointer event; this long stillness fallback only catches a session
    // where neither could be established.
    if (dragMoved_ && dragStablePolls_ >= 25)
      finishDrag();
  }

  // The kernel pushes the button release the instant it happens, no matter
  // whether the pointer ever moves again; the compositor tells this window
  // nothing until it does. Best effort: without permission to read the
  // devices, the pointer-event and stillness paths still finish the drag.
  void openButtonWatch() {
    closeButtonWatch();
    QDir devices(QStringLiteral("/dev/input/by-id"));
    const QStringList entries =
        devices.entryList({QStringLiteral("*-event-mouse")},
                          QDir::System | QDir::Files | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
      const int fd =
          ::open(QFile::encodeName(devices.filePath(entry)).constData(),
                 O_RDONLY | O_NONBLOCK | O_CLOEXEC);
      if (fd < 0)
        continue;
      auto *notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
      connect(notifier, &QSocketNotifier::activated, this,
              [this, fd] { readButtonEvents(fd); });
      buttonWatches_.push_back({fd, notifier});
    }
  }

  void readButtonEvents(int fd) {
    struct input_event events[16];
    for (;;) {
      const ssize_t bytes = ::read(fd, events, sizeof events);
      if (bytes <= 0)
        return;
      const int count = static_cast<int>(bytes / sizeof(input_event));
      for (int index = 0; index < count; ++index) {
        if (events[index].type != EV_KEY || events[index].code != BTN_LEFT ||
            events[index].value != 0 || !dragWatchTimer_.isActive())
          continue;
        finishDrag();
        closeButtonWatch();
        return;
      }
    }
  }

  void closeButtonWatch() {
    for (const auto &[fd, notifier] : buttonWatches_) {
      notifier->setEnabled(false);
      notifier->deleteLater();
      ::close(fd);
    }
    buttonWatches_.clear();
  }

  // The compositor's move grab starves this window of pointer events, so
  // the first enter or hover after the grab began is the release itself,
  // and the snap can happen right then instead of waiting for a poll.
  // Holding the pin still mid-drag stays a drag for as long as the button
  // is down.
  void pointerWokeDuringWatch() {
    if (!dragWatchTimer_.isActive())
      return;
    finishDrag();
  }

  void finishDrag() {
    finishRequested_ = true;
    requestDragSnapshot();
  }

  void finishDragFromSnapshot() {
    dragWatchTimer_.stop();
    closeButtonWatch();
    // One last look at the true final position; the last poll can be a
    // frame behind it.
    const QRect rect = ownCompositorRect();
    if (!dragMoved_ && rect == dragStartRect_)
      return;
    if (!rect.isNull())
      previewInsertion(rect);
    if (!snapSpot_.isNull())
      requestSnap(snapSpot_, ++snapGeneration_);
    else
      compactPinColumn(windowTitle(), dragScreen_);
    spreadActive_ = false;
  }

  void requestSnap(const QRect &target, quint64 generation, int attempt = 0) {
    if (closing_ || generation != snapGeneration_)
      return;
    auto *watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this,
            [this, watcher, target, generation, attempt] {
      const bool moved = watcher->result();
      watcher->deleteLater();
      if (closing_ || generation != snapGeneration_ || moved)
        return;
      if (attempt < 2) {
        QTimer::singleShot(50, this, [this, target, generation, attempt] {
          requestSnap(target, generation, attempt + 1);
        });
      } else {
        compactPinColumn(windowTitle(), dragScreen_);
        showToast(QStringLiteral("Could not snap capture into the stack"));
      }
    });
    watcher->setFuture(movePin(windowTitle(), target));
  }

  // While the drag hovers the column, the others step aside around a hole
  // where this pin would land, live; leaving the column packs them back.
  void previewInsertion(const QRect &rect) {
    if (dragScreen_.isEmpty())
      return;
    QVector<QPair<QString, QRect>> column;
    QVector<QRect> blockers;
    for (const CompositorPin &pin : cachedPins_) {
      if (pin.title == windowTitle() || !dragScreen_.intersects(pin.rect))
        continue;
      if (pinInColumn(pin.rect.translated(-dragScreen_.topLeft()), dragScreen_.size(), qRound(kCornerMargin), kPinGap))
        column.push_back({pin.title, pin.rect.translated(-dragScreen_.topLeft())});
      else
        blockers.push_back(pin.rect.translated(-dragScreen_.topLeft()));
    }
    const PinInsertionPlan plan = pinInsertionPlan(
        column, blockers, rect.translated(-dragScreen_.topLeft()), dragScreen_.size(), kPinGap, qRound(kCornerMargin));
    if (plan.index < 0) {
      if (spreadActive_)
        compactPinColumn(windowTitle(), dragScreen_);
      spreadActive_ = false;
      snapSpot_ = {};
      commandedTargets_.clear();
      return;
    }
    // Dispatch each move once, against what was last commanded rather than
    // the live rect: a pin mid-animation is never at its target yet, and
    // re-sending the same move every poll restarts the animation, which
    // reads as flicker. One command per new target lets the slide play out.
    for (const auto &[title, target] : plan.spread) {
      if (commandedTargets_.value(title, QPoint(INT_MIN, INT_MIN)) !=
          target.topLeft()) {
        static_cast<void>(movePin(title, target.translated(dragScreen_.topLeft())));
        commandedTargets_.insert(title, target.topLeft());
      }
    }
    spreadActive_ = true;
    snapSpot_ = plan.spot.translated(dragScreen_.topLeft());
  }

  [[nodiscard]] QRect ownCompositorRect() const {
    for (const CompositorPin &pin : cachedPins_) {
      if (pin.title == windowTitle())
        return pin.rect;
    }
    return {};
  }

  void runAction(std::function<QString()> worker, QString message,
                 bool reopening = false) {
    if (actionPending_)
      return;
    actionPending_ = true;
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this,
            [this, watcher, message, reopening] {
      const QString error = watcher->result();
      watcher->deleteLater();
      actionPending_ = false;
      if (!error.isEmpty())
        showToast(error);
      else if (reopening) {
        snapshotFile_.preserveForEditor();
        close();
      } else {
        showToast(message);
      }
    });
    watcher->setFuture(QtConcurrent::run(std::move(worker)));
  }

  void copyImage() {
    runAction([image = image_] {
      QString error;
      static_cast<void>(copyImageToClipboard(image, error));
      return error;
    }, QStringLiteral("Copied to clipboard"));
  }

  void reopenInEditor() {
    runAction([program = QCoreApplication::applicationFilePath(), path = path_] {
      PinSnapshotFile handoff(path);
      if (!handoff.isLocked())
        return QStringLiteral("Could not retain the pinned capture");
      if (!QProcess::startDetached(program, {path}))
        return QStringLiteral("Could not start omasnap");
      handoff.preserveForEditor();
      return QString();
    }, {}, true);
  }

  void mouseMoveEvent(QMouseEvent *event) override {
    pointerWokeDuringWatch();
    const QPointF position = event->position();
    setCursor(controlRectAt(position) >= 0 ? Qt::PointingHandCursor
                                           : Qt::ArrowCursor);

    const int control = controlRectAt(position);
    if (control != hoveredControl_) {
      hoveredControl_ = control;
      update();
    }
  }

  // The window is an ordinary toplevel now, so the app's own tooltip can
  // appear at the cursor instead of a pill painted in one corner. Anchored
  // to the control's rect so it stays up while the cursor is inside it.
  bool event(QEvent *event) override {
    if (event->type() == QEvent::ToolTip) {
      auto *help = static_cast<QHelpEvent *>(event);
      const int control = controlRectAt(help->pos());
      if (control >= 0) {
        QToolTip::showText(help->globalPos(), pinControlTip(control), this,
                           controlRect(control).toAlignedRect());
      } else {
        QToolTip::hideText();
      }
      return true;
    }
    return QWidget::event(event);
  }


  // The six-dot control starts a file drag. Its optional PNG and thumbnail
  // payloads are prepared on a worker so pointer input never encodes images.
  void beginFileDrag() {
    QMimeData *mime = new QMimeData;
    const QList<QUrl> urls{QUrl::fromLocalFile(path_)};
    mime->setUrls(urls);
    mime->setText(urls.constFirst().toLocalFile());
    if (!dragPng_.isEmpty())
      mime->setData(QStringLiteral("image/png"), dragPng_);
    QDrag drag(this);
    drag.setMimeData(mime);
    if (!dragPreview_.isNull())
      drag.setPixmap(QPixmap::fromImage(dragPreview_));
    drag.exec(Qt::CopyAction | Qt::MoveAction);
  }

  void wheelEvent(QWheelEvent *event) override {
    // Pinned captures deliberately keep a stable display-shaped frame so the
    // controls remain usable and the image area never reflows.
    event->accept();
  }

  void keyPressEvent(QKeyEvent *event) override {
    disarmIdleDismiss();
    if (event->key() == Qt::Key_Escape) {
      close();
      return;
    }
    if (event->matches(QKeySequence::Copy)) {
      copyImage();
      return;
    }
    QWidget::keyPressEvent(event);
  }

  void closeEvent(QCloseEvent *event) override {
    closing_ = true;
    dragWatchTimer_.stop();
    disarmIdleDismiss();
    closeButtonWatch();
    // The compositor may still list this window while it closes, so it is
    // excluded by name rather than trusted to be gone.
    compactPinColumn(windowTitle(), dragScreen_);
    QWidget::closeEvent(event);
  }

  void enterEvent(QEnterEvent *) override {
    disarmIdleDismiss();
    pointerWokeDuringWatch();
    hovered_ = true;
    hoveredControl_ = -1;
    update();
  }

  void leaveEvent(QEvent *) override {
    hovered_ = false;
    hoveredControl_ = -1;
    setCursor(Qt::ArrowCursor);
    update();
  }

private:
  void showToast(QString message) {
    toast_ = std::move(message);
    update();
    QTimer::singleShot(kToastMs, this, [this] {
      toast_.clear();
      update();
    });
  }

  // The wide drag handle stands alone in the top-left; edit, path, copy, and
  // close remain grouped in the top-right.
  [[nodiscard]] QRectF closeButtonRect() const { return controlRect(0); }

  [[nodiscard]] QRectF copyButtonRect() const { return controlRect(1); }

  [[nodiscard]] QRectF pathButtonRect() const { return controlRect(2); }

  [[nodiscard]] QRectF editButtonRect() const { return controlRect(3); }

  [[nodiscard]] QRectF dragButtonRect() const { return controlRect(4); }

  [[nodiscard]] QRectF controlRect(int index) const {
    const qreal right = width() - kCloseButtonSize - kCloseButtonInset;
    if (index < 4) {
      return QRectF(right - index * (kCloseButtonSize + kControlGap),
                    kCloseButtonInset, kCloseButtonSize, kCloseButtonSize);
    }
    return QRectF(kCloseButtonInset, kCloseButtonInset, kDragButtonWidth,
                  kCloseButtonSize);
  }

  [[nodiscard]] int controlRectAt(const QPointF &position) const {
    for (int index = 0; index < 5; ++index) {
      if (controlRect(index).contains(position))
        return index;
    }
    return -1;
  }

  QImage image_;
  QByteArray dragPng_;
  QImage dragPreview_;
  bool actionPending_ = false;
  QString path_;
  PinSnapshotFile snapshotFile_;
  QVector<CompositorPin> cachedPins_;
  bool closing_ = false;
  quint64 snapGeneration_ = 0;
  bool queryPending_ = false;
  bool finishRequested_ = false;
  QTimer dragWatchTimer_;
  QTimer dismissTimer_;
  QTimer dismissTick_;
  QElapsedTimer dismissClock_;
  qint64 dismissTotalMs_ = 0;
  QRect dragStartRect_;
  QRect dragPreviousRect_;
  QRect dragScreen_;
  QHash<QString, QPoint> commandedTargets_;
  QVector<QPair<int, QSocketNotifier *>> buttonWatches_;
  QRect snapSpot_;
  bool spreadActive_ = false;
  bool dragMoved_ = false;
  int dragPolls_ = 0;
  int dragStablePolls_ = 0;
  QString toast_;
  bool hovered_ = false;
  int hoveredControl_ = -1;
};

} // namespace

int runPinnedCapture(const QString &path) {
  QImage image(path);
  if (image.isNull()) {
    qWarning("omasnap: could not load pinned image %s", qUtf8Printable(path));
    return 1;
  }

  PinWindow window(std::move(image), path, pinFrameSize({}));
  if (!window.hasPinLock()) {
    qWarning("omasnap: could not lock pinned image %s", qUtf8Printable(path));
    return 1;
  }
  window.armIdleDismiss(loadPinDismissAfterSeconds(defaultConfigPath()));
  auto *settle = new QTimer(&window);
  settle->setSingleShot(true);
  settle->setInterval(50);
  using PlacementResult = QPair<QRect, QVector<CompositorPin>>;
  auto *watcher = new QFutureWatcher<PlacementResult>(&window);
  QObject::connect(watcher, &QFutureWatcher<PlacementResult>::finished, &window,
                   [&window, watcher, settle, attempts = 0]() mutable {
    const auto result = watcher->result();
    if (!result.first.isEmpty()) {
      window.setPlacementSnapshot(result.first, result.second);
      watcher->deleteLater();
      settle->deleteLater();
    } else if (++attempts < 10) {
      settle->start();
    } else {
      watcher->deleteLater();
      settle->deleteLater();
    }
  });
  auto screen = std::make_shared<QRect>();
  QObject::connect(settle, &QTimer::timeout, &window, [&window, watcher, screen] {
    const QString title = window.windowTitle();
    const QSize frame = window.size();
    const QRect geometry = *screen;
    watcher->setFuture(QtConcurrent::run(&pinPool(), [title, frame, screen = geometry]() -> PlacementResult {
      PinPlacement placement;
      if (!placement.ready() || screen.isEmpty())
        return {};
      auto own = std::find_if(placement.pins.begin(), placement.pins.end(),
                              [&](const CompositorPin &pin) { return pin.title == title; });
      if (own == placement.pins.end())
        return {};
      if (!own->floating && !hyprDispatch(pinFloatDispatch(own->address)))
        return {};
      if (!own->pinned && !hyprDispatch(pinPinDispatch(own->address)))
        return {};
      QVector<QRect> blockers;
      for (const CompositorPin &pin : placement.pins) {
        if (pin.title != title && screen.intersects(pin.rect))
          blockers.push_back(pin.rect.translated(-screen.topLeft()));
      }
      const auto at = pinPackedPosition(blockers, screen.size(), frame,
                                        kPinGap, qRound(kCornerMargin));
      if (at) {
        own->rect = QRect(*at + screen.topLeft(), frame);
        if (!placement.move(title, own->rect))
          return {};
      }
      return {screen, placement.pins};
    }));
  });
  auto *monitor = new QFutureWatcher<QRect>(&window);
  QObject::connect(monitor, &QFutureWatcher<QRect>::finished, &window,
                   [&window, monitor, watcher, settle, screen, attempts = 0]() mutable {
    *screen = monitor->result();
    if (screen->isEmpty() && ++attempts < 10) {
      QTimer::singleShot(50, &window, [monitor] {
        monitor->setFuture(QtConcurrent::run(&pinPool(), [] { return compositorScreenRect(); }));
      });
      return;
    }
    monitor->deleteLater();
    if (screen->isEmpty()) {
      qWarning("omasnap: could not determine the pin monitor after retries");
      settle->deleteLater();
      watcher->deleteLater();
      return;
    }
    window.setFixedSize(pinFrameSize(screen->size()));
    settle->start();
  });
  // Register before mapping: a post-capture preview must not take keyboard
  // focus from the app the user is returning to. Keep click-to-focus so pin
  // shortcuts and compositor dragging still work when deliberately selected.
  const auto applyRules = [] {
    bool ok = false;
    const QString output = runForOutput(
        QStringLiteral("hyprctl"),
        {QStringLiteral("eval"),
         QStringLiteral("hl.window_rule({ name = \"omasnap-pins\", "
                        "match = { class = \"^omasnap$\", title = \"^omasnap-pin [0-9]+$\" }, "
                        "float = true, pin = true, no_initial_focus = true, "
                        "no_follow_mouse = true })")}, &ok);
    return ok && !output.contains(QStringLiteral("error"), Qt::CaseInsensitive);
  };
  auto *rules = new QFutureWatcher<bool>(&window);
  QObject::connect(rules, &QFutureWatcher<bool>::finished, &window,
                   [&window, rules, monitor, applyRules, attempts = 0]() mutable {
    if (!rules->result()) {
      if (++attempts < 3) {
        QTimer::singleShot(50, &window, [rules, applyRules] {
          rules->setFuture(QtConcurrent::run(&pinPool(), applyRules));
        });
      } else {
        qWarning("omasnap: could not configure floating pin window");
        QApplication::exit(1);
      }
      return;
    }
    rules->deleteLater();
    window.show();
    monitor->setFuture(QtConcurrent::run(&pinPool(), [] {
      return compositorScreenRect();
    }));
  });
  rules->setFuture(QtConcurrent::run(&pinPool(), applyRules));
  return QApplication::exec();
}
