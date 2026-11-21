// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// A tree or list with alternating rows (the Log panel) paints both row colours from the theme: the platform's
// AlternateBase is a near-white that leaves light text unreadable on every other row of the dark theme.

#include <gtest/gtest.h>

#include <QApplication>
#include <QListView>
#include <QTreeView>

#include "Theme.h"
#include "pj_widgets/FrameworkTokens.h"

using namespace Qt::StringLiterals;

namespace {

template <typename View>
void expectRowsOnTheDataBackdrop(const QString& theme_name) {
  PJ::Theme theme;
  theme.setTheme(theme_name);
  qApp->setStyleSheet(theme.expandedQss());
  View view;
  view.setAlternatingRowColors(true);
  view.ensurePolished();

  const QColor backdrop =
      PJ::theme::surface(PJ::theme::Surface::DataBackdrop, PJ::theme::themeFor(theme_name == u"light"_s));
  EXPECT_EQ(view.palette().color(QPalette::Base).name(), backdrop.name());
  EXPECT_EQ(view.palette().color(QPalette::AlternateBase).name(), backdrop.name());
  qApp->setStyleSheet(QString());
}

TEST(ItemViewRowsThemeTest, TreeRowsInTheDarkTheme) {
  expectRowsOnTheDataBackdrop<QTreeView>(u"dark"_s);
}

TEST(ItemViewRowsThemeTest, TreeRowsInTheLightTheme) {
  expectRowsOnTheDataBackdrop<QTreeView>(u"light"_s);
}

TEST(ItemViewRowsThemeTest, ListRowsInTheDarkTheme) {
  expectRowsOnTheDataBackdrop<QListView>(u"dark"_s);
}

}  // namespace
