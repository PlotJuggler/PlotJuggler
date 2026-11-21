// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QImage>
#include <QPainter>
#include <QPushButton>
#include <QStyleOptionFocusRect>

#include "pj_widgets/Style.h"
using namespace Qt::StringLiterals;

namespace {

QImage paintFocusRect(const PJ::Style& style, const QPushButton& button, QStyle::State state) {
  QImage image(button.size(), QImage::Format_ARGB32);
  image.fill(Qt::transparent);
  QStyleOptionFocusRect opt;
  opt.initFrom(&button);
  opt.state = state;
  opt.rect = button.rect().adjusted(8, 8, -8, -8);  // the label's contents rect, as Qt passes it under a stylesheet
  QPalette palette = opt.palette;
  palette.setColor(QPalette::Button, QColor(0, 0, 200));
  palette.setColor(QPalette::ButtonText, QColor(200, 0, 0));
  opt.palette = palette;
  QPainter painter(&image);
  style.drawPrimitive(QStyle::PE_FrameFocusRect, &opt, &painter, &button);
  return image;
}

TEST(StyleFocusRing, KeyboardFocusDrawsTheButtonEdgeHalfwayFromFillToTextInk) {
  PJ::Style style(u"Fusion"_s);
  QPushButton button(u"Select All"_s);
  button.resize(80, 28);

  const QImage image = paintFocusRect(style, button, QStyle::State_HasFocus | QStyle::State_KeyboardFocusChange);
  // The ring is the button's own edge (its outermost pixels), the fill mixed half and half with the theme's text ink.
  const QColor ink = PJ::theme::text(PJ::theme::appTheme());
  const QColor ring(ink.red() / 2, ink.green() / 2, (200 + ink.blue()) / 2);
  EXPECT_EQ(image.pixelColor(0, 14), ring);
  EXPECT_EQ(image.pixelColor(1, 14), ring);
  EXPECT_EQ(image.pixelColor(3, 14).alpha(), 0);
  // Inside the label's contents rect: untouched, so the ring never cuts the text.
  EXPECT_EQ(image.pixelColor(10, 14).alpha(), 0);
}

TEST(StyleFocusRing, MouseFocusDrawsNothing) {
  PJ::Style style(u"Fusion"_s);
  QPushButton button(u"Select All"_s);
  button.resize(80, 28);

  const QImage image = paintFocusRect(style, button, QStyle::State_HasFocus);
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      ASSERT_EQ(image.pixelColor(x, y).alpha(), 0) << x << "," << y;
    }
  }
}

}  // namespace
