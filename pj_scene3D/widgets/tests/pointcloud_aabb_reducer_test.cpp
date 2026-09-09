// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// GL>=4.5-gated equivalence test for the GPU AABB compute reduction: it must
// produce the same min/max corner (bit-exact — min/max are select ops, no
// rounding) as a straightforward CPU scan, skip non-finite points, and report an
// invalid box when no finite points exist. Skips on a context that cannot compile
// the #version 430 compute shader (e.g. Windows software GL).

#include "pj_scene3d_widgets/passes/pointcloud_aabb_reducer.h"

#include <gtest/gtest.h>

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QString>
#include <QStringList>
#include <QSurfaceFormat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <glm/glm.hpp>
#include <limits>
#include <optional>
#include <random>
#include <utility>
#include <vector>

namespace {

using pj::scene3d::AABB;
using pj::scene3d::PointcloudAabbReducer;

constexpr uint32_t kStride = 16;  // xyz float32 + one trailing float (intensity)

struct OffscreenGlContext {
  QOffscreenSurface surface;
  QOpenGLContext context;

  OffscreenGlContext() {
    QSurfaceFormat format;
    format.setVersion(4, 5);
    format.setProfile(QSurfaceFormat::CoreProfile);
    surface.setFormat(format);
    context.setFormat(format);
  }

  [[nodiscard]] bool createAndMakeCurrent() {
    surface.create();
    if (!surface.isValid() || !context.create() || !context.isValid()) {
      return false;
    }
    return context.makeCurrent(&surface);
  }
};

std::pair<int, int> currentGlVersion(QOpenGLContext& context) {
  const auto* version = reinterpret_cast<const char*>(context.functions()->glGetString(GL_VERSION));
  if (version == nullptr) {
    return {0, 0};
  }
  const QStringList parts = QString::fromLatin1(version).section(QLatin1Char(' '), 0, 0).split(QLatin1Char('.'));
  return {parts.value(0).toInt(), parts.value(1).toInt()};
}

// Pack one point's xyz (intensity left zero) into a stride-major byte buffer.
void packPoint(std::vector<uint8_t>& bytes, std::size_t index, const glm::vec3& p) {
  const std::size_t base = index * kStride;
  std::memcpy(bytes.data() + base + 0, &p.x, sizeof(float));
  std::memcpy(bytes.data() + base + 4, &p.y, sizeof(float));
  std::memcpy(bytes.data() + base + 8, &p.z, sizeof(float));
}

std::vector<uint8_t> packCloud(const std::vector<glm::vec3>& points) {
  std::vector<uint8_t> bytes(points.size() * kStride, 0);
  for (std::size_t i = 0; i < points.size(); ++i) {
    packPoint(bytes, i, points[i]);
  }
  return bytes;
}

GLuint uploadBuffer(QOpenGLFunctions* f, const std::vector<uint8_t>& bytes) {
  GLuint id = 0;
  f->glGenBuffers(1, &id);
  f->glBindBuffer(GL_ARRAY_BUFFER, id);
  f->glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes.size()), bytes.data(), GL_STATIC_DRAW);
  f->glBindBuffer(GL_ARRAY_BUFFER, 0);
  return id;
}

// Direct CPU AABB over the finite points, the reference the GPU must match.
AABB cpuReference(const std::vector<glm::vec3>& points) {
  AABB box;
  for (const glm::vec3& p : points) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
      continue;
    }
    if (!box.valid) {
      box.min = p;
      box.max = p;
      box.valid = true;
    } else {
      box.min = glm::min(box.min, p);
      box.max = glm::max(box.max, p);
    }
  }
  return box;
}

// Dispatch + force completion + poll, so the test reads back synchronously.
std::optional<AABB> reduceBlocking(
    QOpenGLFunctions* f, PointcloudAabbReducer& reducer, GLuint buffer, std::size_t point_count) {
  reducer.dispatch(buffer, point_count, kStride, /*x_offset_bytes=*/0);
  f->glFinish();  // tests want a synchronous answer; production polls across frames
  return reducer.poll();
}

class AabbReducerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!gl_.createAndMakeCurrent()) {
      GTEST_SKIP() << "No usable offscreen OpenGL context";
    }
    if (currentGlVersion(gl_.context) < std::pair<int, int>(4, 5)) {
      GTEST_SKIP() << "GL < 4.5 cannot compile the #version 430 compute shader";
    }
    f_ = gl_.context.functions();
  }

  OffscreenGlContext gl_;
  QOpenGLFunctions* f_{nullptr};
};

TEST_F(AabbReducerTest, MatchesCpuOnKnownPoints) {
  const std::vector<glm::vec3> points = {
      {1.0f, -2.0f, 3.0f}, {-4.0f, 5.0f, -6.0f}, {0.5f, 0.5f, 0.5f}, {7.0f, -8.0f, 9.0f}};
  const std::vector<uint8_t> bytes = packCloud(points);
  const GLuint buffer = uploadBuffer(f_, bytes);

  PointcloudAabbReducer reducer;
  const auto box = reduceBlocking(f_, reducer, buffer, points.size());
  ASSERT_TRUE(box.has_value());
  ASSERT_TRUE(box->valid);
  const AABB expected = cpuReference(points);
  EXPECT_FLOAT_EQ(box->min.x, expected.min.x);
  EXPECT_FLOAT_EQ(box->min.y, expected.min.y);
  EXPECT_FLOAT_EQ(box->min.z, expected.min.z);
  EXPECT_FLOAT_EQ(box->max.x, expected.max.x);
  EXPECT_FLOAT_EQ(box->max.y, expected.max.y);
  EXPECT_FLOAT_EQ(box->max.z, expected.max.z);
  EXPECT_TRUE(reducer.available());

  f_->glDeleteBuffers(1, &buffer);
}

TEST_F(AabbReducerTest, SkipsNonFinitePoints) {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  const std::vector<glm::vec3> points = {
      {1.0f, 1.0f, 1.0f}, {nan, 0.0f, 0.0f}, {2.0f, -3.0f, 4.0f}, {0.0f, inf, 0.0f}, {-5.0f, 6.0f, -7.0f}};
  const std::vector<uint8_t> bytes = packCloud(points);
  const GLuint buffer = uploadBuffer(f_, bytes);

  PointcloudAabbReducer reducer;
  const auto box = reduceBlocking(f_, reducer, buffer, points.size());
  ASSERT_TRUE(box.has_value());
  ASSERT_TRUE(box->valid);
  const AABB expected = cpuReference(points);  // ignores the NaN/inf points
  EXPECT_FLOAT_EQ(box->min.x, expected.min.x);
  EXPECT_FLOAT_EQ(box->min.y, expected.min.y);
  EXPECT_FLOAT_EQ(box->min.z, expected.min.z);
  EXPECT_FLOAT_EQ(box->max.x, expected.max.x);
  EXPECT_FLOAT_EQ(box->max.y, expected.max.y);
  EXPECT_FLOAT_EQ(box->max.z, expected.max.z);

  f_->glDeleteBuffers(1, &buffer);
}

TEST_F(AabbReducerTest, AllNonFiniteYieldsInvalidBox) {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const std::vector<glm::vec3> points = {{nan, nan, nan}, {nan, nan, nan}};
  const std::vector<uint8_t> bytes = packCloud(points);
  const GLuint buffer = uploadBuffer(f_, bytes);

  PointcloudAabbReducer reducer;
  const auto box = reduceBlocking(f_, reducer, buffer, points.size());
  ASSERT_TRUE(box.has_value());
  EXPECT_FALSE(box->valid);

  f_->glDeleteBuffers(1, &buffer);
}

TEST_F(AabbReducerTest, MatchesCpuOnLargeRandomCloud) {
  std::mt19937 rng(1234);
  std::uniform_real_distribution<float> dist(-1000.0f, 1000.0f);
  std::vector<glm::vec3> points(100000);
  for (glm::vec3& p : points) {
    p = {dist(rng), dist(rng), dist(rng)};
  }
  const std::vector<uint8_t> bytes = packCloud(points);
  const GLuint buffer = uploadBuffer(f_, bytes);

  PointcloudAabbReducer reducer;
  const auto box = reduceBlocking(f_, reducer, buffer, points.size());
  ASSERT_TRUE(box.has_value());
  ASSERT_TRUE(box->valid);
  const AABB expected = cpuReference(points);
  // min/max are select ops over the same float values — expect bit-exact agreement.
  EXPECT_FLOAT_EQ(box->min.x, expected.min.x);
  EXPECT_FLOAT_EQ(box->min.y, expected.min.y);
  EXPECT_FLOAT_EQ(box->min.z, expected.min.z);
  EXPECT_FLOAT_EQ(box->max.x, expected.max.x);
  EXPECT_FLOAT_EQ(box->max.y, expected.max.y);
  EXPECT_FLOAT_EQ(box->max.z, expected.max.z);

  f_->glDeleteBuffers(1, &buffer);
}

}  // namespace
