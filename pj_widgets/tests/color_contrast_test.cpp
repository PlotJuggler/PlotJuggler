// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QColor>

#include "pj_widgets/ColorContrast.h"

namespace {

const QColor kDarkBackdrop("#212128");
const QColor kLightBackdrop("#FFFFFF");

}  // namespace

TEST(ColorContrast, RatioMatchesWcagReferenceValues) {
  EXPECT_NEAR(PJ::contrastRatio(Qt::black, Qt::white), 21.0, 1e-9);
  EXPECT_NEAR(PJ::contrastRatio(Qt::white, Qt::black), 21.0, 1e-9);
  EXPECT_NEAR(PJ::contrastRatio(Qt::white, Qt::white), 1.0, 1e-9);
  // The two curve colours the dark-theme tracker text started from.
  EXPECT_NEAR(PJ::contrastRatio(QColor("#D62728"), kDarkBackdrop), 3.18, 0.01);
  EXPECT_NEAR(PJ::contrastRatio(QColor("#1F77B4"), kDarkBackdrop), 3.32, 0.01);
}

TEST(ColorContrast, InkThatAlreadyReadsIsReturnedUntouched) {
  EXPECT_EQ(PJ::inkWithContrast(QColor("#FFFFFF"), kDarkBackdrop, 4.5), QColor("#FFFFFF"));
  EXPECT_EQ(PJ::inkWithContrast(QColor("#000000"), kLightBackdrop, 4.5), QColor("#000000"));
}

TEST(ColorContrast, DarkBackgroundLightensTheInkJustEnough) {
  EXPECT_EQ(PJ::inkWithContrast(QColor("#1F77B4"), kDarkBackdrop, 4.5), QColor("#478FC2"));
  EXPECT_EQ(PJ::inkWithContrast(QColor("#D62728"), kDarkBackdrop, 4.5), QColor("#E15F60"));
}

TEST(ColorContrast, LightBackgroundDarkensTheInk) {
  const QColor orange("#FF7F0E");
  ASSERT_LT(PJ::contrastRatio(orange, kLightBackdrop), 4.5);
  const QColor ink = PJ::inkWithContrast(orange, kLightBackdrop, 4.5);
  EXPECT_GE(PJ::contrastRatio(ink, kLightBackdrop), 4.5);
  EXPECT_LT(ink.lightnessF(), orange.lightnessF());
}

TEST(ColorContrast, ResultAlwaysReachesTheRatio) {
  // A mid grey is the background where both extremes are weakest; 4.5 is still reachable.
  const QColor mid_grey("#777777");
  for (const QColor& ink : {QColor("#777777"), QColor("#7A7A80"), QColor("#00FF00")}) {
    EXPECT_GE(PJ::contrastRatio(PJ::inkWithContrast(ink, mid_grey, 4.5), mid_grey), 4.5) << ink.name().toStdString();
  }
}
