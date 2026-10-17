#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <vector>

#include "pj_tf/transform.h"  // TimePoint

namespace pj::tf {
class TransformBuffer;
}  // namespace pj::tf

namespace pj::scene3d {

using pj::tf::TimePoint;
using pj::tf::TransformBuffer;

// Hard ceiling on trail vertices: bounds both the one-time build cost and the
// GPU upload. Over-cap inputs are decimated evenly (endpoints always kept).
inline constexpr std::size_t kMaxTrailPoints = 50'000;

// One trail vertex: origin of the tracked frame in the FIXED frame, in double
// (absolute world — the camera-relative render_origin subtraction happens at
// upload time, per the module's large-coordinate rule).
struct TrailPoint {
  TimePoint t;
  glm::dvec3 pos{0.0};
};

// Time-sorted polyline (ascending t).
using TrailPolyline = std::vector<TrailPoint>;

// Evenly-spaced index subset of [0, n): all of them when n <= max_points, else
// max_points indices including 0 and n-1, strictly increasing, deterministic.
[[nodiscard]] std::vector<std::size_t> decimateEvenly(std::size_t n, std::size_t max_points);

// Trail of `frame`'s origin expressed in `fixed`, sampled at the connecting
// chain's own update times within [lo, hi] (chainSampleTimes in the entry clock,
// the cursor's), decimated to max_points BEFORE the per-stamp lookups. Stamps
// whose lookup fails (edge not yet published at that time) are skipped — the
// polyline bridges the gap.
[[nodiscard]] TrailPolyline buildTfFrameTrail(
    const TransformBuffer& tf, const std::string& fixed, const std::string& frame, TimePoint lo, TimePoint hi,
    std::size_t max_points);

// One decoded pose-topic message: store timestamp + first-pose position + the
// message's frame_id and its own stamp on the TF header clock (objectStamp();
// nullopt when unstamped). The caller (TrailLayer) decodes; this stays decode-free.
struct PoseTrailSample {
  TimePoint t;
  glm::dvec3 local_pos{0.0};
  std::string frame_id;
  std::optional<TimePoint> object_stamp;
};

// Transforms each sample's position into `fixed` where the pose layer places it
// (placeObject(): at its own header stamp, else at its store timestamp t);
// frame_id == fixed short-circuits to identity, so a TF-less dataset still
// trails. Unresolvable samples are skipped. Input must be time-sorted by t.
[[nodiscard]] TrailPolyline buildPoseTopicTrail(
    const TransformBuffer& tf, const std::string& fixed, const std::vector<PoseTrailSample>& samples);

}  // namespace pj::scene3d
