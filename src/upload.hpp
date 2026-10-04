/** @fileoverview Sends a finished capture to the upload host(s) named in
 *  omasnap.conf, copies the link, and records it. Everything here blocks and
 *  belongs on a worker thread (see docs/threading.md), except the two
 *  command-line entry points, which run before any overlay exists. */
#pragma once

#include <QString>
#include <QVariantMap>

#include <atomic>
#include <functional>

/** Where uploads read their configuration and keep their state. */
struct UploadPaths {
  /** omasnap.conf, holding [upload] and [host.<name>]. */
  QString configPath;
  /** Where .sxcu files become hosts named after the file. */
  QString uploadersDir;
  /** Fallback secret file when there's no keyring. */
  QString secretsDir;
  /** One JSON line per upload, deletion links included. */
  QString historyPath;

  /** The XDG locations: ~/.config/omasnap, ~/.local/share/omasnap and
   *  ~/.local/state/omasnap. */
  [[nodiscard]] static UploadPaths defaults();
};

/** One configured destination, its settings resolved and ready to use. */
struct UploadHost {
  QString name;
  QString provider;
  QVariantMap settings;
};

/** The hosts [upload] hosts= lists, in order, or the reason none can be
 *  used. Settings come from the host's [host.<name>] keys, then a
 *  `<key>_command` whose output is the value, then the secret store (where
 *  --sign-in keeps what a browser sign-in returns). */
[[nodiscard]] QList<UploadHost> loadUploadHosts(const UploadPaths &paths,
                                                QString &error);

/** True when [upload] names at least one host. */
[[nodiscard]] bool uploadConfigured(const QString &configPath);

struct UploadResult {
  QString url;
  QString thumbnailUrl;
  QString deletionUrl;
  /** The host that took the upload. */
  QString host;
  /** Empty on success; otherwise why every host failed. */
  QString error;
};

/** Uploads `path` to the first configured host that takes it, running its
 *  own event loop. `progress` gets 0-100; setting `cancel` stops it. A
 *  successful upload is appended to the history. */
[[nodiscard]] UploadResult
uploadFile(const QString &path, const UploadPaths &paths,
           const std::function<void(int)> &progress = {},
           const std::atomic_bool *cancel = nullptr);

/** `omasnap --upload FILE`: uploads, copies the link and prints it. */
[[nodiscard]] int runUploadCommand(const QString &path,
                                   const UploadPaths &paths);

/** `omasnap --sign-in HOST`: runs the host's browser sign-in and keeps what
 *  it returns in the secret store. */
[[nodiscard]] int runUploadSignIn(const QString &hostName,
                                  const UploadPaths &paths);
