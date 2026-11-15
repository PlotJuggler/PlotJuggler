#include <gtest/gtest.h>

#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QImage>
#include <QPainter>

#include "BundledFont.h"

namespace {

QImage render(const QString& family, int weight) {
  QFont font(family);
  font.setPixelSize(28);
  font.setWeight(QFont::Weight(weight));
  QImage image(600, 60, QImage::Format_Grayscale8);
  image.fill(255);
  QPainter painter(&image);
  painter.setFont(font);
  painter.drawText(5, 45, QStringLiteral("Hamburgefonstiv Bold 123"));
  return image;
}

QString styleFor(const QString& family, int weight) {
  QFont font(family);
  font.setWeight(QFont::Weight(weight));
  return QFontInfo(font).styleName();
}

}  // namespace

// Qt registers a variable font as one Regular face and never moves its weight axis, so the bundled font ships as
// static faces whose Bold is a 600: every emphasis must land on that real face, never on synthesized bold.
TEST(BundledFont, EveryEmphasisRendersTheRealSixHundredFace) {
  const QString family = PJ::registerBundledFont(QStringLiteral(PJ_BUNDLED_FONT_DIR));
  ASSERT_EQ(family, QStringLiteral("PlotJuggler Sans"));
  EXPECT_EQ(QFontDatabase::styles(family), (QStringList{QStringLiteral("Regular"), QStringLiteral("Bold")}));

  EXPECT_EQ(styleFor(family, QFont::Normal), QStringLiteral("Regular"));
  for (const int weight : {QFont::DemiBold, QFont::Bold, QFont::ExtraBold}) {
    EXPECT_EQ(styleFor(family, weight), QStringLiteral("Bold")) << weight;
  }
  // A synthesized bold would draw 700 heavier than the 600 face it was derived from.
  const QImage demi_bold = render(family, QFont::DemiBold);
  EXPECT_EQ(render(family, QFont::Bold), demi_bold);
  EXPECT_NE(render(family, QFont::Normal), demi_bold);
}
