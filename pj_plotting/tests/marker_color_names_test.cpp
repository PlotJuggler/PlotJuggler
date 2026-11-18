// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The marker engine carries its own copy of Qt's color names (it cannot link Qt); this pins the copy to the Qt the app
// is built with, so a rule's "orange" is the orange the rest of the UI draws.

#include <gtest/gtest.h>

#include <QColor>
#include <QStringList>

#include "pj_scripting/marker_engine.h"

TEST(MarkerColorNames, MatchQt) {
  const QStringList names = QColor::colorNames();
  ASSERT_FALSE(names.isEmpty());
  for (const QString& name : names) {
    const QColor qt = QColor::fromString(name);
    const auto ours = PJ::scripting::parseMarkerColor(name.toUpper().toStdString());
    ASSERT_TRUE(ours.has_value()) << name.toStdString();
    EXPECT_EQ(ours->r, qt.red()) << name.toStdString();
    EXPECT_EQ(ours->g, qt.green()) << name.toStdString();
    EXPECT_EQ(ours->b, qt.blue()) << name.toStdString();
    EXPECT_EQ(ours->a, qt.alpha()) << name.toStdString();
  }
}
