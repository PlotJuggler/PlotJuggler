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
