// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Layer-level coverage for OccupancyGridLayer's two-topic '<base>_updates'
// wiring (review M.63): the UID-based store-window traversal that feeds
// OccupancyGridReconstructor is cross-checked against an independent
// cell-by-cell replay at several scrub targets (forward, backward, across
// epochs). Also regression-covers the retroactive-ingest invalidate path
// (review H.13: an update ingested behind the already-consumed live edge must
// still appear on the next forward render) and the base-keyframe parse memo
// (review M.47: re-rendering an unchanged keyframe must not re-parse it).

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QString>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "mock_parser_support.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/occupancy_grid.hpp"
#include "pj_base/builtin/occupancy_grid_update.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/layers/occupancy_grid_layer.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
using namespace Qt::StringLiterals;

namespace {

using namespace pj::scene3d::test;

constexpr std::string_view kBaseSchema = "mock/occupancy_grid";
constexpr std::string_view kUpdateSchema = "mock/occupancy_grid_update";

std::atomic<int> g_base_parser_calls{0};
std::atomic<int> g_update_parser_calls{0};

// Mock wire format: a uint8 header followed by row-major cell bytes.
//   base:   [width, height, cells...]      cells.size() == width * height
//   update: [x, y, width, height, cells...]

// Parses the base schema into an sdk::OccupancyGrid whose data Span is anchored
// to a private copy of the payload (the ObjectRecord must own its bytes).
PJ::Expected<PJ::sdk::ObjectRecord> emitBaseGrid(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  auto bytes =
      std::make_shared<std::vector<uint8_t>>(payload.bytes.data(), payload.bytes.data() + payload.bytes.size());
  PJ::sdk::OccupancyGrid grid;
  grid.timestamp_ns = ts;
  grid.frame_id = "map";
  grid.resolution = 1.0;
  grid.width = (*bytes)[0];
  grid.height = (*bytes)[1];
  grid.data = PJ::Span<const uint8_t>(bytes->data() + 2, bytes->size() - 2);
  grid.anchor = bytes;
  return PJ::sdk::ObjectRecord{.ts = ts, .object = grid};
}

// Parses the update schema into an sdk::OccupancyGridUpdate whose data Span is
// anchored to a private copy of the payload (the ObjectRecord must own its bytes).
PJ::Expected<PJ::sdk::ObjectRecord> emitGridUpdate(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  auto bytes =
      std::make_shared<std::vector<uint8_t>>(payload.bytes.data(), payload.bytes.data() + payload.bytes.size());
  PJ::sdk::OccupancyGridUpdate update;
  update.timestamp_ns = ts;
  update.frame_id = "map";
  update.x = (*bytes)[0];
  update.y = (*bytes)[1];
  update.width = (*bytes)[2];
  update.height = (*bytes)[3];
  update.data = PJ::Span<const uint8_t>(bytes->data() + 4, bytes->size() - 4);
  update.anchor = bytes;
  return PJ::sdk::ObjectRecord{.ts = ts, .object = update};
}

// --- Independent reference replay (mirror of the reconstructor contract) ---

struct RefBase {
  int64_t ts = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t fill = 0;
};

struct RefUpdate {
  int64_t ts = 0;
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t value = 0;
};

struct RefGrid {
  int64_t base_ts = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<int8_t> cells;
};

// displayed(t) = latest base with ts <= t, plus every update in (base_ts, t]
// applied in ascending-ts order (insertion order within equal-ts runs).
// Equal-ts bases: the LAST pushed wins (>=), matching the store's
// indexAtOrBefore pick — the store permits duplicate timestamps.
std::optional<RefGrid> refReplay(const std::vector<RefBase>& bases, std::vector<RefUpdate> updates, int64_t t) {
  const RefBase* base = nullptr;
  for (const auto& candidate : bases) {
    if (candidate.ts <= t && (base == nullptr || candidate.ts >= base->ts)) {
      base = &candidate;
    }
  }
  if (base == nullptr) {
    return std::nullopt;
  }
  RefGrid grid{
      base->ts, base->width, base->height,
      std::vector<int8_t>(static_cast<std::size_t>(base->width) * base->height, static_cast<int8_t>(base->fill))};
  std::stable_sort(updates.begin(), updates.end(), [](const RefUpdate& a, const RefUpdate& b) { return a.ts < b.ts; });
  for (const auto& update : updates) {
    if (update.ts <= base->ts || update.ts > t) {
      continue;
    }
    for (uint32_t row = 0; row < update.height; ++row) {
      for (uint32_t col = 0; col < update.width; ++col) {
        grid.cells[static_cast<std::size_t>(update.y + row) * grid.width + update.x + col] =
            static_cast<int8_t>(update.value);
      }
    }
  }
  return grid;
}

class OccupancyGridLayerUpdatesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_base_parser_calls = 0;
    g_update_parser_calls = 0;

    base_topic_ = registerObjectTopic(session_, "/map");
    updates_topic_ = registerObjectTopic(session_, "/map_updates");

    session_.registerObjectTopicParser(base_topic_, makeBoundHandle(kBaseSchema, []() noexcept -> void* {
                                         return new CountingObjectParser(
                                             kBaseSchema, PJ::sdk::BuiltinObjectType::kOccupancyGrid,
                                             &g_base_parser_calls, &emitBaseGrid);
                                       }));
    session_.registerObjectTopicParser(updates_topic_, makeBoundHandle(kUpdateSchema, []() noexcept -> void* {
                                         return new CountingObjectParser(
                                             kUpdateSchema, PJ::sdk::BuiltinObjectType::kOccupancyGridUpdate,
                                             &g_update_parser_calls, &emitGridUpdate);
                                       }));
  }

  PJ::ObjectStore& store() {
    return session_.objectStore();
  }

  // Pushes a uniformly-filled width x height base keyframe and mirrors it into
  // the reference timeline.
  void pushBase(int64_t ts, uint32_t width, uint32_t height, uint8_t fill) {
    std::vector<uint8_t> payload{static_cast<uint8_t>(width), static_cast<uint8_t>(height)};
    payload.insert(payload.end(), static_cast<std::size_t>(width) * height, fill);
    ASSERT_TRUE(store().pushOwned(base_topic_, ts, std::move(payload)).has_value());
    ref_bases_.push_back(RefBase{ts, width, height, fill});
  }

  // pushBase via a LAZY counting fetcher (pushLazyCounting), so a test can assert
  // whether a render actually resolved payload bytes (not just whether it re-parsed).
  void pushLazyBase(int64_t ts, uint32_t width, uint32_t height, uint8_t fill, std::shared_ptr<int> counter) {
    std::vector<uint8_t> payload{static_cast<uint8_t>(width), static_cast<uint8_t>(height)};
    payload.insert(payload.end(), static_cast<std::size_t>(width) * height, fill);
    pushLazyCounting(store(), base_topic_, ts, std::move(payload), std::move(counter));
    ref_bases_.push_back(RefBase{ts, width, height, fill});
  }

  // Pushes a uniformly-filled patch rect and mirrors it into the reference.
  void pushUpdate(int64_t ts, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint8_t value) {
    std::vector<uint8_t> payload{
        static_cast<uint8_t>(x), static_cast<uint8_t>(y), static_cast<uint8_t>(width), static_cast<uint8_t>(height)};
    payload.insert(payload.end(), static_cast<std::size_t>(width) * height, value);
    ASSERT_TRUE(store().pushOwned(updates_topic_, ts, std::move(payload)).has_value());
    ref_updates_.push_back(RefUpdate{ts, x, y, width, height, value});
  }

  // Renders the layer at t and asserts its reconstructed grid matches the
  // independent reference replay cell-by-cell.
  void renderAndExpectRef(pj::scene3d::OccupancyGridLayer& layer, int64_t t) {
    layer.renderAtForTest(t);
    const auto expected = refReplay(ref_bases_, ref_updates_, t);
    ASSERT_TRUE(expected.has_value()) << "reference has no base at t=" << t;
    const auto& grid = layer.reconstructedGridForTest();
    ASSERT_FALSE(grid.empty()) << "layer reconstructed an empty grid at t=" << t;
    EXPECT_EQ(grid.base_timestamp_ns, expected->base_ts) << "wrong epoch keyframe at t=" << t;
    ASSERT_EQ(grid.width, expected->width);
    ASSERT_EQ(grid.height, expected->height);
    EXPECT_EQ(grid.cells, expected->cells) << "reconstructed cells diverge from reference replay at t=" << t;
  }

  PJ::SessionManager session_;
  PJ::ObjectTopicId base_topic_;
  PJ::ObjectTopicId updates_topic_;
  std::vector<RefBase> ref_bases_;
  std::vector<RefUpdate> ref_updates_;
};

// M.63: the store-window arithmetic feeding the reconstructor, exercised at the
// layer boundary across forward steps, equal-ts update runs, backward seeks,
// and an epoch change — each target cross-checked against the reference replay.
TEST_F(OccupancyGridLayerUpdatesTest, ScrubReplayMatchesReference) {
  pushBase(100, 4, 4, 0);
  pushUpdate(110, 1, 1, 2, 2, 50);
  pushUpdate(120, 0, 0, 1, 1, 100);
  pushUpdate(120, 3, 3, 1, 1, 75);  // equal-ts run with the previous patch
  pushUpdate(130, 2, 0, 2, 1, 25);

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session_;
  pj::scene3d::OccupancyGridLayer layer(base_topic_, u"map"_s);
  ASSERT_TRUE(layer.attach(ctx));

  renderAndExpectRef(layer, 100);  // base only
  renderAndExpectRef(layer, 115);  // + first patch
  renderAndExpectRef(layer, 120);  // + the equal-ts pair
  renderAndExpectRef(layer, 130);  // everything
  renderAndExpectRef(layer, 110);  // backward seek inside the epoch
  renderAndExpectRef(layer, 125);  // forward again from the restored state

  // Epoch change: a second keyframe supersedes the first; updates before it no
  // longer apply, updates after it do.
  pushBase(300, 4, 4, 10);
  pushUpdate(310, 0, 3, 2, 1, 90);
  renderAndExpectRef(layer, 310);  // new epoch + its patch
  renderAndExpectRef(layer, 305);  // backward inside the new epoch (base2 only)
  renderAndExpectRef(layer, 125);  // backward across epochs, back to base1
}

// Out-of-order arrival in the updates topic: an update stamped with an OLDER ts
// but pushed LAST carries the newest SequentialUID. A time-window query
// (ObjectStore::rangeByTime) must include it by timestamp regardless of UID order.
// Here a newest-UID entry sits at ts=200 (outside the render window) while the
// out-of-order ts=150 patch inside the window must still be applied.
TEST_F(OccupancyGridLayerUpdatesTest, OutOfOrderArrivalUpdateInWindowIsApplied) {
  pushBase(100, 4, 4, 0);
  pushUpdate(110, 0, 0, 1, 1, 50);  // uid1, cell[0]
  pushUpdate(200, 3, 3, 1, 1, 60);  // uid2, cell[15] — newest UID, ts beyond the window
  pushUpdate(150, 1, 1, 1, 1, 70);  // uid3 (OOO arrival): newest UID, ts inside the window, cell[5]

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session_;
  pj::scene3d::OccupancyGridLayer layer(base_topic_, u"map"_s);
  ASSERT_TRUE(layer.attach(ctx));

  // Window (100, 160] contains ts=110 and ts=150 but NOT ts=200 (whose newest UID
  // must not pull it into the window).
  renderAndExpectRef(layer, 160);
  EXPECT_EQ(layer.reconstructedGridForTest().cells[5], static_cast<int8_t>(70))
      << "out-of-order in-window update must be applied";
  EXPECT_EQ(layer.reconstructedGridForTest().cells[15], static_cast<int8_t>(0))
      << "ts=200 update must stay outside the (100,160] window";
}

// Out-of-order arrival at-or-before the window's LOW edge must not cause an
// in-window update to be skipped. The in-window ts=120 patch is pushed first
// (small UID); then an out-of-order update stamped 95 (<= lo=100) is pushed last
// (largest UID). A timestamp-keyed window query (rangeByTime) still returns the
// ts=120 patch — a UID cursor starting past the ts=95 boundary would have dropped it.
TEST_F(OccupancyGridLayerUpdatesTest, OutOfOrderArrivalBelowWindowLowEdgeDoesNotSkipInWindowUpdate) {
  pushBase(100, 4, 4, 0);
  pushUpdate(120, 2, 2, 1, 1, 80);  // uid1: in-window, SMALL UID, cell[10]
  pushUpdate(95, 0, 0, 1, 1, 40);   // uid2 (OOO): ts <= lo=100, LARGEST UID

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session_;
  pj::scene3d::OccupancyGridLayer layer(base_topic_, u"map"_s);
  ASSERT_TRUE(layer.attach(ctx));

  // Window (100, 160]: ts=120 is in, ts=95 is out (<= lo). A latestAt(100) start
  // boundary resolves the ts=95 entry (largest UID) and skips ts=120.
  renderAndExpectRef(layer, 160);
  EXPECT_EQ(layer.reconstructedGridForTest().cells[10], static_cast<int8_t>(80))
      << "in-window update with a UID below the <=lo boundary entry was dropped";
}

// H.13: the live drive advances the consumed window past the updates topic's
// ingest; an update that lands later with ts at-or-before the consumed time
// must still be folded in (via reconstructor invalidate + full rebuild) instead
// of being skipped forever by the forward (last_t_, t] window.
TEST_F(OccupancyGridLayerUpdatesTest, RetroactiveUpdateIsReappliedAfterLiveEdgeOvershoot) {
  pushBase(100, 4, 4, 0);
  pushUpdate(110, 1, 1, 2, 2, 50);

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session_;
  pj::scene3d::OccupancyGridLayer layer(base_topic_, u"map"_s);
  ASSERT_TRUE(layer.attach(ctx));

  // The tracker is driven by a faster sibling topic's live edge, far past the
  // updates topic's newest entry (ts=110).
  renderAndExpectRef(layer, 200);

  // Transport jitter: an update stamped 150 is ingested AFTER time 200 was
  // already consumed. The plain forward window (200, 210] would never see it.
  pushUpdate(150, 0, 0, 1, 1, 100);
  renderAndExpectRef(layer, 210);
  EXPECT_EQ(layer.reconstructedGridForTest().cells[0], static_cast<int8_t>(100))
      << "update ingested behind the consumed live edge was dropped";

  // And the rebuild must not loop: another forward render keeps the patch.
  renderAndExpectRef(layer, 220);
}

// H.13b: the retroactive-ingest cursor must SETTLE, not re-invalidate every frame.
// H.13 above uses an in-order late update (ts after every retained entry), so its
// UID matches its timestamp order and the cursor advances normally. Here the late
// update is ALSO out-of-order in the array (older ts than an already-retained
// entry), so it lands at an earlier array slot yet carries the newest UID. The
// post-reconstruct cursor must be the max UID among entries with ts <= t (not
// latestAt(t)'s UID, which resolves the newest-TIMESTAMP entry and is smaller):
// otherwise the cursor parks below the late entry and re-detects it — a full
// rebuild every frame.
TEST_F(OccupancyGridLayerUpdatesTest, RetroactiveOutOfOrderUpdateSettlesCursorWithoutReinvalidateLoop) {
  pushBase(100, 4, 4, 0);
  pushUpdate(110, 0, 0, 1, 1, 50);  // uid1
  pushUpdate(160, 3, 3, 1, 1, 60);  // uid2 — newest timestamp so far

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session_;
  pj::scene3d::OccupancyGridLayer layer(base_topic_, u"map"_s);
  ASSERT_TRUE(layer.attach(ctx));

  renderAndExpectRef(layer, 200);  // consume base + 110 + 160; cursor settles

  // Late AND out-of-order: ts=150 (<= consumed 200) lands BEFORE the retained
  // ts=160 in the array but carries the newest UID. latestAt(200) still resolves
  // ts=160 (a lower UID), so a latestAt-keyed cursor never advances past ts=150.
  pushUpdate(150, 2, 2, 1, 1, 70);  // uid3 (out-of-order), cell[10]

  // Frame A: the retroactive change is detected once (invalidate) and applied.
  layer.renderAtForTest(200);
  const size_t invalidations_after_detect = layer.retroactiveInvalidateCountForTest();
  EXPECT_EQ(invalidations_after_detect, 1u) << "the retroactive change must be detected exactly once";
  EXPECT_EQ(layer.reconstructedGridForTest().cells[10], static_cast<int8_t>(70))
      << "retroactive out-of-order update must be folded in";

  // Frame B (same time, no new data): the cursor must have advanced past the late
  // entry, so no further invalidation fires. With a latestAt-keyed cursor the count
  // keeps rising — a rebuild every frame.
  layer.renderAtForTest(200);
  EXPECT_EQ(layer.retroactiveInvalidateCountForTest(), invalidations_after_detect)
      << "cursor did not settle: the late out-of-order update re-invalidates every frame";
  EXPECT_EQ(layer.reconstructedGridForTest().cells[10], static_cast<int8_t>(70))
      << "the folded-in update must survive the settled render";
}

// M.47: re-rendering while the active base keyframe is unchanged must reuse the
// memoized parse instead of re-parsing (and deep-copying) the cell payload on
// every tracker tick.
TEST_F(OccupancyGridLayerUpdatesTest, UnchangedBaseKeyframeIsNotReparsed) {
  pushBase(100, 4, 4, 0);
  pushUpdate(110, 1, 1, 2, 2, 50);

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session_;
  pj::scene3d::OccupancyGridLayer layer(base_topic_, u"map"_s);
  ASSERT_TRUE(layer.attach(ctx));  // bootstrap parses the first sample once

  renderAndExpectRef(layer, 120);  // first renderAt populates the memo
  const int base_parses_after_first_render = g_base_parser_calls.load();

  renderAndExpectRef(layer, 121);
  renderAndExpectRef(layer, 122);
  renderAndExpectRef(layer, 105);  // backward seek: same keyframe, memo still valid
  EXPECT_EQ(g_base_parser_calls.load(), base_parses_after_first_render)
      << "unchanged base keyframe was re-parsed on a later tick";

  // A new keyframe is a different store entry: exactly one more parse.
  pushBase(300, 4, 4, 10);
  renderAndExpectRef(layer, 300);
  EXPECT_EQ(g_base_parser_calls.load(), base_parses_after_first_render + 1);
}

// Resolve-before-memo regression: while the active base keyframe is unchanged,
// renderAt must not even RESOLVE its payload bytes — a resolve is a cold refetch
// (file read + whole-chunk decompress on the GUI thread) once the
// ResidentPayloadPool has evicted the entry's seed. The store's warm latestAt
// cache would mask a redundant resolve, so it is deliberately reset between
// renders via an out-of-order push — exactly the streaming condition that made
// the redundant resolve go cold in production.
TEST_F(OccupancyGridLayerUpdatesTest, UnchangedBaseKeyframeDoesNotResolvePayload) {
  auto fetch_count = std::make_shared<int>(0);
  pushLazyBase(100, 4, 4, 0, fetch_count);

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session_;
  pj::scene3d::OccupancyGridLayer layer(base_topic_, u"map"_s);
  ASSERT_TRUE(layer.attach(ctx));

  renderAndExpectRef(layer, 120);  // first renderAt resolves + parses + memoizes
  const int fetches_after_first_render = *fetch_count;
  const int parses_after_first_render = g_base_parser_calls.load();
  ASSERT_GT(fetches_after_first_render, 0);

  // Bust the warm latestAt cache (an out-of-order insert resets it): any resolve
  // issued by the next render now HAS to re-invoke the lazy fetcher.
  auto old_fetch_count = std::make_shared<int>(0);
  pushLazyBase(50, 4, 4, 7, old_fetch_count);

  renderAndExpectRef(layer, 121);  // same active keyframe -> metadata-only skip
  EXPECT_EQ(*fetch_count, fetches_after_first_render)
      << "unchanged base keyframe resolved payload bytes on a later tick";
  EXPECT_EQ(g_base_parser_calls.load(), parses_after_first_render);
  EXPECT_EQ(*old_fetch_count, 0) << "the out-of-order older keyframe must not be resolved at all";
}

// A replacement base keyframe at the SAME timestamp as the displayed one (the
// store permits duplicate stamps; indexAtOrBefore picks the last pushed) is a
// DIFFERENT store entry and must start a new epoch. The reconstructor keys the
// epoch on the base entry's identity, not its decoded timestamp — keyed on the
// timestamp alone, the forward path would keep showing the old keyframe's cells.
TEST_F(OccupancyGridLayerUpdatesTest, SameTimestampReplacementBaseStartsNewEpoch) {
  pushBase(100, 4, 4, 10);
  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session_;
  pj::scene3d::OccupancyGridLayer layer(base_topic_, u"map"_s);
  ASSERT_TRUE(layer.attach(ctx));
  renderAndExpectRef(layer, 100);
  ASSERT_EQ(layer.reconstructedGridForTest().cells[0], static_cast<int8_t>(10));

  pushBase(100, 4, 4, 33);  // same stamp, new entry, different cells
  renderAndExpectRef(layer, 100);
  EXPECT_EQ(layer.reconstructedGridForTest().cells[0], static_cast<int8_t>(33))
      << "same-timestamp replacement keyframe did not reset the epoch";
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
