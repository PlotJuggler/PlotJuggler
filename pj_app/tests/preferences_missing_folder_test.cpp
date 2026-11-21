// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// A plugin folder that no longer exists stays listed in Preferences, drawn in a muted grey that still reads on the
// list's surface in both themes, so the user can find it and remove it.

#include <gtest/gtest.h>

#include <QListWidget>
#include <QSettings>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>

#include "MainWindow.h"
#include "PreferencesDialog.h"
#include "Theme.h"
#include "pj_widgets/FrameworkTokens.h"

using namespace Qt::StringLiterals;

namespace {

double contrast(const QColor& a, const QColor& b) {
  const auto luminance = [](const QColor& c) {
    const auto channel = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * channel(c.redF()) + 0.7152 * channel(c.greenF()) + 0.0722 * channel(c.blueF());
  };
  const double la = luminance(a);
  const double lb = luminance(b);
  return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

void expectMissingFolderReadable(const QString& theme_name) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString missing = dir.filePath(u"gone"_s);
  QSettings().setValue(u"Preferences::plugin_folders"_s, QStringList{dir.path(), missing});

  PJ::MainWindow window(dir.filePath(u"extensions"_s));
  PJ::Theme theme;
  theme.setTheme(theme_name);
  PJ::PreferencesDialog dialog(theme, &window);
  auto* list = dialog.findChild<QListWidget*>(u"listCustomPluginFolders"_s);
  ASSERT_NE(list, nullptr);
  ASSERT_EQ(list->count(), 2);
  ASSERT_EQ(list->item(1)->text(), missing);

  const auto tokens = PJ::theme::themeFor(theme_name == u"light"_s);
  // The dialog's lists are drawn on the input surface.
  const QColor backdrop = PJ::theme::surface(PJ::theme::Surface::Input, tokens);
  const QColor ink = list->item(1)->foreground().color();
  EXPECT_NE(list->item(1)->foreground(), list->item(0)->foreground()) << "a missing folder looks like a present one";
  // 3:1 is the WCAG floor for secondary text; the pale Highlight fill measured 1.2:1 on the light list.
  EXPECT_GE(contrast(ink, backdrop), 3.0) << ink.name().toStdString() << " on " << backdrop.name().toStdString();
  EXPECT_LT(ink.hsvSaturationF(), 0.1) << ink.name().toStdString() << " is not a grey";
}

TEST(PreferencesMissingFolderTest, MissingFolderIsAReadableGreyInTheLightTheme) {
  expectMissingFolderReadable(u"light"_s);
}

TEST(PreferencesMissingFolderTest, MissingFolderIsAReadableGreyInTheDarkTheme) {
  expectMissingFolderReadable(u"dark"_s);
}

}  // namespace
