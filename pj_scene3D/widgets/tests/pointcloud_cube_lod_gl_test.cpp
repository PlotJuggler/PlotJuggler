// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The cube LOD replaces a whole cloud's hexagon fans with point sprites once no cube in
// it reaches the threshold. That is only acceptable if it is INVISIBLE, and "invisible"
// is a measurable claim, so this file measures it rather than asserting it:
//
//  1. Far cloud — total emitted light must match the fan's. Sprites are matched to the
//     cube's projected AREA (not its side) and shaded with the area-weighted mean of the
//     three visible faces' Lambert terms, times the mean darkening of the edge band.
//     Measured on an RTX 4070 the agreement is within 3.4% from 0.67 px down to 0.25 px;
//     dropping the edge term costs 13%, and matching the side instead of the area ~37%.
//     Hence the 8% band: comfortably above MSAA sampling noise, well below any real
//     mistake. Below ~0.25 px the fan stops being a usable reference — see the sweep.
//  2. No popping — the same must hold at several distances, including just past the
//     switch. LOD-off brightness is continuous in distance, so LOD-on tracking it
//     everywhere is exactly the statement that crossing the threshold shows nothing.
//  3. The gate is WHOLE-CLOUD. Near and straddling clouds must come out BIT-identical
//     with the LOD on and off — the sprite pass is not allowed to claim part of a cloud,
//     because a discarded fan instance costs about what a drawn one does and splitting a
//     cloud between the two programs is measurably slower than not doing it.
//
// Skips cleanly when no usable GL context is available, or below GL 4.5 (the scene's
// #version 450 shaders won't compile) — mirroring pointcloud_cube_faces_gl_test.

#include <gtest/gtest.h>

#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QString>
#include <QStringList>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>
#include <string>
#include <utility>

#include "pj_base/time.hpp"
#include "pj_scene3d_core/camera/camera.h"
#include "pj_scene3d_core/pointcloud.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/passes/pointcloud_render_pass.h"
#include "pj_scene3d_widgets/render_pass.h"

namespace {

using namespace pj::scene3d;

constexpr int kW = 256;
constexpr int kH = 256;
constexpr float kCubeSide = 0.05f;
constexpr float kFovYDegrees = 60.0f;

constexpr int kGridSide = 40;

// Mid grey, NOT black. Black is the single background against which "the sprite conserves
// the cube's total light" and "the sprite shows the cube's colour" are indistinguishable —
// so a black-cleared test cannot tell a correctly coloured sprite from one dimmed almost
// to nothing. That is exactly the bug this file failed to catch once.
constexpr float kBackground = 0.25f;

// The grid's half-extent is a fraction of the viewing distance, so its SCREEN footprint
// is the same at every distance tested and the only thing varying is how many pixels one
// cube covers. Without that, moving the camera back also crowds the cubes together, and
// depth-tested overlap — not the LOD — starts eating the total. At this fraction the
// projected spacing is ~3.4 px against a sub-pixel cube, so cubes essentially never
// overlap and total light is a clean measure of what the cloud emits.
constexpr float kGridExtentPerDistance = 0.30f;

float focalLength() {
  return 1.0f / std::tan(glm::radians(kFovYDegrees) * 0.5f);
}

// Projected size of one cube, in pixels, at a given view-axis depth — the same
// size_px = size_meters * proj[1][1] * height / 2 / depth the shader applies. Lets the
// test place the camera by the quantity the LOD actually keys on.
float projectedCubePx(float depth) {
  return kCubeSide * focalLength() * (static_cast<float>(kH) * 0.5f) / depth;
}

// The distance at which a cube projects to exactly `pixels` — where the LOD switches.
float distanceForProjectedPx(float pixels) {
  return kCubeSide * focalLength() * (static_cast<float>(kH) * 0.5f) / pixels;
}

float gridHalfExtent(float distance) {
  return kGridExtentPerDistance * distance;
}

// Flat grid in the z = 0 plane of frame "map", centred on the origin.
std::shared_ptr<const DecodedPointCloud> gridCloud(float half_extent) {
  auto cloud = std::make_shared<DecodedPointCloud>();
  cloud->frame_id = "map";
  cloud->positions.reserve(static_cast<std::size_t>(kGridSide) * kGridSide);
  for (int row = 0; row < kGridSide; ++row) {
    for (int column = 0; column < kGridSide; ++column) {
      const float u = -half_extent + 2.0f * half_extent * static_cast<float>(column) / (kGridSide - 1);
      const float v = -half_extent + 2.0f * half_extent * static_cast<float>(row) / (kGridSide - 1);
      cloud->positions.emplace_back(u, v, 0.0f);
    }
  }
  return cloud;
}

AABB gridBounds(float half_extent) {
  return AABB{glm::vec3(-half_extent, -half_extent, 0.0f), glm::vec3(half_extent, half_extent, 0.0f), true};
}

class PointcloudCubeLodGlTest : public ::testing::Test {
 protected:
  void SetUp() override {
    QSurfaceFormat fmt;
    fmt.setVersion(4, 5);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(24);

    surface_ = std::make_unique<QOffscreenSurface>();
    surface_->setFormat(fmt);
    surface_->create();
    if (!surface_->isValid()) {
      GTEST_SKIP() << "no usable offscreen surface (headless without GL)";
    }
    ctx_ = std::make_unique<QOpenGLContext>();
    ctx_->setFormat(fmt);
    if (!ctx_->create() || !ctx_->makeCurrent(surface_.get())) {
      GTEST_SKIP() << "could not create/make-current an OpenGL context";
    }
    const auto* version = reinterpret_cast<const char*>(ctx_->functions()->glGetString(GL_VERSION));
    const QStringList parts = QString::fromLatin1(version).section(QLatin1Char(' '), 0, 0).split(QLatin1Char('.'));
    if (std::pair<int, int>(parts.value(0).toInt(), parts.value(1).toInt()) < std::pair<int, int>(4, 5)) {
      GTEST_SKIP() << "GL " << (version != nullptr ? version : "?") << " below 4.5 — can't compile scene shaders";
    }

    QOpenGLFramebufferObjectFormat fbo_fmt;
    fbo_fmt.setAttachment(QOpenGLFramebufferObject::Depth);
    // The scene renders multisampled, and a sub-pixel cube's coverage IS its
    // brightness — resolving 4 samples is what lets the fan and the sprite agree.
    fbo_fmt.setSamples(4);
    fbo_ = std::make_unique<QOpenGLFramebufferObject>(kW, kH, fbo_fmt);
    ASSERT_TRUE(fbo_->bind());

    auto* functions = ctx_->functions();
    functions->glViewport(0, 0, kW, kH);
    functions->glClearColor(kBackground, kBackground, kBackground, 1.0f);
    functions->glEnable(GL_DEPTH_TEST);
    functions->glEnable(GL_MULTISAMPLE);
  }

  void TearDown() override {
    fbo_.reset();
    if (ctx_ != nullptr) {
      ctx_->doneCurrent();
    }
  }

  // Renders the grid seen from `distance` along an oblique direction, so all three cube
  // faces have area and the area-weighted shading is actually exercised (a face-on view
  // would pass even with the weights ignored).
  QImage renderGrid(float distance, float lod_threshold_px) {
    return renderGrid(distance, lod_threshold_px, /*screen_offset=*/0.0f);
  }

  // `screen_offset` slides the grid sideways in the image without moving the camera, in
  // units of the frustum half-width at that depth: 0 puts it on the optical axis, 0.8
  // near the edge of the view. Off-axis is where a sprite whose face foreshortening was
  // computed from the optical axis rather than its own eye ray goes wrong.
  QImage renderGrid(float distance, float lod_threshold_px, float screen_offset) {
    // Re-bind every time, NOT once in SetUp: toImage() on a multisample FBO resolves
    // through a temporary and leaves that bound, so a second render would land somewhere
    // else and toImage() would hand back the previous frame — making any A/B in this
    // file compare an image with itself and pass for the wrong reason.
    EXPECT_TRUE(fbo_->bind());
    ctx_->functions()->glViewport(0, 0, kW, kH);
    ctx_->functions()->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const float half_extent = gridHalfExtent(distance);
    PointcloudRenderPass pass;
    pass.setShape(PointcloudRenderPass::Shape::kCube);
    pass.setCubeLodThresholdPx(lod_threshold_px);
    pass.setColorType(PointcloudRenderPass::ColorType::kSolid);
    pass.setSolidColor(glm::vec3(1.0f, 1.0f, 1.0f));
    pass.setSizeMeters(kCubeSide);
    pass.initializeGL();
    pass.setActiveCloud(gridCloud(half_extent));
    // The LOD gate needs an extent; the layer always publishes one. setActiveCloud
    // clears it deliberately, so this has to come after.
    pass.setGeometryBounds(gridBounds(half_extent));

    const glm::vec3 eye = glm::normalize(glm::vec3(0.45f, 0.62f, 0.65f)) * distance;
    // Aim off the grid rather than at it: the grid keeps its distance and its size on
    // screen, but now sits away from the optical axis.
    const float half_width = distance * std::tan(glm::radians(kFovYDegrees) * 0.5f);
    const glm::vec3 sideways = glm::normalize(glm::cross(-eye, glm::vec3(0.0f, 0.0f, 1.0f)));
    ViewParams vp;
    vp.view = glm::lookAt(eye, sideways * (screen_offset * half_width), glm::vec3(0.0f, 0.0f, 1.0f));
    vp.proj = glm::perspective(glm::radians(kFovYDegrees), 1.0f, 0.05f, 500.0f);
    vp.viewport_width_px = kW;
    vp.viewport_height_px = kH;
    vp.device_width_px = kW;
    vp.device_height_px = kH;
    vp.camera_pos_world = eye;

    const TransformBuffer tf(TransformBuffer::kKeepAll);
    const std::string fixed = "map";  // same as the cloud frame → identity model
    const FrameContext fc{tf, fixed, PJ::fromRaw(0)};
    pass.render(vp, fc);
    ctx_->functions()->glFlush();

    const QImage image = fbo_->toImage();
    EXPECT_FALSE(image.isNull());
    return image;
  }

  // Total light in the frame. For sub-pixel geometry this is the only meaningful
  // comparison: individual pixels differ because a square sprite and a hexagon land on
  // different samples, while the integral is what the eye reads as cloud density.
  static double totalBrightness(const QImage& image) {
    double total = 0.0;
    for (int y = 0; y < image.height(); ++y) {
      for (int x = 0; x < image.width(); ++x) {
        const QColor pixel = image.pixelColor(x, y);
        total += pixel.red() + pixel.green() + pixel.blue();
      }
    }
    return total;
  }

  std::unique_ptr<QOffscreenSurface> surface_;
  std::unique_ptr<QOpenGLContext> ctx_;
  std::unique_ptr<QOpenGLFramebufferObject> fbo_;
};

// The headline claim. At a distance where every cube is well under a pixel, swapping the
// fans for sprites must not change how much light the cloud puts on screen.
TEST_F(PointcloudCubeLodGlTest, SpritesEmitTheSameTotalLightAsTheFansTheyReplace) {
  const float distance = 1.1f * distanceForProjectedPx(kDefaultCubeLodThresholdPx);
  ASSERT_LT(projectedCubePx(distance), kDefaultCubeLodThresholdPx) << "test camera is too close to engage the LOD";

  const double fan = totalBrightness(renderGrid(distance, /*lod_threshold_px=*/0.0f));
  const double lod = totalBrightness(renderGrid(distance, kDefaultCubeLodThresholdPx));

  ASSERT_GT(fan, 1000.0) << "the fan render is empty — the test camera sees nothing";
  ASSERT_GT(lod, 1000.0) << "the LOD render is empty — sprites are not being drawn";
  EXPECT_NEAR(lod, fan, 0.08 * fan) << "sprite brightness " << lod << " vs cube brightness " << fan << " (ratio "
                                    << lod / fan
                                    << ") — check the projected-area match, the area-weighted Lambert "
                                       "mean, and kCubeEdgeMeanScale in cube_mesh.h";
}

// Popping check. LOD-off brightness varies smoothly with distance, so if LOD-on tracks it
// at every distance — including immediately past the switch — then crossing the threshold
// cannot produce a visible step.
TEST_F(PointcloudCubeLodGlTest, BrightnessTracksTheFanAcrossTheSwitchDistance) {
  const float switch_distance = distanceForProjectedPx(kDefaultCubeLodThresholdPx);
  // Only just past the switch. This is the band where popping WOULD be visible, and the
  // one where an exact match is even the right expectation: the sprite is ~1.2 px there,
  // above the driver's minimum point size, so nothing is clamped. Further out the sprite
  // deliberately floors at one pixel — the same contract kPoint and kSphere have — so it
  // grows brighter than the cube it replaces, and equality stops being the invariant.
  // DistantSpritesKeepTheCloudColour covers that regime instead.
  for (const float scale : {1.02f, 1.1f}) {
    const float distance = switch_distance * scale;
    const double fan = totalBrightness(renderGrid(distance, /*lod_threshold_px=*/0.0f));
    const double lod = totalBrightness(renderGrid(distance, kDefaultCubeLodThresholdPx));
    ASSERT_GT(fan, 500.0) << "empty frame at distance " << distance;
    EXPECT_NEAR(lod, fan, 0.08 * fan) << "at " << distance << " m (" << scale << "x the switch distance, cube "
                                      << projectedCubePx(distance) << " px) the LOD emits " << lod
                                      << " against the fan's " << fan << " (ratio " << lod / fan << ")";
  }
}

// The gate is whole-cloud, so a cloud with any cube at or above the threshold must be
// drawn entirely by the fan — identically to the LOD being off. Bit-identical, not close:
// the LOD is supposed to not have run at all.
TEST_F(PointcloudCubeLodGlTest, NearCloudIsUntouchedByTheLod) {
  const float distance = 0.2f * distanceForProjectedPx(kDefaultCubeLodThresholdPx);
  ASSERT_GT(projectedCubePx(distance), kDefaultCubeLodThresholdPx) << "test camera is too far to exercise the gate";

  const QImage fan = renderGrid(distance, /*lod_threshold_px=*/0.0f);
  const QImage lod = renderGrid(distance, kDefaultCubeLodThresholdPx);
  EXPECT_EQ(fan, lod) << "a cloud with above-threshold cubes must render exactly as it did without the LOD";
}

// A cloud STRADDLING the threshold is the case where splitting it between the two
// programs would be tempting, and it is measurably slower than not splitting: a discarded
// fan instance still costs 7 vertex invocations. So it must fall back to the fan alone,
// leaving the frame unchanged.
TEST_F(PointcloudCubeLodGlTest, StraddlingCloudFallsBackToTheFan) {
  // Centred exactly on the switch, so the near half of the grid is above the threshold
  // and the far half below it.
  const float distance = distanceForProjectedPx(kDefaultCubeLodThresholdPx);
  const float half_extent = gridHalfExtent(distance);
  ASSERT_GT(projectedCubePx(distance - half_extent), kDefaultCubeLodThresholdPx) << "grid does not reach above";
  ASSERT_LT(projectedCubePx(distance + half_extent), kDefaultCubeLodThresholdPx) << "grid does not reach below";

  const QImage fan = renderGrid(distance, /*lod_threshold_px=*/0.0f);
  const QImage lod = renderGrid(distance, kDefaultCubeLodThresholdPx);
  EXPECT_EQ(fan, lod) << "a cloud spanning the threshold must stay on the fan — the LOD gate is whole-cloud";
}

// THE REGRESSION THIS FILE ONCE MISSED. Far out, the sprite floors at the driver's
// minimum point size, and what matters is no longer "does it emit the same total light as
// the fan" — it is "does it still look like a point of the cloud's colour". Dimming by the
// area overshoot conserves the light and fails this: the pass writes alpha 1 with blending
// off, so a heavily scaled-down sprite paints an almost-black pixel over the background
// instead of blending into it, and a zoomed-out cloud goes black. Against the black
// background this file used to clear to, that was invisible.
TEST_F(PointcloudCubeLodGlTest, DistantSpritesKeepTheCloudColour) {
  // 6x and 60x the switch: at the far one a cube projects to a hundredth of a pixel, so
  // this also pins the guarantee that a point never shrinks below one device pixel however
  // far the camera pulls back — it must still be there, and still be the cloud's colour.
  for (const float scale : {6.0f, 60.0f}) {
    const float distance = scale * distanceForProjectedPx(kDefaultCubeLodThresholdPx);
    const QImage image = renderGrid(distance, kDefaultCubeLodThresholdPx);

    // The cloud is solid white on mid grey, so every drawn pixel must be BRIGHTER than the
    // background. A dimmed sprite goes the other way, toward black.
    const int background = qRound(kBackground * 255.0f);
    int brighter = 0;
    int darker = 0;
    for (int y = 0; y < image.height(); ++y) {
      for (int x = 0; x < image.width(); ++x) {
        const int red = image.pixelColor(x, y).red();
        if (red > background + 12) {
          ++brighter;
        } else if (red < background - 12) {
          ++darker;
        }
      }
    }
    EXPECT_GT(brighter, 100) << "a distant cloud drew no pixels brighter than the background — "
                                "the sprites are not showing the cloud's colour";
    EXPECT_LT(darker, brighter / 10)
        << darker << " pixels came out DARKER than the background against " << brighter
        << " brighter: sub-pixel sprites are being dimmed toward black instead of drawn in "
           "the cloud's colour";
  }
}

// Off the optical axis, a cube is seen along its OWN eye ray, not along the view
// direction, so which faces it shows and how much they foreshorten both change. A sprite
// that derived its area and shading from the global view matrix instead lands up to a
// third too dark near the edge of the frame — a brightness gradient across a far cloud
// that the centred cases above cannot see.
TEST_F(PointcloudCubeLodGlTest, SpritesMatchTheFansOffTheOpticalAxis) {
  const float distance = 1.1f * distanceForProjectedPx(kDefaultCubeLodThresholdPx);
  for (const float offset : {0.5f, 0.85f}) {
    const double fan = totalBrightness(renderGrid(distance, /*lod_threshold_px=*/0.0f, offset));
    const double lod = totalBrightness(renderGrid(distance, kDefaultCubeLodThresholdPx, offset));
    ASSERT_GT(fan, 500.0) << "grid left the frame at offset " << offset;
    EXPECT_NEAR(lod, fan, 0.08 * fan) << "at " << offset << " of the frustum half-width the LOD emits " << lod
                                      << " against the fan's " << fan << " (ratio " << lod / fan
                                      << ") — the sprite's face weights are not following the per-instance eye ray";
  }
}

// Both bounds gates — the LOD switch above and the whole-cloud frustum reject — key off
// the SAME box, and a bounds scan reports the extent of point CENTRES. A cube reaches half
// its world size past each centre, so testing the raw box culls a cloud whose geometry
// still pokes into view. Invisible at the 1 cm default; half a metre of vanished data at a
// 1 m cube size, which the user can set. Rendered here with an ordinary perspective camera
// and a cube straddling the frustum edge.
TEST_F(PointcloudCubeLodGlTest, CubeStraddlingTheFrustumEdgeIsNotCulled) {
  constexpr float kBigCube = 2.0f;  // half-extent 1 m, so the overhang is unmistakable
  constexpr float kDepth = 10.0f;
  // Frustum half-height at that depth, so a centre placed just beyond it is outside the
  // view volume while the cube around it is not.
  const float half_height = kDepth * std::tan(glm::radians(kFovYDegrees) * 0.5f);
  const float centre_y = half_height + 0.4f * kBigCube;

  EXPECT_TRUE(fbo_->bind());
  ctx_->functions()->glViewport(0, 0, kW, kH);
  ctx_->functions()->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  auto cloud = std::make_shared<DecodedPointCloud>();
  cloud->frame_id = "map";
  cloud->positions = {glm::vec3(0.0f, centre_y, -kDepth)};

  PointcloudRenderPass pass;
  pass.setShape(PointcloudRenderPass::Shape::kCube);
  pass.setColorType(PointcloudRenderPass::ColorType::kSolid);
  pass.setSolidColor(glm::vec3(1.0f));
  pass.setSizeMeters(kBigCube);
  pass.initializeGL();
  pass.setActiveCloud(cloud);
  // Exactly what a bounds scan would report: the single point, with no cube extent.
  pass.setGeometryBounds(AABB{cloud->positions[0], cloud->positions[0], true});

  ViewParams vp;
  vp.view = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
  vp.proj = glm::perspective(glm::radians(kFovYDegrees), 1.0f, 0.05f, 500.0f);
  vp.viewport_width_px = kW;
  vp.viewport_height_px = kH;
  vp.device_width_px = kW;
  vp.device_height_px = kH;

  const TransformBuffer tf(TransformBuffer::kKeepAll);
  const std::string fixed = "map";
  const FrameContext fc{tf, fixed, PJ::fromRaw(0)};
  pass.render(vp, fc);
  ctx_->functions()->glFlush();

  EXPECT_GT(totalBrightness(fbo_->toImage()), 0.0)
      << "the cube overhangs the frustum edge but nothing was drawn — the reject is testing "
         "the point-centre box instead of the box the geometry actually occupies";
}

}  // namespace
