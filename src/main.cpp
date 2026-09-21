#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include "capture.hpp"
#include "capture-delay.hpp"
#include "chrome-theme.hpp"
#include "cli-path.hpp"
#include "editor.hpp"
#include "instance-lock.hpp"
#include "output-config.hpp"
#include "overlay-chrome.hpp"
#include "overlay-dismissal.hpp"
#include "pin.hpp"
#include "pin-file.hpp"
#include "recent-snaps.hpp"
#include "startup-timing.hpp"

#include <LayerShellQt/Window>


#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <QApplication>

#include <algorithm>
#include <memory>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QLockFile>
#include <QScreen>
#include <QSocketNotifier>
#include <QUrl>
#include <QWindow>

#include <csignal>
#include <optional>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>

namespace {
class PosixSignalNotifier final : public QObject {
public:
  explicit PosixSignalNotifier(QObject *parent = nullptr) : QObject(parent) {
    if (::socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                     fds_) != 0)
      return; // Default signal disposition stays in effect.
    signalFd_ = fds_[0];
    signalReceived_ = 0;

    struct sigaction sa{};
    sa.sa_handler = [](int) {
      const int savedErrno = errno;
      signalReceived_ = 1;
      const char byte = 1;
      const int fd = signalFd_;
      if (fd >= 0)
        static_cast<void>(::write(fd, &byte, sizeof(byte)));
      errno = savedErrno;
    };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigintInstalled_ = ::sigaction(SIGINT, &sa, &previousSigint_) == 0;
    sigtermInstalled_ = ::sigaction(SIGTERM, &sa, &previousSigterm_) == 0;
    if (!sigintInstalled_ && !sigtermInstalled_) {
      closeSockets();
      return;
    }

    notifier_ = new QSocketNotifier(fds_[1], QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, [this] {
      notifier_->setEnabled(false);
      char bytes[32];
      while (::read(fds_[1], bytes, sizeof(bytes)) > 0) {
      }
      QCoreApplication::quit();
    });
  }

  ~PosixSignalNotifier() override {
    if (sigintInstalled_)
      ::sigaction(SIGINT, &previousSigint_, nullptr);
    if (sigtermInstalled_)
      ::sigaction(SIGTERM, &previousSigterm_, nullptr);
    closeSockets();
  }

  // The delay timer can quit the event loop before its queued socket event
  // runs. Observe receipt in the handler so that pending cancellation still
  // prevents the synchronous fullscreen capture/output path from starting.
  [[nodiscard]] bool wasNotified() const { return signalReceived_ != 0; }

private:
  void closeSockets() {
    signalFd_ = -1;
    for (int &fd : fds_) {
      if (fd >= 0) {
        ::close(fd);
        fd = -1;
      }
    }
  }

  static inline int fds_[2]{-1, -1};
  static inline volatile sig_atomic_t signalFd_ = -1;
  static inline volatile sig_atomic_t signalReceived_ = 0;
  struct sigaction previousSigint_{};
  struct sigaction previousSigterm_{};
  bool sigintInstalled_ = false;
  bool sigtermInstalled_ = false;
  QSocketNotifier *notifier_ = nullptr;
};
// All compositor IPC runs on a worker, including the pre-map rules. A
// missing or wedged hyprctl is bounded and never stalls a visible editor.
QByteArray hyprctlOutput(const QStringList &arguments) {
  QProcess process;
  process.start(QStringLiteral("hyprctl"), arguments);
  if (!process.waitForFinished(500)) {
    process.kill();
    process.waitForFinished(500);
    return {};
  }
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    return {};
  return process.readAllStandardOutput();
}

} // namespace

int main(int argc, char **argv) {
  startupTimingMark("entered main");
  QCoreApplication::setApplicationName(QStringLiteral("omasnap"));
  QCoreApplication::setApplicationVersion(QString::fromLatin1(OMASNAP_VERSION));
  QCoreApplication::setOrganizationName(QStringLiteral("Omarchy"));
  // Choose the Wayland shell before Qt connects. Pins and file editors
  // use compositor windows; fresh captures select on a fullscreen overlay.
  QStringList rawArguments;
  for (int index = 0; index < argc; ++index)
    rawArguments.push_back(QString::fromLocal8Bit(argv[index]));
  QCommandLineParser startupParser;
  configureCaptureCommandLine(startupParser, true);
  const bool startupParsed = startupParser.parse(rawArguments);
  const bool pinInvocation = startupParsed &&
      (startupParser.isSet(QStringLiteral("pin")) || startupParser.isSet(QStringLiteral("preview")));
  const bool windowedEditorProcess = startupParsed &&
      windowedEditorRequested(startupParser, loadEditorWindowMode(defaultConfigPath()));
  if (pinInvocation || windowedEditorProcess) {
    // Child windows can inherit layer-shell from their capture overlay.
    qunsetenv("QT_WAYLAND_SHELL_INTEGRATION");
  } else {
    qputenv("QT_WAYLAND_SHELL_INTEGRATION", "layer-shell");
  }
  // Omarchy exports QT_QPA_PLATFORMTHEME=gtk3 session-wide. Honouring it
  // loads the qgtk3 plugin, which initialises GTK inside this process
  // (measured 81-112 ms of QApplication construction, plus ~20-24 MiB of
  // RSS) for a hand-painted overlay that opens no dialogs and reads no palette.
  // Qt's built-in generic theme is all it needs, so select it by name
  // (an empty value would let Qt pick a theme from XDG_CURRENT_DESKTOP
  // instead). The chrome font is pinned in chromeFont() rather than taken
  // from the theme. `-platformtheme gtk3` on the command line still
  // overrides this for debugging.
  qputenv("QT_QPA_PLATFORMTHEME", "generic");
  QGuiApplication::setDesktopFileName(QStringLiteral("omasnap"));
  QApplication application(argc, argv);
  startupTimingMark("QApplication constructed");
  // With the external desktop theme bypassed, Qt's default font would be
  // generic "Sans Serif 9"; pin what the theme used to install before any
  // widget is created, so painter/widget default-font text keeps its size and
  // face.
  QApplication::setFont(chromeDefaultFont());
  initializeChromeTheme();

  // A stitched scroll capture (or any tall pinned image) exceeds Qt's default
  // 256 MB image-decode allocation limit; lift it so --file/--pin can open it.
  QImageReader::setAllocationLimit(0);
  PosixSignalNotifier signalNotifier(&application);

  QCommandLineParser parser;
  configureCaptureCommandLine(parser);
  parser.process(application);
  startupTimingMark("command line parsed");

  QString filePath = parser.value(QStringLiteral("file"));
  const bool clipboardInput = parser.isSet(QStringLiteral("clipboard"));
  int delaySeconds = 0;
  if (parser.isSet(QStringLiteral("delay")) &&
      !parseCaptureDelay(parser.value(QStringLiteral("delay")), delaySeconds)) {
    qCritical() << "--delay takes a whole number from 0 through 3600 seconds";
    return 2;
  }

  const QString editorModeArg = parser.value(QStringLiteral("editor")).trimmed().toLower();
  if (!editorModeArg.isEmpty() &&
      editorModeArg != QStringLiteral("window") &&
      editorModeArg != QStringLiteral("overlay")) {
    qCritical() << "--editor takes window or overlay";
    return 2;
  }
  const bool editorWindowMode =
      editorModeArg == QStringLiteral("window") ||
      (editorModeArg.isEmpty() && loadEditorWindowMode(defaultConfigPath()));

  QuickOutputMode quickOutputMode = QuickOutputMode::None;
  if (parser.isSet(QStringLiteral("copy")) && parser.isSet(QStringLiteral("save")))
    quickOutputMode = QuickOutputMode::Both;
  else if (parser.isSet(QStringLiteral("copy")))
    quickOutputMode = QuickOutputMode::Copy;
  else if (parser.isSet(QStringLiteral("save")))
    quickOutputMode = QuickOutputMode::Save;

  CaptureEditor::CaptureMode captureMode = CaptureEditor::CaptureMode::Smart;
  int requestedModes = parser.isSet(QStringLiteral("capture-fullscreen")) +
                       parser.isSet(QStringLiteral("capture-window")) + parser.isSet(QStringLiteral("capture-region")) +
                       parser.isSet(QStringLiteral("scroll"));
  if (parser.isSet(QStringLiteral("capture-fullscreen")))
    captureMode = CaptureEditor::CaptureMode::Fullscreen;
  else if (parser.isSet(QStringLiteral("capture-window")))
    captureMode = CaptureEditor::CaptureMode::Window;
  else if (parser.isSet(QStringLiteral("scroll")))
    captureMode = CaptureEditor::CaptureMode::Scroll;
  else if (parser.isSet(QStringLiteral("capture-region")))
    captureMode = CaptureEditor::CaptureMode::Region;

  const QStringList positional = parser.positionalArguments();
  if (parser.isSet(QStringLiteral("pin")) || parser.isSet(QStringLiteral("preview"))) {
    if (!filePath.isEmpty() || clipboardInput || requestedModes > 0 ||
        !positional.isEmpty() || quickOutputMode != QuickOutputMode::None ||
        parser.isSet(QStringLiteral("delay")) ||
        (parser.isSet(QStringLiteral("pin")) && parser.isSet(QStringLiteral("preview")))) {
      qCritical()
          << "Pinned mode cannot be combined with capture or edit targets";
      return 2;
    }
    const bool preview = parser.isSet(QStringLiteral("preview"));
    const QString target = parser.value(preview ? QStringLiteral("preview") : QStringLiteral("pin"));
    QString pinPath = QUrl(target).toLocalFile();
    if (pinPath.isEmpty())
      pinPath = target;
    return runPinnedCapture(pinPath, preview ? PinLifetime::Timed : PinLifetime::Persistent);
  }
  if (positional.size() > 1) {
    qCritical() << "Only one capture target may be specified";
    return 2;
  }
  if (!positional.isEmpty()) {
    const QString localTarget = resolveLocalImagePath(positional.first());
    if (filePath.isEmpty() && !localTarget.isEmpty()) {
      filePath = localTarget;
    } else {
      ++requestedModes;
      const QString mode = positional.first();
      if (mode == QStringLiteral("fullscreen"))
        captureMode = CaptureEditor::CaptureMode::Fullscreen;
      else if (mode == QStringLiteral("windows") ||
               mode == QStringLiteral("window"))
        captureMode = CaptureEditor::CaptureMode::Window;
      else if (mode == QStringLiteral("smart"))
        captureMode = CaptureEditor::CaptureMode::Smart;
      else if (mode == QStringLiteral("region"))
        captureMode = CaptureEditor::CaptureMode::Region;
      else if (mode == QStringLiteral("scroll"))
        captureMode = CaptureEditor::CaptureMode::Scroll;
      else {
        qCritical().noquote()
            << QStringLiteral("Unknown capture target: %1").arg(mode);
        return 2;
      }
    }
  }
  if (filePath.isEmpty() && requestedModes > 1) {
    qCritical() << "Capture mode options are mutually exclusive";
    return 2;
  }
  if (!filePath.isEmpty() && requestedModes > 0) {
    qCritical() << "An image file cannot be combined with a capture mode";
    return 2;
  }
  if (clipboardInput && (!filePath.isEmpty() || requestedModes > 0)) {
    qCritical() << "Clipboard input cannot be combined with another target";
    return 2;
  }
  const bool editingImage = clipboardInput || !filePath.isEmpty();
  if (parser.isSet(QStringLiteral("delay")) &&
      (editingImage || parser.isSet(QStringLiteral("file")))) {
    qCritical() << "--delay cannot be combined with an image input";
    return 2;
  }
  if (editingImage && quickOutputMode != QuickOutputMode::None) {
    qCritical()
        << "Quick output options cannot be combined with an image input";
    return 2;
  }
  if (!editingImage && quickOutputMode == QuickOutputMode::None &&
      !parser.isSet(QStringLiteral("editor")))
    quickOutputMode = QuickOutputMode::CopyAndPreview;
  startupTimingMark("options resolved");
  if (!loadCaptureFonts())
    return 1;
  startupTimingMark("capture font loaded");
  application.setQuitOnLastWindowClosed(true);

  const QString runtime = secureRuntimeDirectory();
  startupTimingMark("runtime directory ready");
  if (runtime.isEmpty()) {
    qCritical() << "Could not create private runtime directory";
    return 1;
  }
  QLockFile instanceLock(
      QDir(runtime).filePath(QStringLiteral("omasnap.instance")));
  // Every capture, quick output included, dismisses a running overlay instead
  // of starting a second one: a late capture would otherwise photograph that overlay.
  // Editing an image always takes over so the requested editor can open.
  const InstanceLockResult lockResult = acquireInstanceLock(
      instanceLock, editingImage ? InstanceMode::EditFile
                                 : InstanceMode::Capture);
  startupTimingMark("instance lock acquired");
  if (lockResult.signalledPid != 0)
    qInfo().noquote() << QStringLiteral("Asked the running omasnap (pid %1) to "
                                        "quit")
                             .arg(lockResult.signalledPid);
  if (!lockResult.proceed) {
    if (!lockResult.error.isEmpty())
      qCritical().noquote() << lockResult.error;
    return lockResult.exitCode;
  }

  CaptureData capture;
  OperationLog restoredLog;
  QString error;
  std::shared_ptr<PinSnapshotFile> pinDocument;
  if (parser.isSet(QStringLiteral("pin-document"))) {
    const QString path = parser.value(QStringLiteral("pin-document"));
    if (!editingImage || !PinSnapshotFile::isOwnedPath(path)) {
      qCritical("Invalid pinned document");
      return 1;
    }
    pinDocument = std::make_shared<PinSnapshotFile>(path);
    if (!pinDocument->isLocked()) {
      qCritical("Could not retain the pinned document");
      return 1;
    }
  }
  if (editingImage) {
    QImage image;
    QString inputName;
    if (clipboardInput) {
      if (!loadClipboardImage(image, error)) {
        const QString message =
            QStringLiteral("Could not load clipboard image: %1").arg(error);
        qCritical().noquote() << message;
        sendCaptureNotification(message);
        return 1;
      }
      inputName = QStringLiteral("clipboard image");
    } else {
      QString localFile = QUrl(filePath).toLocalFile();
      if (localFile.isEmpty())
        localFile = filePath;
      image.load(localFile);
      if (image.isNull()) {
        qCritical().noquote()
            << QStringLiteral("Could not load image: %1").arg(filePath);
        return 1;
      }
      inputName = localFile;
      const QString sidecar = operationLogPath(localFile);
      if (QFile::exists(sidecar) &&
          !loadOperationLog(sidecar, restoredLog, error)) {
        qCritical().noquote()
            << QStringLiteral("Could not restore operation log: %1").arg(error);
        return 1;
      }
      // Ownership of private handoff files ends once both source and log
      // are in memory. Ordinary user files are excluded by the helper.
      removeEditorHandoff(localFile, parser.value(QStringLiteral("handoff-token")));
    }
    describeFileCapture(capture, image, restoredLog);
    capture.monitor.name = parser.value(QStringLiteral("handoff-monitor"));
    captureMode = CaptureEditor::CaptureMode::File;
    qInfo().noquote() << QStringLiteral("Opened %1 for annotation (%2x%3)")
                             .arg(inputName)
                             .arg(image.width())
                             .arg(image.height());
  } else if (!probeFocusedMonitor(capture.monitor, error)) {
    qCritical().noquote() << error;
    sendCaptureNotification(QStringLiteral("Screenshot failed: %1").arg(error));
    return 1;
  }
  startupTimingMark(editingImage ? "input image prepared"
                                 : "focused monitor probed");

  if (delaySeconds > 0) {
    qInfo().noquote() << QStringLiteral("Capturing in %1 seconds; run omasnap "
                                       "again to cancel.").arg(delaySeconds);
    if (signalNotifier.wasNotified() || !runCaptureDelay(delaySeconds) ||
        signalNotifier.wasNotified())
      return 0;
  }

  // Grab the output before the layer exists. ext-image-copy-capture waits for
  // a composited frame, so mapping the dim overlay first photographs the veil.
  const bool instantFullscreenOutput =
      !editingImage && captureMode == CaptureEditor::CaptureMode::Fullscreen &&
      quickOutputMode != QuickOutputMode::None &&
      quickOutputMode != QuickOutputMode::CopyAndPreview;
  if (!editingImage &&
      !captureMonitorPixels(capture.monitor, capture,
                            !instantFullscreenOutput, error)) {
    qCritical().noquote() << error;
    sendCaptureNotification(QStringLiteral("Screenshot failed: %1").arg(error));
    return 1;
  }
  startupTimingMark(editingImage ? "pixel capture skipped"
                                 : "monitor pixels captured");

  if (instantFullscreenOutput) {
    QString outputError;
    const QSize expectedSize(
        qRound(capture.previewSize.width() * capture.monitor.scale),
        qRound(capture.previewSize.height() * capture.monitor.scale));
    const QImage output = capture.monitor.scale <= 1.0 ||
                                  capture.source.size() == expectedSize
                              ? capture.source
                              : renderCapture(capture,
                                              QRectF(QPointF(), capture.previewSize), {},
                                              BackgroundStyle::None);
    if (!quickOutput(output, quickOutputMode, outputError, capture.previewSize)) {
      qCritical().noquote() << outputError;
      return 1;
    }
    return 0;
  }

  QScreen *targetScreen = QGuiApplication::primaryScreen();
  for (QScreen *screen : QGuiApplication::screens()) {
    if (screen->name() == capture.monitor.name) {
      targetScreen = screen;
      break;
    }
  }

  if (!editingImage) {
    qInfo().noquote() << QStringLiteral(
                             "Captured %1 workspace %2 with %3 selectable "
                             "windows")
                             .arg(capture.monitor.name)
                             .arg(capture.monitor.workspaceId)
                             .arg(capture.windows.size());
  }

  const QSize editingPreview = capture.previewSize;
  CaptureEditor editor(std::move(capture), captureMode, quickOutputMode,
                       restoredLog, nullptr, editorWindowMode && !editingImage);
  editor.setPinDocument(std::move(pinDocument));
  startupTimingMark("CaptureEditor constructed");
  editor.setScreen(targetScreen);
  if (windowedEditorProcess && editingImage) {
    // An ordinary compositor window: the compositor manages it, and its own
    // float toggle works either way. The overlay chrome carries over
    // unchanged; only the surface role differs.
    editor.setWindowedPresentation(true);
    editor.setWindowedBackdropOpaque(
        loadEditorWindowBackdropOpaque(defaultConfigPath()));
    editor.setWindowTitle(
        filePath.isEmpty() ? QStringLiteral("omasnap")
                           : QStringLiteral("omasnap %1")
                                 .arg(QFileInfo(filePath).fileName()));
    // Size to the visible selection, not the pristine canvas: a handed-off
    // capture keeps its whole monitor underneath, but the window should hug
    // what is actually being annotated.
    const QSizeF selectionSize = editor.currentSelection().size();
    const QSize hugged =
        selectionSize.isEmpty() ? editingPreview : selectionSize.toSize();
    // The guide band's height depends on how wide the card may be, so
    // measure it at the width this window will have.
    const int legendHeight =
        hotkeyLegendAnchoredSize(editorHotkeyEntries(),
                                 std::max(392, hugged.width() + 100))
            .height();
    const QSize naturalSize =
        editorWindowSize(hugged, targetScreen->availableGeometry().size(),
                         legendHeight);
    // A hard floor clamps interactive floating resizes where the toolbar
    // still reads; a tiled window's compositor overrides the hint, and
    // that path is accepted as the tiled look.
    editor.setMinimumSize(640, 420);
    editor.resize(naturalSize);
    // Both rules registered before the window maps, so the compositor
    // floats, centers, and keeps it opaque from the first frame instead of
    // tiling briefly and popping out.
    const bool floatingWindow = loadEditorWindowFloating(defaultConfigPath());
    auto *rules = new QFutureWatcher<void>(&editor);
    QObject::connect(rules, &QFutureWatcher<void>::finished, &editor,
                     [&editor, rules, floatingWindow, naturalSize] {
      rules->deleteLater();
      editor.show();
      editor.setFocus(Qt::ActiveWindowFocusReason);
      if (!floatingWindow)
        return;
      auto *settle = new QTimer(&editor);
      settle->setInterval(50);
      settle->setSingleShot(true);
      auto *probe = new QFutureWatcher<bool>(&editor);
      QObject::connect(probe, &QFutureWatcher<bool>::finished, &editor,
                       [probe, settle, attempts = 0]() mutable {
        if (!probe->result() && ++attempts < 10)
          settle->start();
        else {
          settle->deleteLater();
          probe->deleteLater();
        }
      });
      const auto placementApplied = std::make_shared<bool>(false);
      QObject::connect(settle, &QTimer::timeout, &editor, [probe, naturalSize, placementApplied] {
        const qint64 pid = QCoreApplication::applicationPid();
        probe->setFuture(QtConcurrent::run([pid, naturalSize, placementApplied] {
          const QJsonArray clients = QJsonDocument::fromJson(
              hyprctlOutput({QStringLiteral("-j"), QStringLiteral("clients")})).array();
          for (const QJsonValue &value : clients) {
            const QJsonObject client = value.toObject();
            if (client.value(QStringLiteral("pid")).toInteger() != pid)
              continue;
            const bool floating = client.value(QStringLiteral("floating")).toBool();
            const QJsonArray size = client.value(QStringLiteral("size")).toArray();
            const bool sized = size.size() == 2 &&
                std::abs(size.at(0).toInt() - naturalSize.width()) <= 1 &&
                std::abs(size.at(1).toInt() - naturalSize.height()) <= 1;
            if (*placementApplied && floating && sized)
              return true;
            const QString selector = QStringLiteral("window = \"pid:%1\"").arg(pid);
            const auto dispatch = [](const QString &command) {
              return hyprctlOutput({QStringLiteral("dispatch"), command}).trimmed() == "ok";
            };
            if (!floating && !dispatch(QStringLiteral("hl.dsp.window.float({ %1 })").arg(selector)))
              return false;
            if (!dispatch(QStringLiteral("hl.dsp.window.resize({ x = %1, y = %2, relative = false, %3 })")
                              .arg(naturalSize.width()).arg(naturalSize.height()).arg(selector)) ||
                !dispatch(QStringLiteral("hl.dsp.window.center({ %1 })").arg(selector)))
              return false;
            *placementApplied = true;
            // A successful command precedes the compositor's state update.
            // Probe again rather than treating dispatch as confirmed placement.
            return false;
          }
          return false;
        }));
      });
      settle->start();
    });
    rules->setFuture(QtConcurrent::run([floatingWindow] {
      hyprctlOutput({QStringLiteral("eval"), editorFloatRuleScript(floatingWindow)});
      hyprctlOutput({QStringLiteral("eval"),
                     QStringLiteral("hl.window_rule({ name = \"omasnap-editor-opaque\", "
                                    "match = { title = \"^omasnap( .+)?$\" }, opacity = 1 })")});
    }));
    const int result = application.exec();
    // The closed editor may still be retaining its recent document. A fresh
    // capture must not mistake that background save for an active overlay.
    editor.hide();
    instanceLock.unlock();
    return result;
  }
  editor.setGeometry(targetScreen->geometry());
  editor.winId();
  QWindow *window = editor.windowHandle();
  LayerShellQt::Window *layerWindow = LayerShellQt::Window::get(window);
  if (!window || !layerWindow) {
    qCritical() << "Could not create capture overlay layer";
    return 1;
  }
  layerWindow->setScope(QStringLiteral("omasnap"));
  layerWindow->setScreen(targetScreen);
  layerWindow->setLayer(LayerShellQt::Window::LayerOverlay);
  LayerShellQt::Window::Anchors anchors;
  anchors.setFlag(LayerShellQt::Window::AnchorTop);
  anchors.setFlag(LayerShellQt::Window::AnchorBottom);
  anchors.setFlag(LayerShellQt::Window::AnchorLeft);
  anchors.setFlag(LayerShellQt::Window::AnchorRight);
  layerWindow->setAnchors(anchors);
  layerWindow->setExclusiveZone(-1);
  layerWindow->setKeyboardInteractivity(
      LayerShellQt::Window::KeyboardInteractivityExclusive);
  layerWindow->setActivateOnShow(true);
  editor.setLayerWindow(layerWindow);
  OverlayDismissal overlayDismissal(editor);
  startupTimingMark("layer surface configured");
  editor.show();
  editor.setFocus(Qt::ActiveWindowFocusReason);
  startupTimingMark("show requested; entering event loop");

  const int result = application.exec();
  editor.hide();
  instanceLock.unlock();
  return result;
}
