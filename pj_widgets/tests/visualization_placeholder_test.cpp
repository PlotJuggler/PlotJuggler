// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QBuffer>
#include <QCoreApplication>
#include <QImageReader>
#include <QLabel>
#include <QToolButton>
#include <set>
#include <vector>

#include "pj_widgets/SectionHeaderBand.h"
#include "pj_widgets/VisualizationPlaceholderWidget.h"
using namespace Qt::StringLiterals;

namespace {

TEST(VisualizationPlaceholderEasterEggTest, CipherMatchesThePackScript) {
  QByteArray plain;
  for (int i = 0; i < 70; ++i) {
    plain.push_back(static_cast<char>(i));
  }
  // scripts/pack_easter_eggs.py cipher(bytes(range(70)), b"test-key", "x_A.gif.enc")
  const QByteArray expected = QByteArray::fromHex(
      "0d58dc80112ee15215b5481d641a35f4cc334296878a4fc1af535b0b2285ceb44dfc0242650ae0b00c567c8487c2184a9013885d263b36d1"
      "9725be9cf9a6d07029b1bfb7b1b5");
  const QByteArray encrypted = PJ::easterEggCipher(plain, "test-key", u"x_A.gif.enc"_s);
  EXPECT_EQ(encrypted, expected);
  EXPECT_EQ(PJ::easterEggCipher(encrypted, "test-key", u"x_A.gif.enc"_s), plain);
  EXPECT_NE(PJ::easterEggCipher(encrypted, "other-key", u"x_A.gif.enc"_s), plain);
}

TEST(VisualizationPlaceholderEasterEggTest, EveryPooledGifIsATransparentAnimated128Square) {
  const QStringList pool = PJ::easterEggPool();
  ASSERT_FALSE(pool.isEmpty());
  if (PJ::loadEasterEgg(pool.front()).isEmpty()) {
    GTEST_SKIP() << "built without PJ_EASTER_EGG_KEY: the pool cannot be decrypted, and the easter egg is off";
  }
  for (const QString& path : pool) {
    QByteArray gif = PJ::loadEasterEgg(path);
    QBuffer buffer(&gif);
    QImageReader reader(&buffer);
    EXPECT_TRUE(reader.canRead()) << path.toStdString();
    EXPECT_EQ(reader.size(), QSize(128, 128)) << path.toStdString();
    EXPECT_TRUE(reader.supportsAnimation()) << path.toStdString();
    EXPECT_EQ(reader.read().pixelColor(0, 0).alpha(), 0) << path.toStdString();
  }
}

TEST(VisualizationPlaceholderEasterEggTest, ACharacterOnlyEverShowsItsOwnVariants) {
  EXPECT_EQ(PJ::easterEggCharacter(u":/resources/gif/llama-coat_A.gif.enc"_s), u"llama-coat"_s);
  EXPECT_EQ(PJ::easterEggCharacter(u":/resources/gif/capybara-sipping.gif.enc"_s), u"capybara-sipping"_s);

  const QStringList pool{u"a/llama-coat_A.gif"_s, u"a/llama-coat_B.gif"_s, u"a/newt-hat_A.gif"_s};
  for (int i = 0; i < 50; ++i) {
    EXPECT_EQ(PJ::easterEggCharacter(PJ::pickEasterEggGif(pool, u"llama-coat"_s)), u"llama-coat"_s);
    EXPECT_EQ(PJ::pickEasterEggGif(pool, u"newt-hat"_s), u"a/newt-hat_A.gif"_s);
    EXPECT_TRUE(pool.contains(PJ::pickEasterEggGif(pool, u"retired-character"_s)));
  }
  EXPECT_TRUE(PJ::pickEasterEggGif({}, u"llama-coat"_s).isEmpty());
}

TEST(VisualizationPlaceholderEasterEggTest, SitsBottomLeftOnlyWhileClearOfTheIcons) {
  const QByteArray gif("GIF89a");  // placement only: the label shows whatever QMovie makes of it
  PJ::VisualizationPlaceholderWidget placeholder;
  EXPECT_FALSE(placeholder.showEasterEgg({}));
  EXPECT_FALSE(placeholder.showEasterEgg(gif));  // hidden

  placeholder.resize(300, 230);
  placeholder.show();
  EXPECT_FALSE(placeholder.showEasterEgg(gif));  // the 128 px GIF would cover the icon grid

  placeholder.resize(700, 600);
  ASSERT_TRUE(placeholder.showEasterEgg(gif));
  auto* egg = placeholder.findChild<QLabel*>(u"placeholderEasterEgg"_s);
  ASSERT_NE(egg, nullptr);
  EXPECT_TRUE(egg->isVisible());
  EXPECT_TRUE(egg->testAttribute(Qt::WA_TransparentForMouseEvents));
  EXPECT_EQ(egg->size(), QSize(128, 128));
  EXPECT_LT(egg->x(), 30);
  EXPECT_EQ(egg->geometry().bottom() + egg->x(), placeholder.height() - 1);

  placeholder.resize(700, 300);
  EXPECT_EQ(egg->geometry().bottom() + egg->x(), placeholder.height() - 1);
  placeholder.resize(300, 230);
  EXPECT_FALSE(egg->isVisible());
}

TEST(VisualizationPlaceholderFamiliesTest, OffersSixFamiliesInOrder) {
  PJ::VisualizationPlaceholderWidget placeholder;
  const QStringList expected{u"buttonVizPlot"_s, u"buttonVizScene2D"_s,          u"buttonVizScene3D"_s,
                             u"buttonVizLog"_s,  u"buttonVizStateTransitions"_s, u"buttonVizCustomWidget"_s};
  QStringList names;
  for (auto* button : placeholder.findChildren<QToolButton*>(Qt::FindDirectChildrenOnly)) {
    names.push_back(button->objectName());
    EXPECT_TRUE(button->isEnabled()) << button->objectName().toStdString();
    EXPECT_FALSE(button->icon().isNull()) << button->objectName().toStdString();
  }
  EXPECT_EQ(names, expected);
}

TEST(VisualizationPlaceholderFamiliesTest, CustomWidgetIconRequestsItsFamily) {
  PJ::VisualizationPlaceholderWidget placeholder;
  std::vector<PJ::VisualizationKind> requested;
  QObject::connect(
      &placeholder, &PJ::VisualizationPlaceholderWidget::visualizationRequested, &placeholder,
      [&](PJ::VisualizationKind kind) { requested.push_back(kind); });
  auto* button = placeholder.findChild<QToolButton*>(u"buttonVizCustomWidget"_s);
  ASSERT_NE(button, nullptr);
  EXPECT_EQ(button->toolTip(), u"Custom Widget"_s);
  button->click();
  ASSERT_EQ(requested.size(), 1U);
  EXPECT_EQ(requested[0], PJ::VisualizationKind::kCustomWidget);
}

TEST(VisualizationPlaceholderFamiliesTest, CustomWidgetIconFollowsAvailability) {
  PJ::VisualizationPlaceholderWidget placeholder;
  std::vector<PJ::VisualizationKind> requested;
  QObject::connect(
      &placeholder, &PJ::VisualizationPlaceholderWidget::visualizationRequested, &placeholder,
      [&](PJ::VisualizationKind kind) { requested.push_back(kind); });
  QToolButton* button = placeholder.customWidgetButton();
  ASSERT_NE(button, nullptr);
  EXPECT_TRUE(button->isEnabled());
  EXPECT_EQ(button->toolTip(), u"Custom Widget"_s);
  EXPECT_EQ(button->cursor().shape(), Qt::PointingHandCursor);

  placeholder.setCustomWidgetsAvailable(false);
  EXPECT_FALSE(button->isEnabled());
#ifdef PJ_TARGET_WASM
  EXPECT_EQ(button->toolTip(), u"No custom widgets installed."_s);
#else
  EXPECT_EQ(button->toolTip(), u"No custom widgets installed. Get them from the Marketplace."_s);
#endif
  EXPECT_NE(button->cursor().shape(), Qt::PointingHandCursor);
  button->click();
  EXPECT_TRUE(requested.empty());
  for (auto* other : placeholder.findChildren<QToolButton*>(Qt::FindDirectChildrenOnly)) {
    if (other != button) {
      EXPECT_TRUE(other->isEnabled()) << other->objectName().toStdString();
      EXPECT_EQ(other->cursor().shape(), Qt::PointingHandCursor) << other->objectName().toStdString();
    }
  }
  placeholder.findChild<QToolButton*>(u"buttonVizPlot"_s)->click();
  EXPECT_EQ(requested.size(), 1U);

  placeholder.setCustomWidgetsAvailable(true);
  EXPECT_TRUE(button->isEnabled());
  EXPECT_EQ(button->toolTip(), u"Custom Widget"_s);
  EXPECT_EQ(button->cursor().shape(), Qt::PointingHandCursor);
  button->click();
  ASSERT_EQ(requested.size(), 2U);
  EXPECT_EQ(requested[1], PJ::VisualizationKind::kCustomWidget);
}

TEST(VisualizationPlaceholderFamiliesTest, IconsFillAGridThreeColumnsWide) {
  PJ::VisualizationPlaceholderWidget placeholder;
  placeholder.show();
  placeholder.resize(900, 400);
  QCoreApplication::processEvents();
  const auto buttons = placeholder.findChildren<QToolButton*>(Qt::FindDirectChildrenOnly);
  ASSERT_EQ(buttons.size(), 6);
  std::set<int> columns;
  std::set<int> rows;
  for (const QToolButton* button : buttons) {
    columns.insert(button->x());
    rows.insert(button->y());
  }
  EXPECT_EQ(columns.size(), 3U);
  EXPECT_EQ(rows.size(), 2U);
  const QRect first = buttons.front()->geometry();
  EXPECT_NEAR(first.left(), placeholder.width() - buttons[2]->geometry().right(), 2) << "the grid is centred";
}

TEST(VisualizationPlaceholderFamiliesTest, IconGridShrinksToFitASmallDock) {
  PJ::VisualizationPlaceholderWidget placeholder;
  // Resize events reach a widget only once it is shown.
  placeholder.show();
  const auto buttons = placeholder.findChildren<QToolButton*>(Qt::FindDirectChildrenOnly);
  ASSERT_EQ(buttons.size(), 6);
  EXPECT_LE(placeholder.minimumSizeHint().width(), 180);
  EXPECT_LE(placeholder.minimumSizeHint().height(), 110);
  EXPECT_GE(placeholder.sizeHint().width(), 3 * 66) << "the preferred grid keeps full-size icons";
  EXPECT_GE(placeholder.sizeHint().height(), 2 * 66);

  for (const QSize size : {QSize(180, 400), QSize(900, 120)}) {
    placeholder.resize(size);
    QCoreApplication::processEvents();
    for (const QToolButton* button : buttons) {
      EXPECT_LT(button->width(), 66) << button->objectName().toStdString();
      EXPECT_GE(button->width(), 44) << button->objectName().toStdString();
      EXPECT_LT(button->iconSize().width(), 48) << button->objectName().toStdString();
      EXPECT_LE(button->geometry().right(), placeholder.width()) << "no icon is cut off";
      EXPECT_LE(button->geometry().bottom(), placeholder.height()) << "no icon is cut off";
    }
  }

  placeholder.resize(900, 400);
  QCoreApplication::processEvents();
  for (const QToolButton* button : buttons) {
    EXPECT_EQ(button->width(), 66) << button->objectName().toStdString();
    EXPECT_EQ(button->iconSize(), QSize(48, 48)) << button->objectName().toStdString();
  }
}

TEST(SectionHeaderBandUicTest, ParentOnlyConstructorTakesItsTitleFromTheTextProperty) {
  QWidget parent;
  auto* band = new PJ::SectionHeaderBand(&parent);
  EXPECT_EQ(band->parentWidget(), &parent);
  EXPECT_TRUE(band->text().isEmpty());
  band->setProperty("text", u"Inputs"_s);
  EXPECT_EQ(band->text(), u"Inputs"_s);
}

}  // namespace
