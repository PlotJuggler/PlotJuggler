// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Behavioural contract for MarkerTimeline's mark model (the logic the mouse
// handlers drive): add/move/resize/delete clamp into range, regions keep
// start<=end, events stay points, setMarks() is silent while edits emit
// marksChanged().

#include <gtest/gtest.h>

#include <QApplication>
#include <QSignalSpy>

#include "pj_widgets/MarkerTimeline.h"

namespace {

using PJ::MarkerTimeline;
using Kind = PJ::MarkerTimeline::Kind;

TEST(MarkerTimelineTest, AddRegionAndEventAssignDistinctIds) {
  MarkerTimeline t;
  t.setRange(0, 1000);
  const int a = t.addMark(Kind::kRegion, 100, 300);
  const int b = t.addMark(Kind::kEvent, 500, 500);
  EXPECT_NE(a, b);
  ASSERT_EQ(t.marks().size(), 2);
  EXPECT_EQ(t.marks()[0].kind, Kind::kRegion);
  EXPECT_EQ(t.marks()[1].kind, Kind::kEvent);
}

TEST(MarkerTimelineTest, AddClampsAndOrdersRegion) {
  MarkerTimeline t;
  t.setRange(0, 1000);
  t.addMark(Kind::kRegion, 900, 200);  // reversed + in range
  EXPECT_EQ(t.marks()[0].start, 200);
  EXPECT_EQ(t.marks()[0].end, 900);

  t.addMark(Kind::kRegion, -50, 5000);  // out of range both sides
  EXPECT_EQ(t.marks()[1].start, 0);
  EXPECT_EQ(t.marks()[1].end, 1000);
}

TEST(MarkerTimelineTest, EventKeepsEndEqualStart) {
  MarkerTimeline t;
  t.setRange(0, 1000);
  const int id = t.addMark(Kind::kEvent, 400, 999);  // end ignored for events
  const auto& m = t.marks()[0];
  EXPECT_EQ(m.start, 400);
  EXPECT_EQ(m.end, 400);
  t.moveMark(id, 700);
  EXPECT_EQ(t.marks()[0].start, 700);
  EXPECT_EQ(t.marks()[0].end, 700);
}

TEST(MarkerTimelineTest, MoveRegionPreservesWidthAndClampsAtEdge) {
  MarkerTimeline t;
  t.setRange(0, 1000);
  const int id = t.addMark(Kind::kRegion, 100, 300);  // width 200
  t.moveMark(id, 950);                                // would push end past 1000
  EXPECT_EQ(t.marks()[0].start, 800);
  EXPECT_EQ(t.marks()[0].end, 1000);
}

TEST(MarkerTimelineTest, ResizeRegionReordersWhenEdgesCross) {
  MarkerTimeline t;
  t.setRange(0, 1000);
  const int id = t.addMark(Kind::kRegion, 100, 300);
  t.resizeRegion(id, 500, 200);  // start dragged past end
  EXPECT_EQ(t.marks()[0].start, 200);
  EXPECT_EQ(t.marks()[0].end, 500);
}

TEST(MarkerTimelineTest, DeleteRemovesById) {
  MarkerTimeline t;
  t.setRange(0, 1000);
  const int a = t.addMark(Kind::kRegion, 100, 300);
  const int b = t.addMark(Kind::kEvent, 500, 500);
  t.deleteMark(a);
  ASSERT_EQ(t.marks().size(), 1);
  EXPECT_EQ(t.marks()[0].id, b);
}

TEST(MarkerTimelineTest, SetMarksIsSilentButEditsEmit) {
  MarkerTimeline t;
  t.setRange(0, 1000);
  QSignalSpy spy(&t, &MarkerTimeline::marksChanged);

  QVector<MarkerTimeline::Mark> set;
  set.push_back({7, Kind::kRegion, 100, 200});
  set.push_back({9, Kind::kEvent, 400, 400});
  t.setMarks(set);
  EXPECT_EQ(spy.count(), 0);  // owner-driven update must not loop the protocol

  // next_id_ must clear existing ids so a new add does not collide.
  const int id = t.addMark(Kind::kEvent, 600, 600);
  EXPECT_GT(id, 9);
  EXPECT_EQ(spy.count(), 1);

  t.moveMark(id, 650);
  t.deleteMark(id);
  EXPECT_EQ(spy.count(), 3);
}

TEST(MarkerTimelineTest, SetRangeClampsExistingMarks) {
  MarkerTimeline t;
  t.setRange(0, 1000);
  t.addMark(Kind::kRegion, 100, 900);
  t.setRange(0, 500);
  EXPECT_LE(t.marks()[0].end, 500);
  EXPECT_GE(t.marks()[0].start, 0);
}

TEST(MarkerTimelineTest, ResizeRegionEnforcesNonZeroWidth) {
  MarkerTimeline t;
  t.setRange(0, 1000);
  const int id = t.addMark(Kind::kRegion, 400, 600);
  t.resizeRegion(id, 500, 500);                         // try to collapse to zero width
  EXPECT_GT(t.marks()[0].end - t.marks()[0].start, 0);  // stays grabbable
}

}  // namespace
