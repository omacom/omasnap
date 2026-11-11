/** @fileoverview Declares the pre-QApplication Wayland display smoke check. */
#pragma once

#include <QString>

/** Checks the display probe against unset, missing, stale, and live sockets,
 *  and that omasnap exits 1 with a message instead of aborting without one. */
[[nodiscard]] bool runWaylandDisplaySmoke(QString &error);
