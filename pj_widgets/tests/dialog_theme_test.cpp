// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// A dialog open during a live theme switch (Preferences, where the toggle lives) re-tints its title-bar close icon: an
// icon drawn once in the old ink is near-invisible on the new title bar.

#include <gtest/gtest.h>

#include <QApplication>
#include <QSettings>
#include <QToolButton>

#include "pj_widgets/Dialog.h"
#include "pj_widgets/SvgUtil.h"

using namespace Qt::StringLiterals;

namespace {

QImage closeIcon(const PJ::Dialog& dialog) {
  const auto* button = dialog.findChild<QToolButton*>(u"buttonClose"_s);
  return button != nullptr ? button->icon().pixmap(QSize(16, 16)).toImage() : QImage();
}

TEST(DialogThemeTest, CloseIconFollowsALiveThemeSwitch) {
  QSettings().setValue(QString::fromLatin1(PJ::kThemeSettingsKey), u"light"_s);
  PJ::Dialog dialog;
  const QImage light = closeIcon(dialog);
  ASSERT_FALSE(light.isNull());

  // The app switches theme by storing the choice, then applying the new stylesheet and application palette.
  QSettings().setValue(QString::fromLatin1(PJ::kThemeSettingsKey), u"dark"_s);
  qApp->setStyleSheet(u"QDialog { background: black; }"_s);

  EXPECT_NE(closeIcon(dialog), light) << "the close icon kept the light theme's ink";
  qApp->setStyleSheet(QString());
  QSettings().setValue(QString::fromLatin1(PJ::kThemeSettingsKey), u"light"_s);
}

}  // namespace
