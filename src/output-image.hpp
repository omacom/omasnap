/** @fileoverview Final Copy/Save image sizing, after rendering and redaction. */
#pragma once

#include <QImage>

/** Reads output configuration and optionally downsizes a flattened image.
 *  Call on the output worker, never while painting or persisting source pixels. */
[[nodiscard]] QImage prepareOutputImage(const QImage &image, qreal scale);
