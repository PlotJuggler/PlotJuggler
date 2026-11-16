// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The footer's "Reset to defaults" button only resets the chrome-metric scrubbers,
// which ship hidden outside --debug-mode: the button must be shown exactly when
// they are, or clicking it changes nothing the user can see.

#include <gtest/gtest.h>

#include <QWidget>
#include <utility>

#include "DebugMode.h"
#include "PreferencesDialog.h"
#include "Theme.h"

using namespace Qt::StringLiterals;

namespace {

// Opens the dialog under `debug_mode` and returns {button visible, scrubbers visible}.
std::pair<bool, bool> resetButtonAndScrubbersVisible(bool debug_mode) {
  PJ::setDebugMode(debug_mode);
  PJ::Theme theme;
  PJ::PreferencesDialog dialog(theme);
  dialog.show();
  auto* button = dialog.findChild<QWidget*>(u"buttonResetDefaults"_s);
  auto* scrubbers = dialog.findChild<QWidget*>(u"debugChromeWidget"_s);
  EXPECT_NE(button, nullptr);
  EXPECT_NE(scrubbers, nullptr);
  const std::pair<bool, bool> visible{
      button != nullptr && button->isVisible(), scrubbers != nullptr && scrubbers->isVisible()};
  PJ::setDebugMode(false);  // process-global: a runner executing every case in one process
  return visible;
}

TEST(PreferencesResetButtonTest, HiddenWithTheChromeScrubbersOutsideDebugMode) {
  const auto [button_visible, scrubbers_visible] = resetButtonAndScrubbersVisible(/*debug_mode=*/false);
  EXPECT_FALSE(scrubbers_visible);
  EXPECT_FALSE(button_visible);
}

TEST(PreferencesResetButtonTest, ShownWithTheChromeScrubbersInDebugMode) {
  const auto [button_visible, scrubbers_visible] = resetButtonAndScrubbersVisible(/*debug_mode=*/true);
  EXPECT_TRUE(scrubbers_visible);
  EXPECT_TRUE(button_visible);
}

}  // namespace
