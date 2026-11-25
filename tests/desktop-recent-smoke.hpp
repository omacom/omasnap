/** @fileoverview Declares the desktop recent-files smoke test. */
#pragma once

#include <QString>

/** Set to a file path, re-executes the suite as one concurrent writer. */
inline constexpr char kDesktopRecentChild[] = "OMASNAP_TEST_DESKTOP_RECENT_CHILD";

[[nodiscard]] bool runDesktopRecentSmoke(QString &error);
/** The child: adds the path in kDesktopRecentChild and exits 0 on success. */
[[nodiscard]] int runDesktopRecentChild(int argc, char **argv);
