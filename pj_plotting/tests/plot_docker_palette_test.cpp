// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// A theme switch (Theme.cpp) changes the application palette. PlotDocker styles
// itself and must keep ADS's built-in stylesheet off across that change: ADS 5's
// FollowPalette re-applies the stylesheet on ApplicationPaletteChange unless
// DisableStylesheet is set.

#include <gtest/gtest.h>

#include <QApplication>
#include <QPalette>

#include "pj_plotting/PlotDocker.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
using namespace Qt::StringLiterals;

TEST(PlotDockerPaletteTest, ThemeSwitchKeepsAdsStylesheetOff) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  ASSERT_TRUE(docker.styleSheet().isEmpty());

  const QPalette original = QApplication::palette();
  for (const QColor& window : {QColor(0x20, 0x20, 0x20), QColor(0xf0, 0xf0, 0xf0)}) {
    QPalette pal = QApplication::palette();
    pal.setColor(QPalette::Window, window);
    pal.setColor(QPalette::WindowText, window.lightness() < 128 ? Qt::white : Qt::black);
    QApplication::setPalette(pal);
    QApplication::processEvents();
    EXPECT_TRUE(docker.styleSheet().isEmpty()) << "ADS re-applied its stylesheet on a palette change";
  }
  QApplication::setPalette(original);
}
