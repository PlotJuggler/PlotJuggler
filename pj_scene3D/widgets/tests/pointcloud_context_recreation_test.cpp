// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QString>
#include <QStringList>
#include <QSurfaceFormat>
#include <cstdint>
#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/span.hpp"
#include "pj_base/time.hpp"
#include "pj_scene3d_core/pointcloud.h"
#include "pj_scene3d_core/pointcloud_convert.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_core/tf/transform.h"
#include "pj_scene3d_widgets/passes/pointcloud_render_pass.h"

namespace {

using PJ::Span;
using pj::scene3d::checkFastPath;
using pj::scene3d::DecodedPointCloud;
using pj::scene3d::FastCloudData;
using pj::scene3d::FrameContext;
using pj::scene3d::PointcloudRenderPass;
using pj::scene3d::StampedTransform;
using pj::scene3d::Transform;
using pj::scene3d::TransformBuffer;
using pj::scene3d::ViewParams;
using PJ::sdk::PointCloud;
using PJ::sdk::PointField;
using DT = PointField::Datatype;

struct CloudFixture {
  std::vector<uint8_t> bytes;
  PointCloud cloud;

  CloudFixture(uint32_t point_count, uint32_t point_step)
      : bytes(static_cast<std::size_t>(point_count) * static_cast<std::size_t>(point_step), 0) {
    cloud.width = point_count;
    cloud.height = 1;
    cloud.point_step = point_step;
    cloud.row_step = point_count * point_step;
    cloud.is_bigendian = false;
    cloud.frame_id = "lidar";
    cloud.fields = {
        {"x", 0, DT::kFloat32, 1},
        {"y", 4, DT::kFloat32, 1},
        {"z", 8, DT::kFloat32, 1},
        {"intensity", 12, DT::kFloat32, 1},
    };
    rebind();
  }

  CloudFixture(const CloudFixture&) = delete;
  CloudFixture& operator=(const CloudFixture&) = delete;

  CloudFixture(CloudFixture&& other) noexcept : bytes(std::move(other.bytes)), cloud(std::move(other.cloud)) {
    rebind();
  }

  CloudFixture& operator=(CloudFixture&& other) noexcept {
    bytes = std::move(other.bytes);
    cloud = std::move(other.cloud);
    rebind();
    return *this;
  }

  void rebind() {
    cloud.data = Span<const uint8_t>(bytes.data(), bytes.size());
  }
};

template <typename Value>
void putValue(std::vector<uint8_t>& bytes, std::size_t offset, Value value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void putPoint(CloudFixture& fixture, std::size_t point_index, float x, float y, float z, float intensity) {
  const std::size_t base = point_index * fixture.cloud.point_step;
  putValue(fixture.bytes, base + 0U, x);
  putValue(fixture.bytes, base + 4U, y);
  putValue(fixture.bytes, base + 8U, z);
  putValue(fixture.bytes, base + 12U, intensity);
}

FastCloudData makeFastCloud(CloudFixture& fixture) {
  const auto layout = checkFastPath(fixture.cloud, "intensity");
  if (!layout.has_value()) {
    ADD_FAILURE() << "fixture did not satisfy fast-path predicate";
    return {};
  }
  return FastCloudData{
      .wire = fixture.cloud,
      .point_count = fixture.cloud.width * fixture.cloud.height,
      .layout = *layout,
  };
}

std::pair<int, int> currentGlVersion(QOpenGLContext& context) {
  const auto* version = reinterpret_cast<const char*>(context.functions()->glGetString(GL_VERSION));
  if (version == nullptr) {
    return {0, 0};
  }
  const QStringList parts = QString::fromLatin1(version).section(QLatin1Char(' '), 0, 0).split(QLatin1Char('.'));
  return {parts.value(0).toInt(), parts.value(1).toInt()};
}

struct OffscreenGlContext {
  QOffscreenSurface surface;
  QOpenGLContext context;

  OffscreenGlContext() {
    QSurfaceFormat format;
    format.setVersion(4, 5);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    surface.setFormat(format);
    context.setFormat(format);
  }

  [[nodiscard]] bool createAndMakeCurrent() {
    surface.create();
    if (!surface.isValid()) {
      return false;
    }
    if (!context.create() || !context.isValid()) {
      return false;
    }
    return context.makeCurrent(&surface);
  }
};

TEST(PointcloudContextRecreationTest, ReleaseRetainsFastCloudAndRearmsUploadWithoutGl) {
  CloudFixture fixture(/*point_count=*/1U, /*point_step=*/16U);
  putPoint(fixture, 0U, 1.0f, 2.0f, 3.0f, 4.0f);

  PointcloudRenderPass pass;
  pass.setActiveFastCloud(makeFastCloud(fixture));
  pass.releaseGL();

  EXPECT_TRUE(pass.cloudDirtyForTest());
  EXPECT_TRUE(pass.activeCloudIsFastForTest());
}

TEST(PointcloudContextRecreationTest, ReleaseRetainsFallbackCloudAndRearmsUploadWithoutGl) {
  auto decoded = std::make_shared<DecodedPointCloud>();
  decoded->frame_id = "lidar";
  decoded->positions.push_back(glm::vec3{1.0f, 2.0f, 3.0f});
  decoded->scalar.push_back(4.0f);

  PointcloudRenderPass pass;
  pass.setActiveCloud(decoded);
  pass.releaseGL();

  EXPECT_TRUE(pass.cloudDirtyForTest());
  EXPECT_FALSE(pass.activeCloudIsFastForTest());
}

TEST(PointcloudContextRecreationTest, ReleaseLeavesNoCloudCleanWithoutGl) {
  PointcloudRenderPass pass;
  pass.setActiveCloud(nullptr);
  pass.releaseGL();

  EXPECT_FALSE(pass.cloudDirtyForTest());
  EXPECT_FALSE(pass.activeCloudIsFastForTest());
}

TEST(PointcloudContextRecreationTest, FastCloudReuploadsAfterContextRecreationWhenGl45IsAvailable) {
  OffscreenGlContext gl;
  if (!gl.createAndMakeCurrent()) {
    GTEST_SKIP() << "No usable offscreen OpenGL context";
  }
  const auto gl_version = currentGlVersion(gl.context);
  if (gl_version < std::pair<int, int>(4, 5)) {
    GTEST_SKIP() << "GL " << gl_version.first << "." << gl_version.second
                 << " context cannot compile the scene's #version 450 shaders (need GL 4.5)";
  }

  CloudFixture fixture(/*point_count=*/2U, /*point_step=*/16U);
  putPoint(fixture, 0U, 0.0f, 0.0f, 0.0f, 1.0f);
  putPoint(fixture, 1U, 0.1f, 0.1f, 0.0f, 2.0f);

  TransformBuffer tf(TransformBuffer::kKeepAll);
  ASSERT_TRUE(tf.setTransform(
                    StampedTransform{
                        .stamp = PJ::fromRaw(0),
                        .parent_frame = "world",
                        .child_frame = fixture.cloud.frame_id,
                        .transform = Transform(glm::dvec3{0.0, 0.0, 0.0}, glm::dquat{1.0, 0.0, 0.0, 0.0}),
                    })
                  .has_value());

  const std::string fixed_frame = "world";
  const FrameContext frame_ctx{tf, fixed_frame, PJ::fromRaw(1)};
  const ViewParams view{
      .view = glm::mat4{1.0f},
      .proj = glm::mat4{1.0f},
      .viewport_height_px = 480,
      .viewport_width_px = 640,
      .camera_pos_world = glm::vec3{0.0f, 0.0f, 1.0f},
      .device_width_px = 640,
      .device_height_px = 480,
  };

  PointcloudRenderPass pass;
  pass.initializeGL();
  pass.setActiveFastCloud(makeFastCloud(fixture));
  pass.render(view, frame_ctx);
  EXPECT_GT(pass.vboPointCountForTest(), 0U);

  pass.releaseGL();
  EXPECT_TRUE(pass.cloudDirtyForTest());

  pass.initializeGL();
  pass.render(view, frame_ctx);
  EXPECT_GT(pass.vboPointCountForTest(), 0U);

  pass.releaseGL();
  gl.context.doneCurrent();
}

// End-to-end async GPU AABB: enabling the reducer makes render() dispatch a
// compute reduction; after the GPU completes, a later render() polls it and
// delivers the source-frame AABB through the bounds callback.
TEST(PointcloudContextRecreationTest, GpuAabbReductionFiresBoundsCallbackWhenGl45IsAvailable) {
  OffscreenGlContext gl;
  if (!gl.createAndMakeCurrent()) {
    GTEST_SKIP() << "No usable offscreen OpenGL context";
  }
  if (currentGlVersion(gl.context) < std::pair<int, int>(4, 5)) {
    GTEST_SKIP() << "GL < 4.5 cannot compile the compute reduction";
  }

  CloudFixture fixture(/*point_count=*/3U, /*point_step=*/16U);
  putPoint(fixture, 0U, 1.0f, -2.0f, 3.0f, 0.0f);
  putPoint(fixture, 1U, -4.0f, 5.0f, -6.0f, 0.0f);
  putPoint(fixture, 2U, 0.0f, 0.0f, 0.0f, 0.0f);

  TransformBuffer tf(TransformBuffer::kKeepAll);
  ASSERT_TRUE(tf.setTransform(
                    StampedTransform{
                        .stamp = PJ::fromRaw(0),
                        .parent_frame = "world",
                        .child_frame = fixture.cloud.frame_id,
                        .transform = Transform(glm::dvec3{0.0, 0.0, 0.0}, glm::dquat{1.0, 0.0, 0.0, 0.0}),
                    })
                  .has_value());
  const FrameContext frame_ctx{tf, "world", PJ::fromRaw(1)};
  const ViewParams view{
      .view = glm::mat4{1.0f},
      .proj = glm::mat4{1.0f},
      .viewport_height_px = 480,
      .viewport_width_px = 640,
      .camera_pos_world = glm::vec3{0.0f, 0.0f, 1.0f},
      .device_width_px = 640,
      .device_height_px = 480,
  };

  PointcloudRenderPass pass;
  pass.initializeGL();
  std::optional<pj::scene3d::AABB> received;
  int callback_count = 0;
  pass.setBoundsCallback([&](std::optional<pj::scene3d::AABB> box) {
    received = box;
    ++callback_count;
  });
  pass.setGpuAabbEnabled(true);
  pass.setActiveFastCloud(makeFastCloud(fixture));

  pass.render(view, frame_ctx);  // uploads + dispatches; fence just inserted
  gl.context.functions()->glFinish();
  pass.render(view, frame_ctx);  // polls the now-complete fence -> callback fires

  ASSERT_TRUE(pass.gpuAabbAvailable());
  ASSERT_GE(callback_count, 1);
  ASSERT_TRUE(received.has_value());
  ASSERT_TRUE(received->valid);
  EXPECT_FLOAT_EQ(received->min.x, -4.0f);
  EXPECT_FLOAT_EQ(received->min.y, -2.0f);
  EXPECT_FLOAT_EQ(received->min.z, -6.0f);
  EXPECT_FLOAT_EQ(received->max.x, 1.0f);
  EXPECT_FLOAT_EQ(received->max.y, 5.0f);
  EXPECT_FLOAT_EQ(received->max.z, 3.0f);

  pass.releaseGL();
  gl.context.doneCurrent();
}

}  // namespace
