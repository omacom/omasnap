/** @fileoverview Declares screenshot capture, rendering, and output types. */
#pragma once

#include "cut.hpp"
#include "pin.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include <QPainterPath>
#include <QColor>
#include <QImage>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QVector>

class QFont;
class QPainter;
class QProcess;

struct MonitorInfo {
  QString name;
  QRect geometry;
  QSize pixelSize;
  qreal scale = 1.0;
  int workspaceId = 0;
};

struct WindowTarget {
  QRect rect;
  QString stableId;
  QString title;
  /** Hyprland window class (e.g. `firefox`, `org.gnome.Nautilus`). */
  QString appClass;
};

struct CaptureData {
  MonitorInfo monitor;
  QImage source;
  /** Logical size the native source image is presented at. */
  QSize previewSize;
  QVector<WindowTarget> windows;
  /** Loaded documents retain their exact pixel dimensions during rendering. */
  bool preserveSourceResolution = false;
};

enum class BackgroundStyle {
  None,
  Off,
  Slate,
  Aurora,
  Sunset,
  Lagoon,
  Violet,
  Custom
};
enum class CanvasBoundaryMode { Framed, Overflow, Image };
enum class QuickOutputMode { None, Copy, Save, Both, CopyAndPreview };

enum class SpotlightShape { Ellipse, Rectangle, RoundedRectangle };
enum class RedactionStyle { Solid, Pixelate };
enum class TextBackground { Plain, Pill, Outline };
enum class TextFont { Neucha, JetBrainsMono, InterDisplay };
enum class ArrowStyle { Standard, Pointy, Curved, Double };

struct Annotation {
  enum class Kind {
    Arrow,
    Line,
    Freehand,
    Highlighter,
    Marker,
    Rectangle,
    Ellipse,
    Text,
    Redaction,
    Spotlight
  };

  Kind kind = Kind::Arrow;
  QPointF start;
  QPointF end;
  QString text;
  QColor color;
  qreal size = 4.0;
  int number = 0;
  QVector<QPointF> points;
  bool filled = false;
  qreal cornerRadius = 0.0;
  RedactionStyle redactionStyle = RedactionStyle::Pixelate;
  qreal magnification = 2.0;
  SpotlightShape spotlightShape = SpotlightShape::Ellipse;
  quint32 redactionSeed = 0;
  TextBackground textBackground = TextBackground::Pill;
  /// Typeface is a layer property so reopened and duplicated labels keep it.
  TextFont textFont = TextFont::Neucha;
  ArrowStyle arrowStyle = ArrowStyle::Standard;
  quint64 id = 0;
  /** Explicit quadratic Bezier control for Curved/Double arrows. Empty uses
   *  the calibrated perpendicular-offset curve. */
  std::optional<QPointF> curveControl = std::nullopt;
  /// Wrap width for text layers in image px; 0 leaves the layer unbounded.
  qreal textWidth = 0.0;
  /// Raw pointer geometry retained so smoothing changes never compound.
  QVector<QPointF> rawPoints{};
  /// Pen post-stroke smoothing level (0--6); unused by other layer kinds.
  int smoothingLevel = 0;

  bool operator==(const Annotation &) const = default;
};

struct Operation {
  enum class Type {
    Crop,
    Background,
    CanvasBoundary,
    Annotate,
    Patch,
    Delete,
    Cut
  };

  Type type = Type::Annotate;
  QRectF crop;
  BackgroundStyle background = BackgroundStyle::None;
  bool imageShadow = true;
  CanvasBoundaryMode canvasBoundary = CanvasBoundaryMode::Framed;
  QVector<Annotation> annotations;
  QVector<quint64> ids;
  CutOp cut;

  bool operator==(const Operation &) const = default;
};

struct OperationLog {
  QVector<Operation> ops;
  int index = 0;
  quint64 nextId = 1;
  int nextMarker = 1;
  /// Logical size the source was presented at when the log was written. Op
  /// coordinates live in that space, so a source captured on a scaled
  /// monitor reopens at the same scale. Invalid when unknown.
  QSize previewSize;
  /// Identity shared by a capture's recent entry, preview, and editor handoffs.
  QString recentId = {};

  bool operator==(const OperationLog &) const = default;
};

enum class AnnotationLayer { Redaction, Default };

[[nodiscard]] constexpr AnnotationLayer annotationLayer(Annotation::Kind kind) {
  return kind == Annotation::Kind::Redaction ? AnnotationLayer::Redaction
                                             : AnnotationLayer::Default;
}

[[nodiscard]] bool loadCaptureFonts();
/** User-facing name for a bundled annotation typeface. */
[[nodiscard]] QString annotationTextFontName(TextFont textFont);
/** Bundled annotation font at Omasnap's logical text size. */
[[nodiscard]] QFont annotationTextFont(qreal size,
                                       TextFont textFont = TextFont::Neucha);
/**
 * Discovers the focused monitor (name, geometry, scale). Fast: only one
 * `hyprctl monitors` call. Safe to call on the main thread to position the
 * overlay after the pixel capture has already produced a frozen frame.
 */
[[nodiscard]] bool probeFocusedMonitor(MonitorInfo &monitor, QString &error);
/**
 * Captures the focused monitor's pixels onto the given monitor, and its window
 * list when `includeWindows` is set. Window discovery runs alongside the screen
 * grab, so callers that never show the overlay should skip it. Pure I/O and
 * image work (no GUI objects): safe on any thread.
 */
[[nodiscard]] bool captureMonitorPixels(const MonitorInfo &monitor,
                                        CaptureData &capture,
                                        bool includeWindows, QString &error);
/** Convenience: probes the focused monitor, then captures its pixels. */
[[nodiscard]] bool captureFocusedMonitor(CaptureData &capture,
                                         bool includeWindows, QString &error);
/** Bounds of a text layer's glyph box, or of its readability pill when it
 *  has one; `start` is the baseline origin. */
/// Narrowest wrap width worth producing; below this a line would be a sliver,
/// so the text stays on one line instead.
inline constexpr qreal kMinimumTextWrapWidth = 48.0;
/** Width a text layer wraps to: its own `textWidth`, else the room left
 *  before an optional right edge (`canvasWidth`, 0 = unbounded). The editor
 *  supplies that edge while typing, then stores an explicit width only when
 *  the draft actually wrapped. */
[[nodiscard]] qreal annotationTextWrapWidth(const Annotation &annotation,
                                            qreal canvasWidth);
/** The display lines of a text layer: hard newlines first, then each of those
 *  word-wrapped to annotationTextWrapWidth(). */
[[nodiscard]] QStringList annotationTextLines(const Annotation &annotation,
                                              qreal canvasWidth = 0.0);
[[nodiscard]] QRectF annotationTextBounds(const Annotation &annotation,
                                          qreal canvasWidth = 0.0);
/** Whether a spotlight has an opening inside `bounds`. The first one that
 *  does dims everything else there, so it changes far more than its own
 *  rectangle. */
[[nodiscard]] bool spotlightOpens(const Annotation &annotation,
                                  const QRectF &bounds);
/** Width of the pen a layer's outline is stroked with; 0 when it has none. */
[[nodiscard]] qreal annotationPenWidth(const Annotation &annotation);
/**
 * Extent of everything a layer paints, in annotation space, antialiasing
 * included. Canvas growth and the editor's repaint damage both read it, so a
 * layer can never paint outside what either of them allows for. Empty for a
 * redaction, which only ever replaces source pixels.
 */
[[nodiscard]] QRectF annotationPaintedBounds(const Annotation &annotation);
/**
 * Pixel-aligned annotation space selected by `boundaryMode`. Grow contains
 * every painted extent, Frame stops at the normal backdrop frame, and Image
 * stops at the source frame. The source always starts at 0,0; a negative
 * top/left means background was added before it. Keeping this as a derived
 * value avoids translating layers or repeatedly copying the source.
 */
[[nodiscard]] QRectF
captureCanvasRect(const QSizeF &sourceFrameSize,
                  const QVector<Annotation> &annotations,
                  CanvasBoundaryMode boundaryMode = CanvasBoundaryMode::Framed);
/** Captures the named output through ext-image-copy-capture. */
/** A live native capture session for one output (`MonitorInfo::name`, e.g.
 *  "DP-3") over its own Wayland connection: open once, then grab frames
 *  repeatedly into the same buffer: a scroll capture takes many per second
 *  and must not pay a process spawn or a session handshake for each. Frames
 *  are captured without the cursor and returned upright in output pixels. */
class OutputCapture {
public:
  OutputCapture();
  ~OutputCapture();
  OutputCapture(const OutputCapture &) = delete;
  OutputCapture &operator=(const OutputCapture &) = delete;
  [[nodiscard]] bool open(const QString &outputName, QString &error);
  /// Grab the next frame. `timeoutMs` bounds the wait for the compositor to
  /// deliver damage (a fully static output would otherwise block up to 2 s).
  /// Returns false on timeout as well as on real failures, and `error` is set
  /// either way; poll sessionStopped() to tell a dead session from a quiet
  /// screen and simply retry the rest.
  [[nodiscard]] bool grab(QImage &image, QString &error, int timeoutMs = 2000);
  [[nodiscard]] bool isOpen() const;
  /// True once the compositor has stopped the session (output gone, mode
  /// change it will not resume from); further grabs cannot succeed.
  [[nodiscard]] bool sessionStopped() const;
  /** Pixel size the compositor announced for frames (empty until open). */
  [[nodiscard]] QSize bufferSize() const;
  void close();

private:
  struct State;
  std::unique_ptr<State> state_;
};

[[nodiscard]] bool captureOutputSurface(const MonitorInfo &monitor,
                                        QImage &image, QString &error);
[[nodiscard]] QString operationLogPath(const QString &imagePath);
[[nodiscard]] bool saveOperationLog(const QString &path, const OperationLog &log,
                                    QString &error);
[[nodiscard]] bool loadOperationLog(const QString &path, OperationLog &log,
                                    QString &error);
[[nodiscard]] QString temporaryExportPath();
/** Presents a loaded image as the thing being edited. A log written by the
 *  editor carries the logical size its ops were laid out in; a source taken
 *  on a scaled monitor then opens at that scale rather than at 1:1. */
void describeFileCapture(CaptureData &capture, QImage image,
                         const OperationLog &log);
/** Returns an upright image for captured Wayland buffer contents. */
[[nodiscard]] QImage normalizeWaylandCapture(const QImage &image,
                                             std::uint32_t transform);
/** `customBackdrop` is the image drawn for `BackgroundStyle::Custom`; unused
 *  (and safe to omit) for every other style. */
[[nodiscard]] QImage renderCapture(const CaptureData &capture,
                                   const QRectF &selection,
                                   const QVector<Annotation> &annotations,
                                   BackgroundStyle backgroundStyle,
                                   bool imageShadow = true,
                                   CanvasBoundaryMode boundaryMode =
                                       CanvasBoundaryMode::Framed,
                                   const QImage &customBackdrop = {});
/** Logical size of a flattened render, including backdrop and canvas growth,
 *  using the same pixel scale as renderCapture. */
[[nodiscard]] QSize renderedCaptureLogicalSize(const CaptureData &capture,
                                                const QSize &renderedSize);
/** Lowercase serialization name ("aurora", "custom", ...) for a backdrop
 *  style, used in the operation log and the `[background] default` config
 *  key. */
[[nodiscard]] QString backgroundStyleName(BackgroundStyle style);
/** Parses a backdrop style from its lowercase config/JSON name ("aurora",
 *  "custom", ...). Leaves `style` untouched and returns false when `name`
 *  doesn't match one. */
[[nodiscard]] bool backgroundStyleFromName(const QString &name,
                                           BackgroundStyle &style);
/** Loads the current Wayland clipboard image. */
[[nodiscard]] bool loadClipboardImage(QImage &image, QString &error);
[[nodiscard]] bool copyPngFileToClipboard(const QString &path, QString &error);
[[nodiscard]] bool copyImageToClipboard(const QImage &image, QString &error);
[[nodiscard]] bool quickOutput(const QImage &image, QuickOutputMode mode,
                               QString &error, const QSize &logicalSize = {});
[[nodiscard]] bool copyTextToClipboard(const QString &text, QString &error);
/** Paints one annotation. `arrowDisplayScale` affects only the on-screen tail
 *  legibility floor for Standard/Pointy arrows; exports use the default 1.0. */
void paintAnnotation(QPainter &painter, const Annotation &annotation,
                     qreal arrowDisplayScale = 1.0);
/** Visual extent of an arrow, including its head and stroke. The optional
 *  display scale matches the preview-only Standard/Pointy tail floor; canvas
 *  growth and exports use the natural 1.0 default. */
[[nodiscard]] QRectF arrowVisualBounds(const Annotation &annotation,
                                       qreal displayScale = 1.0);
/** The on-curve midpoint handle used to reshape Curved/Double arrows. */
[[nodiscard]] QPointF arrowCurveHandlePoint(const Annotation &annotation);
/** Shape-aware arrow hit test in annotation-space pixels. */
[[nodiscard]] bool arrowContainsPoint(const Annotation &annotation,
                                      const QPointF &point,
                                      qreal tolerance = 0.0);
[[nodiscard]] QPainterPath spotlightPath(const Annotation &annotation);
/**
 * Pixels of a composed canvas that the spotlights among `annotations`
 * magnify, when all of that canvas (`sourceRect`) maps onto `targetBounds`.
 * Lenses read a fraction of their own area, so a caller that composes the
 * canvas on every paint needs only this much of it. Null when none opens.
 */
[[nodiscard]] QRectF spotlightSampleBounds(const QVector<Annotation> &annotations,
                                           const QRectF &targetBounds,
                                           const QRectF &sourceRect);
/** `sourceRect` is the whole composed canvas in source pixels. `source` holds
 *  all of it, or only the part that starts at `sourceOrigin` within it. */
void paintSpotlights(QPainter &painter, const QImage &source,
                     const QRectF &targetBounds, const QRectF &sourceRect,
                     const QVector<Annotation> &annotations,
                     const QPoint &sourceOrigin = {});
/**
 * Paints the default annotation layer (spotlights, then vectors) in selection
 * space. Spotlights sample `redacted`, which must already include the
 * redaction layer so a loupe cannot magnify source pixels. A null
 * `sourceRect` means `redacted` is the whole canvas; otherwise the two follow
 * paintSpotlights().
 */
void paintDefaultLayer(QPainter &painter, const QImage &redacted,
                       const QRectF &logicalBounds,
                       const QVector<Annotation> &annotations,
                       qreal arrowDisplayScale = 1.0,
                       const QRectF &sourceRect = {},
                       const QPoint &sourceOrigin = {});
// Logical image-frame dimensions, shared by the editor and native-pixel export.
inline constexpr qreal kBackdropMargin = 64.0;
inline constexpr qreal kCaptureImageRadius = 14.0;

/** `customBackdrop` is the image drawn (cover-fit) for
 *  `BackgroundStyle::Custom`; a null image there paints nothing, same as
 *  `BackgroundStyle::None`. */
void paintCaptureBackground(QPainter &painter, const QRectF &bounds,
                            BackgroundStyle backgroundStyle,
                            const QImage &customBackdrop = {});
/** Paints the app's soft ambient-plus-key shadow around `imageRect`. */
void paintCaptureImageShadow(QPainter &painter, const QRectF &imageRect,
                             qreal scaleX = 1.0, qreal scaleY = 1.0);
/**
 * Renders the selection region at `targetSize` for the redaction layer. The
 * result carries no annotations; callers overlay redactions with
 * applyRedactionsScaled and cache it while the selection is unchanged.
 */
[[nodiscard]] QImage renderSelectionBase(const CaptureData &capture,
                                         const QRectF &selection,
                                         const QSize &targetSize);
/**
 * Paints redaction annotations over a display-resolution selection image. The
 * source image MUST be the exact selection region scaled to `targetSize`;
 * annotations are selection-relative, spanning 0..`selection` size.
 */
QImage applyRedactionsScaled(QImage image, const QVector<Annotation> &redactions,
                             const QRectF &selection, const QSizeF &targetSize);
/** Creates or repairs a private directory owned by the current user. */
[[nodiscard]] bool ensurePrivateDirectory(const QString &path);
/** Returns Omasnap's private runtime directory, or empty on failure. */
[[nodiscard]] QString secureRuntimeDirectory();
/**
 * Filename-safe token for a window class: lowercase, `[a-z0-9-]` only,
 * last segment of a reverse-DNS class, at most 24 characters. Empty when
 * nothing usable remains.
 */
[[nodiscard]] QString appFilenameSlug(const QString &appClass);
/**
 * Class of the window covering most of `selection` (preview coordinates),
 * or empty when no window overlaps it.
 */
[[nodiscard]] QString dominantAppClass(const QVector<WindowTarget> &windows,
                                       const QRectF &selection);
/**
 * Moves a finished export into the screenshots directory as
 * `screenshot-<yyyy-MM-dd_HH-mm-ss>[-<appSlug>].png`. The date leads so the
 * folder always sorts chronologically.
 */
[[nodiscard]] QString moveSnapshotToScreenshots(const QString &sourcePath,
                                                QString &error,
                                                const QString &appSlug = {});
/** Saves an already-rendered PNG without consuming the live pin's source. */
[[nodiscard]] QString copySnapshotToScreenshots(const QString &sourcePath,
                                                QString &error);
[[nodiscard]] QString temporarySnapshotPath();
[[nodiscard]] QString pinnedSnapshotPath(int index);
void prunePinnedSnapshots();
/** Private runtime path for handing a live edit to the other editor
 *  presentation (overlay to window or back). */
[[nodiscard]] QString editorHandoffPath();
void pruneEditorHandoffs();
/** Writes a private handoff and an ownership marker for the receiving editor. */
bool saveEditorHandoff(const QImage &source, const QString &path,
                       const OperationLog &log, const QString &token,
                       QString &error);
/** Consumes only a handoff whose ownership marker matches the launch token. */
bool removeEditorHandoff(const QString &path, const QString &token);
/** Window size for a windowed editor: the capture at its logical size
 *  plus the chrome, where `legendHeight` is the measured key guide band,
 *  scaled down to fit inside `available` with a little margin, never
 *  smaller than a usable floor. */
[[nodiscard]] QSize editorWindowSize(const QSize &preview,
                                     const QSize &available,
                                     int legendHeight);
/**
 * Writes `image` into the private runtime directory. `quality` is the Qt PNG
 * quality knob, which maps inversely onto zlib levels: -1 keeps the default
 * level, higher values compress less and encode faster.
 */
/** Saves a pinned snapshot plus a sidecar log recording the logical size,
 *  so editing the pin later reopens at the captured scale. */
[[nodiscard]] bool savePinnedSnapshot(const QImage &image, const QString &path,
                                      const QSize &logicalSize, QString &error,
                                      const QString &recentId = {});
/** Saves and launches a private pin, optionally copying the same PNG first.
 *  Call on a worker: encoding, clipboard verification and process launch block.
 *  Returns the owned snapshot path, or removes it on failure. */
[[nodiscard]] QString launchPinnedCapture(
    const QImage &image, const QSize &logicalSize, bool copy,
    PinLifetime lifetime, QString &error,
    const std::function<bool(const QString &, const QStringList &)> &launcher = {},
    const QString &recentId = {});
[[nodiscard]] bool saveTemporarySnapshot(const QImage &image, QString path,
                                         QString &error, int quality = -1);
[[nodiscard]] QString recognizeText(const QImage &image, QString &error);
/** Builds the omarchy-notification-send argv. With an image, the click command
 *  follows --exec as separate words (program, then file URL) and nothing else
 *  comes after it, since --exec consumes the rest of the line unparsed. */
[[nodiscard]] QStringList
captureNotificationArguments(const QString &message,
                             const QString &imagePath = {});
void sendCaptureNotification(const QString &message,
                             const QString &imagePath = {});
