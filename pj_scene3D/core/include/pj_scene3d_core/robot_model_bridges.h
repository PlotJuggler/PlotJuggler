#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Turns a URDF's FIXED joints into static TF edges, so a robot model renders links the data's /tf never places (e.g.
// a gripper whose mount transform lives only in the URDF). Depends only on robot_model and the TF buffer (no Qt, no
// GL).
//
// FIXED only: a fixed joint has a constant transform, a safe /tf_static-style edge; movable joints need a joint-angle
// source and are not injected. The recording's own /tf always wins: injection never overrides a frame the data
// publishes.

#include <vector>

#include "pj_scene3d_core/robot_model.h"
#include "pj_tf/tf_buffer.h"
#include "pj_tf/transform.h"

namespace pj::scene3d {

using pj::tf::StampedTransform;
using pj::tf::TimePoint;
using pj::tf::Transform;
using pj::tf::TransformBuffer;

// One static StampedTransform per FIXED joint in `model` (parent_frame = joint.parent, child_frame = joint.child),
// stamped at `stamp`. Movable joints (and joints missing a parent or child) are skipped. The default `stamp` is the
// epoch sentinel, so the single-sample edge resolves at every real query time via nearest-previous (the /tf_static
// semantic) and the buffer never evicts it.
[[nodiscard]] std::vector<StampedTransform> fixedJointStaticTransforms(
    const RobotModel& model, TimePoint stamp = TimePoint{});

// Insert each edge into `buf` ONLY when its child_frame currently has no parent
// (TransformBuffer::getParent == nullopt) — so a frame the data already publishes
// is never reparented. (The buffer is first-parent-wins: a conflicting reparent
// would drop the real edge forever.) Idempotent — an already-present bridge is
// skipped on a re-run, which is what makes the layer's per-tick call cheap and
// self-healing after a buffer clear. Returns the number of edges inserted.
int injectMissingStaticTransforms(TransformBuffer& buf, const std::vector<StampedTransform>& edges);

}  // namespace pj::scene3d
