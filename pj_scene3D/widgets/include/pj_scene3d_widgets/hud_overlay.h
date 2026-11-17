// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QColor>
#include <QFont>
#include <QImage>
#include <QStringList>

namespace pj::scene3d {

// Rasterizes a translucent, rounded HUD panel holding `lines` (left-aligned, stacked top to bottom) into a new QImage
// at `device_pixel_ratio` physical resolution, tagged with that ratio: `painter.drawImage(logical_top_left, img)` is
// crisp at any DPR, and the panel's logical box is `img.deviceIndependentSize()`.
//
// `padding` is the logical inset between panel edge and text on every side; `panel_alpha` is unused (the panel fill
// comes from the framework HUD overlay token); lines use `text_color`. Returns a NULL QImage when there is nothing to
// draw (no lines, or every line empty), so the caller can skip the blit.
//
// Why CPU-rasterized: the perf HUD and the TF hover label are drawn by the view as a plain textured quad, never with
// QPainter glyphs on the view, because the view's QRhi is *recreated* whenever ADS reparents the dock (dock/float/
// split, layout restore) and a GPU glyph cache tied to it would go stale or keep the wrong device-pixel ratio. A
// CPU-rasterized image is immune to the recreation and to later DPR changes.
QImage renderHudPanel(
    const QStringList& lines, const QFont& font, qreal device_pixel_ratio, int padding, int panel_alpha,
    const QColor& text_color);

}  // namespace pj::scene3d
