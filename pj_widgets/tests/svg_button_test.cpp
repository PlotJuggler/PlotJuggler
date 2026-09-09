// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QEvent>

#include "pj_widgets/SvgButton.h"

namespace PJ {
namespace {

// One QApplication for the whole test binary; QWidget construction requires it.
// A real icon present on origin/main, so loadSvg (resources.qrc is linked into the
// test binary) yields a non-null themed pixmap.
constexpr auto kIcon = ":/resources/svg/trash.svg";

TEST(SvgButton, DefaultSizeIs24) {
  SvgButton b;
  EXPECT_EQ(b.iconSize(), QSize(SvgButton::kDefaultExtent, SvgButton::kDefaultExtent));
  EXPECT_EQ(b.minimumSize(), QSize(24, 24));
  EXPECT_EQ(b.maximumSize(), QSize(24, 24));
  // Default chrome: flat icon button, not focus-stealing.
  EXPECT_TRUE(b.autoRaise());
  EXPECT_EQ(b.focusPolicy(), Qt::NoFocus);
}

TEST(SvgButton, SmallerSizeIs20) {
  SvgButton b(kIcon, SvgButton::Size::kSmaller);
  EXPECT_EQ(b.iconSize(), QSize(20, 20));
  EXPECT_EQ(b.minimumSize(), QSize(20, 20));
  EXPECT_EQ(b.maximumSize(), QSize(20, 20));
}

TEST(SvgButton, SetExtentDecouplesButtonAndIcon) {
  SvgButton b(kIcon);
  b.setExtent(26, 24);  // chrome-metrics style: icon_size 24 + padding 2
  EXPECT_EQ(b.minimumSize(), QSize(26, 26));
  EXPECT_EQ(b.iconSize(), QSize(24, 24));
}

TEST(SvgButton, IconPathStoredAndRendered) {
  SvgButton b(kIcon);
  EXPECT_EQ(b.iconPath(), QString(kIcon));
  EXPECT_FALSE(b.icon().isNull()) << "loadSvg should yield a themed icon (resources linked)";

  // Re-pointing the glyph (state swap) updates the stored path + the icon.
  b.setIconPath(":/resources/svg/refresh.svg");
  EXPECT_EQ(b.iconPath(), QString(":/resources/svg/refresh.svg"));
  EXPECT_FALSE(b.icon().isNull());
}

TEST(SvgButton, Checkable) {
  SvgButton b(kIcon);
  EXPECT_FALSE(b.isCheckable());
  b.setCheckable(true);
  EXPECT_TRUE(b.isCheckable());
}

// A theme-change event re-tints in place without losing the icon — the
// transparent retint hook (no host wiring).
TEST(SvgButton, RetintsOnThemeChangeEvent) {
  SvgButton b(kIcon);
  ASSERT_FALSE(b.icon().isNull());
  QEvent style_change(QEvent::StyleChange);
  QApplication::sendEvent(&b, &style_change);
  EXPECT_FALSE(b.icon().isNull());
  EXPECT_EQ(b.iconPath(), QString(kIcon));
}

}  // namespace
}  // namespace PJ
