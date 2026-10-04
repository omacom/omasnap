/** @fileoverview Host-mode session, result reporting and capability probe. */
#include "host-mode.hpp"

#include "capture.hpp"
#include "png.hpp"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>

#include <wayland-client.h>

namespace {
std::optional<HostSession> &sessionStorage() {
  static std::optional<HostSession> session;
  return session;
}

std::mutex &reportMutex() {
  static std::mutex mutex;
  return mutex;
}

std::atomic<bool> reported{false};
QString reportedStatus;

int exitCodeForStatus(const QString &status) {
  if (status == QStringLiteral("ok"))
    return kHostExitOk;
  if (status == QStringLiteral("cancelled"))
    return kHostExitCancelled;
  if (status == QStringLiteral("usage"))
    return kHostExitUsage;
  return kHostExitFailure;
}

QJsonObject rectJson(const QRect &rect) {
  return {{QStringLiteral("x"), rect.x()},
          {QStringLiteral("y"), rect.y()},
          {QStringLiteral("width"), rect.width()},
          {QStringLiteral("height"), rect.height()}};
}

struct RegistryScan {
  bool extImageCopyCapture = false;
  bool layerShell = false;
};

void registryGlobal(void *data, wl_registry *, uint32_t, const char *interface,
                    uint32_t) {
  auto *scan = static_cast<RegistryScan *>(data);
  if (std::strcmp(interface, "ext_image_copy_capture_manager_v1") == 0)
    scan->extImageCopyCapture = true;
  else if (std::strcmp(interface, "zwlr_layer_shell_v1") == 0)
    scan->layerShell = true;
}

void registryGlobalRemove(void *, wl_registry *, uint32_t) {}

const wl_registry_listener kRegistryListener = {registryGlobal,
                                                registryGlobalRemove};
} // namespace

void setHostSession(const HostSession &session) {
  sessionStorage() = session;
  reported = false;
  reportedStatus.clear();
}

void clearHostSession() {
  sessionStorage().reset();
  reported = false;
  reportedStatus.clear();
}

const HostSession *hostSession() {
  const auto &session = sessionStorage();
  return session ? &*session : nullptr;
}

bool parseHostRegion(const QString &text, QRect &region) {
  const QStringList parts = text.split(QLatin1Char(','));
  if (parts.size() != 4)
    return false;
  int values[4] = {};
  for (int index = 0; index < 4; ++index) {
    bool ok = false;
    values[index] = parts.at(index).trimmed().toInt(&ok);
    if (!ok)
      return false;
  }
  if (values[2] <= 0 || values[3] <= 0)
    return false;
  region = QRect(values[0], values[1], values[2], values[3]);
  return true;
}

QJsonObject hostResultJson(const HostResult &result, const QString &version) {
  QJsonObject object{
      {QStringLiteral("schemaVersion"), 1},
      {QStringLiteral("status"), result.status},
      {QStringLiteral("target"), result.target},
      {QStringLiteral("omasnapVersion"), version},
  };
  if (result.status == QStringLiteral("ok")) {
    object.insert(QStringLiteral("path"), result.path);
    object.insert(QStringLiteral("pixelWidth"), result.pixelSize.width());
    object.insert(QStringLiteral("pixelHeight"), result.pixelSize.height());
    object.insert(QStringLiteral("logicalWidth"), result.logicalSize.width());
    object.insert(QStringLiteral("logicalHeight"), result.logicalSize.height());
    object.insert(QStringLiteral("scale"), result.scale);
    object.insert(QStringLiteral("monitor"), result.monitor);
    object.insert(QStringLiteral("region"), result.region.isNull()
                                                ? QJsonValue()
                                                : QJsonValue(rectJson(result.region)));
    if (result.window) {
      object.insert(QStringLiteral("window"),
                    QJsonObject{{QStringLiteral("class"), result.window->appClass},
                                {QStringLiteral("title"), result.window->title},
                                {QStringLiteral("address"), result.window->address}});
    } else {
      object.insert(QStringLiteral("window"), QJsonValue());
    }
    object.insert(QStringLiteral("annotated"), result.annotated);
    object.insert(QStringLiteral("documentPath"), QJsonValue());
  } else if (!result.error.isEmpty()) {
    object.insert(QStringLiteral("error"), result.error);
  }
  return object;
}

bool reportHostResult(const HostResult &result) {
  const HostSession *session = hostSession();
  if (!session)
    return false;
  std::lock_guard lock(reportMutex());
  if (reported.exchange(true))
    return true;
  reportedStatus = result.status;
  const QByteArray json =
      QJsonDocument(hostResultJson(result, session->version))
          .toJson(QJsonDocument::Compact) +
      '\n';
  if (session->resultPath.isEmpty() || session->resultPath == QStringLiteral("-")) {
    const bool ok = std::fwrite(json.constData(), 1, json.size(), stdout) ==
                    static_cast<size_t>(json.size());
    std::fflush(stdout);
    return ok;
  }
  QSaveFile file(session->resultPath);
  if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size() ||
      !file.commit()) {
    qCritical().noquote() << QStringLiteral("Could not write host result %1")
                                 .arg(session->resultPath);
    return false;
  }
  return true;
}

bool hostResultReported() { return reported.load(); }

int hostExitCodeForReported(int fallback) {
  if (!reported.load())
    return fallback;
  std::lock_guard lock(reportMutex());
  return exitCodeForStatus(reportedStatus);
}

int finishHostRun(int exitCode, const QString &target, const QString &error) {
  if (!hostModeActive())
    return exitCode;
  if (hostResultReported())
    return hostExitCodeForReported(exitCode);
  HostResult result;
  result.target = target;
  if (exitCode == 0 || exitCode == kHostExitCancelled) {
    result.status = QStringLiteral("cancelled");
  } else if (exitCode == kHostExitUsage) {
    result.status = QStringLiteral("usage");
    result.error = error.isEmpty() ? QStringLiteral("Invalid arguments") : error;
  } else {
    result.status = QStringLiteral("error");
    result.error = error.isEmpty() ? QStringLiteral("Capture failed") : error;
  }
  reportHostResult(result);
  return exitCodeForStatus(result.status);
}

bool writeHostOutput(QImage image, const QSize &logicalSize, QString &error) {
  const HostSession *session = hostSession();
  if (!session || session->outputPath.isEmpty()) {
    error = QStringLiteral("No host output path");
    return false;
  }
  if (image.isNull()) {
    error = QStringLiteral("Could not prepare screenshot snapshot");
    return false;
  }
  const QString directory = QFileInfo(session->outputPath).absolutePath();
  if (!QDir().mkpath(directory)) {
    error = QStringLiteral("Could not create %1").arg(directory);
    return false;
  }
  setPngLogicalSize(image, logicalSize.isEmpty() ? image.size() : logicalSize);
  return savePngFile(image, session->outputPath, error);
}

HostCapabilities probeHostCapabilities() {
  HostCapabilities caps;
  if (!qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")) {
    QProcess hyprctl;
    hyprctl.start(QStringLiteral("hyprctl"),
                  {QStringLiteral("-j"), QStringLiteral("version")});
    if (hyprctl.waitForFinished(1000)) {
      caps.hyprland = hyprctl.exitStatus() == QProcess::NormalExit &&
                      hyprctl.exitCode() == 0;
    } else {
      hyprctl.kill();
      hyprctl.waitForFinished(500);
    }
  }
  const QString waylandDisplay = qEnvironmentVariable("WAYLAND_DISPLAY");
  if (qEnvironmentVariableIsEmpty("XDG_RUNTIME_DIR") &&
      !waylandDisplay.startsWith(QLatin1Char('/')))
    return caps; // libwayland would only complain on stderr.
  wl_display *display = wl_display_connect(nullptr);
  if (!display)
    return caps;
  caps.wayland = true;
  RegistryScan scan;
  wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &kRegistryListener, &scan);
  wl_display_roundtrip(display);
  wl_registry_destroy(registry);
  wl_display_disconnect(display);
  caps.extImageCopyCapture = scan.extImageCopyCapture;
  caps.layerShell = scan.layerShell;
  return caps;
}

QJsonObject hostCapabilitiesJson(const HostCapabilities &caps,
                                 const QString &version) {
  const bool ok = caps.hyprland && caps.wayland && caps.extImageCopyCapture &&
                  caps.layerShell;
  return {
      {QStringLiteral("schemaVersion"), 1},
      {QStringLiteral("ok"), ok},
      {QStringLiteral("version"), version},
      {QStringLiteral("hyprland"), caps.hyprland},
      {QStringLiteral("wayland"), caps.wayland},
      {QStringLiteral("extImageCopyCapture"), caps.extImageCopyCapture},
      {QStringLiteral("layerShell"), caps.layerShell},
      {QStringLiteral("hostMode"), kHostModeVersion},
      {QStringLiteral("targets"),
       QJsonArray{QStringLiteral("smart"), QStringLiteral("region"),
                  QStringLiteral("windows"), QStringLiteral("fullscreen"),
                  QStringLiteral("scroll")}},
      {QStringLiteral("editor"),
       QJsonArray{QStringLiteral("overlay"), QStringLiteral("window")}},
      {QStringLiteral("pin"), true},
  };
}

int runHostCapabilities(const QString &version) {
  const QJsonObject json = hostCapabilitiesJson(probeHostCapabilities(), version);
  const QByteArray bytes = QJsonDocument(json).toJson(QJsonDocument::Compact) + '\n';
  std::fwrite(bytes.constData(), 1, bytes.size(), stdout);
  std::fflush(stdout);
  return json.value(QStringLiteral("ok")).toBool() ? 0 : 1;
}

QStringList splitHostCommand(const QString &command) {
  QStringList arguments;
  QString current;
  bool inToken = false;
  QChar quote;
  for (qsizetype index = 0; index < command.size(); ++index) {
    const QChar ch = command.at(index);
    if (!quote.isNull()) {
      if (ch == quote) {
        quote = QChar();
      } else if (ch == QLatin1Char('\\') && quote == QLatin1Char('"') &&
                 index + 1 < command.size()) {
        current += command.at(++index);
      } else {
        current += ch;
      }
      continue;
    }
    if (ch.isSpace()) {
      if (inToken) {
        arguments.push_back(current);
        current.clear();
        inToken = false;
      }
      continue;
    }
    inToken = true;
    if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
      quote = ch;
    } else if (ch == QLatin1Char('\\') && index + 1 < command.size()) {
      current += command.at(++index);
    } else {
      current += ch;
    }
  }
  if (inToken)
    arguments.push_back(current);
  return arguments;
}

QStringList hostUploadCommand() {
  return splitHostCommand(
      qEnvironmentVariable("OMASNAP_HOST_UPLOAD_COMMAND").trimmed());
}

QString hostUploadUrl(const QString &line) {
  const QString trimmed = line.trimmed();
  QString url = trimmed;
  if (trimmed.startsWith(QLatin1Char('{'))) {
    const QJsonDocument document = QJsonDocument::fromJson(trimmed.toUtf8());
    url = document.isObject()
              ? document.object().value(QStringLiteral("url")).toString().trimmed()
              : QString();
  }
  if (url.startsWith(QStringLiteral("http://")) ||
      url.startsWith(QStringLiteral("https://")))
    return url;
  return {};
}
