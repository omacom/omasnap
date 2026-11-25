/** @fileoverview Registers saved screenshots in the desktop's recent files. */
#pragma once

#include <QString>

/**
 * The freedesktop recently-used list, `$XDG_DATA_HOME/recently-used.xbel`.
 * GTK's RecentManager, Nautilus's Recent view, and GTK file pickers read it.
 */
[[nodiscard]] QString desktopRecentFilesPath();

/**
 * Records `path` as a recently used file, replacing any older entry for it.
 * Blocks on disk I/O, so call it from a worker. Leaves the list untouched and
 * fails when the existing file is not a well-formed XBEL document.
 */
[[nodiscard]] bool addToDesktopRecentFiles(const QString &path,
                                           QString &error);
