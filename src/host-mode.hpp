/** @fileoverview Host mode: another application (e.g. XerahS) drives omasnap
 *  as its capture front end. The host names an output PNG and receives one
 *  JSON result; omasnap performs no clipboard, save, notification, preview
 *  or pin side effects of its own. Off unless `--host` is passed. */
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>

#include <optional>

class QImage;

/** Host-mode protocol version reported by `--host-capabilities`. */
inline constexpr int kHostModeVersion = 1;

/** Exit codes shared with hosts. */
inline constexpr int kHostExitOk = 0;
inline constexpr int kHostExitFailure = 1;
inline constexpr int kHostExitUsage = 2;
inline constexpr int kHostExitCancelled = 3;

struct HostSession {
  /** Host name from `--host` (informational). */
  QString name;
  /** Where the flattened PNG goes. */
  QString outputPath;
  /** Result JSON path, or "-" for stdout. */
  QString resultPath = QStringLiteral("-");
  /** Whether finished captures still enter the recents shelf. */
  bool recents = true;
  /** omasnap version reported in results. */
  QString version;
  /** Requested target ("smart", "region", ...) reported in results. */
  QString target;
};

struct HostWindowInfo {
  QString appClass;
  QString title;
  QString address;
};

struct HostResult {
  /** "ok", "cancelled" or "error". */
  QString status;
  /** "smart", "region", "window", "fullscreen", "scroll" or "file". */
  QString target;
  QString path;
  QSize pixelSize;
  QSize logicalSize;
  double scale = 1.0;
  QString monitor;
  QRect region;
  std::optional<HostWindowInfo> window;
  bool annotated = false;
  QString error;
};

/** Installs the process-wide host session. Call once, on the main thread,
 *  before any capture work starts. */
void setHostSession(const HostSession &session);
/** Clears the host session (tests). */
void clearHostSession();
/** The active host session, or null in standalone mode. Safe from workers
 *  once installed: it never changes after startup. */
[[nodiscard]] const HostSession *hostSession();
[[nodiscard]] inline bool hostModeActive() { return hostSession() != nullptr; }

/** Parses `x,y,w,h` (integers, positive size) in global logical pixels. */
[[nodiscard]] bool parseHostRegion(const QString &text, QRect &region);

/** Serializes a result as the schemaVersion 1 JSON object. */
[[nodiscard]] QJsonObject hostResultJson(const HostResult &result,
                                         const QString &version);

/** Writes the result once per process (later calls are ignored) to the
 *  session's result path. Returns false when writing failed. */
bool reportHostResult(const HostResult &result);
/** True once a result has been reported. */
[[nodiscard]] bool hostResultReported();
/** Exit code for the reported status, or `fallback` when none was sent. */
[[nodiscard]] int hostExitCodeForReported(int fallback);

/** Resolves the process exit for a host run that ended with `exitCode`
 *  without reporting: 0 means the overlay was dismissed (cancelled), 2 a
 *  usage error, anything else a failure. Reports and returns the host code. */
int finishHostRun(int exitCode, const QString &target,
                  const QString &error = {});

/** Saves `image` (tagged with `logicalSize`) to the host output path.
 *  Blocks on PNG encoding: call on a worker. */
[[nodiscard]] bool writeHostOutput(QImage image, const QSize &logicalSize,
                                   QString &error);

struct HostCapabilities {
  bool hyprland = false;
  bool wayland = false;
  bool extImageCopyCapture = false;
  bool layerShell = false;
};
/** Checks the session without mapping a surface: Hyprland instance and
 *  hyprctl, then the compositor's advertised globals. */
[[nodiscard]] HostCapabilities probeHostCapabilities();
[[nodiscard]] QJsonObject hostCapabilitiesJson(const HostCapabilities &caps,
                                               const QString &version);
/** Prints the capabilities JSON and returns the process exit code. */
int runHostCapabilities(const QString &version);

/** Command a hosted pin runs to upload its PNG, from
 *  `OMASNAP_HOST_UPLOAD_COMMAND`, split into argv. Empty when unset. */
[[nodiscard]] QStringList hostUploadCommand();
/** The URL in one line of upload-command output: a bare http(s) URL, or a
 *  JSON object with a "url" string (omaxerahs upload). Empty otherwise. */
[[nodiscard]] QString hostUploadUrl(const QString &line);
/** Splits a command line into argv with shell-like quoting (no expansion). */
[[nodiscard]] QStringList splitHostCommand(const QString &command);
