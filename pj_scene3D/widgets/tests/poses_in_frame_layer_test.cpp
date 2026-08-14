// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Layer-level coverage for PosesInFrameLayer: tracker-time decode -> triad
// expansion, scrub coalescing (an unchanged sample is not re-parsed), style edits
// re-expanding the current sample, the empty/orphan edges, the per-use parseLocked
// rebind guard (a re-registered parser must not leave a dangling pointer), and the
// copy/paste/apply-to-family param round-trip (serializeLayerParams).

#include "pj_scene3d_widgets/layers/poses_in_frame_layer.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QString>
#include <atomic>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string_view>
#include <vector>

#include "mock_parser_support.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/poses_in_frame.hpp"
#include "pj_base/builtin/poses_in_frame_codec.hpp"  // serializePosesInFrame (canonical-blob, no-parser test)
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
#include "pj_scene_common/layer_params.h"
using namespace Qt::StringLiterals;

namespace {

using namespace pj::scene3d::test;

constexpr std::string_view kSchema = "mock/poses_in_frame";
std::atomic<int> g_parser_calls{0};

// Mock wire: a single uint8 pose-count. The decoded poses sit at (i+1, 0, 0) with
// identity orientation, all in frame "map".
PJ::Expected<PJ::sdk::ObjectRecord> emitPoses(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  PJ::sdk::PosesInFrame msg;
  msg.timestamp_ns = ts;
  msg.frame_id = "map";
  const uint8_t count = payload.bytes.empty() ? 0 : payload.bytes.data()[0];
  for (uint8_t i = 0; i < count; ++i) {
    PJ::sdk::Pose pose;
    pose.position = {static_cast<double>(i + 1), 0.0, 0.0};
    msg.poses.push_back(pose);
  }
  return PJ::sdk::ObjectRecord{.ts = ts, .object = msg};
}

class PosesInFrameLayerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_parser_calls = 0;
    topic_ = registerObjectTopic(session_, "/poses");
    session_.registerObjectTopicParser(topic_, makeBoundHandle(kSchema, []() noexcept -> void* {
                                         return new CountingObjectParser(
                                             kSchema, PJ::sdk::BuiltinObjectType::kPosesInFrame, &g_parser_calls,
                                             &emitPoses);
                                       }));
  }

  void pushPoses(int64_t ts, uint8_t count) {
    std::vector<uint8_t> payload{count};
    ASSERT_TRUE(session_.objectStore().pushOwned(topic_, ts, std::move(payload)).has_value());
  }

  // |model * local_+X|, i.e. the world-space length of an arm's axis -> the
  // triad arm length (== gizmo size for an identity-orientation pose).
  static float armLength(const pj::scene3d::PoseTriadInstance& inst) {
    return glm::length(glm::vec3(inst.model * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)));
  }

  PJ::SessionManager session_;
  PJ::ObjectTopicId topic_;
};

// Resolve-before-memo regression: an unchanged staged sample must skip on
// metadata alone WITHOUT resolving payload bytes — a resolve is a cold refetch
// (file read + whole-chunk decompress on the GUI thread) once the
// ResidentPayloadPool has evicted the entry's seed. The store's warm latestAt
// cache would mask a redundant resolve, so it is deliberately reset between
// renders via an out-of-order push.
TEST_F(PosesInFrameLayerTest, UnchangedSampleDoesNotResolvePayload) {
  auto fetch_count = std::make_shared<int>(0);
  pushLazyCounting(session_.objectStore(), topic_, 100, std::vector<uint8_t>{2}, fetch_count);
  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session_;
  pj::scene3d::PosesInFrameLayer layer(topic_, u"poses"_s);
  ASSERT_TRUE(layer.attach(ctx));

  layer.renderAtForTest(100);
  ASSERT_EQ(layer.instancesForTest().size(), 6U);  // 2 poses * 3 arms
  const int fetches_after_first_render = *fetch_count;
  const int parses_after_first_render = g_parser_calls.load();
  ASSERT_GT(fetches_after_first_render, 0);

  // Bust the warm latestAt cache (an out-of-order insert resets it): any resolve
  // issued by the next render now HAS to re-invoke the lazy fetcher.
  auto old_fetch_count = std::make_shared<int>(0);
  pushLazyCounting(session_.objectStore(), topic_, 50, std::vector<uint8_t>{1}, old_fetch_count);

  layer.renderAtForTest(101);  // same staged sample, same style -> metadata-only skip
  EXPECT_EQ(*fetch_count, fetches_after_first_render) << "unchanged sample resolved payload bytes on a later tick";
  EXPECT_EQ(g_parser_calls.load(), parses_after_first_render);
  EXPECT_EQ(*old_fetch_count, 0) << "the out-of-order older sample must not be resolved at all";
}

TEST_F(PosesInFrameLayerTest, DecodesPosesIntoThreeArmsEachAtTrackerTime) {
  pushPoses(100, 3);
  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session_;
  pj::scene3d::PosesInFrameLayer layer(topic_, u"poses"_s);
  ASSERT_TRUE(layer.attach(ctx));

  layer.renderAtForTest(100);
  EXPECT_EQ(layer.instancesForTest().size(), 9U);  // 3 poses * 3 arms
  EXPECT_EQ(layer.sourceFrame(), u"map"_s);
}

// A data-source/toolbox canonical topic carries serialized pj_base blobs and has
// NO MessageParser bound (the Mosaico toolbox path). The layer must still attach
// and decode the blob host-side via the canonical codec (resolveObject's
// fallback) — previously attach() rejected any parser-less object topic.
TEST_F(PosesInFrameLayerTest, RendersCanonicalBlobWithoutParser) {
  const PJ::ObjectTopicId canon_topic = registerObjectTopic(session_, "/poses_canonical");
  // Deliberately NO registerObjectTopicParser for this topic.
  PJ::sdk::PosesInFrame msg;
  msg.frame_id = "map";
  msg.timestamp_ns = 200;
  PJ::sdk::Pose pose;
  pose.position = {1.0, 2.0, 3.0};
  pose.orientation.w = 1.0;
  msg.poses.push_back(pose);
  std::vector<uint8_t> blob = PJ::serializePosesInFrame(msg);
  ASSERT_TRUE(session_.objectStore().pushOwned(canon_topic, 200, std::move(blob)).has_value());

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session_;
  pj::scene3d::PosesInFrameLayer layer(canon_topic, u"poses_canon"_s);
  ASSERT_TRUE(layer.attach(ctx));  // would FAIL before the canonical fallback: "no parser"

  layer.renderAtForTest(200);
  EXPECT_EQ(layer.instancesForTest().size(), 3U);  // 1 pose * 3 arms
  EXPECT_EQ(layer.sourceFrame(), u"map"_s);
  EXPECT_EQ(g_parser_calls.load(), 0);  // canonical path, parser never invoked
}

TEST_F(PosesInFrameLayerTest, UnchangedSampleIsNotRedecoded) {
  pushPoses(100, 2);
  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session_;
  pj::scene3d::PosesInFrameLayer layer(topic_, u"poses"_s);
  ASSERT_TRUE(layer.attach(ctx));

  layer.renderAtForTest(100);
  const int parses_after_first = g_parser_calls.load();
  layer.renderAtForTest(101);  // same latest sample (ts=100)
  layer.renderAtForTest(150);
  EXPECT_EQ(g_parser_calls.load(), parses_after_first) << "an unchanged sample was re-parsed on a later tick";
  EXPECT_EQ(layer.instancesForTest().size(), 6U);
}

TEST_F(PosesInFrameLayerTest, SizeEditReexpandsCurrentSample) {
  pushPoses(100, 1);
  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session_;
  pj::scene3d::PosesInFrameLayer layer(topic_, u"poses"_s);
  ASSERT_TRUE(layer.attach(ctx));

  layer.setGizmoSize(0.5f);
  layer.renderAtForTest(100);
  ASSERT_EQ(layer.instancesForTest().size(), 3U);
  EXPECT_NEAR(armLength(layer.instancesForTest()[0]), 0.5f, 1e-4f);

  layer.setGizmoSize(1.0f);
  layer.renderAtForTest(100);  // same sample, new style -> must re-expand
  ASSERT_EQ(layer.instancesForTest().size(), 3U);
  EXPECT_NEAR(armLength(layer.instancesForTest()[0]), 1.0f, 1e-4f);
}

TEST_F(PosesInFrameLayerTest, OpacityEditReexpandsCurrentSample) {
  pushPoses(100, 1);
  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session_;
  pj::scene3d::PosesInFrameLayer layer(topic_, u"poses"_s);
  ASSERT_TRUE(layer.attach(ctx));

  layer.setGizmoOpacity(0.4f);
  layer.renderAtForTest(100);
  ASSERT_EQ(layer.instancesForTest().size(), 3U);
  for (const auto& inst : layer.instancesForTest()) {
    EXPECT_NEAR(inst.color.a, 0.4f, 1e-5f);
  }
}

TEST_F(PosesInFrameLayerTest, EmptyPoseSetStagesNoArmsButKeepsFrame) {
  pushPoses(100, 0);
  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session_;
  pj::scene3d::PosesInFrameLayer layer(topic_, u"poses"_s);
  ASSERT_TRUE(layer.attach(ctx));

  layer.renderAtForTest(100);
  EXPECT_TRUE(layer.instancesForTest().empty());
  EXPECT_EQ(layer.sourceFrame(), u"map"_s);
}

// Per-use parseLocked guard: re-registering the topic's parser (a file reload)
// destroys the previous parser instance. A decode after that must resolve a fresh
// binding, never a cached/dangling one (ASAN would flag a use-after-free here).
TEST_F(PosesInFrameLayerTest, DecodeSurvivesParserReRegistration) {
  pushPoses(100, 2);
  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session_;
  pj::scene3d::PosesInFrameLayer layer(topic_, u"poses"_s);
  ASSERT_TRUE(layer.attach(ctx));
  layer.renderAtForTest(100);
  ASSERT_EQ(layer.instancesForTest().size(), 6U);

  // Reload: a brand-new parser instance replaces the one the first decode used.
  session_.registerObjectTopicParser(topic_, makeBoundHandle(kSchema, []() noexcept -> void* {
                                       return new CountingObjectParser(
                                           kSchema, PJ::sdk::BuiltinObjectType::kPosesInFrame, &g_parser_calls,
                                           &emitPoses);
                                     }));
  pushPoses(200, 4);
  layer.renderAtForTest(200);                       // must decode through the fresh binding
  EXPECT_EQ(layer.instancesForTest().size(), 12U);  // 4 poses * 3 arms
}

// X-arrow-only mode stages a single arm per pose (not a full triad), re-expanding
// the current sample when toggled.
TEST_F(PosesInFrameLayerTest, XArrowOnlyStagesOneArmPerPose) {
  pushPoses(100, 3);
  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session_;
  pj::scene3d::PosesInFrameLayer layer(topic_, u"poses"_s);
  ASSERT_TRUE(layer.attach(ctx));

  layer.renderAtForTest(100);
  ASSERT_EQ(layer.instancesForTest().size(), 9U);  // full triad: 3 poses * 3 arms
  layer.setXArrowOnly(true);
  layer.renderAtForTest(100);
  EXPECT_EQ(layer.instancesForTest().size(), 3U);  // X only: 3 poses * 1 arm
}

// Override color in X-only mode: the single arm carries the user-picked color.
TEST_F(PosesInFrameLayerTest, OverrideColorAppliesInXOnlyMode) {
  pushPoses(100, 1);
  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session_;
  pj::scene3d::PosesInFrameLayer layer(topic_, u"poses"_s);
  ASSERT_TRUE(layer.attach(ctx));

  layer.setXArrowOnly(true);
  layer.setOverrideColorEnabled(true);
  layer.setOverrideColor(QColor::fromRgbF(0.2f, 0.4f, 0.6f));
  layer.renderAtForTest(100);
  ASSERT_EQ(layer.instancesForTest().size(), 1U);
  const auto& c = layer.instancesForTest()[0].color;
  EXPECT_NEAR(c.r, 0.2f, 1e-2f);
  EXPECT_NEAR(c.g, 0.4f, 1e-2f);
  EXPECT_NEAR(c.b, 0.6f, 1e-2f);
}

// Override color also recolors the FULL triad (override is orthogonal to the
// X-only geometry toggle): every arm of every pose carries the picked color.
TEST_F(PosesInFrameLayerTest, OverrideColorRecolorsFullTriad) {
  pushPoses(100, 1);
  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session_;
  pj::scene3d::PosesInFrameLayer layer(topic_, u"poses"_s);
  ASSERT_TRUE(layer.attach(ctx));

  layer.setOverrideColorEnabled(true);  // full triad, x_arrow_only stays false
  layer.setOverrideColor(QColor::fromRgbF(0.2f, 0.4f, 0.6f));
  layer.renderAtForTest(100);
  ASSERT_EQ(layer.instancesForTest().size(), 3U);  // 1 pose * 3 arms, all recolored
  for (const auto& inst : layer.instancesForTest()) {
    EXPECT_NEAR(inst.color.r, 0.2f, 1e-2f);
    EXPECT_NEAR(inst.color.g, 0.4f, 1e-2f);
    EXPECT_NEAR(inst.color.b, 0.6f, 1e-2f);
  }
}

// Copy/paste/apply-to-family: serializeLayerParams(src) -> applyLayerParams(dst)
// must round-trip every per-layer param (size + opacity) between real layers.
TEST(PosesInFrameLayerParamTransfer, SerializeApplyRoundTripsEveryParam) {
  PJ::ObjectTopicId a;
  a.id = 1;
  PJ::ObjectTopicId b;
  b.id = 2;
  pj::scene3d::PosesInFrameLayer src(a, u"A"_s);
  pj::scene3d::PosesInFrameLayer dst(b, u"B"_s);

  src.setGizmoSize(0.42f);            // default 0.15
  src.setGizmoOpacity(0.33f);         // default 1.0
  src.setXArrowOnly(true);            // default false
  src.setOverrideColorEnabled(true);  // default false
  src.setOverrideColor(QColor(10, 20, 30));
  ASSERT_NE(dst.gizmoSize(), src.gizmoSize()) << "test premise: dst still at defaults before paste";

  const QString xml = PJ::serializeLayerParams(src);
  ASSERT_FALSE(xml.isEmpty());
  ASSERT_TRUE(PJ::applyLayerParams(dst, xml));

  EXPECT_FLOAT_EQ(dst.gizmoSize(), src.gizmoSize());
  EXPECT_FLOAT_EQ(dst.gizmoOpacity(), src.gizmoOpacity());
  EXPECT_EQ(dst.xArrowOnly(), src.xArrowOnly());
  EXPECT_EQ(dst.overrideColorEnabled(), src.overrideColorEnabled());
  EXPECT_EQ(dst.overrideColor(), src.overrideColor());
  EXPECT_EQ(PJ::serializeLayerParams(dst), xml);  // byte-identical blob
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
