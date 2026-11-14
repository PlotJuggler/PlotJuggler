// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QToolButton>
#include <vector>

#include "pj_widgets/SectionHeaderBand.h"
#include "pj_widgets/VisualizationPlaceholderWidget.h"
using namespace Qt::StringLiterals;

namespace {

TEST(VisualizationPlaceholderFamiliesTest, OffersFiveFamiliesInOrder) {
  PJ::VisualizationPlaceholderWidget placeholder;
  const QStringList expected{
      u"buttonVizPlot"_s, u"buttonVizStateTransitions"_s, u"buttonVizScene2D"_s, u"buttonVizScene3D"_s,
      u"buttonVizCustomWidget"_s};
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

TEST(VisualizationPlaceholderFamiliesTest, IconRowShrinksToFitANarrowDock) {
  PJ::VisualizationPlaceholderWidget placeholder;
  // Resize events reach a widget only once it is shown.
  placeholder.show();
  const auto buttons = placeholder.findChildren<QToolButton*>(Qt::FindDirectChildrenOnly);
  ASSERT_EQ(buttons.size(), 5);
  EXPECT_LE(placeholder.minimumSizeHint().width(), 300);
  EXPECT_GE(placeholder.sizeHint().width(), 5 * 66) << "the preferred row keeps full-size icons";

  placeholder.resize(300, 400);
  QCoreApplication::processEvents();
  for (const QToolButton* button : buttons) {
    EXPECT_LT(button->width(), 66) << button->objectName().toStdString();
    EXPECT_GE(button->width(), 44) << button->objectName().toStdString();
    EXPECT_LT(button->iconSize().width(), 48) << button->objectName().toStdString();
  }
  EXPECT_LT(buttons.back()->geometry().right(), placeholder.width()) << "the last icon is not cut off";
  EXPECT_GE(buttons.front()->geometry().left(), 0);

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
