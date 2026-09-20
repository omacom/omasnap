/** @fileoverview Declares pin file lifecycle smoke checks. */
#pragma once

class QString;

[[nodiscard]] bool runPinLifecycleSmoke(QString &error);
[[nodiscard]] bool runPinExpirySmoke(QString &error);
