// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Headless TrailLayer tests (no GL, no session): renderKey coalescing and split
// counts, timeRange sentinel, XML round-trip (style-only load), synchronous TF
// rebuild against a hand-fed TransformBuffer, and live-edge append with the
// ring cap. GUI-thread-only Qt pieces run under a QCoreApplication.

#include "pj_scene3d_widgets/layers/trail_layer.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDomDocument>
#include <QThread>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/time.hpp"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/render_pass.h"

namespace pj::scene3d {
namespace {

using namespace std::chrono_literals;
using namespace Qt::StringLiterals;

constexpr TimePoint tp(std::chrono::nanoseconds ns) {
  return TimePoint{ns};
}

StampedTransform stampedAt(std::chrono::nanoseconds ns, std::string parent, std::string child, double x) {
  StampedTransform tf;
  tf.stamp = tp(ns);
  tf.parent_frame = std::move(parent);
  tf.child_frame = std::move(child);
  tf.transform.t = glm::dvec3(x, 0.0, 0.0);
  return tf;
}

TrailPolyline linePolyline(int count, int64_t stamp_step_ns) {
  TrailPolyline polyline;
  polyline.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    polyline.push_back(
        TrailPoint{tp(std::chrono::nanoseconds(i * stamp_step_ns)), glm::dvec3(static_cast<double>(i), 0.0, 0.0)});
  }
  return polyline;
}

TEST(TrailLayerTest, RenderKeyStableBetweenSamplesAndSplitCounts) {
  TrailLayer layer(PJ::ObjectTopicId{42}, TrailSource::tfFrame(u"base"_s));
  layer.adoptPolylineForTest(linePolyline(5, 100));

  EXPECT_EQ(layer.pastCountForTest(tp(-1ns)), 0U);
  EXPECT_EQ(layer.pastCountForTest(tp(0ns)), 1U);
  EXPECT_EQ(layer.pastCountForTest(tp(250ns)), 3U);
  EXPECT_EQ(layer.pastCountForTest(tp(9999ns)), 5U);

  // Scrubbing BETWEEN samples must not change the key (repaint coalescing) ...
  EXPECT_EQ(layer.renderKey(tp(210ns)), layer.renderKey(tp(290ns)));
  // ... crossing a sample must.
  EXPECT_NE(layer.renderKey(tp(290ns)), layer.renderKey(tp(310ns)));
  // A style edit must repaint even at the same time.
  const uint64_t key_before = layer.renderKey(tp(250ns));
  layer.setPastColor(QColor(u"#ff0000"_s));
  EXPECT_NE(layer.renderKey(tp(250ns)), key_before);
}

TEST(TrailLayerTest, TimeRangeIsSpanOrInvertedSentinel) {
  TrailLayer layer(PJ::ObjectTopicId{42}, TrailSource::tfFrame(u"base"_s));
  const auto empty_range = layer.timeRange();
  EXPECT_GT(empty_range.min, empty_range.max);  // inverted: must not widen the timeline

  TrailPolyline polyline;
  polyline.push_back(TrailPoint{tp(100ns), glm::dvec3(0.0)});
  polyline.push_back(TrailPoint{tp(900ns), glm::dvec3(1.0, 0.0, 0.0)});
  layer.adoptPolylineForTest(std::move(polyline));
  EXPECT_EQ(layer.timeRange().min, tp(100ns));
  EXPECT_EQ(layer.timeRange().max, tp(900ns));
}

TEST(TrailLayerTest, XmlRoundTripStyleOnly) {
  TrailLayer layer(PJ::ObjectTopicId{42}, TrailSource::tfFrame(u"base_link"_s));
  layer.setPastColor(QColor(u"#112233"_s));
  layer.setFutureColor(QColor(u"#445566"_s));
  layer.setThickness(5.0F);
  EXPECT_FLOAT_EQ(layer.thickness(), 5.0F);  // default is 4.0F
  layer.setPastVisible(false);
  EXPECT_TRUE(layer.futureVisible());  // toggles are independent
  QDomDocument doc;
  const QDomElement saved = layer.xmlSaveState(doc);
  EXPECT_EQ(saved.tagName(), u"trail"_s);
  EXPECT_EQ(saved.attribute(u"source_kind"_s), u"tf_frame"_s);
  EXPECT_EQ(saved.attribute(u"frame"_s), u"base_link"_s);

  TrailLayer restored(PJ::ObjectTopicId{43}, TrailSource::tfFrame(u"other"_s));
  ASSERT_TRUE(restored.xmlLoadState(saved));
  EXPECT_EQ(restored.pastColor().name(), u"#112233"_s);
  EXPECT_EQ(restored.futureColor().name(), u"#445566"_s);
  EXPECT_FLOAT_EQ(restored.thickness(), 5.0F);
  EXPECT_FALSE(restored.pastVisible());
  EXPECT_TRUE(restored.futureVisible());
  EXPECT_EQ(restored.source().frame, u"other"_s);  // load NEVER retargets the source

  QDomElement bad_color = doc.createElement(u"trail"_s);
  bad_color.setAttribute(u"past_color"_s, u"not-a-color"_s);
  EXPECT_FALSE(restored.xmlLoadState(bad_color));

  QDomElement bad_thickness = doc.createElement(u"trail"_s);
  bad_thickness.setAttribute(u"thickness"_s, u"nan"_s);
  EXPECT_FALSE(restored.xmlLoadState(bad_thickness));

  QDomElement wrong_tag = doc.createElement(u"poses_in_frame"_s);
  EXPECT_FALSE(restored.xmlLoadState(wrong_tag));

  // A pose-topic trail persists its kind but no frame attribute.
  const TrailLayer pose_trail(PJ::ObjectTopicId{44}, TrailSource::poseTopic(PJ::ObjectTopicId{7}));
  const QDomElement pose_saved = pose_trail.xmlSaveState(doc);
  EXPECT_EQ(pose_saved.attribute(u"source_kind"_s), u"pose_topic"_s);
  EXPECT_FALSE(pose_saved.hasAttribute(u"frame"_s));
}

TEST(TrailLayerTest, XmlLoadStateResultInstallsSourceOrDefers) {
  TrailLayer saved(PJ::ObjectTopicId{42}, TrailSource::tfFrame(u"base_link"_s));
  saved.setPastColor(QColor(u"#112233"_s));
  QDomDocument doc;
  const QDomElement tf_payload = saved.xmlSaveState(doc);

  // TF source installs sessionless (orphan-until-frame-appears philosophy).
  TrailLayer restored(PJ::ObjectTopicId{43});
  ASSERT_EQ(restored.xmlLoadStateResult(tf_payload), TrailLayer::XmlLoadResult::kRestored);
  EXPECT_EQ(restored.source().kind, TrailSource::Kind::kTfFrame);
  EXPECT_EQ(restored.source().frame, u"base_link"_s);
  EXPECT_EQ(restored.pastColor().name(), u"#112233"_s);

  // A pose source needs the session's resolution ladder: without one, defer.
  QDomElement pose_payload = doc.createElement(u"trail"_s);
  pose_payload.setAttribute(u"source_kind"_s, u"pose_topic"_s);
  pose_payload.setAttribute(u"source_topic_name"_s, u"/odom"_s);
  TrailLayer pose_restored(PJ::ObjectTopicId{44});
  EXPECT_EQ(pose_restored.xmlLoadStateResult(pose_payload), TrailLayer::XmlLoadResult::kDeferred);

  QDomElement bad = doc.createElement(u"trail"_s);
  bad.setAttribute(u"source_kind"_s, u"warp_drive"_s);
  EXPECT_EQ(pose_restored.xmlLoadStateResult(bad), TrailLayer::XmlLoadResult::kInvalid);
}

TEST(TrailLayerTest, SynchronousTfRebuildAgainstBuffer) {
  auto buffer = std::make_shared<TransformBuffer>(TransformBuffer::kKeepAll);
  (void)buffer->setTransform(stampedAt(10ns, "odom", "base", 1.0));
  (void)buffer->setTransform(stampedAt(20ns, "odom", "base", 2.0));

  TrailLayer layer(PJ::ObjectTopicId{42}, TrailSource::tfFrame(u"base"_s));
  layer.injectContextForTest(buffer);
  layer.rebuildNowForTest(u"odom"_s);
  EXPECT_EQ(layer.pointCountForTest(), 2U);
  EXPECT_TRUE(layer.statusWarning().isEmpty());

  // Disconnected fixed frame -> empty trail + a warning naming both frames.
  layer.rebuildNowForTest(u"mars"_s);
  EXPECT_EQ(layer.pointCountForTest(), 0U);
  EXPECT_FALSE(layer.statusWarning().isEmpty());
}

TEST(TrailLayerTest, LiveAppendGrowsTrailAndRespectsRevisionGate) {
  auto buffer = std::make_shared<TransformBuffer>(TransformBuffer::kKeepAll);
  (void)buffer->setTransform(stampedAt(10ns, "odom", "base", 1.0));
  (void)buffer->setTransform(stampedAt(20ns, "odom", "base", 2.0));

  TrailLayer layer(PJ::ObjectTopicId{42}, TrailSource::tfFrame(u"base"_s));
  layer.injectContextForTest(buffer);
  layer.rebuildNowForTest(u"odom"_s);
  ASSERT_EQ(layer.pointCountForTest(), 2U);
  const uint64_t key_before = layer.renderKey(tp(9999ns));

  // No TF change -> a tracker tick appends nothing (revision gate).
  layer.setTrackerTime(tp(20ns));
  EXPECT_EQ(layer.pointCountForTest(), 2U);

  // Two newer stamps arrive (streaming): the next tick appends exactly those.
  (void)buffer->setTransform(stampedAt(30ns, "odom", "base", 3.0));
  (void)buffer->setTransform(stampedAt(40ns, "odom", "base", 4.0));
  layer.setTrackerTime(tp(40ns));
  EXPECT_EQ(layer.pointCountForTest(), 4U);
  EXPECT_EQ(layer.timeRange().max, tp(40ns));
  EXPECT_NE(layer.renderKey(tp(9999ns)), key_before);
}

// Spin the event loop until `done` returns true (QFutureWatcher::finished is
// a queued signal, so the async rebuild only lands through the loop).
template <typename Predicate>
void spinUntil(Predicate&& done) {
  for (int i = 0; i < 400 && !done(); ++i) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(5);
  }
}

TEST(TrailLayerTest, RenderSelfHealsFixedFrameAndAsyncRebuildLands) {
  auto buffer = std::make_shared<TransformBuffer>(TransformBuffer::kKeepAll);
  (void)buffer->setTransform(stampedAt(10ns, "odom", "base", 1.0));
  (void)buffer->setTransform(stampedAt(20ns, "odom", "base", 2.0));

  TrailLayer layer(PJ::ObjectTopicId{42}, TrailSource::tfFrame(u"base"_s));
  layer.injectContextForTest(buffer);

  // The PRODUCTION rebuild entry point: render() adopts the fixed frame from
  // the FrameContext and launches the ASYNC rebuild (the GL pass is
  // uninitialized, so render() touches no GL).
  const std::string fixed_frame = "odom";
  const FrameContext frame_ctx{*buffer, fixed_frame, tp(20ns), glm::dvec3(0.0)};
  ViewParams view_params;
  layer.render(view_params, frame_ctx);
  spinUntil([&layer]() { return layer.pointCountForTest() > 0; });
  ASSERT_EQ(layer.pointCountForTest(), 2U);
  EXPECT_TRUE(layer.statusWarning().isEmpty());

  // Latest-wins: adopting a DISCONNECTED fixed frame mid-flight must end on
  // the newest parameters — an empty trail plus a warning, never the stale one.
  const std::string mars = "mars";
  const FrameContext mars_ctx{*buffer, mars, tp(20ns), glm::dvec3(0.0)};
  layer.render(view_params, mars_ctx);
  layer.render(view_params, mars_ctx);  // second call while possibly inflight: coalesced
  spinUntil([&layer]() { return !layer.statusWarning().isEmpty(); });
  EXPECT_EQ(layer.pointCountForTest(), 0U);
  EXPECT_FALSE(layer.statusWarning().isEmpty());
}

TEST(TrailLayerTest, EmptyTrailRevivesWhenTfDataArrives) {
  auto buffer = std::make_shared<TransformBuffer>(TransformBuffer::kKeepAll);
  // The tracked frame does not exist yet: the initial build is empty.
  (void)buffer->setTransform(stampedAt(10ns, "odom", "decoy", 1.0));

  TrailLayer layer(PJ::ObjectTopicId{42}, TrailSource::tfFrame(u"base"_s));
  layer.injectContextForTest(buffer);
  layer.rebuildNowForTest(u"odom"_s);
  ASSERT_EQ(layer.pointCountForTest(), 0U);
  ASSERT_FALSE(layer.statusWarning().isEmpty());

  // The frame starts publishing: the next tracker tick must REVIVE the trail
  // (a full rebuild), not stay stuck behind the empty-trail append gate.
  (void)buffer->setTransform(stampedAt(30ns, "odom", "base", 3.0));
  (void)buffer->setTransform(stampedAt(40ns, "odom", "base", 4.0));
  layer.setTrackerTime(tp(40ns));
  spinUntil([&layer]() { return layer.pointCountForTest() > 0; });
  EXPECT_EQ(layer.pointCountForTest(), 2U);
  EXPECT_TRUE(layer.statusWarning().isEmpty());
}

}  // namespace
}  // namespace pj::scene3d
