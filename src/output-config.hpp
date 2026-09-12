/** @fileoverview Screenshot destination and filename pattern from the user's
 *  INI config. Every key is optional; defaults match the built-in behavior. */
#pragma once

#include <QDateTime>
#include <QString>

struct OutputConfig {
  /** Screenshot directory; empty means `~/Pictures/Screenshots`. */
  QString directory;
  /** Filename pattern without extension. Tokens: `{date}` (yyyy-MM-dd),
   *  `{time}` (HH-mm-ss), `{app}` (slug of the app under the selection). */
  QString filename = QStringLiteral("screenshot-{date}_{time}-{app}");
  /** Copy/Save at logical size on scaled monitors; native pixels by default. */
  bool logicalSize = false;
};

/** Reads [output] directory, filename, and logical_size. A missing file or key
 *  leaves the default untouched; `~` in directory expands to $HOME. */
[[nodiscard]] OutputConfig loadOutputConfig(const QString &filePath);

/** Expands `pattern` for `when` and `appSlug` into a safe `.png` filename.
 *  An empty `appSlug` removes `{app}` together with one separator before it
 *  so `screenshot-{date}-{app}` yields `screenshot-<date>.png`, not a
 *  trailing dash. Never returns an empty or path-like name. */
[[nodiscard]] QString formatScreenshotFilename(const QString &pattern,
                                               const QDateTime &when,
                                               const QString &appSlug);

/** ~/.config/omasnap/omasnap.conf (XDG config location). */
[[nodiscard]] QString defaultConfigPath();
