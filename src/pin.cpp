#include <QThreadPool>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QLockFile>
#include <QSaveFile>
#include <QDateTime>
#include "pin.hpp"
#include "capture.hpp"
#include "pin-file.hpp"
#include "pin-expiry.hpp"
#include "pin-layout.hpp"
#include "icons.hpp"
#include "overlay-chrome.hpp"

#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QDrag>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QEnterEvent>
#include <QElapsedTimer>
#include <QFontMetrics>
#include <QGuiApplication>
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
#include <QShowEvent>
#include <QCloseEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QToolTip>
#include <QVariantAnimation>
#include <QRegion>
#include <QtNumeric>

#include <QUrl>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>
#include <Qt>

#include <fcntl.h>
#include <linux/input.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <memory>
#include <optional>
#include <functional>
#include <utility>

namespace {

constexpr qreal kCornerMargin = 14;
constexpr int kPinGap = 10;
constexpr int kToastMs = 1200;
constexpr int kStackWatchMs = 180;
constexpr int kStackCloseMs = 360;

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

struct CompositorMonitor {
  QRect geometry;
  QRect workArea;
};

QJsonArray compositorMonitors() {
  return QJsonDocument::fromJson(
      runForOutput(QStringLiteral("hyprctl"),
                   {QStringLiteral("-j"), QStringLiteral("monitors")}).toUtf8()).array();
}

CompositorMonitor compositorMonitor(const QJsonArray &monitors,
                                    const QPoint &point = {}, bool usePoint = false) {
  CompositorMonitor focused;
  for (const QJsonValue &value : monitors) {
    const QJsonObject monitor = value.toObject();
    const CompositorMonitor screen{pinMonitorGeometry(monitor), pinMonitorWorkArea(monitor)};
    // The bar belongs to the monitor too; only placement uses the work area.
    if (usePoint && screen.geometry.contains(point))
      return screen;
    if (monitor.value(QStringLiteral("focused")).toBool())
      focused = screen;
  }
  return focused;
}

struct CompositorPin {
  QString title;
  QString address;
  QRect rect;
  bool floating = false;
  bool pinned = false;
  bool free = false;
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
      state_ = QJsonDocument::fromJson(file.readAll()).object();
    targets_ = state_.value(QStringLiteral("targets")).toObject();
    pins = compositorPinRects();
    const QJsonObject free = state_.value(QStringLiteral("free")).toObject();
    QJsonObject liveFree;
    for (CompositorPin &pin : pins) {
      pin.free = free.value(pin.title).toBool();
      if (pin.free)
        liveFree.insert(pin.title, true);
    }
    state_.insert(QStringLiteral("free"), liveFree);
    QJsonObject tilts = state_.value(QStringLiteral("tilts")).toObject();
    for (const QString &title : tilts.keys()) {
      if (std::none_of(pins.cbegin(), pins.cend(), [&](const CompositorPin &pin) {
            return pin.title == title;
          }))
        tilts.remove(title);
    }
    state_.insert(QStringLiteral("tilts"), tilts);
    for (const QString &key : {QStringLiteral("hover"), QStringLiteral("drag")}) {
      const QString title = state_.value(key).toString();
      if (std::none_of(pins.cbegin(), pins.cend(), [&](const CompositorPin &pin) {
            return pin.title == title;
          }))
        state_.remove(key);
    }
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
  QString hoverOwner() const { return state_.value(QStringLiteral("hover")).toString(); }
  QString dragging() const { return state_.value(QStringLiteral("drag")).toString(); }
  QRect hoverScreen() const {
    const QJsonArray rect = state_.value(QStringLiteral("screen")).toArray();
    return rect.size() == 4 ? QRect(rect.at(0).toInt(), rect.at(1).toInt(),
                                    rect.at(2).toInt(), rect.at(3).toInt()) : QRect();
  }
  bool setHover(const QString &title, const QRect &screen = {}) {
    state_.insert(QStringLiteral("hover"), title);
    state_.insert(QStringLiteral("screen"),
                  QJsonArray{screen.x(), screen.y(), screen.width(), screen.height()});
    return save();
  }
  void beginDrag(const QString &title) {
    state_.insert(QStringLiteral("drag"), title);
    setTilt(title, 0.0);
    release(title);
  }
  void endDrag(const QString &title) {
    if (dragging() == title) {
      state_.remove(QStringLiteral("drag"));
      save();
    }
  }
  void setFree(const QString &title, bool free) {
    setTilt(title, 0.0);
    auto entries = state_.value(QStringLiteral("free")).toObject();
    if (free)
      entries.insert(title, true);
    else
      entries.remove(title);
    state_.insert(QStringLiteral("free"), entries);
    for (CompositorPin &pin : pins) {
      if (pin.title == title)
        pin.free = free;
    }
    save();
  }
  void setTilt(const QString &title, qreal degrees) {
    auto tilts = state_.value(QStringLiteral("tilts")).toObject();
    tilts.insert(title, degrees);
    state_.insert(QStringLiteral("tilts"), tilts);
  }
  void release(const QString &title) {
    targets_.remove(title);
    save();
  }
  bool move(const QString &title, const QRect &rect) {
    const auto pin = std::find_if(pins.begin(), pins.end(), [&](const CompositorPin &p) {
      return p.title == title;
    });
    if (pin == pins.end())
      return false;
    targets_.insert(title, QJsonObject{{QStringLiteral("x"), rect.x()},
                                      {QStringLiteral("y"), rect.y()},
                                      {QStringLiteral("w"), rect.width()},
                                      {QStringLiteral("h"), rect.height()},
                                      {QStringLiteral("time"), QDateTime::currentMSecsSinceEpoch()}});
    if (!save())
      return false;
    if (hyprDispatch(pinMoveDispatch(pin->address, rect.x(), rect.y()))) {
      pin->rect = rect;
      return true;
    }
    targets_.remove(title);
    save();
    return false;
  }
  QVector<QPair<QString, QRect>> column(const QRect &screen,
                                       const QString &excluded = {}) const {
    QVector<QPair<QString, QRect>> result;
    for (const CompositorPin &pin : pins) {
      const QRect local = pin.rect.translated(-screen.topLeft());
      if (!pin.free && pin.title != excluded && screen.intersects(pin.rect) &&
          pinInColumn(local, screen.size(), qRound(kCornerMargin), kPinGap))
        result.push_back({pin.title, local});
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
      if (a.second.right() != b.second.right())
        return a.second.right() > b.second.right();
      return a.second.bottom() > b.second.bottom();
    });
    return result;
  }
  bool arrange(const QRect &screen, bool expanded, const QString &excluded = {},
               const QString &newFront = {}) {
    if (screen.isEmpty())
      return false;
    auto ordered = column(screen, newFront.isEmpty() ? excluded : newFront);
    QVector<QRect> blockers;
    for (const CompositorPin &pin : pins) {
      if (pin.title == excluded)
        continue;
      if (pin.title == newFront) {
        ordered.prepend({pin.title, pin.rect.translated(-screen.topLeft())});
      } else if (screen.intersects(pin.rect) &&
                 std::none_of(ordered.cbegin(), ordered.cend(), [&](const auto &card) {
                   return card.first == pin.title;
                 })) {
        blockers.push_back(pin.rect.translated(-screen.topLeft()));
      }
    }
    const auto layout = pinStackLayout(ordered, blockers, screen.size(),
                                        kPinGap, qRound(kCornerMargin), expanded);
    if (layout.size() != ordered.size())
      return true; // No complete layout fits: leave every window where it is.
    auto tilts = state_.value(QStringLiteral("tilts")).toObject();
    int depth = 0;
    int columnRight = INT_MIN;
    for (const auto &[title, rect] : layout) {
      if (rect.right() != columnRight) {
        columnRight = rect.right();
        depth = 0;
      }
      tilts.insert(title, pinStackTilt(depth++, expanded));
    }
    if (tilts != state_.value(QStringLiteral("tilts")).toObject()) {
      state_.insert(QStringLiteral("tilts"), tilts);
      if (!save())
        return false;
    }
    bool changed = !newFront.isEmpty();
    for (const auto &[title, rect] : layout) {
      const auto pin = std::find_if(pins.cbegin(), pins.cend(), [&](const CompositorPin &p) {
        return p.title == title;
      });
      const QRect target = rect.translated(screen.topLeft());
      if (pin != pins.cend() && pin->rect != target) {
        if (!move(title, target))
          return false;
        changed = true;
      }
    }
    // Hover focus can raise any exposed card. Restore the deck from back
    // to front without transferring keyboard focus when it folds.
    if (!expanded && changed) {
      for (auto card = layout.crbegin(); card != layout.crend(); ++card) {
        const auto pin = std::find_if(pins.cbegin(), pins.cend(), [&](const CompositorPin &p) {
          return p.title == card->first;
        });
        if (pin != pins.cend() && !hyprDispatch(pinRaiseDispatch(pin->address)))
          return false;
      }
    }
    return true;
  }
  void focusNext(const QString &excluded, const QRect &screen) const {
    const auto ordered = column(screen, excluded);
    const CompositorPin *next = nullptr;
    for (const CompositorPin &pin : pins) {
      if (pin.title == excluded || !screen.contains(pin.rect.center()))
        continue;
      if (!ordered.isEmpty() && pin.title != ordered.front().first)
        continue;
      if (!next || pin.rect.bottom() > next->rect.bottom())
        next = &pin;
    }
    if (next)
      hyprDispatch(pinFocusDispatch(next->address));
  }
  QVector<CompositorPin> pins;
private:
  bool save() {
    QSaveFile file(QDir(root_).filePath(QStringLiteral("pin-targets.json")));
    state_.insert(QStringLiteral("targets"), targets_);
    const QByteArray data = QJsonDocument(state_).toJson(QJsonDocument::Compact);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
  }
  QString root_;
  QLockFile lock_;
  QJsonObject state_;
  QJsonObject targets_;
  bool ready_ = false;
};

QFuture<bool> movePin(const QString &title, const QRect &target) {
  return QtConcurrent::run(&pinPool(), [title, target] {
    PinPlacement placement;
    placement.setTilt(title, 0.0);
    return placement.ready() && placement.move(title, target);
  });
}

void compactPinColumn(const QString &excludedTitle, const QRect &screen) {
  static_cast<void>(QtConcurrent::run(&pinPool(), [excludedTitle, screen] {
    PinPlacement placement;
    if (!placement.ready() || screen.isEmpty())
      return;
    const bool expanded = !placement.dragging().isEmpty() ||
        (!placement.hoverOwner().isEmpty() && placement.hoverScreen() == screen);
    placement.arrange(screen, expanded, excludedTitle);
  }));
}

struct StackSnapshot {
  QRect screen;
  QVector<CompositorPin> pins;
  bool watching = false;
  bool outside = false;
};

// Exactly one hovered pin owns the short-lived fan watch. Ownership moves
// with the pointer; crossing the gaps keeps the fan open. No idle polling.
StackSnapshot watchPinStack(const QString &title, const QRect &screen,
                            bool opening, bool mayClose) {
  PinPlacement placement;
  if (!placement.ready())
    return {screen, {}, true};
  if (!placement.dragging().isEmpty())
    return {screen, placement.pins, placement.hoverOwner() == title};
  if (opening) {
    const auto column = placement.column(screen);
    if (std::none_of(column.cbegin(), column.cend(), [&](const auto &card) {
          return card.first == title;
        })) {
      if (placement.hoverOwner() == title) {
        placement.arrange(placement.hoverScreen(), false);
        placement.setHover({});
      }
      return {};
    }
    if (!placement.hoverOwner().isEmpty() && placement.hoverScreen() != screen)
      placement.arrange(placement.hoverScreen(), false);
    if (!placement.setHover(title, screen) || !placement.arrange(screen, true))
      return {screen, placement.pins, true};
  } else if (placement.hoverOwner() != title) {
    return {};
  }

  const QJsonObject cursor = QJsonDocument::fromJson(
      runForOutput(QStringLiteral("hyprctl"),
                   {QStringLiteral("-j"), QStringLiteral("cursorpos")}).toUtf8()).object();
  if (!cursor.contains(QStringLiteral("x")) || !cursor.contains(QStringLiteral("y")))
    return {screen, placement.pins, true};
  QVector<QRect> cards;
  for (const auto &card : placement.column(screen))
    cards.push_back(card.second.translated(screen.topLeft()));
  const QPoint pointer(cursor.value(QStringLiteral("x")).toInt(),
                        cursor.value(QStringLiteral("y")).toInt());
  const bool outside = !pinStackHotZone(cards, screen).contains(pointer);
  if (outside && mayClose) {
    if (placement.arrange(screen, false)) {
      placement.setHover({});
      return {screen, placement.pins};
    }
  }
  return {screen, placement.pins, true, outside};
}

struct PinStackState {
  qreal tilt = 0.0;
  bool interacting = false;
};

struct PinPreview {
  QImage image;
  QByteArray png;
  QImage drag;
};

class PinWindow final : public QWidget {
public:
  explicit PinWindow(QImage image, QString path, const QSize &frame,
                      PinLifetime lifetime = PinLifetime::Persistent)
      : image_(std::move(image)), expiry_(lifetime == PinLifetime::Persistent),
        path_(std::move(path)), snapshotFile_(path_) {
    setWindowTitle(pinTitle());
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TranslucentBackground);
    // Fixed, not merely sized: min equal to max is the hint a compositor
    // honors when floating, and Hyprland floats an unresizable window on
    // its own instead of first stretching it into a tile.
    setFixedSize(frame);
    setAttribute(Qt::WA_AlwaysShowToolTips, true);
    // Native tooltips need the same explicit font and dark chrome as the
    // painted controls; the generic Qt theme otherwise supplies yellow tips.
    QToolTip::setFont(chromeFont(11));
    setStyleSheet(QStringLiteral(
        "QToolTip {"
        " color: #f5f5f7;"
        " background-color: #121216;"
        " border: 1px solid #3a3a40;"
        " border-radius: 4px;"
        " padding: 0;"
        "}"));
    setMouseTracking(true);
    connect(&expiry_, &PinExpiry::opacityChanged, this, [this](qreal opacity) {
      opacity_ = opacity;
      update();
    });
    connect(&expiry_, &PinExpiry::expired, this, [this] {
      expired_ = true;
      close();
    });
    dragWatchTimer_.setInterval(80);
    connect(&dragWatchTimer_, &QTimer::timeout, this,
            [this] { requestDragSnapshot(); });
    stackWatchTimer_.setSingleShot(true);
    stackWatchTimer_.setInterval(kStackWatchMs);
    connect(&stackWatchTimer_, &QTimer::timeout, this,
            [this] { requestStackWatch(); });
    connect(&stackWatcher_, &QFutureWatcher<StackSnapshot>::finished, this,
            [this] { finishStackWatch(); });
    tiltAnimation_.setDuration(150);
    tiltAnimation_.setEasingCurve(QEasingCurve::OutCubic);
    connect(&tiltAnimation_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) {
      tilt_ = value.toReal();
      updateCardMask();
      update();
    });
    stackStateReloadTimer_.setSingleShot(true);
    stackStateReloadTimer_.setInterval(20);
    connect(&stackStateReloadTimer_, &QTimer::timeout, this, [this] { reloadStackState(); });
    connect(&stackFiles_, &QFileSystemWatcher::directoryChanged, this,
            [this] { stackStateReloadTimer_.start(); });
    connect(&stackStateWatcher_, &QFutureWatcher<PinStackState>::finished, this, [this] {
      stackStateQueryPending_ = false;
      if (!closing_) {
        const auto state = stackStateWatcher_.result();
        setTilt(hovered_ || dragWatchTimer_.isActive() ? 0.0 : state.tilt);
        stackInteracting_ = state.interacting;
        updateExpiryPause();
      }
      if (stackStateReloadPending_)
        reloadStackState();
    });
    auto *runtime = new QFutureWatcher<QString>(this);
    connect(runtime, &QFutureWatcher<QString>::finished, this, [this, runtime] {
      const QString root = runtime->result();
      runtime->deleteLater();
      if (!root.isEmpty()) {
        stackStatePath_ = QDir(root).filePath(QStringLiteral("pin-targets.json"));
        stackFiles_.addPath(root);
        reloadStackState();
      }
    });
    runtime->setFuture(QtConcurrent::run(&pinPool(), [] { return secureRuntimeDirectory(); }));
    connect(&documentFiles_, &QFileSystemWatcher::fileChanged, this, [this](const QString &path) {
      // QSaveFile replaces the inode, so each completed edit needs a new watch.
      documentFiles_.addPath(path);
      reloadPinDocument();
    });
    connect(&documentWatcher_, &QFutureWatcher<PinPreview>::finished, this, [this] {
      documentLoading_ = false;
      const auto preview = documentWatcher_.result();
      if (!closing_ && !preview.image.isNull()) {
        image_ = preview.image;
        dragPng_ = preview.png;
        dragPreview_ = preview.drag;
        path_ = pinDocument_->previewPath();
        sharedPath_.clear();
        update();
      }
      if (std::exchange(documentReloadPending_, false))
        reloadPinDocument();
    });
    using DragPayload = QPair<QByteArray, QImage>;
    auto *payload = new QFutureWatcher<DragPayload>(this);
    connect(payload, &QFutureWatcher<DragPayload>::finished, this, [this, payload, path = path_] {
      auto result = payload->result();
      if (path_ == path) {
        dragPng_ = std::move(result.first);
        dragPreview_ = std::move(result.second);
      }
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
    if (hovered_)
      openStack();
    reloadStackState();
  }

  void openStack() {
    stackOpenRequested_ = true;
    stackOutside_.invalidate();
    requestStackWatch();
  }

  void requestStackWatch() {
    if (closing_ || dragScreen_.isEmpty() || stackQueryPending_)
      return;
    if (dragWatchTimer_.isActive() || finishRequested_ || snapPending_) {
      stackWatchTimer_.start();
      return;
    }
    stackQueryPending_ = true;
    const bool opening = std::exchange(stackOpenRequested_, false);
    const bool mayClose = !opening && stackOutside_.isValid() &&
                          stackOutside_.elapsed() >= kStackCloseMs;
    stackGeneration_ = snapGeneration_;
    stackWatcher_.setFuture(QtConcurrent::run(&pinPool(),
        [title = windowTitle(), screen = dragScreen_, opening, mayClose] {
      return watchPinStack(title, screen, opening, mayClose);
    }));
  }

  void finishStackWatch() {
    const auto snapshot = stackWatcher_.result();
    stackQueryPending_ = false;
    if (closing_)
      return;
    if (stackGeneration_ == snapGeneration_ && !dragWatchTimer_.isActive() &&
        !snapshot.pins.isEmpty())
      cachedPins_ = snapshot.pins;
    if (snapshot.outside) {
      if (!stackOutside_.isValid())
        stackOutside_.start();
    } else {
      stackOutside_.invalidate();
    }
    if (stackOpenRequested_)
      requestStackWatch();
    else if (snapshot.watching)
      stackWatchTimer_.start();
  }

  void endStackDrag() {
    snapPending_ = false;
    updateExpiryPause();
    static_cast<void>(QtConcurrent::run(&pinPool(),
        [title = windowTitle(), screen = dragScreen_, origin = dragOriginScreen_,
         free = dragFree_] {
      PinPlacement placement;
      if (!placement.ready())
        return;
      placement.endDrag(title);
      if (free)
        placement.setFree(title, *free);
      if (placement.hoverOwner().isEmpty()) {
        placement.arrange(screen, false);
        if (origin != screen)
          placement.arrange(origin, false);
      }
    }));
    // Reclaim the hover watch even when the compositor never sent another
    // enter after its move grab. A free pin does not join just by hovering.
    openStack();
  }

  void requestDragSnapshot() {
    if (queryPending_)
      return;
    queryPending_ = true;
    const bool finalSnapshot = finishRequested_;
    finishRequested_ = false;
    struct Snapshot {
      QRect screen;
      QRect origin;
      QVector<CompositorPin> pins;
    };
    auto *watcher = new QFutureWatcher<Snapshot>(this);
    connect(watcher, &QFutureWatcher<Snapshot>::finished, this, [this, watcher, finalSnapshot] {
      const auto snapshot = watcher->result();
      cachedPins_ = snapshot.pins;
      if (!snapshot.origin.isEmpty())
        dragOriginScreen_ = snapshot.origin;
      if (!snapshot.screen.isEmpty() && snapshot.screen != dragScreen_) {
        compactPinColumn(windowTitle(), dragScreen_);
        dragScreen_ = snapshot.screen;
        commandedTargets_.clear();
      }
      watcher->deleteLater();
      queryPending_ = false;
      if (closing_)
        return;
      if (finishRequested_) {
        // A release arrived while this query was already in flight. Read
        // again so the drop uses a position observed after the release.
        requestDragSnapshot();
      } else if (finalSnapshot) {
        finishDragFromSnapshot();
      } else if (dragWatchTimer_.isActive()) {
        observeDrag();
      }
    });
    const QString title = windowTitle();
    // The previous final snapshot can precede a recovery move. Its pin
    // position may still be clipped on another output; the saved work area
    // identifies the actual starting monitor and lets us refresh its bar.
    const QPoint origin = dragOriginScreen_.center();
    watcher->setFuture(QtConcurrent::run(&pinPool(), [title, origin]() -> Snapshot {
      const PinPlacement placement;
      const auto pins = placement.pins;
      for (const CompositorPin &pin : pins) {
        if (pin.title == title) {
          const auto monitors = compositorMonitors();
          return {compositorMonitor(monitors, pin.rect.center(), true).workArea,
                  compositorMonitor(monitors, origin, true).workArea, pins};
        }
      }
      return {{}, {}, pins};
    }));
  }

  ~PinWindow() override { closeButtonWatch(); }

  [[nodiscard]] bool hasPinLock() const { return snapshotFile_.isLocked(); }

protected:
  void showEvent(QShowEvent *event) override {
    QWidget::showEvent(event);
    expiry_.start();
  }

  void resizeEvent(QResizeEvent *event) override {
    QWidget::resizeEvent(event);
    updateCardMask();
  }

  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // Paint the frame with the image so an idle card can lean without an
    // upright compositor border around it. Keep the existing window bounds
    // for packing and dragging; transparent corners take no pointer input.
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setOpacity(opacity_);
    painter.save();
    painter.setTransform(pinCardTransform(size(), tilt_));
    const QPainterPath card = cardPath();
    painter.save();
    painter.setClipPath(card);
    painter.fillRect(rect(), QColor(18, 18, 22));
    if (!image_.isNull()) {
      const qreal scale =
          std::max(static_cast<qreal>(width()) / image_.width(),
                   static_cast<qreal>(height()) / image_.height());
      const QRectF source((image_.width() - width() / scale) / 2.0, 0.0,
                          width() / scale, height() / scale);
      painter.drawImage(QRectF(rect()), image_, source);
    }
    painter.restore();
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(hovered_ ? QColor(140, 179, 209, 210)
                                : QColor(255, 255, 255, 75), 1.5));
    painter.drawPath(card);
    if (expiry_.kept() && !hovered_)
      drawControlButton(painter, pinButtonRect(), QStringLiteral("pin"), true);
    painter.restore();
    if (!toast_.isEmpty())
      paintToast(painter);
    if (!hovered_)
      return;

    drawControlButton(painter, dragButtonRect(), QStringLiteral("drag-handle"));
    drawControlButton(painter, editButtonRect(), QStringLiteral("edit"), false,
                       QStringLiteral("Edit"));
    drawControlButton(painter, pinButtonRect(), QStringLiteral("pin"), expiry_.kept());
    drawControlButton(painter, pathButtonRect(), QStringLiteral("path"));
    drawControlButton(painter, copyButtonRect(), QStringLiteral("copy"), false,
                       QStringLiteral("Copy"));
    drawControlButton(painter, closeButtonRect(), QStringLiteral("close"));
  }

  void drawControlButton(QPainter &painter, const QRectF &rect,
                         const QString &action, bool active = false,
                         const QString &label = {}) const {
    painter.setPen(Qt::NoPen);
    const bool hovered = hovered_ && rect == controlRect(hoveredControl_);
    painter.setBrush(active ? QColor(37, 58, 75, 235)
                            : hovered ? QColor(37, 42, 52, 240)
                                      : QColor(12, 12, 16, 210));
    painter.drawRoundedRect(rect, 6, 6);
    const QColor foreground = active ? QColor(140, 179, 209)
                                     : QColor(245, 245, 247);
    if (label.isEmpty()) {
      drawToolbarIcon(painter, rect, action, {}, foreground);
    } else {
      painter.setFont(chromeFont(11, true));
      painter.setPen(foreground);
      painter.drawText(rect, Qt::AlignCenter, label);
    }
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

  void mousePressEvent(QMouseEvent *event) override {
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
      if (pinButtonRect().contains(position)) {
        toggleKept();
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
        copyPath();
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
  void beginDragWatch(bool compositorDrag = false) {
    if (compositorDrag && dragWatchTimer_.isActive())
      return;
    setTilt(0.0, false);
    ++snapGeneration_;
    dragStartRect_ = ownCompositorRect();
    if (dragStartRect_.isNull())
      return;
    const QString title = windowTitle();
    static_cast<void>(QtConcurrent::run(&pinPool(), [title] {
      PinPlacement placement;
      if (placement.ready())
        placement.beginDrag(title);
    }));
    dragOriginScreen_ = dragScreen_;
    compositorDrag_ = compositorDrag;
    dragFree_.reset();
    dragButtonDown_ = std::nullopt;
    finishRequested_ = false;

    dragPreviousRect_ = {};
    commandedTargets_.clear();
    dragMoved_ = false;
    dragPolls_ = 0;
    dragStablePolls_ = 0;
    spreadActive_ = false;
    snapSpot_ = {};
    openButtonWatch();
    dragWatchTimer_.start();
    updateExpiryPause();
  }

  void observeDrag() {
    const QRect rect = ownCompositorRect();
    ++dragPolls_;
    const bool superHeld = QGuiApplication::keyboardModifiers().testFlag(Qt::MetaModifier);
    if (compositorDrag_ && !superHeld && dragButtonDown_ != true) {
      finishDrag();
      return;
    }
    const bool clickTimeout = !dragMoved_ && dragPolls_ >= 12 &&
                              (!compositorDrag_ || !hovered_ || !superHeld);
    if (rect.isNull() || clickTimeout || dragPolls_ >= 750) {
      dragWatchTimer_.stop();
      closeButtonWatch();
      if (spreadActive_)
        compactPinColumn(windowTitle(), dragScreen_);
      spreadActive_ = false;
      endStackDrag();
      return;
    }
    if (!dragMoved_ && rect != dragStartRect_)
      dragMoved_ = true;
    if (dragMoved_)
      previewInsertion(rect);
    const bool still = rect == dragPreviousRect_;
    dragStablePolls_ = dragMoved_ && still ? dragStablePolls_ + 1 : 0;
    dragPreviousRect_ = rect;
    // The release normally arrives from the input device watch or as a
    // pointer event; this long stillness fallback only catches a session
    // where neither could be established.
    if (dragMoved_ && dragStablePolls_ >= 25 && dragButtonDown_ != true &&
        (!compositorDrag_ || !superHeld))
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
            !dragWatchTimer_.isActive())
          continue;
        dragButtonDown_ = events[index].value != 0;
        if (*dragButtonDown_)
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
    // Unlike an XDG move, a compositor keybind can keep delivering motion
    // throughout its drag. Those events are not a release.
    if (compositorDrag_ && (dragButtonDown_ == true ||
        QGuiApplication::keyboardModifiers().testFlag(Qt::MetaModifier)))
      return;
    finishDrag();
  }

  void watchCompositorDrag(Qt::KeyboardModifiers modifiers) {
    // Super+mouse is consumed by Hyprland, so arm the same geometry watch
    // when Super reaches a hovered pin, including entering with it held.
    if (hovered_ && modifiers.testFlag(Qt::MetaModifier))
      beginDragWatch(true);
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
    if (!dragMoved_ && rect == dragStartRect_) {
      endStackDrag();
      return;
    }
    QRect visible = rect;
    if (!rect.isEmpty() && !dragScreen_.contains(rect) && !dragOriginScreen_.isEmpty()) {
      // Remember the starting monitor throughout the drag. A clipped drop
      // returns there even if its centre crossed into a neighbouring output
      // or an empty part of the desktop layout. Fully visible transfers stay.
      visible = pinVisibleRect(rect, dragOriginScreen_, qRound(kCornerMargin));
      if (dragScreen_ != dragOriginScreen_) {
        compactPinColumn(windowTitle(), dragScreen_);
        dragScreen_ = dragOriginScreen_;
        commandedTargets_.clear();
      }
    }
    if (!visible.isNull())
      previewInsertion(visible);
    dragFree_ = snapSpot_.isNull();
    if (!snapSpot_.isNull()) {
      requestSnap(snapSpot_, ++snapGeneration_);
    } else {
      if (visible != rect)
        requestSnap(visible, ++snapGeneration_);
      compactPinColumn(windowTitle(), dragScreen_);
      if (visible == rect)
        endStackDrag();
    }
    spreadActive_ = false;
  }

  void requestSnap(const QRect &target, quint64 generation, int attempt = 0) {
    if (closing_ || generation != snapGeneration_)
      return;
    snapPending_ = true;
    auto *watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this,
            [this, watcher, target, generation, attempt] {
      const bool moved = watcher->result();
      watcher->deleteLater();
      if (closing_ || generation != snapGeneration_)
        return;
      if (moved) {
        for (CompositorPin &pin : cachedPins_) {
          if (pin.title == windowTitle())
            pin.rect = target;
        }
        endStackDrag();
        return;
      }
      if (attempt < 2) {
        QTimer::singleShot(50, this, [this, target, generation, attempt] {
          requestSnap(target, generation, attempt + 1);
        });
      } else {
        compactPinColumn(windowTitle(), dragScreen_);
        showToast(QStringLiteral("Could not position pinned capture"));
        endStackDrag();
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
      if (!pin.free && pinInColumn(pin.rect.translated(-dragScreen_.topLeft()), dragScreen_.size(), qRound(kCornerMargin), kPinGap))
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

  struct ActionResult {
    QString error;
    QString sharedPath;
    std::shared_ptr<PinSnapshotFile> document;
  };

  void runAction(std::function<ActionResult()> worker, QString message) {
    if (actionPending_)
      return;
    actionPending_ = true;
    updateExpiryPause();
    auto *watcher = new QFutureWatcher<ActionResult>(this);
    connect(watcher, &QFutureWatcher<ActionResult>::finished, this,
            [this, watcher, message] {
      const auto result = watcher->result();
      watcher->deleteLater();
      actionPending_ = false;
      updateExpiryPause();
      // Reuse a successful save even if the following clipboard write failed.
      if (!result.sharedPath.isEmpty())
        sharedPath_ = result.sharedPath;
      if (result.document && !pinDocument_) {
        pinDocument_ = result.document;
        documentFiles_.addPath(operationLogPath(pinDocument_->path()));
        reloadPinDocument();
      }
      if (!result.error.isEmpty())
        showToast(result.error);
      else if (!message.isEmpty()) {
        showToast(message);
      }
    });
    watcher->setFuture(QtConcurrent::run(std::move(worker)));
  }

  void copyImage() {
    runAction([image = image_] {
      QString error;
      static_cast<void>(copyImageToClipboard(image, error));
      return ActionResult{error, {}, {}};
    }, QStringLiteral("Copied to clipboard"));
  }

  void copyPath() {
    runAction([path = path_, shared = sharedPath_]() mutable {
      QString error;
      if (shared.isEmpty() || !QFileInfo::exists(shared)) {
        // Runtime captures disappear with their previews. Copy the encoded
        // PNG once so a shared path remains useful after expiry or closing.
        // An existing file opened with --pin already has its own lifetime.
        const QFileInfo source(path);
        shared = source.absolutePath() == secureRuntimeDirectory()
                     ? copySnapshotToScreenshots(path, error) : source.absoluteFilePath();
      }
      if (!shared.isEmpty())
        static_cast<void>(copyTextToClipboard(shared, error));
      return ActionResult{error, shared, {}};
    }, QStringLiteral("Copied path"));
  }

  void reopenInEditor() {
    runAction([program = QCoreApplication::applicationFilePath(), path = path_,
               document = pinDocument_]() mutable -> ActionResult {
      QString error;
      if (!document)
        document = copyPinDocument(path, error);
      if (!document)
        return {error, {}, {}};
      const QStringList arguments{QStringLiteral("--file"), document->path(),
                                   QStringLiteral("--pin-document"), document->path()};
      if (!QProcess::startDetached(program, arguments))
        return {QStringLiteral("Could not start omasnap"), {}, document};
      return {{}, {}, document};
    }, {});
  }

  void reloadPinDocument() {
    if (closing_ || !pinDocument_)
      return;
    if (documentLoading_) {
      documentReloadPending_ = true;
      return;
    }
    documentLoading_ = true;
    documentWatcher_.setFuture(QtConcurrent::run([document = pinDocument_] {
      PinPreview result;
      QFile file(document->previewPath());
      if (file.open(QIODevice::ReadOnly)) {
        result.png = file.readAll();
        result.image = QImage::fromData(result.png, "PNG");
        if (!result.image.isNull())
          result.drag = result.image.scaled(256, 256, Qt::KeepAspectRatio,
                                            Qt::SmoothTransformation);
      }
      return result;
    }));
  }

  void mouseMoveEvent(QMouseEvent *event) override {
    pointerWokeDuringWatch();
    watchCompositorDrag(event->modifiers());
    const int control = controlRectAt(event->position());
    setCursor(control >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    if (control != hoveredControl_) {
      hoveredControl_ = control;
      QToolTip::hideText();
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
      const QString tip = pinControlTip(control, expiry_.kept());
      if (!tip.isEmpty()) {
        QToolTip::showText(help->globalPos(), tip, this,
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
    fileDragActive_ = true;
    updateExpiryPause();
    dragFree_.reset();
    static_cast<void>(QtConcurrent::run(&pinPool(), [title = windowTitle()] {
      PinPlacement placement;
      if (placement.ready())
        placement.beginDrag(title);
    }));
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
    fileDragActive_ = false;
    endStackDrag();
  }

  void wheelEvent(QWheelEvent *event) override {
    // Pinned captures deliberately keep a stable display-shaped frame so the
    // controls remain usable and the image area never reflows.
    event->accept();
  }

  void keyPressEvent(QKeyEvent *event) override {
    if (event->isAutoRepeat()) {
      event->accept();
      return;
    }
    if (event->key() == Qt::Key_Meta) {
      watchCompositorDrag(event->modifiers() | Qt::MetaModifier);
      event->accept();
      return;
    }
    if (event->key() == Qt::Key_P && event->modifiers() == Qt::ControlModifier) {
      toggleKept();
      return;
    }
    // Closing transfers keyboard focus to the next pin, which need not have
    // received a pointer-enter event before the next key press arrives.
    if ((event->key() == Qt::Key_X && event->modifiers() == Qt::NoModifier) ||
        (event->key() == Qt::Key_W && event->modifiers() == Qt::MetaModifier)) {
      close();
      return;
    }
    if (hovered_) {
      if (event->modifiers() == Qt::NoModifier) {
        switch (event->key()) {
        case Qt::Key_A:
        case Qt::Key_E:
          reopenInEditor();
          return;
        case Qt::Key_C:
          copyImage();
          return;
        case Qt::Key_L:
        case Qt::Key_F:
          copyPath();
          return;
        default:
          break;
        }
      }
    }
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

  void keyReleaseEvent(QKeyEvent *event) override {
    if (event->key() == Qt::Key_Meta && !event->isAutoRepeat() &&
        compositorDrag_ && dragWatchTimer_.isActive() && dragButtonDown_ != true)
      finishDrag();
    QWidget::keyReleaseEvent(event);
  }

  void closeEvent(QCloseEvent *event) override {
    closing_ = true;
    expiry_.setPaused(true);
    stackStateReloadTimer_.stop();
    tiltAnimation_.stop();
    dragWatchTimer_.stop();
    stackWatchTimer_.stop();
    closeButtonWatch();
    // The compositor may still list this window while it closes, so it is
    // excluded by name rather than trusted to be gone.
    static_cast<void>(QtConcurrent::run(&pinPool(),
        [title = windowTitle(), screen = dragScreen_,
         advanceFocus = !expired_ && (hovered_ || isActiveWindow())] {
      PinPlacement placement;
      if (!placement.ready())
        return;
      placement.endDrag(title);
      if (placement.hoverOwner() == title)
        placement.setHover({});
      placement.release(title);
      placement.arrange(screen, !placement.hoverOwner().isEmpty() &&
                                 placement.hoverScreen() == screen, title);
      if (advanceFocus)
        placement.focusNext(title, screen);
    }));
    QWidget::closeEvent(event);
  }

  void enterEvent(QEnterEvent *event) override {
    pointerWokeDuringWatch();
    hovered_ = true;
    updateExpiryPause();
    setTilt(0.0, false);
    watchCompositorDrag(QGuiApplication::keyboardModifiers());
    openStack();
    hoveredControl_ = controlRectAt(event->position());
    setCursor(hoveredControl_ >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    update();
  }

  void leaveEvent(QEvent *) override {
    hovered_ = false;
    updateExpiryPause();
    hoveredControl_ = -1;
    setCursor(Qt::ArrowCursor);
    reloadStackState();
    update();
  }

private:
  void updateExpiryPause() {
    expiry_.setPaused(closing_ || hovered_ || stackInteracting_ || actionPending_ ||
                      fileDragActive_ || dragWatchTimer_.isActive() ||
                      finishRequested_ || snapPending_);
  }

  void toggleKept() {
    expiry_.setKept(!expiry_.kept());
    QToolTip::hideText();
    showToast(expiry_.kept() ? QStringLiteral("Pinned")
                             : QStringLiteral("Fades after 10 seconds"));
  }

  QPainterPath cardPath() const {
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 7, 7);
    return path;
  }

  void updateCardMask() {
    if (QWindow *handle = windowHandle()) {
      const QPainterPath card = pinCardTransform(size(), tilt_).map(cardPath());
      handle->setMask(QRegion(card.toFillPolygon().toPolygon()));
    }
  }

  void setTilt(qreal degrees, bool animate = true) {
    if (animate && qFuzzyCompare(tiltTarget_ + 1.0, degrees + 1.0))
      return;
    tiltTarget_ = degrees;
    tiltAnimation_.stop();
    if (!animate) {
      tilt_ = degrees;
      updateCardMask();
      update();
      return;
    }
    tiltAnimation_.setStartValue(tilt_);
    tiltAnimation_.setEndValue(degrees);
    tiltAnimation_.start();
  }

  void reloadStackState() {
    if (closing_ || stackStatePath_.isEmpty())
      return;
    if (stackStateQueryPending_) {
      stackStateReloadPending_ = true;
      return;
    }
    stackStateQueryPending_ = true;
    stackStateReloadPending_ = false;
    stackStateWatcher_.setFuture(QtConcurrent::run(&pinPool(),
        [path = stackStatePath_, title = windowTitle(), screen = dragScreen_]() -> PinStackState {
      QFile file(path);
      if (!file.open(QIODevice::ReadOnly))
        return {};
      const auto state = QJsonDocument::fromJson(file.readAll()).object();
      const QJsonArray area = state.value(QStringLiteral("screen")).toArray();
      const bool sameScreen = area == QJsonArray{screen.x(), screen.y(), screen.width(), screen.height()};
      const bool free = state.value(QStringLiteral("free")).toObject().value(title).toBool();
      const bool interacting = !state.value(QStringLiteral("drag")).toString().isEmpty() ||
          (!free && sameScreen && !state.value(QStringLiteral("hover")).toString().isEmpty());
      return {state.value(QStringLiteral("tilts")).toObject().value(title).toDouble(), interacting};
    }));
  }

  void showToast(QString message) {
    toast_ = std::move(message);
    update();
    QTimer::singleShot(kToastMs, this, [this] {
      toast_.clear();
      update();
    });
  }

  // Painting, hover feedback, and clicks all use the same control geometry.
  [[nodiscard]] QRectF closeButtonRect() const { return controlRect(0); }

  [[nodiscard]] QRectF copyButtonRect() const { return controlRect(1); }

  [[nodiscard]] QRectF pathButtonRect() const { return controlRect(2); }

  [[nodiscard]] QRectF editButtonRect() const { return controlRect(3); }

  [[nodiscard]] QRectF dragButtonRect() const { return controlRect(4); }

  [[nodiscard]] QRectF pinButtonRect() const { return controlRect(5); }

  [[nodiscard]] QRectF controlRect(int index) const {
    return pinControlRect(size(), index);
  }

  [[nodiscard]] int controlRectAt(const QPointF &position) const {
    for (int index = 0; index < 6; ++index) {
      if (controlRect(index).contains(position))
        return index;
    }
    return -1;
  }

  QImage image_;
  PinExpiry expiry_;
  qreal opacity_ = 1.0;
  bool expired_ = false;
  bool stackInteracting_ = false;
  bool fileDragActive_ = false;
  qreal tilt_ = 0.0;
  qreal tiltTarget_ = 0.0;
  QVariantAnimation tiltAnimation_;
  QFileSystemWatcher stackFiles_;
  QTimer stackStateReloadTimer_;
  QFutureWatcher<PinStackState> stackStateWatcher_;
  QString stackStatePath_;
  bool stackStateQueryPending_ = false;
  bool stackStateReloadPending_ = false;
  QByteArray dragPng_;
  QImage dragPreview_;
  bool actionPending_ = false;
  QString path_;
  QString sharedPath_;
  PinSnapshotFile snapshotFile_;
  std::shared_ptr<PinSnapshotFile> pinDocument_;
  QFileSystemWatcher documentFiles_;
  QFutureWatcher<PinPreview> documentWatcher_;
  bool documentLoading_ = false;
  bool documentReloadPending_ = false;
  QVector<CompositorPin> cachedPins_;
  bool closing_ = false;
  quint64 snapGeneration_ = 0;
  bool queryPending_ = false;
  bool finishRequested_ = false;
  bool compositorDrag_ = false;
  std::optional<bool> dragButtonDown_;
  std::optional<bool> dragFree_;
  QTimer dragWatchTimer_;
  QTimer stackWatchTimer_;
  QFutureWatcher<StackSnapshot> stackWatcher_;
  quint64 stackGeneration_ = 0;
  QElapsedTimer stackOutside_;
  bool stackQueryPending_ = false;
  bool stackOpenRequested_ = false;
  bool snapPending_ = false;
  QRect dragStartRect_;
  QRect dragOriginScreen_;
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

int runPinnedCapture(const QString &path, PinLifetime lifetime) {
  QImage image(path);
  if (image.isNull()) {
    qWarning("omasnap: could not load pinned image %s", qUtf8Printable(path));
    return 1;
  }

  PinWindow window(std::move(image), path, pinFrameSize({}), lifetime);
  if (!window.hasPinLock()) {
    qWarning("omasnap: could not lock pinned image %s", qUtf8Printable(path));
    return 1;
  }
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
      // A new capture is the front of the idle deck. Keep an already-open
      // fan exposed, and never rearrange pins during somebody else's drag.
      if (!placement.dragging().isEmpty())
        return {};
      own->rect.setSize(frame);
      const bool expanded = !placement.hoverOwner().isEmpty() &&
                             placement.hoverScreen() == screen;
      if (!placement.arrange(screen, expanded, {}, title))
        return {};
      return {screen, placement.pins};
    }));
  });
  auto *monitor = new QFutureWatcher<CompositorMonitor>(&window);
  QObject::connect(monitor, &QFutureWatcher<CompositorMonitor>::finished, &window,
                   [&window, monitor, watcher, settle, screen, attempts = 0]() mutable {
    const auto result = monitor->result();
    *screen = result.workArea;
    if (screen->isEmpty() && ++attempts < 10) {
      QTimer::singleShot(50, &window, [monitor] {
        monitor->setFuture(QtConcurrent::run(&pinPool(), [] { return compositorMonitor(compositorMonitors()); }));
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
    window.setFixedSize(pinFrameSize(result.geometry.size()));
    settle->start();
  });
  // Register before mapping: a post-capture preview must not take keyboard
  // focus from the app the user is returning to. Hovering subsequently
  // follows normal mouse focus so pin shortcuts target the hovered capture.
  const auto applyRules = [] {
    bool ok = false;
    const QString output = runForOutput(
        QStringLiteral("hyprctl"),
        {QStringLiteral("eval"),
         QStringLiteral("hl.window_rule({ name = \"omasnap-pins\", "
                        "match = { class = \"^omasnap$\", title = \"^omasnap-pin [0-9]+$\" }, "
                        "float = true, pin = true, no_initial_focus = true, "
                        "no_follow_mouse = false, border_size = 0, rounding = 0, "
                        "no_shadow = true, no_blur = true })")}, &ok);
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
      return compositorMonitor(compositorMonitors());
    }));
  });
  rules->setFuture(QtConcurrent::run(&pinPool(), applyRules));
  return QApplication::exec();
}
