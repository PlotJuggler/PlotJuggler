// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QProgressBar>
#include <QString>

#include "pj_widgets/ProgressBar.h"

namespace {

// The whole reason PJ::ProgressBar exists is to expose a `PJ--ProgressBar`
// QSS selector. QSS type selectors match on metaObject()->className(), so if
// Q_OBJECT were ever dropped this would silently fall back to "QProgressBar"
// and the bespoke style would stop applying. Pin the class name so that
// regression is a red test, not an unnoticed visual change.
TEST(ProgressBarTest, ClassNameDrivesQssSelector) {
  PJ::ProgressBar bar;
  EXPECT_STREQ(bar.metaObject()->className(), "PJ::ProgressBar");
}

// Sensible defaults so the widget reads like the mockup without per-instance
// setup: centred caption, text shown.
TEST(ProgressBarTest, CentresTextByDefault) {
  PJ::ProgressBar bar;
  EXPECT_TRUE(bar.isTextVisible());
  EXPECT_EQ(bar.alignment(), Qt::AlignCenter);
}

// It is a real QProgressBar: value/range and the format caption behave exactly
// as the base class, since the variant adds styling, not behaviour.
TEST(ProgressBarTest, BehavesLikeQProgressBar) {
  PJ::ProgressBar bar;
  bar.setRange(0, 100);
  bar.setValue(40);
  EXPECT_EQ(bar.value(), 40);

  bar.setFormat("Loading MCAP");
  EXPECT_EQ(bar.text(), QString("Loading MCAP"));
}

}  // namespace
