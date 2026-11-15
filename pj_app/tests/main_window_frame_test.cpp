// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Who owns the main window's frame: the window manager by default, PlotJuggler's
// own frameless chrome when the user opts in under Preferences -> Appearance.

#include <gtest/gtest.h>

#include <QApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QVBoxLayout>

#include "MainWindow.h"
#include "PreferencesDialog.h"
#include "Theme.h"
#include "TitleBar.h"
#include "pj_widgets/SvgUtil.h"
#include "pj_widgets/ToggleSwitch.h"
#include "support/gui_test_env.h"

using namespace Qt::StringLiterals;

namespace {

void settle() {
  for (int pass = 0; pass < 5; ++pass) {
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
  }
}

// Checks the app icon and the three window controls one by one, so a bar with
// only some of them hidden cannot pass as either frame.
[[nodiscard]] ::testing::AssertionResult frameWidgetsHidden(PJ::TitleBar& bar, bool expect_hidden) {
  for (const QString& name : {u"appIcon"_s, u"buttonMinimize"_s, u"buttonMaximize"_s, u"buttonClose"_s}) {
    auto* widget = bar.findChild<QToolButton*>(name);
    if (widget == nullptr) {
      return ::testing::AssertionFailure() << name.toStdString() << " not found";
    }
    if (widget->isHidden() != expect_hidden) {
      return ::testing::AssertionFailure() << name.toStdString() << (expect_hidden ? " is shown" : " is hidden");
    }
  }
  return ::testing::AssertionSuccess();
}

// A bar in a plain host window, laid out so its empty centre region exists.
struct BarHost {
  QWidget host;
  PJ::TitleBar* bar = new PJ::TitleBar(&host);

  BarHost() {
    auto* layout = new QVBoxLayout(&host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(bar);
    host.resize(800, 100);
    host.show();
    settle();
  }

  // Double-click on the bar's empty region: the spot a native title bar
  // answers with maximize.
  void doubleClickEmptyRegion() {
    auto* center = bar->findChild<QWidget*>(u"centerContainer"_s);
    ASSERT_NE(center, nullptr);
    QTest::mouseDClick(bar, Qt::LeftButton, Qt::NoModifier, center->mapTo(bar, center->rect().center()));
    settle();
  }
};

TEST(WindowFrameTest, CustomFrameBarKeepsIconAndWindowControls) {
  BarHost bar_host;
  EXPECT_TRUE(frameWidgetsHidden(*bar_host.bar, /*expect_hidden=*/false));
}

TEST(WindowFrameTest, CustomFrameDoubleClickMaximizes) {
  BarHost bar_host;
  bar_host.doubleClickEmptyRegion();
  EXPECT_TRUE(bar_host.host.isMaximized());
}

TEST(WindowFrameTest, SystemFrameBarDropsIconAndWindowControls) {
  BarHost bar_host;
  bar_host.bar->setSystemFrame(true);
  EXPECT_TRUE(frameWidgetsHidden(*bar_host.bar, /*expect_hidden=*/true));
}

TEST(WindowFrameTest, SystemFrameDoubleClickLeavesMaximizeToTheWindowManager) {
  BarHost bar_host;
  bar_host.bar->setSystemFrame(true);
  bar_host.doubleClickEmptyRegion();
  EXPECT_FALSE(bar_host.host.isMaximized());
}

TEST(WindowFrameTest, MainWindowDefaultsToTheSystemFrame) {
  PJ::MainWindow window;
  EXPECT_FALSE(window.windowFlags().testFlag(Qt::FramelessWindowHint));
  EXPECT_TRUE(frameWidgetsHidden(*window.titleBar(), /*expect_hidden=*/true));
}

TEST(WindowFrameTest, MainWindowGoesFramelessWhenTheUserOptsIn) {
#ifdef Q_OS_MACOS
  GTEST_SKIP() << "macOS always keeps the system frame";
#endif
  QSettings().setValue(PJ::MainWindow::kCustomTitleBarKey, true);
  PJ::MainWindow window;
  EXPECT_TRUE(window.windowFlags().testFlag(Qt::FramelessWindowHint));
  EXPECT_TRUE(frameWidgetsHidden(*window.titleBar(), /*expect_hidden=*/false));
}

// The system title bar is drawn by the platform, so it can only follow the app's
// own light/dark choice through Qt's color scheme. Headless platforms ignore the
// request, so these check what the app asks Qt for.
TEST(WindowFrameTest, ThemeSwitchRequestsTheMatchingColorScheme) {
  PJ::Theme theme;
  theme.setTheme(u"dark"_s);
  EXPECT_EQ(theme.colorScheme(), Qt::ColorScheme::Dark);
  theme.setTheme(u"light"_s);
  EXPECT_EQ(theme.colorScheme(), Qt::ColorScheme::Light);
}

TEST(WindowFrameTest, SavedDarkThemeStartsWithTheDarkColorScheme) {
  QSettings().setValue(QString::fromLatin1(PJ::kThemeSettingsKey), u"dark"_s);
  PJ::Theme theme;
  EXPECT_EQ(theme.colorScheme(), Qt::ColorScheme::Dark);
}

TEST(WindowFrameTest, PreferencesToggleStartsOffAndKeepsTheChoiceAcrossDialogs) {
  PJ::Theme theme;
  {
    PJ::PreferencesDialog dialog(theme);
    auto* toggle = dialog.findChild<PJ::ToggleSwitch*>(u"customTitleBarToggle"_s);
    ASSERT_NE(toggle, nullptr);
    EXPECT_FALSE(toggle->isChecked());

    toggle->setChecked(true, /*animate=*/false);
    dialog.accept();
  }
  EXPECT_TRUE(QSettings().value(PJ::MainWindow::kCustomTitleBarKey).toBool());

  PJ::PreferencesDialog reopened(theme);
  auto* toggle = reopened.findChild<PJ::ToggleSwitch*>(u"customTitleBarToggle"_s);
  ASSERT_NE(toggle, nullptr);
  EXPECT_TRUE(toggle->isChecked());
}

// Cases run as parallel processes, and several write the frame choice: a
// private settings directory per process keeps one case's saved choice from
// bleeding into another's, or into the real preferences.
class SettingsEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    if (!pj_app_test::isTestSuiteSelected("WindowFrameTest")) {
      return;
    }
    QCoreApplication::setOrganizationName(u"PlotJugglerTest"_s);
    QCoreApplication::setApplicationName(u"main_window_frame_test"_s);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir_.path());
  }

 private:
  QTemporaryDir settings_dir_;
};

static auto* const kEnv = ::testing::AddGlobalTestEnvironment(new SettingsEnvironment);

}  // namespace
