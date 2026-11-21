// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Icon inks the theme must reach: the disabled look PJ::Style gives every pixmap icon, and SVGs whose authored ink has
// to fold onto the theme's icon ink.

#include <gtest/gtest.h>

#include <QApplication>
#include <QColor>
#include <QIcon>
#include <QImage>
#include <QPixmap>

#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/Style.h"
#include "pj_widgets/SvgUtil.h"

using namespace Qt::StringLiterals;

namespace {

using PJ::theme::Theme;

TEST(IconInkTest, DisabledPixmapIconIsPaintedInTheDisabledInk) {
  QApplication::setStyle(new PJ::Style(u"Fusion"_s));
  // Under the app stylesheet the style is wrapped in a QStyleSheetStyle, which must still reach PJ::Style.
  qApp->setStyleSheet(u"QWidget {}"_s);

  QPixmap glyph(16, 16);
  glyph.fill(Qt::white);
  const QImage disabled = QIcon(glyph).pixmap(QSize(16, 16), QIcon::Disabled).toImage();

  const QColor expected = PJ::theme::iconInkDisabled(PJ::theme::appTheme());
  ASSERT_TRUE(expected.isValid());
  EXPECT_EQ(disabled.pixelColor(8, 8).rgb(), expected.rgb());
  qApp->setStyleSheet(QString());
}

TEST(IconInkTest, InvertGlyphFollowsTheDarkIconInk) {
  const QImage image = PJ::loadSvg(u":/resources/svg/invert.svg"_s, u"dark"_s).toImage();
  const QRgb ink = PJ::theme::iconInk(Theme::Dark).rgb();
  int opaque = 0;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QColor c = image.pixelColor(x, y);
      if (c.alpha() == 255) {
        ++opaque;
        EXPECT_EQ(c.rgb(), ink) << "at " << x << "," << y;
      }
    }
  }
  EXPECT_GT(opaque, 0);
}

TEST(IconInkTest, LogoAxesFollowTheDarkIconInk) {
  const QImage image = PJ::loadSvg(u":/resources/svg/plotjuggler.svg"_s, u"dark"_s).toImage();
  // The vertical axis runs at x 31..47 of the 512-unit viewBox: pixel column 4 at 64 px.
  const QColor axis = image.pixelColor(4, 32);
  EXPECT_GT(axis.alpha(), 200);
  // The axes path is drawn at 94% fill opacity, so its colour is compared within rounding.
  const QColor ink = PJ::theme::iconInk(Theme::Dark);
  EXPECT_NEAR(axis.red(), ink.red(), 2);
  EXPECT_NEAR(axis.green(), ink.green(), 2);
  EXPECT_NEAR(axis.blue(), ink.blue(), 2);
}

}  // namespace
