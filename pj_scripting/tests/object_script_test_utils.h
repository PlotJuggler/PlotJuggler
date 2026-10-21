// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Fixtures the Luau and Python object-script tests share: synthetic clouds, a fake image decoder, a fake video and a TF
// snapshot.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/buffer_anchor.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/builtin/video_frame.hpp"
#include "pj_base/builtin/voxel_grid.hpp"
#include "pj_scripting/image_decoder.h"
#include "pj_scripting/memory_budget.h"
#include "pj_scripting/object_script.h"
#include "pj_scripting/video_frame_source.h"
#include "pj_tf/tf_buffer.h"

namespace PJ::scripting::test {

inline constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
inline constexpr int64_t kSecond = 1'000'000'000;

/// A flat cloud: x/y/z/intensity as float32 at offsets 0/4/8/12, `point_step` bytes per record (>= 16; the rest is
/// padding, as a lidar's extra channels would be), in "base_link" at 1 s.
inline sdk::PointCloud makeCloud(const std::vector<std::array<float, 4>>& points, uint32_t point_step = 16) {
  using sdk::PointField;
  sdk::PointCloud cloud;
  cloud.width = static_cast<uint32_t>(points.size());
  cloud.height = 1;
  cloud.point_step = point_step;
  cloud.row_step = cloud.point_step * cloud.width;
  cloud.is_dense = true;
  cloud.frame_id = "base_link";
  cloud.fields = {
      PointField{.name = "x", .offset = 0, .datatype = PointField::Datatype::kFloat32, .count = 1},
      PointField{.name = "y", .offset = 4, .datatype = PointField::Datatype::kFloat32, .count = 1},
      PointField{.name = "z", .offset = 8, .datatype = PointField::Datatype::kFloat32, .count = 1},
      PointField{.name = "intensity", .offset = 12, .datatype = PointField::Datatype::kFloat32, .count = 1},
  };
  cloud.timestamp_ns = kSecond;
  std::vector<uint8_t> bytes(static_cast<size_t>(cloud.point_step) * points.size());
  for (size_t index = 0; index < points.size(); ++index) {
    std::memcpy(bytes.data() + index * cloud.point_step, points[index].data(), sizeof(float) * 4);
  }
  sdk::PayloadView view = sdk::makePayloadView(std::move(bytes));
  cloud.data = view.bytes;
  cloud.anchor = view.anchor;
  return cloud;
}

/// `count` points along x, at x = 0, 1, 2, ...
inline sdk::PointCloud makeLineCloud(size_t count, uint32_t point_step = 16) {
  std::vector<std::array<float, 4>> points(count);
  for (size_t index = 0; index < count; ++index) {
    points[index] = {static_cast<float>(index), 0, 0, 0};
  }
  return makeCloud(points, point_step);
}

/// A 1 x 1 x `slices` float32 `value` grid in "odom", cells of 0.25 m, cell k holding k.
inline sdk::VoxelGrid makeColumnGrid(uint32_t slices) {
  sdk::VoxelGrid grid;
  grid.frame_id = "odom";
  grid.cell_size = {0.25, 0.25, 0.25};
  grid.column_count = 1;
  grid.row_count = 1;
  grid.slice_count = slices;
  grid.cell_stride = sizeof(float);
  grid.row_stride = sizeof(float);
  grid.slice_stride = sizeof(float);
  grid.fields = {
      sdk::PointField{.name = "value", .offset = 0, .datatype = sdk::PointField::Datatype::kFloat32, .count = 1}};
  std::vector<uint8_t> bytes(sizeof(float) * slices);
  for (uint32_t slice = 0; slice < slices; ++slice) {
    const auto value = static_cast<float>(slice);
    std::memcpy(bytes.data() + slice * sizeof(float), &value, sizeof(float));
  }
  sdk::PayloadView view = sdk::makePayloadView(std::move(bytes));
  grid.data = view.bytes;
  grid.anchor = view.anchor;
  return grid;
}

/// Decodes any image into width x height samples, charged to the budget; counts its calls. `value` < 0: sample = its
/// index.
class FakeImageDecoder final : public IImageDecoder {
 public:
  FakeImageDecoder(uint32_t width, uint32_t height, float value = -1) : width_(width), height_(height), value_(value) {}
  mutable int calls = 0;

  Expected<DecodedPixels> decode(
      const sdk::Image& /*image*/, const std::shared_ptr<native::MemoryBudget>& budget) const override {
    ++calls;
    const std::size_t count = std::size_t{width_} * height_;
    auto charge = std::make_shared<native::MemoryReservation>(budget);
    if (Status grown = charge->grow(count * sizeof(float)); !grown) {
      return unexpected(grown.error());
    }
    std::vector<float> samples(count, value_);
    if (value_ < 0) {
      for (std::size_t index = 0; index < count; ++index) {
        samples[index] = static_cast<float>(index);
      }
    }
    DecodedPixels out;
    out.width = width_;
    out.height = height_;
    out.channels = 1;
    out.samples = std::make_shared<const std::vector<float>>(std::move(samples));
    out.charge = std::move(charge);
    return out;
  }

 private:
  uint32_t width_;
  uint32_t height_;
  float value_;
};

inline pj::tf::TimePoint at(int64_t ns) {
  return PJ::fromRaw(ns);
}

/// Adds `parent <- child` to `buffer`: header `stamp`, entry time `entry`.
inline void addTf(
    pj::tf::TransformBuffer& buffer, const std::string& parent, const std::string& child, int64_t stamp, int64_t entry,
    glm::dvec3 translation, glm::dquat rotation = glm::dquat(1, 0, 0, 0)) {
  const pj::tf::StampedTransform tf{at(stamp), parent, child, pj::tf::Transform(translation, rotation)};
  ASSERT_TRUE(buffer.setTransform(tf, at(entry)).has_value());
}

/// A snapshot holding one static `map <- base_link` edge, translation {1, 2, 3}, stamped at 1 s.
inline std::shared_ptr<const pj::tf::TransformSnapshot> mapFromBaseLink() {
  pj::tf::TransformBuffer buffer(pj::tf::TransformBuffer::kKeepAll);
  addTf(buffer, "map", "base_link", kSecond, kSecond, {1, 2, 3});
  return buffer.snapshot();
}

inline constexpr ObjectTopicId kVideoTopic{7};

/// A video whose frame at `t` presents at t - t % 10: 16 x 16 rgb8, charged before it exists; counts its calls.
class BucketVideo final : public IVideoFrameSource {
 public:
  int calls = 0;
  Timestamp received = 0;         ///< the last query's time
  bool saw_cancel_check = false;  ///< the last query came with the run's cancel check
  Expected<VideoFrameResult> frameAt(
      ObjectTopicId topic, Timestamp t, const std::shared_ptr<native::MemoryBudget>& budget,
      const std::function<bool()>& cancelled) override {
    ++calls;
    received = t;
    saw_cancel_check = static_cast<bool>(cancelled);
    if (topic != kVideoTopic) {
      return unexpected("unknown video topic");
    }
    auto charge = std::make_shared<native::MemoryReservation>(budget);
    constexpr uint32_t kSide = 16;
    if (Status grown = charge->grow(kSide * kSide * 3); !grown) {
      return unexpected(grown.error());
    }
    struct Held {
      std::vector<uint8_t> rgb;
      std::shared_ptr<native::MemoryReservation> charge;
    };
    auto held = std::make_shared<Held>(Held{std::vector<uint8_t>(kSide * kSide * 3, 7), std::move(charge)});
    VideoFrameResult frame;
    frame.presentation_ns = t - t % 10;
    frame.image.encoding = "rgb8";
    frame.image.width = kSide;
    frame.image.height = kSide;
    frame.image.row_step = kSide * 3;
    frame.image.timestamp_ns = frame.presentation_ns;
    frame.image.data = {held->rgb.data(), held->rgb.size()};
    frame.image.anchor = held;
    return frame;
  }
  void retireDataset(DatasetId /*dataset*/) override {}
};

/// The video input "video" on store topic `/video` (kVideoTopic), its packet stamped 99.
inline NamedValue videoInput() {
  sdk::VideoFrame packet;
  packet.timestamp_ns = 99;
  NamedValue input{.name = "video", .value = ScriptValue{sdk::BuiltinObject(packet)}};
  input.topic = "/video";
  input.topic_id = kVideoTopic;
  return input;
}

/// The run options of a video run: `video` behind `frame_at`, `decoder` behind `decode`.
inline RunOptions withVideo(std::shared_ptr<BucketVideo> video, std::shared_ptr<FakeImageDecoder> decoder) {
  RunOptions options;
  options.video_source = std::move(video);
  options.image_decoder = std::move(decoder);
  return options;
}

}  // namespace PJ::scripting::test
