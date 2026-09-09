// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QSignalSpy>
#include <cmath>
#include <vector>

#include "pj_widgets/StateTransitionsView.h"
#include "state_transitions_test_helpers.h"
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

// One QApplication for the whole test binary; QWidget construction requires it.
TEST(StateTransitionsView, RowLifecycle) {
  StateTransitionsView view;
  EXPECT_EQ(view.rowCountForTest(), 0);
  view.setRows({makeRow(1, u"a"_s, {seg(0, 5, "X")}), makeRow(2, u"b"_s, {seg(0, 3, "Y")})});
  EXPECT_EQ(view.rowCountForTest(), 2);
  view.updateRow(makeRow(1, u"a"_s, {seg(0, 4, "X"), seg(4, 5, "Z")}));
  EXPECT_EQ(view.rowCountForTest(), 2);
  view.removeRow(1);
  EXPECT_EQ(view.rowCountForTest(), 1);
  view.removeRow(99);  // unknown id: no-op
  EXPECT_EQ(view.rowCountForTest(), 1);
}

TEST(StateTransitionsView, PlayheadTracksDisplaySeconds) {
  StateTransitionsView view;
  view.setPlayhead(1.5);
  EXPECT_EQ(view.playheadNsForTest(), 1'500'000'000LL);

  // Epoch-scale display seconds (absolute-time mode): double seconds carry
  // ~240 ns of quantization — the same bound the plots' tracker lives with.
  const double epoch_seconds = 1.7e9 + 0.25;
  view.setPlayhead(epoch_seconds);
  const qint64 expected = 1'700'000'000'250'000'000LL;
  EXPECT_LT(std::llabs(view.playheadNsForTest() - expected), 1000LL);
}

TEST(StateTransitionsView, NarrowSegmentsDropInBandLabels) {
  StateTransitionsView view;
  view.resize(500, 260);
  view.show();
  QApplication::processEvents();
  // 20 one-second states; the name paints INSIDE its segment (Timeline-editor
  // style), so a segment narrower than the bar-painter floor carries no text.
  // Label room is measured on the segment CLIPPED to the viewport (labels pin
  // into view), so only on-screen segments can carry text at all.
  std::vector<StateSegment> segments;
  for (int i = 0; i < 20; ++i) {
    segments.push_back(seg(i, i + 1, i % 2 == 0 ? "STATE_LONG_NAME_A" : "STATE_LONG_NAME_B"));
  }
  view.setRows({makeRow(1, u"dense"_s, std::move(segments))});

  // Zoomed far out: 1 s ≈ 2 px — below the in-band text floor everywhere.
  view.setZoom(2.0 / kSecondNs);
  const std::vector<bool> crowded = view.visibleLabelsForTest(0);
  ASSERT_EQ(crowded.size(), 20U);
  int crowded_count = 0;
  for (const bool visible : crowded) {
    crowded_count += visible ? 1 : 0;
  }
  EXPECT_EQ(crowded_count, 0);

  // Zoomed far in: 1 s ≈ 400 px — the 500 px viewport shows segment 0 in full
  // (labelled) and a 100 px slice of segment 1 (labelled, pinned); the rest sit
  // off-screen right and carry no label.
  view.setZoom(400.0 / kSecondNs);
  QApplication::processEvents();
  const std::vector<bool> roomy = view.visibleLabelsForTest(0);
  ASSERT_EQ(roomy.size(), 20U);
  EXPECT_TRUE(roomy[0]);
  EXPECT_TRUE(roomy[1]);
  int roomy_count = 0;
  for (const bool visible : roomy) {
    roomy_count += visible ? 1 : 0;
  }
  EXPECT_EQ(roomy_count, 2);
}

TEST(StateTransitionsView, DropGateAndSignal) {
  StateTransitionsView view;
  QSignalSpy dropped(&view, &StateTransitionsView::seriesDropped);

  // No predicate: a bare view refuses everything.
  EXPECT_FALSE(view.wouldAcceptDropForTest({u"str:key"_s}));

  view.setDroppablePredicate([](const QString& key) { return key.startsWith(u"str:"_s); });
  EXPECT_TRUE(view.wouldAcceptDropForTest({u"num:a"_s, u"str:b"_s}));  // any droppable key accepts
  EXPECT_FALSE(view.wouldAcceptDropForTest({u"num:a"_s}));

  // The drop emits ALL keys — the host filters.
  view.dropKeysForTest({u"num:a"_s, u"str:b"_s});
  ASSERT_EQ(dropped.count(), 1);
  EXPECT_EQ(dropped.takeFirst().at(0).toStringList(), (QStringList{u"num:a"_s, u"str:b"_s}));
}

TEST(StateTransitionsView, UserZoomEmitsVisibleRangeButExternalApplyDoesNot) {
  StateTransitionsView view;
  view.resize(500, 300);
  view.setRows({makeRow(1, u"a"_s, {seg(0, 100, "X")})});
  QSignalSpy range_changed(&view, &StateTransitionsView::visibleRangeChanged);

  view.wheelZoomForTest(1.25, 100.0);
  EXPECT_GE(range_changed.count(), 1);
  const qsizetype after_user = range_changed.count();

  // Linked-zoom apply from outside must not echo back.
  view.setVisibleRange(10.0, 50.0);
  EXPECT_EQ(range_changed.count(), after_user);
}

TEST(StateTransitionsView, RulerSticksToViewportBottom) {
  StateTransitionsView view;
  view.resize(500, 260);
  view.show();
  QApplication::processEvents();

  // Enough rows that the content scrolls vertically.
  std::vector<StateRow> rows;
  for (quint64 id = 1; id <= 12; ++id) {
    rows.push_back(makeRow(id, u"row%1"_s.arg(id), {seg(0, 10, "A")}));
  }
  view.setRows(rows);
  QApplication::processEvents();

  const double before_scroll = view.rulerViewportYForTest();
  view.setVerticalScrollForTest(view.verticalScrollForTest() + 120);
  const double after_scroll = view.rulerViewportYForTest();
  // Pinned: the ruler's viewport-y must not move when rows scroll underneath.
  EXPECT_DOUBLE_EQ(before_scroll, after_scroll);
  EXPECT_GT(view.verticalScrollForTest(), 0);
}

TEST(StateTransitionsView, LabelPinsIntoViewWhenSegmentStartScrollsOffLeft) {
  StateTransitionsView view;
  view.resize(500, 260);
  view.show();
  QApplication::processEvents();

  // Two long states; zoomed so each spans far wider than the 500 px viewport.
  view.setRows({makeRow(1, u"mode"_s, {seg(0, 100, "RUNNING"), seg(100, 200, "IDLE")})});
  view.setZoom(10.0 / kSecondNs);  // 1 s = 10 px → 1000 px per segment
  QApplication::processEvents();

  // Viewport panned into the middle of RUNNING: its left edge is ~500 px
  // off-screen, yet the label must pin into the visible portion.
  view.setViewportLeftDisplayNs(50 * kSecondNs);
  QApplication::processEvents();
  std::vector<bool> labels = view.visibleLabelsForTest(0);
  ASSERT_EQ(labels.size(), 2U);
  EXPECT_TRUE(labels[0]);

  // Viewport at [99.5 s, 149.5 s]: RUNNING's visible sliver is ~5 px — below
  // the bar-painter text floor — so ITS label drops while IDLE (spanning the
  // rest of the view) keeps its pinned label. The un-clamped rule would call
  // RUNNING labelled here (1000 px wide) with the text drawn 990 px off-screen.
  view.setViewportLeftDisplayNs(99'500'000'000LL);
  QApplication::processEvents();
  labels = view.visibleLabelsForTest(0);
  ASSERT_EQ(labels.size(), 2U);
  EXPECT_FALSE(labels[0]);
  EXPECT_TRUE(labels[1]);
}

TEST(StateTransitionsView, WheelZoomOutClampsToDisplayRange) {
  StateTransitionsView view;
  view.resize(500, 260);
  view.show();
  QApplication::processEvents();
  view.setRows({makeRow(1, u"mode"_s, {seg(0, 100, "RUNNING")})});
  view.setDisplayRange(0.0, 100.0);
  QSignalSpy range_changed(&view, &StateTransitionsView::visibleRangeChanged);

  // Ten aggressive zoom-out steps: unclamped this lands ~1000x past the data.
  for (int i = 0; i < 10; ++i) {
    view.wheelZoomForTest(0.5, 250.0);
  }
  ASSERT_GE(range_changed.count(), 1);
  const QList<QVariant> last = range_changed.last();
  const double span_seconds = last.at(1).toDouble() - last.at(0).toDouble();
  // The visible window stops at the display range (1% slack for px rounding).
  EXPECT_LE(span_seconds, 100.0 * 1.01);
  EXPECT_GT(span_seconds, 90.0);  // and it DID zoom out to roughly the range
}

TEST(StateTransitionsView, WheelZoomOutOnDegenerateRangeFloorsAtOneSecond) {
  StateTransitionsView view;
  view.resize(500, 260);
  view.show();
  QApplication::processEvents();
  // A single-sample dataset: display range min == max. The zoom-out floor must
  // not vanish (divide-by-zero span) — it falls back to a 1 s window.
  view.setRows({makeRow(1, u"mode"_s, {seg(5, 5, "ONLY")})});
  view.setDisplayRange(5.0, 5.0);
  QSignalSpy range_changed(&view, &StateTransitionsView::visibleRangeChanged);
  for (int i = 0; i < 10; ++i) {
    view.wheelZoomForTest(0.5, 250.0);
  }
  ASSERT_GE(range_changed.count(), 1);
  const QList<QVariant> last = range_changed.last();
  const double span_seconds = last.at(1).toDouble() - last.at(0).toDouble();
  EXPECT_LE(span_seconds, 1.01);
}

TEST(StateTransitionsView, ViewStateAccessorsRoundTrip) {
  StateTransitionsView view;
  view.resize(500, 300);
  view.setRows({makeRow(1, u"a"_s, {seg(0, 100, "X")})});

  view.setZoom(3e-8);
  EXPECT_DOUBLE_EQ(view.zoom(), 3e-8);
  view.setViewportLeftDisplayNs(20 * kSecondNs);
  // Scroll granularity is whole pixels: allow one pixel of ns error.
  const qint64 one_px_ns = static_cast<qint64>(1.0 / view.zoom());
  EXPECT_LT(std::llabs(view.viewportLeftDisplayNs() - 20 * kSecondNs), one_px_ns + 1);
}

}  // namespace
}  // namespace PJ
