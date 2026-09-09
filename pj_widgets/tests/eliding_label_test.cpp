// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QFontMetrics>

#include "pj_widgets/ElidingLabel.h"
using namespace Qt::StringLiterals;

namespace {

// Qt elides with U+2026 when the font has it, "..." otherwise.
bool hasEllipsis(const QString& text) {
  return text.contains(QChar(0x2026)) || text.contains(u"..."_s);
}

const QString kLongText =
    u"Receives live H.264 video via standard WHEP and feeds each stream to "
    u"PlotJuggler as canonical VideoFrame messages over one HTTP endpoint"_s;

TEST(ElidingLabelTest, SingleLineElidesByDefault) {
  PJ::ElidingLabel label;
  label.setFullText(kLongText);
  label.applyAvailableWidth(120);

  EXPECT_FALSE(label.text().contains(u'\n'));
  EXPECT_TRUE(hasEllipsis(label.text()));
  EXPECT_LE(label.fontMetrics().horizontalAdvance(label.text()), 120);
}

TEST(ElidingLabelTest, TwoLinesElideOverflowIntoLastLine) {
  PJ::ElidingLabel label;
  label.setMaxLineCount(2);
  label.setFullText(kLongText);
  label.applyAvailableWidth(150);

  const QStringList lines = label.text().split(u'\n');
  ASSERT_EQ(lines.size(), 2);
  EXPECT_FALSE(hasEllipsis(lines[0]));
  EXPECT_TRUE(hasEllipsis(lines[1]));
  const QFontMetrics metrics = label.fontMetrics();
  EXPECT_LE(metrics.horizontalAdvance(lines[1]), 150);
}

TEST(ElidingLabelTest, ShortTextIsShownVerbatim) {
  PJ::ElidingLabel label;
  label.setMaxLineCount(2);
  label.setFullText(u"Short and sweet"_s);
  label.applyAvailableWidth(400);

  EXPECT_EQ(label.text(), u"Short and sweet"_s);
}

TEST(ElidingLabelTest, TextFittingTwoLinesGetsNoEllipsis) {
  PJ::ElidingLabel label;
  label.setMaxLineCount(2);
  // A width that holds exactly one "lorem ipsum" per line, so two of them
  // wrap onto two full lines with nothing left over to elide.
  const int line_width = label.fontMetrics().horizontalAdvance(u"lorem ipsum"_s) + 4;
  label.setFullText(u"lorem ipsum lorem ipsum"_s);
  label.applyAvailableWidth(line_width);

  EXPECT_EQ(label.text().count(u'\n'), 1);
  EXPECT_FALSE(hasEllipsis(label.text()));
}

TEST(ElidingLabelTest, WiderBudgetRestoresMoreText) {
  PJ::ElidingLabel label;
  label.setMaxLineCount(2);
  label.setFullText(kLongText);
  label.applyAvailableWidth(150);
  const QString narrow = label.text();

  label.applyAvailableWidth(600);
  EXPECT_GT(label.text().length(), narrow.length());
}

// A QSS-polished font arrives after construction; the label must re-elide
// with the new metrics at the width it was last granted, not stay laid out
// for the construction-time font.
TEST(ElidingLabelTest, FontChangeReElidesAtLastBudget) {
  PJ::ElidingLabel label;
  label.setMaxLineCount(2);
  label.setFullText(kLongText);
  label.applyAvailableWidth(200);
  const QString before = label.text();

  QFont bigger = label.font();
  bigger.setPointSize(bigger.pointSize() * 2);
  label.setFont(bigger);

  EXPECT_NE(label.text(), before);
  const QStringList lines = label.text().split(u'\n');
  ASSERT_EQ(lines.size(), 2);
  EXPECT_LE(QFontMetrics(bigger).horizontalAdvance(lines[1]), 200);
}

}  // namespace
