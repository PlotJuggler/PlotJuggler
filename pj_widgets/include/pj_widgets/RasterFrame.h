// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QImage>

namespace PJ {

// Interprets `data` (`size_bytes`) as a pj_raster shared segment and returns a QImage view over the active buffer (no
// copy: the caller keeps `data` alive and locked while using the image, or calls .copy()). A null QImage if the header
// is invalid or the segment is too small.
QImage imageForActiveFrame(const unsigned char* data, int size_bytes);

}  // namespace PJ
