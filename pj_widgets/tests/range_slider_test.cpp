// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QSignalSpy>

#include "pj_widgets/RangeSlider.h"

namespace {

// The host's widget binding forwards EVERY lowerValueChanged/upperValueChanged
// emission to the plugin as a full event round trip, so a setter that re-emits
// an unchanged value turns a single handle drag into an event storm. The
// setters must be no-ops (no signal, no repaint) when the post-clamp value
// equals the current one.
TEST(RangeSliderTest, SetLowerValueEmitsOnlyOnRealChange) {
  PJ::RangeSlider slider(Qt::Horizontal, PJ::RangeSlider::kDoubleHandles);
  slider.setRange(0, 1000);
  QSignalSpy spy(&slider, &PJ::RangeSlider::lowerValueChanged);

  slider.setLowerValue(200);
  EXPECT_EQ(spy.count(), 1);
  EXPECT_EQ(slider.getLowerValue(), 200);

  slider.setLowerValue(200);  // same value: must not emit again
  EXPECT_EQ(spy.count(), 1);
}

TEST(RangeSliderTest, SetUpperValueEmitsOnlyOnRealChange) {
  PJ::RangeSlider slider(Qt::Horizontal, PJ::RangeSlider::kDoubleHandles);
  slider.setRange(0, 1000);
  QSignalSpy spy(&slider, &PJ::RangeSlider::upperValueChanged);

  slider.setUpperValue(800);
  EXPECT_EQ(spy.count(), 1);
  EXPECT_EQ(slider.getUpperValue(), 800);

  slider.setUpperValue(800);  // same value: must not emit again
  EXPECT_EQ(spy.count(), 1);
}

// A value that clamps back onto the current one is also "unchanged": dragging a
// handle past the track end floods setLowerValue/setUpperValue with
// out-of-range positions that all clamp to the same boundary.
TEST(RangeSliderTest, ClampedToCurrentValueDoesNotEmit) {
  PJ::RangeSlider slider(Qt::Horizontal, PJ::RangeSlider::kDoubleHandles);
  slider.setRange(0, 1000);
  QSignalSpy lower_spy(&slider, &PJ::RangeSlider::lowerValueChanged);
  QSignalSpy upper_spy(&slider, &PJ::RangeSlider::upperValueChanged);

  slider.setLowerValue(-50);  // clamps to 0 == current
  EXPECT_EQ(lower_spy.count(), 0);
  slider.setUpperValue(2000);  // clamps to 1000 == current
  EXPECT_EQ(upper_spy.count(), 0);
}

}  // namespace
