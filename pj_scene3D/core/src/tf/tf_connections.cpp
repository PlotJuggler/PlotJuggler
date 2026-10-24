// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/tf/tf_connections.h"

#include <optional>
#include <string>

#include "pj_tf/tf_buffer.h"

namespace pj::scene3d {

void buildTfConnectionSegments(
    const TransformBuffer& tf, const std::string& fixed_frame, TimePoint time, std::vector<TfConnectionSegment>& out,
    const glm::dvec3& render_origin) {
  out.clear();
  for (const std::string& frame : tf.getAllFrames()) {
    const std::optional<std::string> parent = tf.getParent(frame);
    if (!parent.has_value()) {
      continue;  // forest root: no edge to draw.
    }
    // Both endpoints resolved into the fixed frame at `time`. Skip the edge
    // entirely if either side can't resolve, so a frame still streaming in (or a
    // disconnected subtree) never produces a half-anchored line.
    const auto child_w = tf.tryLookupTransform(fixed_frame, frame, time, pj::tf::Clock::kEntry);
    if (!child_w.has_value()) {
      continue;
    }
    const auto parent_w = tf.tryLookupTransform(fixed_frame, *parent, time, pj::tf::Clock::kEntry);
    if (!parent_w.has_value()) {
      continue;
    }
    // Subtract the render origin in double, THEN downcast — so an edge between two
    // far frames keeps its low bits instead of losing them to float32 at ~1e6.
    out.push_back({glm::vec3(child_w->t - render_origin), glm::vec3(parent_w->t - render_origin)});
  }
}

std::vector<TfConnectionSegment> buildTfConnectionSegments(
    const TransformBuffer& tf, const std::string& fixed_frame, TimePoint time, const glm::dvec3& render_origin) {
  std::vector<TfConnectionSegment> out;
  buildTfConnectionSegments(tf, fixed_frame, time, out, render_origin);
  return out;
}

}  // namespace pj::scene3d
