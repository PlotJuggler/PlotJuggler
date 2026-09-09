// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QSignalSpy>
#include <QTest>

#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/ToggleSwitch.h"
using namespace Qt::StringLiterals;

namespace {

TEST(ToggleSwitchTest, TextDefaultsEmpty) {
  PJ::ToggleSwitch sw;
  EXPECT_TRUE(sw.text().isEmpty());
  EXPECT_EQ(sw.labelSide(), PJ::ToggleSwitch::LabelSide::Right);
}

TEST(ToggleSwitchTest, TextAndLabelSideRoundTrip) {
  PJ::ToggleSwitch sw;
  sw.setText(u"Enabled"_s);
  EXPECT_EQ(sw.text(), u"Enabled"_s);
  sw.setLabelSide(PJ::ToggleSwitch::LabelSide::Left);
  EXPECT_EQ(sw.labelSide(), PJ::ToggleSwitch::LabelSide::Left);
}

// Backward-compat pin: with no label the widget keeps its historical compact
// 34x18 size hint exactly, so existing form/grid users are unaffected.
TEST(ToggleSwitchTest, SizeHintUnchangedWithoutText) {
  PJ::ToggleSwitch sw;
  EXPECT_EQ(sw.sizeHint(), QSize(34, 18));
  EXPECT_EQ(sw.minimumSizeHint(), QSize(34, 18));
}

TEST(ToggleSwitchTest, SizeHintGrowsWithLabel) {
  PJ::ToggleSwitch bare;
  PJ::ToggleSwitch labeled;
  labeled.setText(u"Enable streaming"_s);
  EXPECT_GT(labeled.sizeHint().width(), bare.sizeHint().width());

  PJ::ToggleSwitch longer;
  longer.setText(u"Enable streaming playback now"_s);
  EXPECT_GT(longer.sizeHint().width(), labeled.sizeHint().width());
}

// The whole widget is clickable, so clicking over the label region (opposite
// the track) must still toggle — that's the QCheckBox-like behavior.
TEST(ToggleSwitchTest, ClickingLabelRegionToggles) {
  PJ::ToggleSwitch sw;
  sw.setText(u"Enable streaming"_s);
  sw.setLabelSide(PJ::ToggleSwitch::LabelSide::Right);  // track on the LEFT
  sw.resize(sw.sizeHint());
  sw.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&sw));

  ASSERT_FALSE(sw.isChecked());
  // Click far to the right, over the label, not the track.
  QTest::mouseClick(&sw, Qt::LeftButton, Qt::NoModifier, QPoint(sw.width() - 4, sw.height() / 2));
  EXPECT_TRUE(sw.isChecked());
}

// Regression: the inline label's size hint must reserve a few px BEYOND the
// exact text advance. With zero slack (size hint == text width to the pixel),
// any sub-pixel/DPI/font-render difference between the size-hint metrics and the
// actual paint shaves the tail — the real bug was a converted plugin checkbox
// whose "...(if present)" rendered as "...(if prese…".
TEST(ToggleSwitchTest, LabelSizeHintReservesSlackBeyondText) {
  PJ::ToggleSwitch sw;
  sw.setText(u"Override with the message header timestamp (if present)"_s);
  const int advance = sw.fontMetrics().horizontalAdvance(sw.text());
  // Assert a real margin remains after the switch and canonical spacing
  // allowance are accounted for.
  EXPECT_GT(sw.sizeHint().width() - advance, 34 + PJ::theme::space(PJ::theme::Space::Comfortable))
      << "the label must get more than just the switch+spacing width, so the text never elides at the size hint";
}

// Existing no-label click behavior is preserved.
TEST(ToggleSwitchTest, ClickTogglesWithoutLabel) {
  PJ::ToggleSwitch sw;
  sw.resize(sw.sizeHint());
  sw.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&sw));

  ASSERT_FALSE(sw.isChecked());
  QTest::mouseClick(&sw, Qt::LeftButton, Qt::NoModifier, QPoint(sw.width() / 2, sw.height() / 2));
  EXPECT_TRUE(sw.isChecked());
}

}  // namespace
