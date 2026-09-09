// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QGuiApplication>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/span.hpp"
#include "pj_scene3d_core/pointcloud_convert.h"
#include "pj_scene3d_widgets/passes/pointcloud_render_pass.h"

namespace {

using PJ::Span;
using pj::scene3d::checkFastPath;
using pj::scene3d::FastCloudData;
using pj::scene3d::PointcloudRenderPass;
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

TEST(PointcloudCubeInstanceAttribsTest, FastCloudSwapRearmsCubeInstanceBindings) {
  CloudFixture first(/*point_count=*/2U, /*point_step=*/16U);
  putPoint(first, 0U, 1.0f, 2.0f, 3.0f, 10.0f);
  putPoint(first, 1U, 4.0f, 5.0f, 6.0f, 20.0f);

  PointcloudRenderPass pass;
  pass.setActiveFastCloud(makeFastCloud(first));

  EXPECT_TRUE(pass.activeCloudIsFastForTest());
  EXPECT_TRUE(pass.cubeInstanceBindingsDirtyForTest());

  CloudFixture second(/*point_count=*/2U, /*point_step=*/32U);
  putPoint(second, 0U, -1.0f, -2.0f, -3.0f, 30.0f);
  putPoint(second, 1U, -4.0f, -5.0f, -6.0f, 40.0f);
  pass.setActiveFastCloud(makeFastCloud(second));

  EXPECT_TRUE(pass.activeCloudIsFastForTest());
  EXPECT_TRUE(pass.cubeInstanceBindingsDirtyForTest());
}

}  // namespace
