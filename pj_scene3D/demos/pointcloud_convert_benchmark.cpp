// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// CPU benchmark of the browser point-cloud hot path: the shared canonical decoder, then assembly of a 20-byte vertex
// (xyz, scalar, rgba), the benchmark's own retained shape (the layer keeps 12- or 16-byte records). No Qt, GL, parser
// or file I/O in the timed region.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/span.hpp"
#include "pj_scene3d_core/pointcloud_convert.h"

namespace {

using Clock = std::chrono::steady_clock;
using PJ::sdk::PointCloud;
using PJ::sdk::PointField;
using Datatype = PointField::Datatype;

struct RetainedVertex {
  float x;
  float y;
  float z;
  float scalar;
  std::uint32_t rgba;
};
static_assert(sizeof(RetainedVertex) == 20);

bool invalidPoint(std::size_t index, std::size_t count) {
  return index == 17U || index == count / 2U || index + 9U == count;
}

struct SyntheticCloud {
  std::vector<std::uint8_t> bytes;
  PointCloud cloud;

  explicit SyntheticCloud(std::size_t count) : bytes(count * sizeof(RetainedVertex)) {
    for (std::size_t index = 0; index < count; ++index) {
      const float phase = static_cast<float>(index % 4096U) * 0.0015339808F;
      const float radius = 1.0F + static_cast<float>(index % 997U) * 0.002F;
      float values[4] = {
          radius * std::cos(phase), radius * std::sin(phase), static_cast<float>(index % 251U) * 0.01F,
          static_cast<float>(index % 1024U) / 1023.0F};
      if (invalidPoint(index, count)) {
        values[0] = std::numeric_limits<float>::quiet_NaN();
      }
      std::uint8_t* destination = bytes.data() + index * sizeof(RetainedVertex);
      std::memcpy(destination, values, sizeof(values));
      destination[16] = static_cast<std::uint8_t>(index & 0xFFU);
      destination[17] = static_cast<std::uint8_t>((index >> 3U) & 0xFFU);
      destination[18] = static_cast<std::uint8_t>((index >> 7U) & 0xFFU);
      destination[19] = 255U;
    }
    cloud.width = static_cast<std::uint32_t>(count);
    cloud.height = 1;
    cloud.point_step = sizeof(RetainedVertex);
    cloud.row_step = static_cast<std::uint32_t>(count * sizeof(RetainedVertex));
    cloud.frame_id = "laser";
    cloud.fields = {
        {"x", 0, Datatype::kFloat32, 1},          {"y", 4, Datatype::kFloat32, 1},    {"z", 8, Datatype::kFloat32, 1},
        {"intensity", 12, Datatype::kFloat32, 1}, {"rgba", 16, Datatype::kUint32, 1},
    };
    cloud.data = PJ::Span<const std::uint8_t>(bytes.data(), bytes.size());
  }
};

double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2U];
}

int run(std::size_t count, bool rgb) {
  SyntheticCloud source(count);
  const int iterations = count >= 1'000'000U ? 7 : 15;
  std::vector<double> conversion_times;
  std::vector<double> assembly_times;
  conversion_times.reserve(iterations);
  assembly_times.reserve(iterations);
  std::uint64_t checksum = 0;
  std::size_t retained_count = 0;

  for (int iteration = 0; iteration < iterations; ++iteration) {
    const auto start = Clock::now();
    const pj::scene3d::ConvertedPointCloud converted =
        pj::scene3d::convertCanonical(source.cloud, rgb ? std::string_view{} : std::string_view{"intensity"}, rgb);
    const auto converted_at = Clock::now();
    std::vector<RetainedVertex> retained;
    retained.reserve(converted.cloud.positions.size());
    for (std::size_t index = 0; index < converted.cloud.positions.size(); ++index) {
      const glm::vec3& position = converted.cloud.positions[index];
      if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
        continue;
      }
      retained.push_back(
          RetainedVertex{
              position.x,
              position.y,
              position.z,
              converted.cloud.scalar.empty() ? 0.0F : converted.cloud.scalar[index],
              converted.cloud.rgba.empty() ? 0xFFFFFFFFU : converted.cloud.rgba[index],
          });
    }
    const auto assembled_at = Clock::now();
    retained_count = retained.size();
    if (!retained.empty()) {
      checksum += retained.front().rgba + retained.back().rgba + static_cast<std::uint64_t>(retained.size());
    }
    conversion_times.push_back(std::chrono::duration<double, std::milli>(converted_at - start).count());
    assembly_times.push_back(std::chrono::duration<double, std::milli>(assembled_at - converted_at).count());
  }

  std::size_t expected = count;
  for (std::size_t index = 0; index < count; ++index) {
    expected -= invalidPoint(index, count) ? 1U : 0U;
  }
  if (retained_count != expected) {
    std::fprintf(stderr, "finite-point mismatch: expected %zu, got %zu\n", expected, retained_count);
    return 2;
  }
  const double convert_ms = median(conversion_times);
  const double assemble_ms = median(assembly_times);
  const std::size_t retained_bytes = retained_count * sizeof(RetainedVertex);
  std::printf(
      "%9zu points | %-6s | convert %8.3f ms | assemble %8.3f ms | total %8.3f ms | retained %6.2f MiB "
      "| checksum %llu\n",
      count, rgb ? "RGB" : "scalar", convert_ms, assemble_ms, convert_ms + assemble_ms,
      static_cast<double>(retained_bytes) / (1024.0 * 1024.0), static_cast<unsigned long long>(checksum));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::size_t> sizes;
  for (int index = 1; index < argc; ++index) {
    sizes.push_back(static_cast<std::size_t>(std::stoull(argv[index])));
  }
  if (sizes.empty()) {
    sizes = {60'000U, 1'000'000U};
  }
  for (const std::size_t size : sizes) {
    if (size == 0U || size > std::numeric_limits<std::uint32_t>::max() / sizeof(RetainedVertex)) {
      std::fprintf(stderr, "unsupported point count: %zu\n", size);
      return 1;
    }
    if (run(size, true) != 0 || run(size, false) != 0) {
      return 2;
    }
  }
  return 0;
}
