// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QSignalSpy>
#include <QTest>

#include "pj_widgets/DualOptionsWidget.h"
using namespace Qt::StringLiterals;

namespace {

TEST(DualOptionsWidgetTest, DefaultsToFirstOption) {
  PJ::DualOptionsWidget widget(u"Frame"_s, u"Arrow"_s);

  EXPECT_EQ(widget.selectedIndex(), 0);
  EXPECT_TRUE(widget.isFirstSelected());
  EXPECT_FALSE(widget.isSecondSelected());
}

TEST(DualOptionsWidgetTest, SetterChangesSelectionAndEmitsOnce) {
  PJ::DualOptionsWidget widget(u"Frame"_s, u"Arrow"_s);
  QSignalSpy spy(&widget, &PJ::DualOptionsWidget::selectionChanged);

  widget.setSelectedIndex(1);

  EXPECT_EQ(widget.selectedIndex(), 1);
  EXPECT_TRUE(widget.isSecondSelected());
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(spy.takeFirst().at(0).toInt(), 1);

  widget.setSelectedIndex(1);
  EXPECT_EQ(spy.count(), 0);
}

TEST(DualOptionsWidgetTest, IgnoresInvalidSelection) {
  PJ::DualOptionsWidget widget(u"Frame"_s, u"Arrow"_s);
  QSignalSpy spy(&widget, &PJ::DualOptionsWidget::selectionChanged);

  widget.setSelectedIndex(-1);
  widget.setSelectedIndex(2);

  EXPECT_EQ(widget.selectedIndex(), 0);
  EXPECT_EQ(spy.count(), 0);
}

TEST(DualOptionsWidgetTest, MouseClickSelectsHalf) {
  PJ::DualOptionsWidget widget(u"Frame"_s, u"Arrow"_s);
  widget.resize(widget.sizeHint());
  widget.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&widget));

  QSignalSpy spy(&widget, &PJ::DualOptionsWidget::selectionChanged);
  QTest::mouseClick(&widget, Qt::LeftButton, Qt::NoModifier, QPoint(widget.width() - 2, widget.height() / 2));

  EXPECT_EQ(widget.selectedIndex(), 1);
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(spy.takeFirst().at(0).toInt(), 1);
}

TEST(DualOptionsWidgetTest, KeyboardChangesSelection) {
  PJ::DualOptionsWidget widget(u"Frame"_s, u"Arrow"_s);
  widget.resize(widget.sizeHint());
  widget.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&widget));
  widget.setFocus();

  QTest::keyClick(&widget, Qt::Key_Right);
  EXPECT_EQ(widget.selectedIndex(), 1);

  QTest::keyClick(&widget, Qt::Key_Left);
  EXPECT_EQ(widget.selectedIndex(), 0);

  QTest::keyClick(&widget, Qt::Key_Space);
  EXPECT_EQ(widget.selectedIndex(), 1);
}

TEST(DualOptionsWidgetTest, SizeHintGrowsWithText) {
  PJ::DualOptionsWidget short_widget(u"F"_s, u"A"_s);
  PJ::DualOptionsWidget long_widget(u"Frame"_s, u"Longer arrow label"_s);

  EXPECT_GT(long_widget.sizeHint().width(), short_widget.sizeHint().width());
  EXPECT_GT(short_widget.sizeHint().height(), 0);
}

TEST(DualOptionsWidgetTest, SupportsThreeOptions) {
  PJ::DualOptionsWidget widget(QStringList{u"Contains"_s, u"Wildcard"_s, u"RegExp"_s});
  QSignalSpy spy(&widget, &PJ::DualOptionsWidget::selectionChanged);

  EXPECT_EQ(widget.optionCount(), 3);
  widget.setSelectedIndex(2);
  EXPECT_EQ(widget.selectedIndex(), 2);
  widget.setSelectedIndex(3);
  EXPECT_EQ(widget.selectedIndex(), 2);
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(spy.takeFirst().at(0).toInt(), 2);
}

TEST(DualOptionsWidgetTest, MouseClickSelectsThird) {
  PJ::DualOptionsWidget widget(QStringList{u"Contains"_s, u"Wildcard"_s, u"RegExp"_s});
  widget.resize(widget.sizeHint());
  widget.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&widget));

  QTest::mouseClick(&widget, Qt::LeftButton, Qt::NoModifier, QPoint(widget.width() - 2, widget.height() / 2));
  EXPECT_EQ(widget.selectedIndex(), 2);

  QTest::mouseClick(&widget, Qt::LeftButton, Qt::NoModifier, QPoint(widget.width() / 2, widget.height() / 2));
  EXPECT_EQ(widget.selectedIndex(), 1);
}

TEST(DualOptionsWidgetTest, KeyboardStepsThroughThreeOptions) {
  PJ::DualOptionsWidget widget(QStringList{u"Contains"_s, u"Wildcard"_s, u"RegExp"_s});
  widget.resize(widget.sizeHint());
  widget.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&widget));
  widget.setFocus();

  QTest::keyClick(&widget, Qt::Key_Right);
  EXPECT_EQ(widget.selectedIndex(), 1);
  QTest::keyClick(&widget, Qt::Key_Right);
  EXPECT_EQ(widget.selectedIndex(), 2);
  QTest::keyClick(&widget, Qt::Key_Right);
  EXPECT_EQ(widget.selectedIndex(), 2) << "Right clamps at the last segment";
  QTest::keyClick(&widget, Qt::Key_Left);
  EXPECT_EQ(widget.selectedIndex(), 1);
  QTest::keyClick(&widget, Qt::Key_Space);
  EXPECT_EQ(widget.selectedIndex(), 2) << "Space cycles to the next segment";
  QTest::keyClick(&widget, Qt::Key_Space);
  EXPECT_EQ(widget.selectedIndex(), 0) << "Space wraps around after the last segment";
}

TEST(DualOptionsWidgetTest, DefaultsToHorizontalOrientation) {
  PJ::DualOptionsWidget widget(u"Frame"_s, u"Arrow"_s);
  EXPECT_EQ(widget.orientation(), Qt::Horizontal);
}

TEST(DualOptionsWidgetTest, VerticalOrientationSwapsSizeHintAxes) {
  PJ::DualOptionsWidget horizontal(QStringList{u"Contains"_s, u"Wildcard"_s, u"RegExp"_s});
  PJ::DualOptionsWidget vertical(QStringList{u"Contains"_s, u"Wildcard"_s, u"RegExp"_s});
  vertical.setOrientation(Qt::Vertical);

  // Vertical stacks the 3 segments: taller than the single-row horizontal strip,
  // and only one segment wide instead of three.
  EXPECT_GT(vertical.sizeHint().height(), horizontal.sizeHint().height());
  EXPECT_LT(vertical.sizeHint().width(), horizontal.sizeHint().width());
}

TEST(DualOptionsWidgetTest, VerticalMouseClickSelectsBySegmentRow) {
  PJ::DualOptionsWidget widget(QStringList{u"Contains"_s, u"Wildcard"_s, u"RegExp"_s});
  widget.setOrientation(Qt::Vertical);
  widget.resize(widget.sizeHint());
  widget.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&widget));

  QTest::mouseClick(&widget, Qt::LeftButton, Qt::NoModifier, QPoint(widget.width() / 2, widget.height() - 2));
  EXPECT_EQ(widget.selectedIndex(), 2) << "click near the bottom selects the last segment";

  QTest::mouseClick(&widget, Qt::LeftButton, Qt::NoModifier, QPoint(widget.width() / 2, 2));
  EXPECT_EQ(widget.selectedIndex(), 0) << "click near the top selects the first segment";
}

TEST(DualOptionsWidgetTest, VerticalKeyboardUsesUpDown) {
  PJ::DualOptionsWidget widget(u"Frame"_s, u"Arrow"_s);
  widget.setOrientation(Qt::Vertical);
  widget.resize(widget.sizeHint());
  widget.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&widget));
  widget.setFocus();

  QTest::keyClick(&widget, Qt::Key_Down);
  EXPECT_EQ(widget.selectedIndex(), 1);
  QTest::keyClick(&widget, Qt::Key_Up);
  EXPECT_EQ(widget.selectedIndex(), 0);
}

TEST(DualOptionsWidgetTest, DisabledSegmentIsNotSelectable) {
  PJ::DualOptionsWidget widget(QStringList{u"Row"_s, u"Column"_s, u"Combine"_s});
  widget.setSegmentEnabled(2, false);
  EXPECT_FALSE(widget.isSegmentEnabled(2));
  QSignalSpy spy(&widget, &PJ::DualOptionsWidget::selectionChanged);

  // Programmatic selection of a disabled segment is a no-op (nothing emitted).
  widget.setSelectedIndex(2);
  EXPECT_EQ(widget.selectedIndex(), 0);
  EXPECT_EQ(spy.count(), 0);

  // An enabled segment still selects normally.
  widget.setSelectedIndex(1);
  EXPECT_EQ(widget.selectedIndex(), 1);
  ASSERT_EQ(spy.count(), 1);

  // Re-enabling makes it selectable again.
  widget.setSegmentEnabled(2, true);
  widget.setSelectedIndex(2);
  EXPECT_EQ(widget.selectedIndex(), 2);
}

TEST(DualOptionsWidgetTest, KeyboardAndClickSkipDisabledSegment) {
  PJ::DualOptionsWidget widget(QStringList{u"Row"_s, u"Column"_s, u"Combine"_s});
  widget.setSegmentEnabled(2, false);
  widget.resize(widget.sizeHint());
  widget.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&widget));
  widget.setFocus();

  QTest::keyClick(&widget, Qt::Key_Right);
  EXPECT_EQ(widget.selectedIndex(), 1);
  QTest::keyClick(&widget, Qt::Key_Right);
  EXPECT_EQ(widget.selectedIndex(), 1) << "Right cannot reach the disabled last segment";
  QTest::keyClick(&widget, Qt::Key_Space);
  EXPECT_EQ(widget.selectedIndex(), 0) << "Space cycles past the disabled segment, wrapping to 0";

  QTest::mouseClick(&widget, Qt::LeftButton, Qt::NoModifier, QPoint(widget.width() - 2, widget.height() / 2));
  EXPECT_EQ(widget.selectedIndex(), 0) << "a click on the disabled segment is inert";
}

}  // namespace
