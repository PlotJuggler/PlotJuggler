#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <glm/glm.hpp>
#include <string>
#include <vector>

#include "pj_tf/transform.h"  // TimePoint

namespace pj::tf {
class TransformBuffer;
}  // namespace pj::tf

namespace pj::scene3d {

using pj::tf::TimePoint;
using pj::tf::TransformBuffer;

// One drawn TF parent-connection edge: the origins (in the fixed frame) of a child frame and its parent, as float
// (the rest of the 3D pipeline downcasts the double TransformBuffer poses alike).
struct TfConnectionSegment {
  glm::vec3 child;
  glm::vec3 parent;
};

// Appends one segment per NON-root frame in `tf`: a line from the frame's origin to its parent's origin, both resolved
// into `fixed_frame` at `time`, the cursor (entry clock, like the triads they connect). A frame is skipped when it has
// no parent or when EITHER endpoint fails to resolve (no sample yet, disconnected): a half-resolved edge is never
// emitted. `out` is cleared, then reused. Order follows TransformBuffer::getAllFrames.
//
// `render_origin` (the camera-relative render origin) is subtracted from each endpoint in DOUBLE before the float
// downcast, so edges at large world coordinates (UTM/GPS) stay precise and line up with the camera-relative scene.
// {0,0,0} yields fixed-frame coordinates.
void buildTfConnectionSegments(
    const TransformBuffer& tf, const std::string& fixed_frame, TimePoint time, std::vector<TfConnectionSegment>& out,
    const glm::dvec3& render_origin = glm::dvec3(0.0));

// Allocating convenience overload. Prefer the out-parameter version on the
// render hot path.
[[nodiscard]] std::vector<TfConnectionSegment> buildTfConnectionSegments(
    const TransformBuffer& tf, const std::string& fixed_frame, TimePoint time,
    const glm::dvec3& render_origin = glm::dvec3(0.0));

}  // namespace pj::scene3d
