#pragma once

#include <QString>

inline constexpr auto kPinSmokeEditorChild = "OMASNAP_PIN_SMOKE_EDITOR_CHILD";
[[nodiscard]] bool runPinInteractionSmoke(QString &error);
