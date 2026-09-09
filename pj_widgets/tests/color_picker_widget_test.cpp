// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Contract for ColorPickerWidget: it holds a current QColor (get/set) and the
// programmatic setColor() is silent — colorChanged() is reserved for user picks
// through the popup. The swatch has a fixed standard size.

#include <gtest/gtest.h>

#include <QApplication>
#include <QSignalSpy>

#include "pj_widgets/ColorPickerWidget.h"

namespace {

TEST(ColorPickerWidgetTest, HoldsAndReturnsColor) {
  PJ::ColorPickerWidget picker;
  picker.setColor(QColor(10, 20, 30));
  EXPECT_EQ(picker.color(), QColor(10, 20, 30));
}

TEST(ColorPickerWidgetTest, SetColorIsSilent) {
  PJ::ColorPickerWidget picker;
  QSignalSpy spy(&picker, &PJ::ColorPickerWidget::colorChanged);
  picker.setColor(QColor(200, 30, 30));
  EXPECT_EQ(spy.count(), 0) << "programmatic setColor must not emit colorChanged";
}

TEST(ColorPickerWidgetTest, HasFixedStandardSize) {
  PJ::ColorPickerWidget picker;
  EXPECT_GT(picker.sizeHint().width(), 0);
  EXPECT_GT(picker.sizeHint().height(), 0);
}

}  // namespace
