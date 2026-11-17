// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// File-reload crash: SessionManager::replaceDataset() re-registers each surviving topic's parser slot, destroying the
// previous MessageParserHandle; a layer that cached the raw parser pointer at attach() would call parseObject() on
// freed memory. Layers must resolve the parser binding through the session on every use, so a reload rebinds them to
// the new parser.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QString>
#include <atomic>
#include <cstdint>
#include <limits>
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
// The per-frame hook the renderer drives, given all the view's cell budget.
constexpr std::uint64_t kNoBudget = std::numeric_limits<std::uint64_t>::max();

std::atomic<int> g_first_parser_calls{0};
std::atomic<int> g_second_parser_calls{0};
std::atomic<int> g_streaming_parser_calls{0};

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

  // The test keeps the first parser's memory alive so a stale-pointer call shows up as a deterministic wrong-parser
  // call instead of undefined behavior (production has no such guard: a use-after-free).
  const auto stale_guard = session.parserKeepaliveForObjectTopic(*topic_id);
  ASSERT_NE(stale_guard, nullptr);

  // Simulate the same-file reload: replaceDataset() re-registers the surviving topic's parser under its stable id,
  // dropping the old handle.
  session.registerObjectTopicParser(*topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
    return new CountingObjectParser(
        kSchema, PJ::sdk::BuiltinObjectType::kOccupancyGrid, &g_second_parser_calls, &emitGrid);
  }));

  const int stale_calls_before = g_first_parser_calls.load();
  layer.setTrackerTime(PJ::fromRaw(100));
  layer.prepareForRender(kNoBudget);

  EXPECT_EQ(g_first_parser_calls.load(), stale_calls_before)
      << "layer called the replaced (freed-in-production) parser after the reload swap";
  EXPECT_GE(g_second_parser_calls.load(), 1) << "layer did not rebind to the re-registered parser";
}

// A grid topic attached before its first sample (layout restore at stream start, drag onto a live session) must NOT be
// dropped: attach() returns true while session + parser binding exist, a bootstrap failing on an empty store is
// warn-and-continue, and the layer self-heals once a sample lands (as the pointcloud and scene-entities layers do).
TEST(OccupancyGridLayerStreamingAttach, AttachSucceedsBeforeFirstSampleAndSelfHeals) {
  g_streaming_parser_calls.store(0);

  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();

  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = 1;
  desc.topic_name = "/map";
  const auto topic_id = store.registerTopic(desc);
  ASSERT_TRUE(topic_id.has_value());

  // Parser registered, NO sample pushed yet (streaming start). The create function must be non-capturing (a plain fn
  // ptr for vtableWithCreate), so it routes through a file-scope counter.
  session.registerObjectTopicParser(*topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
    return new CountingObjectParser(
        kSchema, PJ::sdk::BuiltinObjectType::kOccupancyGrid, &g_streaming_parser_calls, &emitGrid);
  }));

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::OccupancyGridLayer layer(*topic_id, u"map"_s);
  ASSERT_TRUE(layer.attach(ctx)) << "attach must tolerate an empty store and keep the layer";
  EXPECT_EQ(g_streaming_parser_calls.load(), 0) << "bootstrap on an empty store must not parse anything";

  layer.setTrackerTime(PJ::fromRaw(100));
  layer.prepareForRender(kNoBudget);
  EXPECT_TRUE(layer.grid().empty());

  ASSERT_TRUE(store.pushOwned(*topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  layer.setTrackerTime(PJ::fromRaw(100));
  layer.prepareForRender(kNoBudget);

  EXPECT_GE(g_streaming_parser_calls.load(), 1) << "layer did not decode the first sample after it arrived";
  const auto& grid = layer.grid();
  EXPECT_FALSE(grid.empty());
  EXPECT_EQ(grid.frame_id, "map");
}

}  // namespace
