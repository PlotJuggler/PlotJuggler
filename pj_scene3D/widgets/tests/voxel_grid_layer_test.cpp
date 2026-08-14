// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Behavioural coverage for VoxelGridLayer: lazy decode + sample-UID memo (a
// re-scrub to a seen grid does NO re-parse — the zero-per-voxel-CPU contract),
// new-sample re-decode, streaming attach before the first sample, the per-use
// parser binding, XML round-trip, and worldBounds.

#include "pj_scene3d_widgets/layers/voxel_grid_layer.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDomDocument>
#include <QString>
#include <atomic>
#include <memory>
#include <string_view>
#include <vector>

#include "mock_parser_support.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/builtin/voxel_grid.hpp"
#include "pj_base/time.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
using namespace Qt::StringLiterals;

namespace {

using namespace pj::scene3d::test;

constexpr std::string_view kSchema = "mock/voxel_grid";

std::atomic<int> g_parser_calls{0};
std::atomic<int> g_second_parser_calls{0};

// A fixed 2x2x2 occupancy grid (uint8 field, strides {1,2,4}); the static buffer
// outlives every parse so the zero-copy Span stays valid.
PJ::Expected<PJ::sdk::ObjectRecord> emitVoxel(PJ::Timestamp ts, PJ::sdk::PayloadView /*payload*/) {
  static const uint8_t k_data[8] = {0, 10, 0, 20, 0, 0, 30, 0};
  PJ::sdk::VoxelGrid grid;
  grid.timestamp_ns = ts;
  grid.frame_id = "map";
  grid.cell_size = {1.0, 1.0, 1.0};
  grid.column_count = 2;
  grid.row_count = 2;
  grid.slice_count = 2;
  grid.cell_stride = 1;
  grid.row_stride = 2;
  grid.slice_stride = 4;
  PJ::sdk::PointField field;
  field.name = "occupancy";
  field.offset = 0;
  field.datatype = PJ::sdk::PointField::Datatype::kUint8;
  field.count = 1;
  grid.fields = {field};
  grid.data = PJ::Span<const uint8_t>(k_data, 8);
  return PJ::sdk::ObjectRecord{.ts = ts, .object = grid};
}

void registerVoxelParser(PJ::SessionManager& session, PJ::ObjectTopicId topic_id) {
  session.registerObjectTopicParser(topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kSchema, PJ::sdk::BuiltinObjectType::kVoxelGrid, &g_parser_calls, &emitVoxel);
                                    }));
}

TEST(VoxelGridLayer, RescrubToSameGridDoesNoReparse) {
  g_parser_calls.store(0);
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const auto topic_id = registerObjectTopic(session, "/voxels");
  ASSERT_TRUE(store.pushOwned(topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  registerVoxelParser(session, topic_id);

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::VoxelGridLayer layer(topic_id, u"voxels"_s);
  ASSERT_TRUE(layer.attach(ctx));
  EXPECT_EQ(g_parser_calls.load(), 1);  // bootstrap decoded the first sample

  layer.renderAtForTest(100);
  const int after_first = g_parser_calls.load();
  EXPECT_GE(after_first, 1);
  EXPECT_TRUE(layer.hasGridForTest());
  EXPECT_EQ(layer.resolvedFieldForTest(), u"occupancy"_s);

  // Re-scrub to the SAME store entry: no re-parse, no re-pack (req: scrubbing a
  // previously-seen grid does zero per-voxel CPU work).
  layer.renderAtForTest(100);
  layer.renderAtForTest(100);
  EXPECT_EQ(g_parser_calls.load(), after_first) << "re-scrub to a cached grid must not re-parse";
}

// Resolve-before-memo regression: a re-scrub to the SAME grid must skip on
// metadata alone WITHOUT resolving payload bytes — a resolve is a cold refetch
// (file read + whole-chunk decompress on the GUI thread) once the
// ResidentPayloadPool has evicted the entry's seed. The store's warm latestAt
// cache would mask a redundant resolve, so it is deliberately reset between
// renders via an out-of-order push.
TEST(VoxelGridLayer, RescrubToSameGridDoesNotResolvePayload) {
  g_parser_calls.store(0);
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const auto topic_id = registerObjectTopic(session, "/voxels");
  auto fetch_count = std::make_shared<int>(0);
  pushLazyCounting(store, topic_id, 100, std::vector<uint8_t>{0x01}, fetch_count);
  registerVoxelParser(session, topic_id);

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::VoxelGridLayer layer(topic_id, u"voxels"_s);
  ASSERT_TRUE(layer.attach(ctx));
  layer.renderAtForTest(100);
  EXPECT_TRUE(layer.hasGridForTest());
  const int fetches_after_first_render = *fetch_count;
  const int parses_after_first_render = g_parser_calls.load();
  ASSERT_GT(fetches_after_first_render, 0);

  // Bust the warm latestAt cache (an out-of-order insert resets it): any resolve
  // issued by the next render now HAS to re-invoke the lazy fetcher.
  auto old_fetch_count = std::make_shared<int>(0);
  pushLazyCounting(store, topic_id, 50, std::vector<uint8_t>{0x01}, old_fetch_count);

  layer.renderAtForTest(101);  // same active grid, same field -> metadata-only skip
  EXPECT_EQ(*fetch_count, fetches_after_first_render) << "unchanged grid resolved payload bytes on a later tick";
  EXPECT_EQ(g_parser_calls.load(), parses_after_first_render);
  EXPECT_EQ(*old_fetch_count, 0) << "the out-of-order older grid must not be resolved at all";
}

// Regression (triple-review critical): scrubbing to before the first sample
// clears the pass; scrubbing back onto the SAME store entry must re-stage the grid.
// The bug was that renderAt's no-sample branch cleared the pass but left
// uploaded_uid_ intact, so the fast-path skipped the re-upload and the grid
// silently vanished for the rest of the session.
TEST(VoxelGridLayer, BackScrubAfterEmptyTickReStagesGrid) {
  g_parser_calls.store(0);
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const auto topic_id = registerObjectTopic(session, "/voxels");
  ASSERT_TRUE(store.pushOwned(topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  registerVoxelParser(session, topic_id);

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::VoxelGridLayer layer(topic_id, u"voxels"_s);
  ASSERT_TRUE(layer.attach(ctx));

  layer.renderAtForTest(100);
  EXPECT_TRUE(layer.passHasStagedGridForTest());

  // Scrub before the only sample: latestAt() returns nothing -> pass cleared.
  layer.renderAtForTest(50);
  EXPECT_FALSE(layer.passHasStagedGridForTest());

  // Scrub back onto the same entry: the grid must come back.
  layer.renderAtForTest(100);
  EXPECT_TRUE(layer.passHasStagedGridForTest()) << "grid must re-stage after a back-scrub through an empty tick";
}

TEST(VoxelGridLayer, NewSampleReDecodes) {
  g_parser_calls.store(0);
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const auto topic_id = registerObjectTopic(session, "/voxels");
  ASSERT_TRUE(store.pushOwned(topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  registerVoxelParser(session, topic_id);

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::VoxelGridLayer layer(topic_id, u"voxels"_s);
  ASSERT_TRUE(layer.attach(ctx));
  layer.renderAtForTest(100);
  const int after_first = g_parser_calls.load();

  // A second, distinct store entry → a fresh UID → must re-decode.
  ASSERT_TRUE(store.pushOwned(topic_id, 200, std::vector<uint8_t>{0x02}).has_value());
  layer.renderAtForTest(200);
  EXPECT_GT(g_parser_calls.load(), after_first) << "a new sample must trigger a re-decode";
}

TEST(VoxelGridLayer, StreamingAttachBeforeSampleSelfHeals) {
  g_parser_calls.store(0);
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const auto topic_id = registerObjectTopic(session, "/voxels");
  registerVoxelParser(session, topic_id);  // parser, but NO sample yet

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::VoxelGridLayer layer(topic_id, u"voxels"_s);
  ASSERT_TRUE(layer.attach(ctx)) << "attach must tolerate an empty store";
  EXPECT_EQ(g_parser_calls.load(), 0) << "bootstrap on an empty store must not parse";

  layer.renderAtForTest(100);
  EXPECT_FALSE(layer.hasGridForTest());

  ASSERT_TRUE(store.pushOwned(topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  layer.renderAtForTest(100);
  EXPECT_GE(g_parser_calls.load(), 1);
  EXPECT_TRUE(layer.hasGridForTest());
  EXPECT_EQ(layer.sourceFrame(), u"map"_s);
}

TEST(VoxelGridLayer, RebindsToReRegisteredParser) {
  g_parser_calls.store(0);
  g_second_parser_calls.store(0);
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const auto topic_id = registerObjectTopic(session, "/voxels");
  ASSERT_TRUE(store.pushOwned(topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  registerVoxelParser(session, topic_id);  // first parser counts g_parser_calls

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::VoxelGridLayer layer(topic_id, u"voxels"_s);
  ASSERT_TRUE(layer.attach(ctx));  // bootstrap decodes via the first parser

  // Keep the first parser alive so the buggy stale-pointer path (if reintroduced)
  // is a deterministic wrong-counter call rather than a use-after-free crash.
  const auto stale_guard = session.parserKeepaliveForObjectTopic(topic_id);
  ASSERT_NE(stale_guard, nullptr);

  // Re-register the topic's parser (the file-reload swap). The layer resolves the
  // binding per use, so the next decode must reach the NEW parser. bootstrap() did
  // not populate the parse memo, so renderAt re-decodes through the fresh binding.
  session.registerObjectTopicParser(topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kSchema, PJ::sdk::BuiltinObjectType::kVoxelGrid, &g_second_parser_calls,
                                          &emitVoxel);
                                    }));

  const int stale_before = g_parser_calls.load();
  layer.renderAtForTest(100);
  EXPECT_EQ(g_parser_calls.load(), stale_before) << "layer called the replaced parser after reload";
  EXPECT_GE(g_second_parser_calls.load(), 1) << "layer must rebind to the re-registered parser";
}

TEST(VoxelGridLayer, WorldBoundsReflectGrid) {
  g_parser_calls.store(0);
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const auto topic_id = registerObjectTopic(session, "/voxels");
  ASSERT_TRUE(store.pushOwned(topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  registerVoxelParser(session, topic_id);

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::VoxelGridLayer layer(topic_id, u"voxels"_s);
  ASSERT_TRUE(layer.attach(ctx));
  layer.renderAtForTest(100);

  const auto bounds = layer.worldBounds();
  ASSERT_TRUE(bounds.has_value());
  EXPECT_FLOAT_EQ(bounds->min.x, 0.0f);
  EXPECT_FLOAT_EQ(bounds->max.x, 2.0f);  // 2 cells * 1.0 m
  EXPECT_FLOAT_EQ(bounds->max.z, 2.0f);
}

TEST(VoxelGridLayer, XmlRoundTrip) {
  PJ::SessionManager session;
  const auto topic_id = registerObjectTopic(session, "/voxels");

  pj::scene3d::VoxelGridLayer source(topic_id, u"voxels"_s);
  source.setActiveField(u"cost"_s);
  source.setDrawMode(pj::scene3d::VoxelDrawMode::kThreshold);
  source.setThreshold(0.75);
  source.setAutoRange(false);
  source.setManualRange(-2.0, 8.0);
  source.setColormap(PJ::Colormap::kViridis);
  source.setOpacity(0.5);

  QDomDocument doc;
  const QDomElement saved = source.xmlSaveState(doc);

  pj::scene3d::VoxelGridLayer restored(topic_id, u"voxels"_s);
  int configuration_changes = 0;
  QObject::connect(&restored, &PJ::ISceneLayer::configurationChanged, &restored, [&configuration_changes]() {
    ++configuration_changes;
  });
  ASSERT_TRUE(restored.xmlLoadState(saved));
  EXPECT_GT(configuration_changes, 0) << "user-facing XML parameter paste must reach the workspace undo path";
  configuration_changes = 0;
  ASSERT_TRUE(restored.xmlLoadState(saved));
  EXPECT_EQ(configuration_changes, 0) << "re-applying identical parameters is not a workspace mutation";

  // Re-serialize and compare: every persisted attribute must survive the trip.
  QDomDocument doc2;
  const QDomElement re_saved = restored.xmlSaveState(doc2);
  EXPECT_EQ(re_saved.attribute("field"), u"cost"_s);
  EXPECT_EQ(re_saved.attribute("draw_mode").toInt(), static_cast<int>(pj::scene3d::VoxelDrawMode::kThreshold));
  EXPECT_DOUBLE_EQ(re_saved.attribute("threshold").toDouble(), 0.75);
  EXPECT_EQ(re_saved.attribute("auto_range").toInt(), 0);
  EXPECT_DOUBLE_EQ(re_saved.attribute("range_lo").toDouble(), -2.0);
  EXPECT_DOUBLE_EQ(re_saved.attribute("range_hi").toDouble(), 8.0);
  EXPECT_EQ(re_saved.attribute("colormap").toInt(), static_cast<int>(PJ::Colormap::kViridis));
  EXPECT_DOUBLE_EQ(re_saved.attribute("opacity").toDouble(), 0.5);
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
