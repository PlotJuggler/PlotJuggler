// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <vector>

#include "pj_widgets/StateTransitionsView.h"
#include "state_transitions_test_helpers.h"
using namespace Qt::StringLiterals;

namespace {

using PJ::StateRow;
using PJ::StateTransitionsScene;
using PJ::TimelineViewport;

TEST(StateTransitionsScene, RowBookkeeping) {
  StateTransitionsScene scene;
  scene.setRows({makeRow(1, {seg(0, 10, "A")}), makeRow(2, {seg(0, 5, "B")})});
  EXPECT_EQ(scene.rows().size(), 2U);
  EXPECT_EQ(scene.rowIndexOf(1), 0);
  EXPECT_EQ(scene.rowIndexOf(2), 1);
  EXPECT_EQ(scene.rowIndexOf(99), -1);

  // updateRow replaces in place, preserving order.
  StateRow replacement = makeRow(1, {seg(0, 20, "A2")});
  EXPECT_TRUE(scene.updateRow(replacement));
  EXPECT_EQ(scene.rows().front().segments.front().value, u"A2"_s);
  EXPECT_EQ(scene.rowIndexOf(1), 0);
  EXPECT_FALSE(scene.updateRow(makeRow(99, {})));

  EXPECT_TRUE(scene.removeRow(1));
  EXPECT_EQ(scene.rowIndexOf(2), 0);
  EXPECT_FALSE(scene.removeRow(1));
}

TEST(StateTransitionsScene, ExtentUnionAndDefaultFallback) {
  StateTransitionsScene scene;
  // Empty: reports the default extent; setDefaultExtent overrides it.
  scene.setDefaultExtent(5 * kSecondNs, 25 * kSecondNs);
  EXPECT_EQ(scene.sceneExtent().min, 5 * kSecondNs);
  EXPECT_EQ(scene.sceneExtent().max, 25 * kSecondNs);
  // Degenerate default is ignored.
  scene.setDefaultExtent(9, 9);
  EXPECT_EQ(scene.sceneExtent().max, 25 * kSecondNs);

  // A row with no segments contributes nothing; the union spans all others.
  scene.setRows({
      makeRow(1, {seg(10, 20, "A"), seg(20, 30, "B")}),
      makeRow(2, {}),
      makeRow(3, {seg(2, 8, "X")}),
  });
  EXPECT_EQ(scene.sceneExtent().min, 2 * kSecondNs);
  EXPECT_EQ(scene.sceneExtent().max, 30 * kSecondNs);
}

TEST(StateTransitionsScene, RowLayoutArithmetic) {
  constexpr StateTransitionsScene::RowMetrics kMetrics = StateTransitionsScene::rowMetrics();
  EXPECT_DOUBLE_EQ(StateTransitionsScene::rowHeight(), kMetrics.chip_h + kMetrics.band_h + kMetrics.gap);
  EXPECT_DOUBLE_EQ(StateTransitionsScene::rowTop(0), StateTransitionsScene::kTopPad);
  EXPECT_DOUBLE_EQ(
      StateTransitionsScene::rowTop(3), StateTransitionsScene::kTopPad + (3 * StateTransitionsScene::rowHeight()));
  EXPECT_DOUBLE_EQ(StateTransitionsScene::bandTop(0), StateTransitionsScene::rowTop(0) + kMetrics.chip_h);

  StateTransitionsScene scene;
  EXPECT_DOUBLE_EQ(scene.contentHeight(), StateTransitionsScene::kTopPad);
  scene.setRows({makeRow(1, {}), makeRow(2, {})});
  EXPECT_DOUBLE_EQ(scene.contentHeight(), StateTransitionsScene::kTopPad + (2 * StateTransitionsScene::rowHeight()));
}

TEST(StateTransitionsScene, HitTestFindsSegmentsHalfOpen) {
  StateTransitionsScene scene;
  scene.setRows({
      makeRow(1, {seg(0, 10, "A"), seg(10, 20, "B")}),
      makeRow(2, {seg(5, 15, "C")}),
  });
  // 1 px per second, origin at 0.
  const TimelineViewport vp{.px_per_ns = 1.0 / kSecondNs, .origin_ns = 0};

  const double band0_y = StateTransitionsScene::bandTop(0) + 1.0;
  const double band1_y = StateTransitionsScene::bandTop(1) + 1.0;

  // Inside segment A.
  auto hit = scene.hitTest(4.0, band0_y, vp);
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(hit->row, 0);
  EXPECT_EQ(hit->segment, 0);

  // Half-open boundary: exactly t=10s belongs to B, not A.
  hit = scene.hitTest(10.0, band0_y, vp);
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(hit->segment, 1);
  // The final end is exclusive: t=20s hits nothing.
  EXPECT_FALSE(scene.hitTest(20.0, band0_y, vp).has_value());
  // Before the first segment: nothing.
  EXPECT_FALSE(scene.hitTest(4.0, band1_y, vp).has_value());
  // Second row hits its own segment list.
  hit = scene.hitTest(6.0, band1_y, vp);
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(hit->row, 1);
  EXPECT_EQ(hit->segment, 0);

  // A gap between segments misses.
  scene.setRows({makeRow(1, {seg(0, 4, "A"), seg(8, 12, "B")})});
  EXPECT_FALSE(scene.hitTest(6.0, band0_y, vp).has_value());

  // Outside the band strip (chip and label area, or the gap below) misses.
  EXPECT_FALSE(scene.hitTest(2.0, StateTransitionsScene::rowTop(0) + 1.0, vp).has_value());
  EXPECT_FALSE(scene.hitTest(2.0, StateTransitionsScene::kTopPad - 2.0, vp).has_value());
  // Below the last row misses.
  EXPECT_FALSE(scene.hitTest(2.0, StateTransitionsScene::rowTop(5), vp).has_value());
}

}  // namespace
