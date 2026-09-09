// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QToolButton>
#include <QWheelEvent>
#include <memory>

#include "pj_widgets/Timeline.h"
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

// One QApplication for the whole test binary; QWidget construction requires it.
TEST(Timeline, BuildsBarPerTrack) {
  Timeline widget;
  widget.setTracks({
      TimelineTrack{.id = 1, .name = "a", .t_min_ns = 1000, .t_max_ns = 5000, .offset_ns = 0},
      TimelineTrack{.id = 2, .name = "b", .t_min_ns = 2000, .t_max_ns = 9000, .offset_ns = 1000},
  });
  EXPECT_EQ(widget.barCount(), 2);
}

TEST(Timeline, EmptyTracksHasNoBars) {
  Timeline widget;
  widget.setTracks({});
  EXPECT_EQ(widget.barCount(), 0);
}

TEST(Timeline, BarDragEmitsOffsetChange) {
  Timeline widget;
  widget.setTracks({TimelineTrack{.id = 7, .name = "a", .t_min_ns = 0, .t_max_ns = 10'000'000, .offset_ns = 0}});

  quint64 got_id = 0;
  qint64 got_offset = 0;
  int hits = 0;
  QObject::connect(&widget, &Timeline::offsetChangeRequested, [&](quint64 id, qint64 off) {
    got_id = id;
    got_offset = off;
    ++hits;
  });

  // Drag right by 100 px. display = raw - offset; dragging RIGHT (later) => the
  // new offset = base_offset - pxDeltaToNs(+100), i.e. strictly negative.
  widget.applyBarDragForTest(7, /*dx_px=*/100.0);
  EXPECT_EQ(hits, 1);
  EXPECT_EQ(got_id, 7u);
  EXPECT_LT(got_offset, 0);  // dragged later => negative offset

  // Dragging left yields a positive offset (source appears earlier).
  widget.applyBarDragForTest(7, /*dx_px=*/-100.0);
  EXPECT_GT(got_offset, 0);
}

TEST(Timeline, BarDragOffsetAddsToExistingBase) {
  Timeline widget;
  // Base offset 5000; equal-and-opposite drags must straddle it symmetrically.
  widget.setTracks({TimelineTrack{.id = 3, .name = "a", .t_min_ns = 0, .t_max_ns = 10'000'000, .offset_ns = 5000}});

  qint64 right_off = 0;
  qint64 left_off = 0;
  QObject::connect(&widget, &Timeline::offsetChangeRequested, [&](quint64, qint64 off) { right_off = off; });
  widget.applyBarDragForTest(3, 50.0);
  const qint64 after_right = right_off;

  QObject::disconnect(&widget, nullptr, nullptr, nullptr);
  QObject::connect(&widget, &Timeline::offsetChangeRequested, [&](quint64, qint64 off) { left_off = off; });
  widget.applyBarDragForTest(3, -50.0);

  // Both are measured from the same base (5000); their mean is the base.
  EXPECT_EQ((after_right + left_off) / 2, 5000);
}

// Dragging the needle into the empty region past the bars emits a seek CLAMPED to
// the data extent (min/max) instead of an out-of-range value. The playhead is a pure
// slave — seekToSceneX returns the clamped display-seconds it emits (the host writes
// that to the engine, whose echo repositions the needle), so we pin the emitted value.
TEST(Timeline, NeedleDragClampsToDataExtent) {
  Timeline widget;  // not shown -> rebuild never refits, so the zoom below holds
  widget.setTracks({TimelineTrack{.id = 1, .name = "a", .t_min_ns = 0, .t_max_ns = 10'000'000, .offset_ns = 0}});
  widget.setZoom(1e-6);  // 1e-6 px/ns -> scene-x 1e6 maps to 1e12 ns, far past the 1e7 (0.01 s) extent

  double emitted = -1.0;
  QObject::connect(&widget, &Timeline::playheadSeeked, [&](double s) { emitted = s; });

  // Past the right edge -> the emitted seek is pinned to the extent max (0.01 s).
  EXPECT_NEAR(widget.seekToSceneXForTest(1.0e6), 0.01, 1e-9);
  EXPECT_NEAR(emitted, 0.01, 1e-9);
  // Past the left edge -> pinned to the extent min (0 s).
  EXPECT_NEAR(widget.seekToSceneXForTest(-1.0e6), 0.0, 1e-9);
  EXPECT_NEAR(emitted, 0.0, 1e-9);

  // The reference needle (not engine-driven) clamps identically and self-positions.
  EXPECT_NEAR(widget.moveReferenceToSceneXForTest(1.0e6), 0.01, 1e-9);
  EXPECT_NEAR(widget.moveReferenceToSceneXForTest(-1.0e6), 0.0, 1e-9);
}

TEST(Timeline, AlignButtonEmitsAlignRequested) {
  Timeline widget;
  QSignalSpy spy(&widget, &Timeline::alignRequested);
  auto* button = widget.findChild<QPushButton*>();
  ASSERT_NE(button, nullptr);
  button->click();
  EXPECT_EQ(spy.count(), 1);
}

// While locked (the host does this during live streaming) every manipulation
// gesture and needle seek is suppressed and the Align button greys out; the
// needle still tracks playback via setPlayhead. Unlocking restores everything.
TEST(Timeline, InteractionLockSuppressesManipulationAndSeek) {
  Timeline widget;
  widget.setTracks({
      TimelineTrack{.id = 1, .name = "a", .t_min_ns = 0, .t_max_ns = 10'000'000, .offset_ns = 0},
      TimelineTrack{.id = 2, .name = "b", .t_min_ns = 0, .t_max_ns = 10'000'000, .offset_ns = 0},
  });
  QSignalSpy offset_spy(&widget, &Timeline::offsetChangeRequested);
  QSignalSpy seek_spy(&widget, &Timeline::playheadSeeked);
  QSignalSpy reorder_spy(&widget, &Timeline::tracksReordered);
  auto* align_button = widget.findChild<QPushButton*>();
  ASSERT_NE(align_button, nullptr);

  widget.setInteractionLocked(true);
  EXPECT_FALSE(align_button->isEnabled());  // Align greyed out while locked
  widget.applyBarDragForTest(1, /*dx_px=*/100.0);
  (void)widget.seekToSceneXForTest(1.0e6);  // [[nodiscard]]; we assert via the spy below
  widget.reorderForTest(/*from=*/0, /*drop_index=*/2);
  EXPECT_EQ(offset_spy.count(), 0);
  EXPECT_EQ(seek_spy.count(), 0);
  EXPECT_EQ(reorder_spy.count(), 0);

  // setPlayhead still moves the needle while locked (it is a pure slave).
  widget.setPlayhead(0.005);
  EXPECT_EQ(seek_spy.count(), 0);  // slave update emits no seek

  // Unlocking restores manipulation + seeking.
  widget.setInteractionLocked(false);
  EXPECT_TRUE(align_button->isEnabled());
  widget.applyBarDragForTest(1, /*dx_px=*/100.0);
  (void)widget.seekToSceneXForTest(1.0e6);
  EXPECT_EQ(offset_spy.count(), 1);
  EXPECT_EQ(seek_spy.count(), 1);
}

// --- custom scroll pill (replaces the native horizontal scrollbar) ------------

// A pair of wide tracks at default zoom span far more than the viewport, so the
// scene scrolls and the pill is live.
std::unique_ptr<Timeline> makeScrollableTimeline() {
  auto w = std::make_unique<Timeline>();
  w->setTracks({
      TimelineTrack{.id = 1, .name = "a", .t_min_ns = 0, .t_max_ns = 30'000'000'000, .offset_ns = 0},
      TimelineTrack{.id = 2, .name = "b", .t_min_ns = 5'000'000'000, .t_max_ns = 40'000'000'000, .offset_ns = 0},
  });
  w->resize(400, 160);
  w->show();
  QApplication::processEvents();
  return w;
}

TEST(Timeline, NativeHorizontalScrollBarIsHidden) {
  auto w = makeScrollableTimeline();
  // The native horizontal scrollbar is replaced by the pill; it must never show.
  EXPECT_FALSE(w->horizontalScrollBarVisibleForTest());
}

TEST(Timeline, ScrollPillRevealsOnBottomStripHover) {
  auto w = makeScrollableTimeline();
  const int vh = w->viewportHeightForTest();
  ASSERT_GT(vh, 30);

  // Hover up in the body: pill stays hidden.
  w->moveViewportForTest(QPoint(50, vh / 2), /*button_down=*/false);
  EXPECT_FALSE(w->isScrollPillShownForTest());

  // Hover into the bottom strip: pill is revealed.
  w->moveViewportForTest(QPoint(50, vh - 3), /*button_down=*/false);
  EXPECT_TRUE(w->isScrollPillShownForTest());

  // Move back up: pill hides again.
  w->moveViewportForTest(QPoint(50, vh / 2), /*button_down=*/false);
  EXPECT_FALSE(w->isScrollPillShownForTest());
}

TEST(Timeline, ScrollPillDragScrollsTheView) {
  auto w = makeScrollableTimeline();
  const int vh = w->viewportHeightForTest();

  // Grab in the strip, then drag right — the scrollbar value must increase.
  w->pressViewportForTest(QPoint(60, vh - 3));
  EXPECT_TRUE(w->isScrollPillShownForTest());
  const int after_press = w->scrollValueForTest();
  w->moveViewportForTest(QPoint(260, vh - 3), /*button_down=*/true);
  EXPECT_GT(w->scrollValueForTest(), after_press);
  w->releaseViewportForTest(QPoint(260, vh - 3));
}

// The mouse wheel zooms the timeline horizontally (cursor-anchored). Locked (live
// streaming + playing) swallows it so the view stays pinned to the live edge;
// unlocking restores zooming.
TEST(Timeline, InteractionLockBlocksWheelZoom) {
  auto w = makeScrollableTimeline();
  const double before = w->zoom();
  const auto send_wheel = [&w] {
    QWheelEvent we(
        QPointF(50, 50), w->mapToGlobal(QPoint(50, 50)), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
        Qt::NoScrollPhase, false);
    QApplication::sendEvent(w.get(), &we);
  };

  w->setInteractionLocked(true);
  send_wheel();
  EXPECT_EQ(w->zoom(), before);  // frozen

  w->setInteractionLocked(false);
  send_wheel();
  EXPECT_NE(w->zoom(), before);  // zooms again
}

// The "pause playback to interact" overlay appears over the scene only while locked.
TEST(Timeline, InteractionLockShowsOverlay) {
  auto w = makeScrollableTimeline();  // shown, so isVisible() is meaningful
  auto* overlay = w->findChild<QLabel*>(u"timelineLockOverlay"_s);
  ASSERT_NE(overlay, nullptr);
  EXPECT_TRUE(overlay->text().contains("pause"_L1, Qt::CaseInsensitive));
  EXPECT_FALSE(overlay->isVisible());

  w->setInteractionLocked(true);
  EXPECT_TRUE(overlay->isVisible());

  w->setInteractionLocked(false);
  EXPECT_FALSE(overlay->isVisible());
}

// --- left track-name column -------------------------------------------------

TEST(Timeline, NameColumnRowAlignsWithEachBar) {
  auto w = makeScrollableTimeline();  // two tracks
  // One name row per track, and each name's top lines up with its bar's row top
  // (the "align in height perfectly" contract). No vertical scroll at this size,
  // so the panel's viewport-y equals the bar's scene-y exactly.
  ASSERT_EQ(w->nameRowCountForTest(), 2);
  for (int i = 0; i < 2; ++i) {
    EXPECT_DOUBLE_EQ(w->nameRowTopForTest(i), w->barRowTopForTest(i)) << "row " << i << " misaligned";
  }
}

TEST(Timeline, DragReorderMovesTrackAndEmitsNewOrder) {
  auto w = makeScrollableTimeline();  // ids top-to-bottom: [1, 2]
  ASSERT_EQ(w->trackOrderForTest(), (QList<quint64>{1, 2}));
  QSignalSpy spy(w.get(), &Timeline::tracksReordered);

  // Drag row 0 down past row 1 (insertion index 2) → order becomes [2, 1].
  w->reorderForTest(/*from=*/0, /*drop_index=*/2);

  EXPECT_EQ(w->trackOrderForTest(), (QList<quint64>{2, 1}));
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(spy.takeFirst().at(0).value<QList<quint64>>(), (QList<quint64>{2, 1}));
}

// --- drag edge-snap ---------------------------------------------------------

TEST(Timeline, BarDragSnapsToNeighbourEdgeAndReleases) {
  auto w = makeScrollableTimeline();  // bag_a id=1 [0,30s], bag_b id=2 [5s,40s]; px_per_ns 1e-7
  // Drag bag_b left by 500 px (= −5 s): its start (5 s) lands on bag_a's start (0) → snaps.
  w->dragBarForSnapTest(2, -500.0);
  EXPECT_TRUE(w->snapLineVisibleForTest());
  // A smaller drag (250 px) leaves the edges ~250 px apart → beyond threshold → no snap.
  w->dragBarForSnapTest(2, -250.0);
  EXPECT_FALSE(w->snapLineVisibleForTest());
}

TEST(Timeline, BarDragSnapDisabledShowsNoGuide) {
  auto w = makeScrollableTimeline();
  w->setSnapEnabled(false);
  w->dragBarForSnapTest(2, -500.0);  // would snap, but snapping is off
  EXPECT_FALSE(w->snapLineVisibleForTest());
}

TEST(Timeline, BarDragSnapHoldsThroughHysteresis) {
  auto w = makeScrollableTimeline();  // bag_b start 5 s; snaps to bag_a start (0) at −500 px
  // -500 px catches the snap; -490 px (10 px off, past the 8 px catch but inside the
  // ~14 px release band) HOLDS rather than flickering off; -480 px finally releases.
  const std::vector<bool> states = w->dragSnapSequenceForTest(2, {-500.0, -490.0, -480.0});
  ASSERT_EQ(states.size(), 3u);
  EXPECT_TRUE(states[0]);   // caught
  EXPECT_TRUE(states[1]);   // held (no flicker)
  EXPECT_FALSE(states[2]);  // released by dragging further
}

// --- view-state accessors (layout persistence) ------------------------------

TEST(Timeline, ZoomRoundTripsAndClamps) {
  auto w = makeScrollableTimeline();
  // A value inside the widget's [1e-12, 1e-1] limits round-trips exactly.
  w->setZoom(5e-7);
  EXPECT_DOUBLE_EQ(w->zoom(), 5e-7);
  // Above the max clamps to the ceiling (1e-1); non-positive / NaN is ignored.
  w->setZoom(1.0);
  EXPECT_DOUBLE_EQ(w->zoom(), 1e-1);
  const double held = w->zoom();
  w->setZoom(-1.0);
  EXPECT_DOUBLE_EQ(w->zoom(), held);
  w->setZoom(0.0);
  EXPECT_DOUBLE_EQ(w->zoom(), held);
}

TEST(Timeline, ViewportLeftDisplayNsRoundTrips) {
  auto w = makeScrollableTimeline();  // wide tracks → scrollable
  // Self-relative: shift the visible left edge 5 s later and read it back. The
  // tolerance covers the integer-pixel rounding of the scrollbar (≈1 px ≈ 1e7 ns
  // at the default zoom).
  const qint64 before = w->viewportLeftDisplayNs();
  const qint64 target = before + 5'000'000'000LL;
  w->setViewportLeftDisplayNs(target);
  EXPECT_NEAR(static_cast<double>(w->viewportLeftDisplayNs()), static_cast<double>(target), 2e7);
}

TEST(Timeline, NameColumnWidthResizesAndHonorsFloor) {
  auto w = makeScrollableTimeline();
  // setNameColumnWidth pins both the floor and the current width.
  w->setNameColumnWidth(120);
  EXPECT_EQ(w->nameColumnWidth(), 120);
  // resizeNameColumn widens without touching the floor.
  w->resizeNameColumn(200);
  EXPECT_EQ(w->nameColumnWidth(), 200);
  // Below the pinned floor (120) the splitter clamps back up — the column can
  // never be narrower than the playback-aligned floor the host set.
  w->resizeNameColumn(50);
  EXPECT_EQ(w->nameColumnWidth(), 120);
}

TEST(Timeline, EmptyTimelineOpensWithZeroAtLeftEdge) {
  // A freshly booted, data-less timeline must open with 0ms flush at the view's
  // left edge (under the playback track start), not into the negative left
  // buffer. Before the fix the empty extent padded -1 min on the left, so the
  // left edge showed -60 s; now the empty extent starts at 0 (origin 0).
  auto w = std::make_unique<Timeline>();
  w->setTracks({});  // empty
  w->resize(800, 160);
  w->show();
  QApplication::processEvents();
  EXPECT_NEAR(static_cast<double>(w->viewportLeftDisplayNs()), 0.0, 2e7);  // ≈0, within ~1 px
}

TEST(Timeline, EmptyTimelineFitsPlaybackRangeToView) {
  // An empty timeline spans exactly the playback range (setDisplayRange) and
  // fits it to the view width — so it is "exactly as long as the playback".
  auto w = std::make_unique<Timeline>();
  w->resize(800, 160);
  w->show();
  QApplication::processEvents();

  w->setDisplayRange(0.0, 10.0);  // 10 s playback range
  QApplication::processEvents();
  const double zoom_10s = w->zoom();
  EXPECT_NEAR(static_cast<double>(w->viewportLeftDisplayNs()), 0.0, 2e7);  // 0 ms at the left edge

  // Same view, double the range → half the px-per-ns (the span is fitted to the
  // fixed view width, with no absolute viewport-width dependency in the check).
  w->setDisplayRange(0.0, 20.0);
  QApplication::processEvents();
  EXPECT_NEAR(w->zoom(), zoom_10s / 2.0, zoom_10s * 1e-6);
}

// --- auto-zoom ---------------------------------------------------------------

TEST(Timeline, AutoZoomFitsDataExtentToViewOnLoad) {
  // Default on. Loading data WHILE shown fits the largest extent to the view.
  auto w = std::make_unique<Timeline>();
  EXPECT_TRUE(w->isAutoZoomEnabled());
  w->resize(800, 160);
  w->show();
  QApplication::processEvents();

  w->setTracks({TimelineTrack{.id = 1, .name = "a", .t_min_ns = 0, .t_max_ns = 10'000'000'000, .offset_ns = 0}});
  QApplication::processEvents();
  const double zoom_10s = w->zoom();

  // A 20 s dataset in the same view → half the px-per-ns (extent fitted to width).
  w->setTracks({TimelineTrack{.id = 2, .name = "b", .t_min_ns = 0, .t_max_ns = 20'000'000'000, .offset_ns = 0}});
  QApplication::processEvents();
  EXPECT_NEAR(w->zoom(), zoom_10s / 2.0, zoom_10s * 1e-6);
}

TEST(Timeline, AutoZoomDisabledKeepsZoomOnLoad) {
  auto w = std::make_unique<Timeline>();
  w->setAutoZoomEnabled(false);
  EXPECT_FALSE(w->isAutoZoomEnabled());
  w->resize(800, 160);
  w->show();
  QApplication::processEvents();

  w->setTracks({TimelineTrack{.id = 1, .name = "a", .t_min_ns = 0, .t_max_ns = 10'000'000'000, .offset_ns = 0}});
  QApplication::processEvents();
  const double z1 = w->zoom();
  w->setTracks({TimelineTrack{.id = 2, .name = "b", .t_min_ns = 0, .t_max_ns = 20'000'000'000, .offset_ns = 0}});
  QApplication::processEvents();
  EXPECT_DOUBLE_EQ(w->zoom(), z1);  // no auto-fit → zoom unchanged across loads
}

TEST(Timeline, FitToContentsReframesAfterExtentChange) {
  // Mirrors the post-alignment re-fit the controller triggers: after the union of
  // the bars widens (an offset spreads two overlapping tracks apart),
  // fitToContents() re-fits to the new, larger extent.
  auto w = std::make_unique<Timeline>();
  w->resize(800, 160);
  w->show();
  QApplication::processEvents();
  // Two coincident 10 s tracks → union [0,10s]; auto-zoom fits it on load.
  w->setTracks({
      TimelineTrack{.id = 1, .name = "a", .t_min_ns = 0, .t_max_ns = 10'000'000'000, .offset_ns = 0},
      TimelineTrack{.id = 2, .name = "b", .t_min_ns = 0, .t_max_ns = 10'000'000'000, .offset_ns = 0},
  });
  QApplication::processEvents();
  const double zoom_before = w->zoom();

  // Shift track 2 by -10 s (window → [10s,20s]); the union widens to [0,20s]. This
  // is an offset-only change (no auto-fit on its own), so fit explicitly.
  w->setTracks({
      TimelineTrack{.id = 1, .name = "a", .t_min_ns = 0, .t_max_ns = 10'000'000'000, .offset_ns = 0},
      TimelineTrack{.id = 2, .name = "b", .t_min_ns = 0, .t_max_ns = 10'000'000'000, .offset_ns = -10'000'000'000},
  });
  w->fitToContents();
  QApplication::processEvents();
  EXPECT_LT(w->zoom(), zoom_before);  // wider union → smaller px-per-ns
}

// --- vertical scroll pill + sticky ruler -------------------------------------

// Many tracks in a short viewport → the scene overflows vertically, so the
// vertical scrollbar/pill is live and the ruler must stay pinned to the top.
std::unique_ptr<Timeline> makeTallTimeline() {
  auto w = std::make_unique<Timeline>();
  std::vector<TimelineTrack> tracks;
  tracks.reserve(20);
  for (int i = 0; i < 20; ++i) {
    tracks.push_back(
        TimelineTrack{
            .id = static_cast<quint64>(i + 1),
            .name = u"t%1"_s.arg(i),
            .t_min_ns = 0,
            .t_max_ns = 10'000'000'000,
            .offset_ns = 0,
        });
  }
  w->setTracks(tracks);
  w->resize(400, 160);
  w->show();
  QApplication::processEvents();
  return w;
}

TEST(Timeline, NativeVerticalScrollBarIsHidden) {
  auto w = makeTallTimeline();
  // Replaced by the custom vertical pill; the native bar must never show.
  EXPECT_FALSE(w->verticalScrollBarVisibleForTest());
}

TEST(Timeline, VerticalScrollPillRevealsOnRightStripHover) {
  auto w = makeTallTimeline();
  const int vw = w->viewportWidthForTest();
  ASSERT_GT(vw, 30);
  w->moveViewportForTest(QPoint(vw / 2, 40), /*button_down=*/false);
  EXPECT_FALSE(w->isVScrollPillShownForTest());
  w->moveViewportForTest(QPoint(vw - 3, 40), /*button_down=*/false);  // into the right strip
  EXPECT_TRUE(w->isVScrollPillShownForTest());
  w->moveViewportForTest(QPoint(vw / 2, 40), /*button_down=*/false);
  EXPECT_FALSE(w->isVScrollPillShownForTest());
}

TEST(Timeline, VerticalScrollPillDragScrollsTheView) {
  auto w = makeTallTimeline();
  const int vw = w->viewportWidthForTest();
  w->pressViewportForTest(QPoint(vw - 3, 30));
  EXPECT_TRUE(w->isVScrollPillShownForTest());
  const int after_press = w->verticalScrollValueForTest();
  w->moveViewportForTest(QPoint(vw - 3, 130), /*button_down=*/true);  // drag down
  EXPECT_GT(w->verticalScrollValueForTest(), after_press);
  w->releaseViewportForTest(QPoint(vw - 3, 130));
}

TEST(Timeline, StickyRulerTracksVerticalScroll) {
  auto w = makeTallTimeline();
  w->setVerticalScrollForTest(0);
  EXPECT_DOUBLE_EQ(w->rulerSceneYForTest(), 0.0);
  // Scrolling down moves the ruler item to the new viewport top (scene-y == scroll
  // value), so the number line stays put while the rows move underneath it.
  w->setVerticalScrollForTest(50);
  EXPECT_DOUBLE_EQ(w->rulerSceneYForTest(), 50.0);
}

// --- dataset filter ----------------------------------------------------------

TEST(Timeline, DatasetFilterShowsOnlyMatchingTracks) {
  Timeline w;
  w.setTracks({
      TimelineTrack{.id = 1, .name = "alpha", .t_min_ns = 0, .t_max_ns = 1000, .offset_ns = 0},
      TimelineTrack{.id = 2, .name = "beta", .t_min_ns = 0, .t_max_ns = 1000, .offset_ns = 0},
      TimelineTrack{.id = 3, .name = "alphabet", .t_min_ns = 0, .t_max_ns = 1000, .offset_ns = 0},
  });
  EXPECT_EQ(w.barCount(), 3);

  w.setDatasetFilter("alpha");  // substring: "alpha" + "alphabet"
  EXPECT_EQ(w.barCount(), 2);
  EXPECT_EQ(w.trackOrderForTest(), (QList<quint64>{1, 3}));

  w.setDatasetFilter("BETA");  // case-insensitive
  EXPECT_EQ(w.barCount(), 1);
  EXPECT_EQ(w.trackOrderForTest(), (QList<quint64>{2}));

  w.setDatasetFilter("");  // cleared → all shown again
  EXPECT_EQ(w.barCount(), 3);
}

// Name-column multi-selection drives the merge prompt, and the dataset filter
// gates the prompt + merge payload on the VISIBLE selected subset (selection
// itself persists across filtering — a destructive merge never silently includes
// a source the filter is hiding).
TEST(Timeline, NameColumnSelectionGatesMergeButtonOnVisibleSelection) {
  Timeline w;
  w.setTracks({
      TimelineTrack{.id = 1, .name = "alpha", .t_min_ns = 0, .t_max_ns = 1000, .offset_ns = 0},
      TimelineTrack{.id = 2, .name = "beta", .t_min_ns = 0, .t_max_ns = 1000, .offset_ns = 0},
      TimelineTrack{.id = 3, .name = "alphabet", .t_min_ns = 0, .t_max_ns = 1000, .offset_ns = 0},
  });
  EXPECT_FALSE(w.mergeButtonEnabledForTest());

  // Plain click selects one → no prompt (merge needs ≥2).
  w.selectNameRowForTest(0, Qt::NoModifier);
  EXPECT_FALSE(w.mergeButtonEnabledForTest());
  EXPECT_EQ(w.visibleSelectedIdsForTest(), (QList<quint64>{1}));

  // Ctrl+click a second → prompt shows; ids returned in display order.
  w.selectNameRowForTest(1, Qt::ControlModifier);
  EXPECT_TRUE(w.mergeButtonEnabledForTest());
  EXPECT_EQ(w.visibleSelectedIdsForTest(), (QList<quint64>{1, 2}));

  // A filter that hides one of the two selected leaves only one visible-selected →
  // the prompt hides, but the selection persists (restored when the filter clears).
  w.setDatasetFilter("beta");  // shows only id 2
  EXPECT_FALSE(w.mergeButtonEnabledForTest());
  EXPECT_EQ(w.visibleSelectedIdsForTest(), (QList<quint64>{2}));

  w.setDatasetFilter("");  // both visible again → prompt returns
  EXPECT_TRUE(w.mergeButtonEnabledForTest());
  EXPECT_EQ(w.visibleSelectedIdsForTest(), (QList<quint64>{1, 2}));

  // Shift+click extends the range from the anchor (id 2, the last pick) to row 2.
  w.selectNameRowForTest(2, Qt::ShiftModifier);
  EXPECT_TRUE(w.mergeButtonEnabledForTest());
  EXPECT_EQ(w.visibleSelectedIdsForTest(), (QList<quint64>{2, 3}));

  // Plain click collapses back to a single selection → prompt hides.
  w.selectNameRowForTest(0, Qt::NoModifier);
  EXPECT_FALSE(w.mergeButtonEnabledForTest());
  EXPECT_EQ(w.visibleSelectedIdsForTest(), (QList<quint64>{1}));
}

// --- time-frame offset precision --------------------------------------------

// A playback-frame setPlayhead under a large (epoch-scale) time-frame offset must
// land BIT-EXACT in the bar frame. The offset is added in integer ns, so the only
// double involved is the small playback value — adding the epoch offset in `double`
// seconds instead would round to ~240 ns and desync the timeline from playback.
TEST(Timeline, TimeFrameOffsetIsBitExactAtEpochScale) {
  Timeline w;
  constexpr qint64 kEpochNs = 1'700'000'000'000'000'000LL;  // ~2023
  w.setTimeFrameOffsetNs(kEpochNs);

  // Playback frame: 5.123456789 s after the global origin.
  w.setPlayhead(5.123456789);
  EXPECT_EQ(w.playheadNsForTest(), kEpochNs + 5'123'456'789LL);

  // Zero offset (toggle off) is an identity bridge.
  w.setTimeFrameOffsetNs(0);
  w.setPlayhead(5.123456789);
  EXPECT_EQ(w.playheadNsForTest(), 5'123'456'789LL);
}

// The absolute marker label must ROUND to the nearest millisecond (matching the
// playback readout's QString::number('f',3)), not truncate — truncation read a ms
// low and made the timeline disagree with playback by 0.001 s. Always 3 decimals.
TEST(Timeline, AbsoluteLabelRoundsMillisecondsLikePlayback) {
  Timeline w;
  w.setAbsoluteTimeLabels(true);
  // 123.6 ms past the second → rounds UP to .124 (truncation would show .123).
  EXPECT_EQ(w.markerLabelForTest(1'700'000'005'123'600'000LL), u"1700000005.124"_s);
  // A whole second still shows 3 decimals, matching the playback readout's "X.000".
  EXPECT_EQ(w.markerLabelForTest(1'700'000'005'000'000'000LL), u"1700000005.000"_s);
  // Rounding carries into the next second.
  EXPECT_EQ(w.markerLabelForTest(1'700'000'005'999'600'000LL), u"1700000006.000"_s);
}

}  // namespace
}  // namespace PJ
