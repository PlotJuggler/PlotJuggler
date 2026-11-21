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
  palette.setColor(QPalette::ButtonText, Qt::red);
  opt.palette = palette;
  QPainter painter(&image);
  style.drawPrimitive(QStyle::PE_FrameFocusRect, &opt, &painter, &button);
  return image;
}

TEST(StyleFocusRing, KeyboardFocusDrawsALabelColouredRingAtTheButtonEdge) {
  PJ::Style style(u"Fusion"_s);
  QPushButton button(u"Select All"_s);
  button.resize(80, 28);

  const QImage image = paintFocusRect(style, button, QStyle::State_HasFocus | QStyle::State_KeyboardFocusChange);
  // The button's outermost pixels stay for its fill, then the ring, in the label colour.
  EXPECT_EQ(image.pixelColor(0, 14).alpha(), 0);
  EXPECT_EQ(image.pixelColor(3, 14), QColor(Qt::red));
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
