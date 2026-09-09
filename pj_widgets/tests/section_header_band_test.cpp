// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QLayout>
#include <QMargins>
#include <QPushButton>
#include <QRadioButton>
#include <algorithm>

#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/SectionHeaderBand.h"

using PJ::SectionHeaderBand;

namespace {

int layoutIndexOf(const SectionHeaderBand& band, QWidget* w) {
  auto* layout = band.layout();
  return layout != nullptr ? layout->indexOf(w) : -1;
}

}  // namespace

// A widget nested inside the band in a .ui file is reparented onto it by the
// loader; the band must auto-dock it into its own layout (the container path).
TEST(SectionHeaderBandTest, ReparentedChildIsDockedIntoTheBand) {
  SectionHeaderBand band(QStringLiteral("Settings"));
  auto* checkbox = new QCheckBox(QStringLiteral("One file per group"));

  checkbox->setParent(&band);  // what QUiLoader does for a nested child

  EXPECT_EQ(checkbox->parentWidget(), &band);
  EXPECT_GE(layoutIndexOf(band, checkbox), 0) << "reparented child must land in the band layout";
}

// Several nested children dock in insertion order, all to the right of the title
// (the title label is at layout index 0, then the stretch, then docked widgets).
TEST(SectionHeaderBandTest, ChildrenDockInOrderAfterTheTitle) {
  SectionHeaderBand band(QStringLiteral("Settings"));
  auto* csv = new QRadioButton(QStringLiteral("CSV"), &band);
  auto* parquet = new QRadioButton(QStringLiteral("Parquet"), &band);

  const int csv_index = layoutIndexOf(band, csv);
  const int parquet_index = layoutIndexOf(band, parquet);
  EXPECT_GE(csv_index, 2) << "docked children sit after the title (0) and stretch (1)";
  EXPECT_LT(csv_index, parquet_index) << "declaration order preserved";
}

// The band's OWN sub-widgets (built under the internal guard) must not be treated
// as external content — enabling a trailing toggle docks exactly one widget.
TEST(SectionHeaderBandTest, InternalSubWidgetsAreNotAutoDockedTwice) {
  SectionHeaderBand band(QStringLiteral("Settings"));
  band.setTrailingToggleName(QStringLiteral("regexToggle"));
  band.setTrailingToggleText(QStringLiteral(".*"));

  ASSERT_NE(band.trailingToggle(), nullptr);
  EXPECT_EQ(band.trailingToggle()->objectName(), QStringLiteral("regexToggle"));
  // Exactly one toggle exists in the tree (no duplicate from a childEvent dock).
  EXPECT_EQ(band.findChildren<QPushButton*>(QStringLiteral("regexToggle")).size(), 1);
}

// Docked controls are pinned to the app-wide input-row height (clamped by the
// band content height) so they match their siblings outside the band, and carry
// the stylesheet hook that lets the app QSS lift its single-input-row height cap.
TEST(SectionHeaderBandTest, DockedChildrenAreSizedToTheInputRowHeight) {
  SectionHeaderBand band(QStringLiteral("Settings"));
  auto* checkbox = new QCheckBox(QStringLiteral("One file per group"), &band);
  // The dock defers sizing by one event-loop turn (see addTrailingWidget); flush
  // that queued call. processEvents() alone does not reach an unshown widget tree.
  QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);

  // The canonical title-band recipe (CurveListPanel's "Custom Series" band):
  // layout_padding on top/bottom/right, none on the left (the title label leads
  // with its own indent), leaving icon_size + icon_padding for the chrome.
  const PJ::ChromeMetrics metrics{};
  const int content_height = metrics.bandHeight() - (2 * metrics.layout_padding);
  EXPECT_EQ(content_height, metrics.icon_size + metrics.icon_padding);
  const int docked_height = std::min(content_height, PJ::theme::metric(PJ::theme::Metric::InputOuterHeight));
  EXPECT_EQ(checkbox->minimumHeight(), docked_height);
  EXPECT_EQ(checkbox->maximumHeight(), docked_height);
  EXPECT_TRUE(checkbox->property("pjBandDocked").toBool());

  EXPECT_EQ(
      band.layout()->contentsMargins(),
      QMargins(0, metrics.layout_padding, metrics.layout_padding, metrics.layout_padding))
      << "docked chrome must not butt against the band edges";
  EXPECT_EQ(band.layout()->spacing(), metrics.layout_spacing);

  // Input-row-height controls are shorter than the band, so the dock centers
  // them vertically instead of letting them ride the band's top edge.
  auto* layout = band.layout();
  const int index = layout->indexOf(checkbox);
  ASSERT_GE(index, 0);
  EXPECT_EQ(layout->itemAt(index)->alignment(), Qt::Alignment(Qt::AlignVCenter));
}

TEST(SectionHeaderBandTest, DockedChildrenCanFillTheBandContentHeight) {
  SectionHeaderBand band(QStringLiteral("Settings"));
  auto* already_docked = new QCheckBox(QStringLiteral("One file per group"), &band);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);

  band.setFillDockedWidgets(true);
  auto* docked_with_fill = new QCheckBox(QStringLiteral("Include metadata"), &band);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);

  const PJ::ChromeMetrics metrics{};
  const int content_height = metrics.bandHeight() - (2 * metrics.layout_padding);
  EXPECT_TRUE(band.fillDockedWidgets());
  for (auto* checkbox : {already_docked, docked_with_fill}) {
    EXPECT_EQ(checkbox->height(), content_height);
    EXPECT_EQ(checkbox->minimumHeight(), content_height);
    EXPECT_EQ(checkbox->maximumHeight(), content_height);
  }
}

// A metrics broadcast rescales the band, so docked children have to follow it the
// same way the band's own buttons do.
TEST(SectionHeaderBandTest, DockedChildrenFollowAChromeMetricsChange) {
  SectionHeaderBand band(QStringLiteral("Settings"));
  auto* checkbox = new QCheckBox(QStringLiteral("One file per group"), &band);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);

  PJ::ChromeMetrics metrics{};
  metrics.icon_size = 32;
  band.onChromeMetricsChanged(metrics);

  EXPECT_EQ(band.height(), metrics.bandHeight());
  EXPECT_EQ(
      checkbox->maximumHeight(),
      std::min(
          metrics.bandHeight() - (2 * metrics.layout_padding), PJ::theme::metric(PJ::theme::Metric::InputOuterHeight)));
  EXPECT_EQ(
      band.layout()->contentsMargins(),
      QMargins(0, metrics.layout_padding, metrics.layout_padding, metrics.layout_padding));
}
