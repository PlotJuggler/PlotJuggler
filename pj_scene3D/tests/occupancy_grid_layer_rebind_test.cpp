// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Regression test for the file-reload crash: SessionManager::replaceDataset()
// re-registers each surviving topic's parser slot, destroying the previous
// MessageParserHandle. A layer that cached the raw parser pointer at attach()
// then calls parseObject() on freed memory on the next tracker tick. Layers
// must instead resolve the parser binding through the session on every use,
// so a reload transparently rebinds them to the new parser.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QString>
#include <atomic>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "mock_parser_support.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/occupancy_grid.hpp"
#include "pj_base/time.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/layers/occupancy_grid_layer.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
using namespace Qt::StringLiterals;

namespace {

using namespace pj::scene3d::test;

constexpr std::string_view kSchema = "mock/occupancy_grid";

std::atomic<int> g_first_parser_calls{0};
std::atomic<int> g_second_parser_calls{0};
std::atomic<int> g_streaming_parser_calls{0};

// Object-construction body for the mock grid parser: a fixed 1x1 grid. Counting
// is handled by CountingObjectParser, so this is just the emit half (same
// signature as SchemaHandler::parse_object).
PJ::Expected<PJ::sdk::ObjectRecord> emitGrid(PJ::Timestamp ts, PJ::sdk::PayloadView /*payload*/) {
  static const uint8_t k_cell[1] = {0};
  PJ::sdk::OccupancyGrid grid;
  grid.timestamp_ns = ts;
  grid.frame_id = "map";
  grid.resolution = 0.05;
  grid.width = 1;
  grid.height = 1;
  grid.data = PJ::Span<const uint8_t>(k_cell, 1);
  return PJ::sdk::ObjectRecord{.ts = ts, .object = grid};
}

TEST(OccupancyGridLayerRebind, ReloadSwapsParserWithoutTouchingStaleOne) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();

  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = 1;
  desc.topic_name = "/map";
  const auto topic_id = store.registerTopic(desc);
  ASSERT_TRUE(topic_id.has_value());
  ASSERT_TRUE(store.pushOwned(*topic_id, 100, std::vector<uint8_t>{0x01}).has_value());

  session.registerObjectTopicParser(*topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
    return new CountingObjectParser(
        kSchema, PJ::sdk::BuiltinObjectType::kOccupancyGrid, &g_first_parser_calls, &emitGrid);
  }));

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::OccupancyGridLayer layer(*topic_id, u"map"_s);
  ASSERT_TRUE(layer.attach(ctx));
  EXPECT_EQ(g_first_parser_calls.load(), 1);  // bootstrap decoded the first sample

  // Keep the first parser's memory alive from the test, so the buggy
  // stale-pointer call below shows up as a deterministic wrong-parser call
  // instead of undefined behavior. Production holds no such guard — there the
  // same call is a use-after-free crash.
  const auto stale_guard = session.parserKeepaliveForObjectTopic(*topic_id);
  ASSERT_NE(stale_guard, nullptr);

  // Simulate the same-file reload: replaceDataset() re-registers the surviving
  // topic's parser under its stable id, overwriting the slot and dropping the
  // old handle.
  session.registerObjectTopicParser(*topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
    return new CountingObjectParser(
        kSchema, PJ::sdk::BuiltinObjectType::kOccupancyGrid, &g_second_parser_calls, &emitGrid);
  }));

  const int stale_calls_before = g_first_parser_calls.load();
  layer.setTrackerTime(PJ::fromRaw(100));
  layer.renderAtForTest(100);

  EXPECT_EQ(g_first_parser_calls.load(), stale_calls_before)
      << "layer called the replaced (freed-in-production) parser after the reload swap";
  EXPECT_GE(g_second_parser_calls.load(), 1) << "layer did not rebind to the re-registered parser";
}

// M.21/M.22: a grid topic attached before its first sample (layout restore at
// stream start, catalog drag onto a live session) must NOT be dropped. attach()
// returns true as long as session + parser binding exist; bootstrap failing on
// an empty store is a warn-and-continue, and the layer self-heals once a sample
// lands. The sibling pointcloud/scene-entities layers already behave this way.
TEST(OccupancyGridLayerStreamingAttach, AttachSucceedsBeforeFirstSampleAndSelfHeals) {
  g_streaming_parser_calls.store(0);

  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();

  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = 1;
  desc.topic_name = "/map";
  const auto topic_id = store.registerTopic(desc);
  ASSERT_TRUE(topic_id.has_value());

  // Parser registered, but NO sample pushed yet — the streaming-start case.
  // The create function must be non-capturing (a plain fn ptr for vtableWithCreate),
  // so it routes through a file-scope counter rather than a captured local.
  session.registerObjectTopicParser(*topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
    return new CountingObjectParser(
        kSchema, PJ::sdk::BuiltinObjectType::kOccupancyGrid, &g_streaming_parser_calls, &emitGrid);
  }));

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::OccupancyGridLayer layer(*topic_id, u"map"_s);
  ASSERT_TRUE(layer.attach(ctx)) << "attach must tolerate an empty store and keep the layer";
  EXPECT_EQ(g_streaming_parser_calls.load(), 0) << "bootstrap on an empty store must not parse anything";

  // A render before any sample is a no-op (empty reconstruction), not a crash.
  layer.setTrackerTime(PJ::fromRaw(100));
  layer.renderAtForTest(100);
  EXPECT_TRUE(layer.reconstructedGridForTest().empty());

  // First sample arrives post-attach: the layer self-heals on the next render.
  ASSERT_TRUE(store.pushOwned(*topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  layer.setTrackerTime(PJ::fromRaw(100));
  layer.renderAtForTest(100);

  EXPECT_GE(g_streaming_parser_calls.load(), 1) << "layer did not decode the first sample after it arrived";
  const auto& grid = layer.reconstructedGridForTest();
  EXPECT_FALSE(grid.empty());
  EXPECT_EQ(grid.frame_id, "map");
}

}  // namespace
