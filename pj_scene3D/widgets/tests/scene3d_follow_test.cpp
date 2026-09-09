// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Camera "follow a frame" (Position-only): the active camera's anchor tracks the
// followed frame's origin (in the fixed frame) each tracker tick, so the view pans
// to keep the frame in place while orbit/zoom stay world-referenced.
//
// View-level tests drive a bare SceneViewWidget against a hand-built TransformBuffer
// (no parser, no GL — the follow math is Qt/GL-free); dock-level tests cover the
// forwarding slot + per-dock layout persistence. Headless like the other dock tests:
// the GL view defers all GL to initializeGL().

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QDomElement>
#include <QString>
#include <chrono>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <string>

#include "mock_parser_support.h"  // pumpUntil
#include "pj_base/time.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/camera/camera.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_core/tf/transform.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_scene3d_widgets/transform_service.h"
using namespace Qt::StringLiterals;

namespace {

using pj::scene3d::SceneViewWidget;
using pj::scene3d::StampedTransform;
using pj::scene3d::TimePoint;
using pj::scene3d::TransformBuffer;

PJ::Timepoint at(int64_t ns) {
  return PJ::Timepoint{std::chrono::nanoseconds(ns)};
}

// A single world->child edge sampled at one stamp, identity rotation, translation t.
void setEdge(TransformBuffer& buf, const char* parent, const char* child, int64_t ns, const glm::dvec3& t) {
  StampedTransform st;
  st.stamp = TimePoint{std::chrono::nanoseconds(ns)};
  st.parent_frame = parent;
  st.child_frame = child;
  st.transform.t = t;
  st.transform.q = glm::dquat{1.0, 0.0, 0.0, 0.0};  // identity
  EXPECT_TRUE(buf.setTransform(st).has_value());
}

// --- View: the camera pivot tracks a moving followed frame. ---
TEST(Scene3DFollow, OrbitPivotTracksMovingFrame) {
  auto buf = std::make_shared<TransformBuffer>(TransformBuffer::kKeepAll);
  setEdge(*buf, "world", "robot", 100, {0.0, 0.0, 0.0});
  setEdge(*buf, "world", "robot", 200, {1.0, 0.0, 0.0});  // robot drove +1 in x by t=200

  SceneViewWidget view;  // default OrbitCamera, focal at origin
  view.setTransformBuffer(buf);
  view.setFixedFrame("world");
  view.setFollowFrame("robot");

  view.setTrackerTime(at(100));  // seed at robot origin (0,0,0) — no shift
  const glm::vec3 focal0 = view.camera().state().focal;
  view.setTrackerTime(at(200));  // robot now at (1,0,0) → pivot shifts +1 in x
  const glm::vec3 focal1 = view.camera().state().focal;

  EXPECT_NEAR(focal1.x - focal0.x, 1.0f, 1e-4f) << "pivot must track the followed frame's motion";
  EXPECT_NEAR(focal1.y - focal0.y, 0.0f, 1e-4f);
  EXPECT_NEAR(focal1.z - focal0.z, 0.0f, 1e-4f);
}

// --- View: enabling follow on a STATIONARY frame causes no jump. ---
TEST(Scene3DFollow, EnablingFollowDoesNotJump) {
  auto buf = std::make_shared<TransformBuffer>(TransformBuffer::kKeepAll);
  setEdge(*buf, "world", "robot", 100, {5.0, 5.0, 5.0});  // static, off-origin

  SceneViewWidget view;
  const glm::vec3 focal_initial = view.camera().state().focal;
  view.setTransformBuffer(buf);
  view.setFixedFrame("world");
  view.setFollowFrame("robot");
  view.setTrackerTime(at(100));  // seed
  view.setTrackerTime(at(200));  // still (5,5,5) → zero delta

  const glm::vec3 focal_after = view.camera().state().focal;
  EXPECT_NEAR(focal_after.x, focal_initial.x, 1e-4f) << "no recenter / no jump on enable";
  EXPECT_NEAR(focal_after.y, focal_initial.y, 1e-4f);
  EXPECT_NEAR(focal_after.z, focal_initial.z, 1e-4f);
}

// --- View: with follow off, tracker time never moves the camera. ---
TEST(Scene3DFollow, FollowOffLeavesCameraStill) {
  auto buf = std::make_shared<TransformBuffer>(TransformBuffer::kKeepAll);
  setEdge(*buf, "world", "robot", 100, {0.0, 0.0, 0.0});
  setEdge(*buf, "world", "robot", 200, {9.0, 9.0, 9.0});

  SceneViewWidget view;
  const glm::vec3 focal_initial = view.camera().state().focal;
  view.setTransformBuffer(buf);
  view.setFixedFrame("world");
  // no setFollowFrame
  view.setTrackerTime(at(100));
  view.setTrackerTime(at(200));

  const glm::vec3 focal_after = view.camera().state().focal;
  EXPECT_NEAR(focal_after.x, focal_initial.x, 1e-4f);
  EXPECT_NEAR(focal_after.y, focal_initial.y, 1e-4f);
  EXPECT_NEAR(focal_after.z, focal_initial.z, 1e-4f);
}

// --- View: followRenderKey is 0 when off, non-zero when following, and changes
//     as the followed frame moves (so the repaint gate can't coalesce it away). ---
TEST(Scene3DFollow, FollowRenderKeyTracksTargetPose) {
  auto buf = std::make_shared<TransformBuffer>(TransformBuffer::kKeepAll);
  setEdge(*buf, "world", "robot", 100, {0.0, 0.0, 0.0});
  setEdge(*buf, "world", "robot", 200, {1.0, 0.0, 0.0});

  SceneViewWidget view;
  view.setTransformBuffer(buf);
  view.setFixedFrame("world");
  EXPECT_EQ(view.followRenderKey(at(200)), 0U) << "0 when not following";

  view.setFollowFrame("robot");
  const uint64_t key_t100 = view.followRenderKey(at(100));
  const uint64_t key_t200 = view.followRenderKey(at(200));
  EXPECT_NE(key_t100, 0U) << "non-zero while following a resolvable frame";
  EXPECT_NE(key_t100, key_t200) << "must change as the followed frame moves";
}

// --- View: changing the fixed frame while following re-seeds (no jump). The
//     followed origin is cached in the OLD fixed frame; without a re-seed the next
//     tick would shift the camera by the bogus cross-basis delta. ---
TEST(Scene3DFollow, FixedFrameChangeWhileFollowingDoesNotJump) {
  auto buf = std::make_shared<TransformBuffer>(TransformBuffer::kKeepAll);
  setEdge(*buf, "world", "alt", 100, {10.0, 0.0, 0.0});  // alt offset from world (static)
  setEdge(*buf, "world", "robot", 100, {0.0, 0.0, 0.0});
  setEdge(*buf, "world", "robot", 200, {1.0, 0.0, 0.0});
  setEdge(*buf, "world", "robot", 300, {2.0, 0.0, 0.0});

  SceneViewWidget view;
  view.setTransformBuffer(buf);
  view.setFixedFrame("world");
  view.setFollowFrame("robot");
  view.setTrackerTime(at(100));  // seed
  view.setTrackerTime(at(200));  // robot at (1,0,0) → pivot shifted
  const glm::vec3 focal_before = view.camera().state().focal;

  // robot's origin in `alt` (-8,0,0) is wildly different from in `world` (2,0,0):
  // the first tick after the basis change must NOT apply that difference.
  view.setFixedFrame("alt");
  view.setTrackerTime(at(300));

  const glm::vec3 focal_after = view.camera().state().focal;
  EXPECT_NEAR(focal_after.x, focal_before.x, 1e-4f) << "fixed-frame change must re-seed, not jump";
  EXPECT_NEAR(focal_after.y, focal_before.y, 1e-4f);
  EXPECT_NEAR(focal_after.z, focal_before.z, 1e-4f);
}

// --- View: a tick where the followed frame is unresolvable (before its first
//     sample) holds the camera; once it appears the next tick re-seeds (no jump)
//     and subsequent motion tracks. ---
TEST(Scene3DFollow, HoldsThroughLookupGapThenResumes) {
  auto buf = std::make_shared<TransformBuffer>(TransformBuffer::kKeepAll);
  setEdge(*buf, "world", "robot", 100, {0.0, 0.0, 0.0});
  setEdge(*buf, "world", "robot", 200, {3.0, 0.0, 0.0});

  SceneViewWidget view;
  const glm::vec3 focal_initial = view.camera().state().focal;
  view.setTransformBuffer(buf);
  view.setFixedFrame("world");
  view.setFollowFrame("robot");

  view.setTrackerTime(at(50));  // before robot's first sample → lookup fails → hold
  EXPECT_NEAR(view.camera().state().focal.x, focal_initial.x, 1e-4f) << "unresolvable target must not move the camera";

  view.setTrackerTime(at(100));  // robot appears at origin → seed (no shift)
  EXPECT_NEAR(view.camera().state().focal.x, focal_initial.x, 1e-4f) << "seeding tick after the gap must not jump";

  view.setTrackerTime(at(200));  // robot at (3,0,0) → tracks
  EXPECT_NEAR(view.camera().state().focal.x - focal_initial.x, 3.0f, 1e-4f) << "tracks again after re-seeding";
}

// --- View: recenterOnFollowFrame moves the pivot onto the followed frame. ---
TEST(Scene3DFollow, RecenterOnFollowFrameCentersCameraOnTarget) {
  auto buf = std::make_shared<TransformBuffer>(TransformBuffer::kKeepAll);
  setEdge(*buf, "world", "robot", 100, {3.0, 4.0, 5.0});  // static, off-origin

  SceneViewWidget view;  // default OrbitCamera, focal at the origin
  view.setTransformBuffer(buf);
  view.setFixedFrame("world");
  view.setFollowFrame("robot");
  view.setTrackerTime(at(100));  // seed only — Position-follow does NOT recenter on enable
  ASSERT_NEAR(view.camera().state().focal.x, 0.0f, 1e-4f) << "precondition: not yet centered";

  view.recenterOnFollowFrame();

  const glm::vec3 focal = view.camera().state().focal;
  EXPECT_NEAR(focal.x, 3.0f, 1e-4f) << "recenter snaps the pivot onto the followed frame's origin";
  EXPECT_NEAR(focal.y, 4.0f, 1e-4f);
  EXPECT_NEAR(focal.z, 5.0f, 1e-4f);
}

// --- Dock: the follow target round-trips through layout XML. ---
TEST(Scene3DFollow, DockFollowFrameRoundTripsThroughLayout) {
  PJ::SessionManager save_session;
  pj::scene3d::TransformService save_tf(save_session);
  PJ::Scene3DDockWidget save_dock;
  save_dock.setSessionManager(&save_session);
  save_dock.setTransformService(&save_tf);
  ASSERT_TRUE(pj::scene3d::test::pumpUntil([&] { return save_dock.sceneView() != nullptr; }));

  save_dock.setFollowFrame(u"base_link"_s);
  ASSERT_EQ(save_dock.currentFollowFrame(), u"base_link"_s);

  QDomDocument doc;
  const QDomElement state = save_dock.xmlSaveState(doc);
  ASSERT_FALSE(state.isNull());
  EXPECT_EQ(state.attribute(u"follow_frame"_s), u"base_link"_s);

  PJ::SessionManager load_session;
  pj::scene3d::TransformService load_tf(load_session);
  PJ::Scene3DDockWidget load_dock;
  load_dock.setSessionManager(&load_session);
  load_dock.setTransformService(&load_tf);
  ASSERT_TRUE(load_dock.xmlLoadState(state));
  EXPECT_EQ(load_dock.currentFollowFrame(), u"base_link"_s) << "the follow target must restore from the layout";
}

}  // namespace
