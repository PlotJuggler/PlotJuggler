// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/Search.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QLineEdit>
#include <QSignalSpy>

#include "pj_widgets/ChromeMetrics.h"

namespace {

using PJ::ChromeMetrics;
using PJ::Search;

// QSS type selectors match on metaObject()->className(), so the whole styling
// contract (PJ--Search and its [variant=...] override) hinges on the class
// name. Pin it so a dropped Q_OBJECT is a red test, not a silent restyle.
TEST(SearchTest, ClassNameDrivesQssSelector) {
  Search search;
  EXPECT_STREQ(search.metaObject()->className(), "PJ::Search");
}

// The default placeholder is "Filter..." — the common case needs no setup.
TEST(SearchTest, DefaultsToFilterPlaceholder) {
  Search search;
  EXPECT_EQ(search.placeholder(), QString("Filter..."));
}

// text()/setText()/clear() proxy the inner line edit, and textChanged forwards.
TEST(SearchTest, ForwardsTextAndSignal) {
  Search search;
  QSignalSpy spy(&search, &Search::textChanged);

  search.setText("abc");
  EXPECT_EQ(search.text(), QString("abc"));
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(spy.takeFirst().at(0).toString(), QString("abc"));

  search.clear();
  EXPECT_TRUE(search.text().isEmpty());
}

// setFieldObjectName stamps the INNER line edit (not the frame), so a dialog
// host binding by objectName keeps working after the widget is canonicalized.
TEST(SearchTest, FieldObjectNameStampsInnerLineEdit) {
  Search search;
  search.setFieldObjectName("lineEditFilter");
  EXPECT_EQ(search.fieldObjectName(), QString("lineEditFilter"));
  EXPECT_EQ(search.lineEdit()->objectName(), QString("lineEditFilter"));
}

// The variant drives the `variant` dynamic property the stylesheet keys on.
TEST(SearchTest, VariantSetsStyleProperty) {
  Search search;
  EXPECT_EQ(search.variant(), Search::Variant::kBanner);
  EXPECT_TRUE(search.property("variant").toString().isEmpty());

  search.setVariant(Search::Variant::kStandalone);
  EXPECT_EQ(search.variant(), Search::Variant::kStandalone);
  EXPECT_EQ(search.property("variant").toString(), QString("standalone"));
}

// Height tracks the icon-size chrome metric (icon_size + icon_padding), so the
// control lines up with the section bands as the app icon size changes.
TEST(SearchTest, HeightTracksChromeMetrics) {
  Search search;
  ChromeMetrics metrics;
  metrics.icon_size = 28;
  metrics.icon_padding = 6;
  search.setChromeMetrics(metrics);
  EXPECT_EQ(search.minimumHeight(), 34);
  EXPECT_EQ(search.maximumHeight(), 34);
}

}  // namespace
