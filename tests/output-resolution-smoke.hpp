/** @fileoverview Declares the `[output] match_logical_size` downscale checks. */
#pragma once

#include <QString>

/** Verifies matchOutputToLogicalSize's opt-in downscale, its interaction
 *  with loaded documents and unscaled monitors, and that the policy is read
 *  from the `[output] match_logical_size` ini key. */
[[nodiscard]] bool runOutputResolutionSmoke(QString &error);
