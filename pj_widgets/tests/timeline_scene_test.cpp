// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include "pj_widgets/Timeline.h"

namespace PJ {
namespace {

TimelineSpanInput makeTrack(TimelineSourceId id, qint64 lo, qint64 hi, qint64 offset = 0) {
  return TimelineSpanInput{.id = id, .t_min_ns = lo, .t_max_ns = hi, .offset_ns = offset};
}

// setTracks / tracks round trip
TEST(TimelineScene, SetTracksRoundTrips) {
  TimelineScene scene;
  scene.setTracks({makeTrack(1, 1000, 5000), makeTrack(2, 2000, 6000)});
  ASSERT_EQ(scene.tracks().size(), 2u);
  EXPECT_EQ(scene.tracks()[0].id, 1u);
  EXPECT_EQ(scene.tracks()[1].t_max_ns, 6000);
}

// displayWindow
TEST(TimelineScene, DisplayWindowSubtractsOffset) {
  // display = raw - offset: a positive offset shifts the source earlier.
  const auto w = TimelineScene::displayWindow(makeTrack(1, 1000, 5000, 200));
  EXPECT_EQ(w.min, 800);
  EXPECT_EQ(w.max, 4800);
}

TEST(TimelineScene, DisplayWindowNegativeOffsetShiftsLater) {
  const auto w = TimelineScene::displayWindow(makeTrack(1, 1000, 5000, -300));
  EXPECT_EQ(w.min, 1300);
  EXPECT_EQ(w.max, 5300);
}

// sceneExtent
TEST(TimelineScene, SceneExtentUnionsDisplayedWindows) {
  TimelineScene scene;
  scene.setTracks({makeTrack(1, 1000, 5000, 0), makeTrack(2, 3000, 9000, 1000)});
  // track2 displayed: [2000, 8000]. union with [1000,5000] => [1000, 8000].
  const auto ext = scene.sceneExtent();
  EXPECT_EQ(ext.min, 1000);
  EXPECT_EQ(ext.max, 8000);
}

TEST(TimelineScene, SceneExtentEmptyReturns60sDefault) {
  TimelineScene scene;
  const auto ext = scene.sceneExtent();
  EXPECT_EQ(ext.min, 0);
  EXPECT_EQ(ext.max, 60'000'000'000);  // 60 s in ns
}

TEST(TimelineScene, SetDefaultExtentOverridesEmptySpan) {
  TimelineScene scene;
  // The host sets the empty span to the playback range (10 s here) so a data-less
  // timeline is exactly as long as the playback.
  scene.setDefaultExtent(0, 10'000'000'000);
  EXPECT_EQ(scene.sceneExtent().min, 0);
  EXPECT_EQ(scene.sceneExtent().max, 10'000'000'000);
  // An invalid (max <= min) range is ignored, keeping the prior span.
  scene.setDefaultExtent(5, 5);
  EXPECT_EQ(scene.sceneExtent().max, 10'000'000'000);
}

// nsToPx, pxToNs, barSpan
TEST(TimelineScene, NsPxRoundTrip) {
  const TimelineViewport vp{.px_per_ns = 1e-3, .origin_ns = 1000};  // 1 px per 1000 ns
  EXPECT_DOUBLE_EQ(TimelineScene::nsToPx(2000, vp), 1.0);           // (2000-1000)*1e-3
  EXPECT_DOUBLE_EQ(TimelineScene::nsToPx(1000, vp), 0.0);
  EXPECT_EQ(TimelineScene::pxToNs(1.0, vp), 2000);
  EXPECT_EQ(TimelineScene::pxToNs(0.0, vp), 1000);
}

TEST(TimelineScene, BarSpanGeometry) {
  const TimelineViewport vp{.px_per_ns = 1e-3, .origin_ns = 0};
  const auto span = TimelineScene::barSpan(makeTrack(1, 2000, 6000, 1000), vp);
  // displayed [1000, 5000] -> x = 1000*1e-3 = 1.0, width = 4000*1e-3 = 4.0
  EXPECT_DOUBLE_EQ(span.x, 1.0);
  EXPECT_DOUBLE_EQ(span.width, 4.0);
}

// pxDeltaToNs and zoom
TEST(TimelineScene, PxDeltaToNsIsOriginIndependent) {
  const TimelineViewport vp{.px_per_ns = 1e-3, .origin_ns = 999999};
  EXPECT_EQ(TimelineScene::pxDeltaToNs(5.0, vp), 5000);  // 5 px / 1e-3
  EXPECT_EQ(TimelineScene::pxDeltaToNs(-2.0, vp), -2000);
}

TEST(TimelineScene, ZoomKeepsNsUnderAnchorFixed) {
  const TimelineViewport vp{.px_per_ns = 1e-3, .origin_ns = 0};
  const qint64 ns_at_anchor = TimelineScene::pxToNs(100.0, vp);  // 100000 ns
  const TimelineViewport zoomed = TimelineScene::zoom(vp, 2.0, 100.0);
  EXPECT_DOUBLE_EQ(zoomed.px_per_ns, 2e-3);
  // ns under the anchor pixel is unchanged after zoom.
  EXPECT_EQ(TimelineScene::pxToNs(100.0, zoomed), ns_at_anchor);
}

// ruler
TEST(TimelineScene, RulerPicksIntervalAndTicks) {
  // px_per_ns = 1e-3 (1 px per 1000 ns = per 1 us). target 80 px/tick => want
  // ~80000 ns/tick; the ladder's 100000 ns (100 us) is the first >= want.
  const TimelineViewport vp{.px_per_ns = 1e-3, .origin_ns = 0};
  const auto r = TimelineScene::ruler(vp, /*width_px=*/500.0, /*target_px_per_tick=*/80.0);
  EXPECT_EQ(r.interval_ns, 100'000);
  ASSERT_FALSE(r.ticks_ns.empty());
  EXPECT_EQ(r.ticks_ns.front(), 0);                 // first tick aligned to interval at origin
  for (size_t i = 1; i < r.ticks_ns.size(); ++i) {  // monotone, evenly spaced
    EXPECT_EQ(r.ticks_ns[i] - r.ticks_ns[i - 1], 100'000);
  }
  // visible span = 500 px / 1e-3 = 5e5 ns; last tick must not exceed it.
  EXPECT_LE(r.ticks_ns.back(), 500'000);
}

TEST(TimelineScene, RulerHandlesNegativeOrigin) {
  const TimelineViewport vp{.px_per_ns = 1e-3, .origin_ns = -250'000};
  const auto r = TimelineScene::ruler(vp, 500.0, 80.0);
  EXPECT_EQ(r.interval_ns, 100'000);
  // first tick is the smallest interval-multiple >= origin (-250000 -> -200000).
  EXPECT_EQ(r.ticks_ns.front(), -200'000);
}

// Degenerate extent guard: a wall-clock-epoch streaming source unioned with relative-time file datasets blows the span
// up to ~20,000 days, where the widest ladder interval (1 h) would pack hundreds of thousands of ticks into the
// viewport. The guard widens the interval so labels stay legible (>= ~48 px apart) and the tick count stays bounded by
// the pixel budget, not thousands (which smears the ruler black and makes every repaint pay for it).
TEST(TimelineScene, RulerBoundsTickCountForDegenerateExtent) {
  // ~1.73e18 ns span over a 1200 px view -> px_per_ns ~6.9e-16 (the screenshot case).
  const TimelineViewport vp{.px_per_ns = 1200.0 / 1.73e18, .origin_ns = 1'816'585'200'000'000LL};
  constexpr double kWidth = 1200.0;
  const auto r = TimelineScene::ruler(vp, kWidth, /*target_px_per_tick=*/80.0);

  // Bounded by the pixel budget (kMinTickSpacingPx == 48), not the 10000 runaway cap.
  EXPECT_LE(static_cast<int>(r.ticks_ns.size()), 30);
  ASSERT_GE(r.ticks_ns.size(), 2u);
  // Adjacent ticks are at least ~48 px apart (legible, no overpaint smear).
  for (size_t i = 1; i < r.ticks_ns.size(); ++i) {
    const double gap_px = static_cast<double>(r.ticks_ns[i] - r.ticks_ns[i - 1]) * vp.px_per_ns;
    EXPECT_GE(gap_px, 40.0);
  }
}

// alignStartsToCommonOrigin
TEST(TimelineScene, AlignStartsLeftmostUnmovedOthersSnap) {
  TimelineScene scene;
  // displayed starts: t1 raw.min 1000 off 0 -> 1000 (leftmost);
  //                   t2 raw.min 5000 off 0 -> 5000.
  scene.setTracks({makeTrack(1, 1000, 4000, 0), makeTrack(2, 5000, 9000, 0)});
  const auto offsets = scene.alignStartsToCommonOrigin();
  ASSERT_EQ(offsets.size(), 2u);
  // target = min displayed start = 1000. new_offset = raw.min - target.
  EXPECT_EQ(offsets[0].first, 1u);
  EXPECT_EQ(offsets[0].second, 0);  // 1000 - 1000  (leftmost unmoved)
  EXPECT_EQ(offsets[1].first, 2u);
  EXPECT_EQ(offsets[1].second, 4000);  // 5000 - 1000  (shifts earlier to start at 1000)
}

TEST(TimelineScene, AlignStartsIsIdempotent) {
  TimelineScene scene;
  scene.setTracks({makeTrack(1, 1000, 4000, 0), makeTrack(2, 5000, 9000, 4000)});
  // already aligned (both displayed starts == 1000); re-align changes nothing.
  const auto offsets = scene.alignStartsToCommonOrigin();
  EXPECT_EQ(offsets[0].second, 0);
  EXPECT_EQ(offsets[1].second, 4000);
}

TEST(TimelineScene, AlignStartsEmptyIsEmpty) {
  TimelineScene scene;
  EXPECT_TRUE(scene.alignStartsToCommonOrigin().empty());
}

// alignCentersToCommonOrigin
TEST(TimelineScene, AlignCentersLeftmostCenterUnmovedOthersSnap) {
  TimelineScene scene;
  // displayed centers: t1 [1000,4000] -> 2500 (leftmost center);
  //                    t2 [5000,9000] -> 7000.
  scene.setTracks({makeTrack(1, 1000, 4000, 0), makeTrack(2, 5000, 9000, 0)});
  const auto offsets = scene.alignCentersToCommonOrigin();
  ASSERT_EQ(offsets.size(), 2u);
  // target = min displayed center = 2500. new_offset = raw_center - target.
  EXPECT_EQ(offsets[0].first, 1u);
  EXPECT_EQ(offsets[0].second, 0);  // leftmost center unmoved
  EXPECT_EQ(offsets[1].first, 2u);
  EXPECT_EQ(offsets[1].second, 4500);  // raw_center 7000 - target 2500
}

TEST(TimelineScene, AlignCentersIsIdempotent) {
  TimelineScene scene;
  // t2's offset 4500 already lands its center on t1's (both 2500); re-align is a no-op.
  scene.setTracks({makeTrack(1, 1000, 4000, 0), makeTrack(2, 5000, 9000, 4500)});
  const auto offsets = scene.alignCentersToCommonOrigin();
  EXPECT_EQ(offsets[0].second, 0);
  EXPECT_EQ(offsets[1].second, 4500);
}

TEST(TimelineScene, AlignCentersEmptyIsEmpty) {
  TimelineScene scene;
  EXPECT_TRUE(scene.alignCentersToCommonOrigin().empty());
}

// alignEndsToCommonOrigin
TEST(TimelineScene, AlignEndsRightmostUnmovedOthersSnap) {
  TimelineScene scene;
  // displayed ends: t1 [1000,4000] -> 4000; t2 [5000,9000] -> 9000 (rightmost).
  scene.setTracks({makeTrack(1, 1000, 4000, 0), makeTrack(2, 5000, 9000, 0)});
  const auto offsets = scene.alignEndsToCommonOrigin();
  ASSERT_EQ(offsets.size(), 2u);
  // target = max displayed end = 9000. new_offset = t_max - target.
  EXPECT_EQ(offsets[0].first, 1u);
  EXPECT_EQ(offsets[0].second, -5000);  // 4000 - 9000: shifts later so its end hits 9000
  EXPECT_EQ(offsets[1].first, 2u);
  EXPECT_EQ(offsets[1].second, 0);  // 9000 - 9000 (rightmost end unmoved)
}

TEST(TimelineScene, AlignEndsIsIdempotent) {
  TimelineScene scene;
  // t1 offset -5000 already lands its end on 9000 (display [6000,9000]); re-align no-ops.
  scene.setTracks({makeTrack(1, 1000, 4000, -5000), makeTrack(2, 5000, 9000, 0)});
  const auto offsets = scene.alignEndsToCommonOrigin();
  EXPECT_EQ(offsets[0].second, -5000);
  EXPECT_EQ(offsets[1].second, 0);
}

TEST(TimelineScene, AlignEndsEmptyIsEmpty) {
  TimelineScene scene;
  EXPECT_TRUE(scene.alignEndsToCommonOrigin().empty());
}

// snapToEdges (drag edge-snap)
TEST(TimelineScene, SnapToEdgesCatchesWithinThreshold) {
  // Dragged edge 100, neighbour edge 150. Raw delta 45 lands it at 145 (5 from 150),
  // within threshold 10 → snaps to delta 50 so the edge sits exactly on 150.
  const auto s = TimelineScene::snapToEdges({100}, {150}, 45, 10);
  EXPECT_TRUE(s.snapped);
  EXPECT_EQ(s.delta_ns, 50);
  EXPECT_EQ(s.edge_ns, 150);
}

TEST(TimelineScene, SnapToEdgesReleasesBeyondThreshold) {
  // Raw delta 30 → edge at 130, 20 from 150 → beyond threshold 10 → no snap; the raw
  // delta passes through (dragging further releases the snap).
  const auto s = TimelineScene::snapToEdges({100}, {150}, 30, 10);
  EXPECT_FALSE(s.snapped);
  EXPECT_EQ(s.delta_ns, 30);
}

TEST(TimelineScene, SnapToEdgesPicksNearestCandidate) {
  // Edge 100; candidates 150 and 200. Raw delta 95 (edge at 195) → 200 is nearest.
  const auto s = TimelineScene::snapToEdges({100}, {150, 200}, 95, 10);
  EXPECT_TRUE(s.snapped);
  EXPECT_EQ(s.delta_ns, 100);
  EXPECT_EQ(s.edge_ns, 200);
}

TEST(TimelineScene, SnapToEdgesMatchesEitherDraggedEdge) {
  // Dragged start 100 / end 300; candidate 290. Raw delta -5 → the END snaps (295→290).
  const auto s = TimelineScene::snapToEdges({100, 300}, {290}, -5, 10);
  EXPECT_TRUE(s.snapped);
  EXPECT_EQ(s.delta_ns, -10);
  EXPECT_EQ(s.edge_ns, 290);
}

}  // namespace
}  // namespace PJ
