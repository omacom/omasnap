/** @fileoverview A cancellable wait before any capture surface is mapped. */
#pragma once

#include <QString>

/** Accepts whole seconds from 0 through 3600. */
[[nodiscard]] bool parseCaptureDelay(const QString &value, int &seconds);

/** Runs the application's event loop before capture; false means it was quit
 *  before the timer expired. The caller retains the single-instance lock.
 *  Call only at startup, before mapping windows or entering the normal loop. */
[[nodiscard]] bool runCaptureDelay(int seconds);
