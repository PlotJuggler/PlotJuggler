// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_widgets/pixel_inspector.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "pj_widgets/Colormap.h"  // colorFor — reference oracle for the depth swatch colour

namespace {

PJ::DecodedFrame makeFrame(PJ::PixelFormat format, int width, int height, std::vector<uint8_t> pixels) {
  PJ::DecodedFrame frame;
  frame.format = format;
  frame.width = width;
  frame.height = height;
  frame.pixels = std::make_shared<std::vector<uint8_t>>(std::move(pixels));
  return frame;
}

// Builds a kDepthR32F frame from metric-depth floats (row-major), as DepthPipelineSource emits.
PJ::DecodedFrame makeDepthFrame(int width, int height, std::vector<float> depths_m) {
  PJ::DecodedFrame frame;
  frame.format = PJ::PixelFormat::kDepthR32F;
  frame.width = width;
  frame.height = height;
  auto bytes = std::make_shared<std::vector<uint8_t>>(depths_m.size() * sizeof(float));
  std::memcpy(bytes->data(), depths_m.data(), depths_m.size() * sizeof(float));
  frame.pixels = std::move(bytes);
  frame.depth.active = true;
  return frame;
}

TEST(PixelInspectorMapping, CenterWithoutLetterboxMapsToImageCenter) {
  const auto point =
      PJ::widgetPointToImagePixel(QPointF(400.0, 300.0), QSize(800, 600), QSize(640, 480), 1.0f, 0.0f, 0.0f, 0);

  ASSERT_TRUE(point.has_value());
  EXPECT_EQ(*point, QPoint(320, 240));
}

TEST(PixelInspectorMapping, PillarboxRejectsMouseOutsideImage) {
  EXPECT_FALSE(
      PJ::widgetPointToImagePixel(QPointF(49.0, 50.0), QSize(200, 100), QSize(100, 100), 1.0f, 0.0f, 0.0f, 0)
          .has_value());

  const auto left_edge =
      PJ::widgetPointToImagePixel(QPointF(50.0, 50.0), QSize(200, 100), QSize(100, 100), 1.0f, 0.0f, 0.0f, 0);
  ASSERT_TRUE(left_edge.has_value());
  EXPECT_EQ(*left_edge, QPoint(0, 50));
}

TEST(PixelInspectorMapping, ZoomAndPanUseSameTransformAsViewer) {
  const auto point =
      PJ::widgetPointToImagePixel(QPointF(150.0, 50.0), QSize(200, 200), QSize(100, 100), 2.0f, 0.25f, 0.25f, 0);

  ASSERT_TRUE(point.has_value());
  EXPECT_EQ(*point, QPoint(50, 50));
}

// A 100x50 image in a 100x100 widget, sampled half a pixel inside the displayed
// image's top-left corner: that corner is a different source corner per rotation.
TEST(PixelInspectorMapping, RotationMapsDisplayedCornerToRotatedSourceCorner) {
  const QSize widget(100, 100);
  const QSize image(100, 50);
  // Quarter turns show a 50x100 pillarbox starting at x=25.
  EXPECT_EQ(PJ::widgetPointToImagePixel(QPointF(26.5, 1.5), widget, image, 1.0f, 0.0f, 0.0f, 90), QPoint(1, 48));
  EXPECT_EQ(PJ::widgetPointToImagePixel(QPointF(26.5, 1.5), widget, image, 1.0f, 0.0f, 0.0f, -90), QPoint(98, 1));
  // A half turn keeps the 100x50 letterbox starting at y=25.
  EXPECT_EQ(PJ::widgetPointToImagePixel(QPointF(1.5, 26.5), widget, image, 1.0f, 0.0f, 0.0f, 180), QPoint(98, 48));
  EXPECT_FALSE(PJ::widgetPointToImagePixel(QPointF(24.0, 50.0), widget, image, 1.0f, 0.0f, 0.0f, 90).has_value());
}

// Gestures must keep the grabbed image point under the cursor. A 200x100 image
// in a 200x100 widget is letterboxed at +-90, where a zoom-only drag lags.
TEST(ViewGestures, DragAndWheelKeepImagePointUnderCursorAtEveryRotation) {
  const QSize widget(200, 100);
  const QSize image(200, 100);
  constexpr float kZoom = 2.0f;
  for (const int rotation : {0, 90, -90, 180}) {
    const QPointF fit = PJ::fitScale(widget, 2.0f, rotation);

    const QPointF grab(100.3, 50.3);
    const QPointF release(110.3, 45.3);
    const QPointF dragged = PJ::panAfterDrag(QPointF(), kZoom, release - grab, widget, fit);
    const auto grabbed = PJ::widgetPointToImagePixel(grab, widget, image, kZoom, 0.0f, 0.0f, rotation);
    ASSERT_TRUE(grabbed.has_value()) << rotation;
    EXPECT_EQ(
        PJ::widgetPointToImagePixel(
            release, widget, image, kZoom, static_cast<float>(dragged.x()), static_cast<float>(dragged.y()), rotation),
        grabbed)
        << rotation;

    const QPointF cursor(120.3, 20.3);
    const QPointF zoomed = PJ::panAfterZoom(QPointF(), kZoom, 2.5f, cursor, widget, fit);
    const auto under_cursor = PJ::widgetPointToImagePixel(cursor, widget, image, kZoom, 0.0f, 0.0f, rotation);
    ASSERT_TRUE(under_cursor.has_value()) << rotation;
    EXPECT_EQ(
        PJ::widgetPointToImagePixel(
            cursor, widget, image, 2.5f, static_cast<float>(zoomed.x()), static_cast<float>(zoomed.y()), rotation),
        under_cursor)
        << rotation;
  }
}

TEST(PixelInspectorPixels, ReadsRgbFamilyFormatsAsDisplayRgb) {
  const auto expect_pixel = [](PJ::PixelFormat format, std::vector<uint8_t> data, PJ::InspectorRgb expected) {
    const auto rgb = PJ::pixelRgbAt(makeFrame(format, 1, 1, std::move(data)), 0, 0);
    ASSERT_TRUE(rgb.has_value());
    EXPECT_EQ(*rgb, expected);
  };

  expect_pixel(PJ::PixelFormat::kRGB888, {1, 2, 3}, {1, 2, 3});
  expect_pixel(PJ::PixelFormat::kBGR888, {1, 2, 3}, {3, 2, 1});
  expect_pixel(PJ::PixelFormat::kRGBA8888, {1, 2, 3, 4}, {1, 2, 3});
  expect_pixel(PJ::PixelFormat::kBGRA8888, {1, 2, 3, 4}, {3, 2, 1});
}

TEST(PixelInspectorPixels, ReadsYuv420UsingShaderCompatibleConversion) {
  auto frame = makeFrame(
      PJ::PixelFormat::kYUV420P, 2, 2,
      {
          100, 100, 100, 100,  // Y
          128,                 // U
          128                  // V
      });

  const auto rgb = PJ::pixelRgbAt(frame, 1, 1);
  ASSERT_TRUE(rgb.has_value());
  EXPECT_NEAR(rgb->r, rgb->g, 2);
  EXPECT_NEAR(rgb->g, rgb->b, 2);
  EXPECT_NEAR(rgb->r, 100, 3);
}

TEST(PixelInspectorPixels, CropPadsOutsideImageWithBlack) {
  auto frame = makeFrame(
      PJ::PixelFormat::kRGB888, 2, 2,
      {
          10,
          20,
          30,
          40,
          50,
          60,
          70,
          80,
          90,
          100,
          110,
          120,
      });

  const auto crop = PJ::extractRgbCrop(frame, 0, 0, 3);
  ASSERT_EQ(crop.size(), 27U);
  EXPECT_EQ(crop[0], 0);
  EXPECT_EQ(crop[1], 0);
  EXPECT_EQ(crop[2], 0);

  const size_t center = 4U * 3U;
  EXPECT_EQ(crop[center], 10);
  EXPECT_EQ(crop[center + 1], 20);
  EXPECT_EQ(crop[center + 2], 30);
}

TEST(PixelInspectorDepth, ReadsMetricDepthAtPixel) {
  auto frame = makeDepthFrame(2, 2, {1.0f, 2.0f, 3.0f, 4.5f});

  const auto d = PJ::depthMetersAt(frame, 1, 1);
  ASSERT_TRUE(d.has_value());
  EXPECT_FLOAT_EQ(*d, 4.5f);

  const auto d00 = PJ::depthMetersAt(frame, 0, 0);
  ASSERT_TRUE(d00.has_value());
  EXPECT_FLOAT_EQ(*d00, 1.0f);
}

TEST(PixelInspectorDepth, TreatsNonPositiveAndNonFiniteAsNoData) {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  auto frame = makeDepthFrame(2, 2, {0.0f, -1.0f, nan, inf});

  EXPECT_FALSE(PJ::depthMetersAt(frame, 0, 0).has_value());  // zero == no measurement
  EXPECT_FALSE(PJ::depthMetersAt(frame, 1, 0).has_value());  // negative
  EXPECT_FALSE(PJ::depthMetersAt(frame, 0, 1).has_value());  // NaN
  EXPECT_FALSE(PJ::depthMetersAt(frame, 1, 1).has_value());  // +Inf
}

TEST(PixelInspectorDepth, RejectsOutOfBoundsAndNonDepthFrames) {
  auto frame = makeDepthFrame(2, 2, {1.0f, 2.0f, 3.0f, 4.0f});
  EXPECT_FALSE(PJ::depthMetersAt(frame, -1, 0).has_value());
  EXPECT_FALSE(PJ::depthMetersAt(frame, 2, 0).has_value());
  EXPECT_FALSE(PJ::depthMetersAt(frame, 0, 2).has_value());

  // A non-depth frame has no metric depth to sample, even at a valid pixel.
  auto rgb = makeFrame(PJ::PixelFormat::kRGB888, 1, 1, {10, 20, 30});
  EXPECT_FALSE(PJ::depthMetersAt(rgb, 0, 0).has_value());
}

TEST(PixelInspectorDepth, ColormapColorMatchesShaderLutSource) {
  PJ::DepthColorParams p;
  p.near_m = 0.0f;
  p.far_m = 10.0f;
  p.invert = false;
  p.colormap = static_cast<uint8_t>(PJ::Colormap::kTurbo);

  const auto c = PJ::depthColormapColor(5.0f, p);  // midpoint -> t = 0.5
  const auto ref = PJ::colorFor(PJ::Colormap::kTurbo, 0.5f);
  EXPECT_NEAR(c.r, ref.r * 255.0f, 1.0f);
  EXPECT_NEAR(c.g, ref.g * 255.0f, 1.0f);
  EXPECT_NEAR(c.b, ref.b * 255.0f, 1.0f);
}

TEST(PixelInspectorDepth, ColormapColorClampsBelowNearAndHonoursInvert) {
  PJ::DepthColorParams p;
  p.near_m = 2.0f;
  p.far_m = 4.0f;
  p.colormap = static_cast<uint8_t>(PJ::Colormap::kViridis);

  // 1.0 m is below near -> t clamps to 0.
  p.invert = false;
  EXPECT_NEAR(PJ::depthColormapColor(1.0f, p).r, PJ::colorFor(PJ::Colormap::kViridis, 0.0f).r * 255.0f, 1.0f);
  // Inverting mirrors the same clamped sample to t = 1.
  p.invert = true;
  EXPECT_NEAR(PJ::depthColormapColor(1.0f, p).r, PJ::colorFor(PJ::Colormap::kViridis, 1.0f).r * 255.0f, 1.0f);
}

TEST(PixelInspectorDepth, ColormapColorGuardsDegenerateRange) {
  PJ::DepthColorParams p;
  p.near_m = 3.0f;
  p.far_m = 3.0f;  // near == far must not divide by zero (GPU guards with 1e-6)
  p.colormap = static_cast<uint8_t>(PJ::Colormap::kPlasma);

  const auto c = PJ::depthColormapColor(3.0f, p);  // t collapses to 0
  const auto ref = PJ::colorFor(PJ::Colormap::kPlasma, 0.0f);
  EXPECT_NEAR(c.r, ref.r * 255.0f, 1.0f);
  EXPECT_NEAR(c.g, ref.g * 255.0f, 1.0f);
  EXPECT_NEAR(c.b, ref.b * 255.0f, 1.0f);
}

}  // namespace
