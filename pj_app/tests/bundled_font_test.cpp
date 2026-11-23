#include <gtest/gtest.h>

#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
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

// Numbers are kept in columns in this font by padding with spaces as wide as a digit and as a period
// (pj_widgets/NumberPadding.h): a font update that breaks either width misaligns the curve list's Value column, the
// tracker's value box and the pixel inspector.
TEST(BundledFont, DigitsAndTheirSpacesShareAnAdvance) {
  const QString family = PJ::registerBundledFont(QStringLiteral(PJ_BUNDLED_FONT_DIR));
  ASSERT_FALSE(family.isEmpty());
  for (const int weight : {QFont::Normal, QFont::Bold}) {
    QFont font(family, 24);
    font.setWeight(QFont::Weight(weight));
    const QFontMetricsF metrics(font);
    const qreal digit = metrics.horizontalAdvance(QLatin1Char('0'));
    for (const QChar other : QStringLiteral("123456789")) {
      EXPECT_DOUBLE_EQ(metrics.horizontalAdvance(other), digit)
          << weight << " digit " << static_cast<int>(other.unicode());
    }
    // kFigureSpace and kPunctuationSpace: this runner links no pj_widgets.
    EXPECT_DOUBLE_EQ(metrics.horizontalAdvance(QChar(0x2007)), digit) << weight;
    EXPECT_DOUBLE_EQ(metrics.horizontalAdvance(QChar(0x2008)), metrics.horizontalAdvance(QLatin1Char('.'))) << weight;
  }
}
