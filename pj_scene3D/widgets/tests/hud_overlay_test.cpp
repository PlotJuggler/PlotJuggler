// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Unit tests for renderHudPanel — the CPU-rasterized HUD panel used by the perf
// HUD and the TF hover label. These exist because the previous GL-glyph-cache
// path corrupted text (doubled/garbled glyphs) after a Scene3D dock's GL context
// was recreated on layout restore; that corruption itself is only reproducible
// in the live GL app, but moving text rendering to this pure, CPU-side function
// both fixes the bug (no glyph atlas involved) and makes the rendering testable.
// The properties locked in here — a correctly sized image, DPR-independent
// logical layout, and actually-rasterized glyphs — are exactly what the GL path
// failed to guarantee across a context recreation.

#include "pj_scene3d_widgets/hud_overlay.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QImage>
#include <QStringList>
using namespace Qt::StringLiterals;

namespace {

using pj::scene3d::renderHudPanel;

constexpr int kPad = 6;
constexpr int kAlpha = 170;
const QColor kTextColor(235, 235, 235);

QFont hudFont() {
  QFont font;
  font.setPointSizeF(9.5);
  return font;
}

}  // namespace

// A single non-empty line yields a non-null image with room for text + padding.
TEST(HudOverlay, SingleLineProducesSizedImage) {
  const QImage img = renderHudPanel({u"base_link"_s}, hudFont(), 1.0, kPad, kAlpha, kTextColor);
  ASSERT_FALSE(img.isNull());
  EXPECT_GT(img.width(), 2 * kPad);
  EXPECT_GT(img.height(), 2 * kPad);
}

// The logical (device-independent) layout must be identical regardless of the
// device-pixel ratio: only the backing resolution scales. The violation of this
// property is precisely what produced doubled/overlapping glyphs on the GL path.
TEST(HudOverlay, LayoutIsDevicePixelRatioIndependent) {
  const QStringList lines{u"soccer_left_leg9_link"_s};
  const QImage at1 = renderHudPanel(lines, hudFont(), 1.0, kPad, kAlpha, kTextColor);
  const QImage at2 = renderHudPanel(lines, hudFont(), 2.0, kPad, kAlpha, kTextColor);

  ASSERT_FALSE(at1.isNull());
  ASSERT_FALSE(at2.isNull());
  EXPECT_DOUBLE_EQ(at1.devicePixelRatio(), 1.0);
  EXPECT_DOUBLE_EQ(at2.devicePixelRatio(), 2.0);
  // Same logical box; the 2x image has exactly twice the pixels on each axis.
  EXPECT_EQ(at2.width(), at1.width() * 2);
  EXPECT_EQ(at2.height(), at1.height() * 2);
}

// The text is actually rasterized: at least one light, opaque pixel (a glyph
// stroke) stands out against the near-black panel. Guards the "panel present but
// text blank" failure mode.
TEST(HudOverlay, RendersTextPixels) {
  const QImage img = renderHudPanel({u"odom"_s}, hudFont(), 1.0, kPad, kAlpha, kTextColor);
  ASSERT_FALSE(img.isNull());

  bool found_text = false;
  for (int y = 0; y < img.height() && !found_text; ++y) {
    for (int x = 0; x < img.width(); ++x) {
      const QColor pixel = img.pixelColor(x, y);
      // Panel fill is black (r==0); a glyph contributes a clearly lighter pixel.
      if (pixel.alpha() > 120 && pixel.red() > 120) {
        found_text = true;
        break;
      }
    }
  }
  EXPECT_TRUE(found_text);
}

// More lines => a taller panel (height scales with the line count).
TEST(HudOverlay, MultiLineHeightGrowsWithLineCount) {
  const QFont font = hudFont();
  const QImage one = renderHudPanel({u"GPU  1.00 ms"_s}, font, 1.0, kPad, kAlpha, kTextColor);
  const QImage three =
      renderHudPanel({u"GPU  1.00 ms"_s, u"CPU  2.00 ms"_s, u"MSAA 4x"_s}, font, 1.0, kPad, kAlpha, kTextColor);
  ASSERT_FALSE(one.isNull());
  ASSERT_FALSE(three.isNull());
  EXPECT_GT(three.height(), one.height());
}

// Nothing to draw => a null image so the caller skips the blit entirely.
TEST(HudOverlay, EmptyInputYieldsNullImage) {
  EXPECT_TRUE(renderHudPanel({}, hudFont(), 1.0, kPad, kAlpha, kTextColor).isNull());
  EXPECT_TRUE(renderHudPanel({QString(), QString()}, hudFont(), 1.0, kPad, kAlpha, kTextColor).isNull());
}
