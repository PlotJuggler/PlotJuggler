// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Behavioural contract for the pill CheckButton: it is a checkable
// QAbstractButton (a drop-in for QCheckBox via toggled()), a click flips the
// state and emits toggled() with the new value, and its size hint grows with
// the label so the capsule always fits its text.

#include <gtest/gtest.h>

#include <QApplication>
#include <QSignalSpy>

#include "pj_widgets/CheckButton.h"
using namespace Qt::StringLiterals;

namespace {

TEST(CheckButtonTest, IsCheckableByDefault) {
  PJ::CheckButton button;
  EXPECT_TRUE(button.isCheckable());
  EXPECT_FALSE(button.isChecked());
}

TEST(CheckButtonTest, ClickTogglesAndEmits) {
  PJ::CheckButton button(u"Override color"_s);
  QSignalSpy spy(&button, &PJ::CheckButton::toggled);

  button.click();
  EXPECT_TRUE(button.isChecked());
  ASSERT_EQ(spy.count(), 1);
  EXPECT_TRUE(spy.takeFirst().at(0).toBool());

  button.click();
  EXPECT_FALSE(button.isChecked());
  ASSERT_EQ(spy.count(), 1);
  EXPECT_FALSE(spy.takeFirst().at(0).toBool());
}

TEST(CheckButtonTest, ProgrammaticSetCheckedDoesNotClick) {
  PJ::CheckButton button;
  QSignalSpy toggled(&button, &PJ::CheckButton::toggled);
  button.setChecked(true);  // QAbstractButton emits toggled but not clicked
  EXPECT_TRUE(button.isChecked());
  EXPECT_EQ(toggled.count(), 1);
}

TEST(CheckButtonTest, TextRoundTrips) {
  PJ::CheckButton button;
  button.setText(u"X arrow only"_s);
  EXPECT_EQ(button.text(), u"X arrow only"_s);
}

TEST(CheckButtonTest, SizeHintGrowsWithText) {
  PJ::CheckButton short_btn(u"On"_s);
  PJ::CheckButton long_btn(u"A much longer label"_s);
  EXPECT_GT(long_btn.sizeHint().width(), short_btn.sizeHint().width());
  EXPECT_GT(short_btn.sizeHint().height(), 0);
}

}  // namespace
