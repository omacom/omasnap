#pragma once

#include <QString>

[[nodiscard]] bool runPngSmoke(QString &error);
/** Checks that a logical size tag or log claiming an impossible scale is
 *  ignored, while real display scales and long captures are kept. */
[[nodiscard]] bool runPngLogicalSizeBoundsSmoke(QString &error);
