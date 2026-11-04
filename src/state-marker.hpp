/** @fileoverview Remembered overlay choices, each an empty file in omasnap's
 *  state directory: present means the choice is set. */
#pragma once

#include <QString>

/// Whether the marker `name` exists. A cheap stat, fine on the UI thread.
[[nodiscard]] bool stateMarkerSet(const QString &name);

/// Creates or removes the marker on a worker. Each write applies the latest
/// choice for its marker, so quick toggles cannot land out of order.
void setStateMarker(const QString &name, bool set);

/// Blocks until every choice is on disk, so the next capture reads it. Call
/// before releasing the instance lock.
void finishSavingStateMarkers();
