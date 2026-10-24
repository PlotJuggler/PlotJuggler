// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_core/trail_sampling.h"

#include <algorithm>

#include "pj_scene3d_core/tf/object_placement.h"
#include "pj_tf/tf_buffer.h"

namespace pj::scene3d {

std::vector<std::size_t> decimateEvenly(std::size_t n, std::size_t max_points) {
  std::vector<std::size_t> kept;
  if (n == 0 || max_points == 0) {
    return kept;
  }
  if (n <= max_points) {
    kept.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
      kept[i] = i;
    }
    return kept;
  }
  if (max_points == 1) {
    kept.push_back(0);
    return kept;
  }
  kept.reserve(max_points);
  // i * (n-1) / (max-1) hits 0 and n-1 exactly; integer math keeps the choice
  // deterministic across platforms.
  for (std::size_t i = 0; i < max_points; ++i) {
    kept.push_back(i * (n - 1) / (max_points - 1));
  }
  kept.erase(std::unique(kept.begin(), kept.end()), kept.end());
  return kept;
}

TrailPolyline buildTfFrameTrail(
    const TransformBuffer& tf, const std::string& fixed, const std::string& frame, TimePoint lo, TimePoint hi,
    std::size_t max_points) {
  TrailPolyline trail;
  std::vector<TimePoint> stamps;
  // Entry clock: the trail is drawn against the cursor (past/future split, live
  // append), and its head must meet the frame's triad, which sits at the cursor.
  tf.chainSampleTimes(fixed, frame, lo, hi, pj::tf::Clock::kEntry, stamps);
  if (stamps.empty()) {
    return trail;
  }
  const std::vector<std::size_t> kept = decimateEvenly(stamps.size(), max_points);
  trail.reserve(kept.size());
  for (const std::size_t index : kept) {
    const auto transform = tf.tryLookupTransform(fixed, frame, stamps[index], pj::tf::Clock::kEntry);
    if (!transform.has_value()) {
      continue;  // edge not yet published at this stamp -> gap (bridged by the strip)
    }
    trail.push_back(TrailPoint{stamps[index], transform->t});
  }
  return trail;
}

TrailPolyline buildPoseTopicTrail(
    const TransformBuffer& tf, const std::string& fixed, const std::vector<PoseTrailSample>& samples) {
  TrailPolyline trail;
  trail.reserve(samples.size());
  for (const PoseTrailSample& sample : samples) {
    if (sample.frame_id == fixed) {
      trail.push_back(TrailPoint{sample.t, sample.local_pos});
      continue;
    }
    // Placed like the pose triads themselves, so the trail meets the current pose.
    const auto transform = placeObject(tf, fixed, sample.frame_id, sample.object_stamp, sample.t);
    if (!transform.has_value()) {
      continue;
    }
    trail.push_back(TrailPoint{sample.t, (*transform) * sample.local_pos});
  }
  return trail;
}

}  // namespace pj::scene3d
