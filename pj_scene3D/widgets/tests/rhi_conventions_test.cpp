// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/rhi_conventions.h"

#include <gtest/gtest.h>

#include <QVector3D>
#include <vector>

namespace pj::scene3d {
namespace {

// Conventions QRhi reports per backend (Qt 6.11 docs for isYUpInNDC,
// isYUpInFramebuffer, isClipDepthZeroToOne).
constexpr RhiConventions kOpenGl{true, true, false};
constexpr RhiConventions kVulkan{false, false, true};
constexpr RhiConventions kD3dOrMetal{true, false, true};

// Where a corrected-clip-space point lands in render-target texture space.
QVector3D toTexture(const RhiConventions& conventions, QVector3D ndc) {
  return clipToTextureMatrix(conventions).map(ndc);
}

TEST(RhiConventionsTest, OpenGlKeepsTheClassicRemap) {
  EXPECT_FALSE(fullscreenNeedsYFlip(kOpenGl));
  EXPECT_EQ(toTexture(kOpenGl, {-1.0F, -1.0F, -1.0F}), QVector3D(0.0F, 0.0F, 0.0F));
  EXPECT_EQ(toTexture(kOpenGl, {1.0F, 1.0F, 1.0F}), QVector3D(1.0F, 1.0F, 1.0F));
}

// Vulkan's corrected clip space is already y-down, matching row 0 at the top.
TEST(RhiConventionsTest, VulkanNeedsNoFlipAndKeepsZeroToOneDepth) {
  EXPECT_FALSE(fullscreenNeedsYFlip(kVulkan));
  EXPECT_EQ(toTexture(kVulkan, {-1.0F, -1.0F, 0.0F}), QVector3D(0.0F, 0.0F, 0.0F));
  EXPECT_EQ(toTexture(kVulkan, {1.0F, 1.0F, 1.0F}), QVector3D(1.0F, 1.0F, 1.0F));
}

// D3D and Metal: NDC y points up but texture row 0 is the top row.
TEST(RhiConventionsTest, D3dAndMetalFlipV) {
  EXPECT_TRUE(fullscreenNeedsYFlip(kD3dOrMetal));
  EXPECT_EQ(toTexture(kD3dOrMetal, {-1.0F, 1.0F, 0.0F}), QVector3D(0.0F, 0.0F, 0.0F));
  EXPECT_EQ(toTexture(kD3dOrMetal, {1.0F, -1.0F, 1.0F}), QVector3D(1.0F, 1.0F, 1.0F));
}

// The inverse is what SSAO/EDL use to rebuild clip space from (uv, depth).
TEST(RhiConventionsTest, InverseRoundTrips) {
  for (const RhiConventions& conventions : {kOpenGl, kVulkan, kD3dOrMetal}) {
    bool invertible = false;
    const QMatrix4x4 inverse = clipToTextureMatrix(conventions).inverted(&invertible);
    ASSERT_TRUE(invertible);
    const QVector3D point(0.25F, -0.5F, 0.75F);
    const QVector3D round_trip = inverse.map(clipToTextureMatrix(conventions).map(point));
    EXPECT_NEAR(round_trip.x(), point.x(), 1e-6F);
    EXPECT_NEAR(round_trip.y(), point.y(), 1e-6F);
    EXPECT_NEAR(round_trip.z(), point.z(), 1e-6F);
  }
}

// QRhi::clipSpaceCorrMatrix() per backend: identity on OpenGL; depth to [0, 1]
// elsewhere, plus a Y flip on Vulkan.
QMatrix4x4 clipCorrection(const RhiConventions& conventions) {
  QMatrix4x4 correction;
  if (conventions.clip_depth_zero_to_one) {
    correction = QMatrix4x4(1, 0, 0, 0, 0, conventions.y_up_in_ndc ? 1 : -1, 0, 0, 0, 0, 0.5F, 0.5F, 0, 0, 0, 1);
  }
  return correction;
}

std::vector<QMatrix4x4> projections() {
  QMatrix4x4 perspective;
  perspective.perspective(60.0F, 1.5F, 0.1F, 100.0F);
  QMatrix4x4 orthographic;
  orthographic.ortho(-4.0F, 4.0F, -3.0F, 3.0F, 0.1F, 100.0F);
  return {perspective, orthographic};
}

// The full chain the renderer uploads: view -> texture -> view, through w.
TEST(RhiConventionsTest, TextureProjectionRoundTripsThroughW) {
  for (const RhiConventions& conventions : {kOpenGl, kVulkan, kD3dOrMetal}) {
    for (const QMatrix4x4& projection : projections()) {
      const QMatrix4x4 texture_projection = clipToTextureMatrix(conventions) * clipCorrection(conventions) * projection;
      bool invertible = false;
      const QMatrix4x4 inverse = texture_projection.inverted(&invertible);
      ASSERT_TRUE(invertible);
      const QVector3D view_point(0.7F, -0.4F, -5.0F);
      const QVector3D texture_point = texture_projection.map(view_point);  // divides by w
      EXPECT_GE(texture_point.z(), 0.0F);
      EXPECT_LE(texture_point.z(), 1.0F);
      const QVector3D round_trip = inverse.map(texture_point);
      EXPECT_NEAR(round_trip.x(), view_point.x(), 1e-4F);
      EXPECT_NEAR(round_trip.y(), view_point.y(), 1e-4F);
      EXPECT_NEAR(round_trip.z(), view_point.z(), 1e-3F);
    }
  }
}

// A point above the view axis lands in the upper half of the image: texture row
// 0 is the bottom on OpenGL and the top everywhere else.
TEST(RhiConventionsTest, UpInViewMatchesTheBackendsRowOrder) {
  for (const RhiConventions& conventions : {kOpenGl, kVulkan, kD3dOrMetal}) {
    for (const QMatrix4x4& projection : projections()) {
      const QMatrix4x4 texture_projection = clipToTextureMatrix(conventions) * clipCorrection(conventions) * projection;
      const float v = texture_projection.map(QVector3D(0.0F, 1.0F, -5.0F)).y();
      if (conventions.y_up_in_framebuffer) {
        EXPECT_GT(v, 0.5F);
      } else {
        EXPECT_LT(v, 0.5F);
      }
      // SSAO's cross(d/du, d/dv) normal: +v is up on OpenGL, down elsewhere.
      EXPECT_EQ(textureVSign(conventions) > 0.0F, v > 0.5F);
    }
  }
}

}  // namespace
}  // namespace pj::scene3d
