// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <set>
#include <string>

#include "pj_widgets/StateColors.h"
using namespace Qt::StringLiterals;

namespace {

TEST(StateColors, SameStringSameIndexEveryCall) {
  const std::size_t first = PJ::stateColorIndex("RUNNING");
  for (int repeat = 0; repeat < 10; ++repeat) {
    EXPECT_EQ(PJ::stateColorIndex("RUNNING"), first);
  }
}

// Pins concrete assignments: FNV-1a is deterministic, so any change to the hash or the palette size silently recolors
// every existing screenshot and session; this makes that change loud. Values frozen from the FNV-1a definition.
TEST(StateColors, HashAssignmentsArePinned) {
  static_assert(PJ::kStatePalette.size() == 10);
  static_assert(PJ::stateColorIndex("") == 7);
  static_assert(PJ::stateColorIndex("True") == 7);
  static_assert(PJ::stateColorIndex("False") == 4);
  static_assert(PJ::stateColorIndex("RUNNING") == 0);
  // The boolean pair must render distinctly (guards a future palette shrink).
  EXPECT_NE(PJ::stateColorIndex("True"), PJ::stateColorIndex("False"));
}

TEST(StateColors, DistinctStringsSpreadAcrossPalette) {
  std::set<std::size_t> used;
  const std::array<std::string, 12> values = {"IDLE",   "RUNNING", "PAUSED", "ERROR",  "True",   "False",
                                              "MODE_A", "MODE_B",  "MODE_C", "MODE_D", "MODE_E", "MODE_F"};
  for (const std::string& value : values) {
    used.insert(PJ::stateColorIndex(value));
  }
  // 12 hashed values into 10 buckets must hit a healthy portion of the palette.
  EXPECT_GE(used.size(), 5U);
}

TEST(StateColors, QColorAgreesWithRgb) {
  const PJ::StateRgb rgb = PJ::stateRgb("RUNNING");
  const QColor color = PJ::stateColor(u"RUNNING"_s);
  EXPECT_EQ(color.red(), rgb.r);
  EXPECT_EQ(color.green(), rgb.g);
  EXPECT_EQ(color.blue(), rgb.b);
}

TEST(StateColors, Utf8PathMatchesByteHash) {
  // Non-ASCII must hash the UTF-8 encoding, matching the string_view overloads.
  const QString value = QString::fromUtf8("état·δ");
  const QByteArray utf8 = value.toUtf8();
  const PJ::StateRgb rgb = PJ::stateRgb(std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size())));
  const QColor color = PJ::stateColor(value);
  EXPECT_EQ(color.red(), rgb.r);
  EXPECT_EQ(color.green(), rgb.g);
  EXPECT_EQ(color.blue(), rgb.b);
}

}  // namespace
