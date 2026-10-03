/** @fileoverview Declares clipboard image loading smoke checks. */
#pragma once

#include <QString>

/** Runs clipboard image loading checks with a fake wl-paste command. */
[[nodiscard]] bool runClipboardSmoke(QString &error);

/** Checks that an oversized clipboard image is refused without reading it
 *  all into memory first. */
[[nodiscard]] bool runClipboardSizeLimitSmoke(QString &error);
