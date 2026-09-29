// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QColor>
#include <QFont>
#include <QImage>
#include <QStringList>

namespace pj::scene3d {

// Rasterize a translucent, rounded HUD panel holding `lines` (left-aligned,
// stacked top to bottom) into a freshly-allocated QImage and return it. The
// image is allocated at `device_pixel_ratio` physical resolution and tagged
// with that ratio, so a caller can `painter.drawImage(logical_top_left, img)`
// and get a crisp result at any DPR; the panel's logical box is the image's
// device-independent size (`img.deviceIndependentSize()`).
//
// `padding` is the logical inset between the panel edge and the text on every
// side; `panel_alpha` is retained for the legacy call sites while the panel fill
// comes from the framework HUD overlay token; lines are drawn in `text_color`.
// Returns a NULL QImage when there is nothing to draw
// (no lines, or every line empty), so the caller can skip the blit.
//
// Why this exists — the perf HUD and the TF hover label are CPU-rasterized
// here and drawn by the view as a plain textured quad, never painted with
// `QPainter` glyphs on the view: the view's QRhi is *recreated* whenever ADS
// reparents the dock — dock/float/split, and notably **layout restore** — and
// a GPU glyph cache tied to it would go stale or keep the wrong device-pixel
// ratio. A CPU-rasterized image is immune to the recreation and to later DPR
// changes.
QImage renderHudPanel(
    const QStringList& lines, const QFont& font, qreal device_pixel_ratio, int padding, int panel_alpha,
    const QColor& text_color);

}  // namespace pj::scene3d
