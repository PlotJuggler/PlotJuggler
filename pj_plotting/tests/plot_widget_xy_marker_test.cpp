// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The time-driven marker on XY plots: as the playback cursor moves, each XY
// curve's marker must ride along the parametric curve, sitting on the (x,y)
// sample at the current time. A time-series plot must NOT show these markers.

#include <gtest/gtest.h>
#include <qwt_plot_curve.h>
#include <qwt_plot_marker.h>

#include <QApplication>
#include <QtGlobal>
#include <array>
#include <string_view>

#include "pj_datastore/writer.hpp"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/PlotWidgetBase.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
using namespace Qt::StringLiterals;

namespace {

constexpr PJ::Timestamp kNs = 1'000'000'000;  // 1 second in ns

// A scalar topic with samples at t = 1s, 2s, 3s carrying the given values.
PJ::TopicId addScalarTopic(
    PJ::SessionManager& session, PJ::DatasetId dataset_id, std::string_view topic_name, std::array<double, 3> values) {
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle_or = writer.registerScalarSeries(dataset_id, topic_name, PJ::NumericType::kFloat64);
  EXPECT_TRUE(handle_or.has_value()) << handle_or.error();
  if (!handle_or.has_value()) {
    return 0;
  }
  for (int i = 0; i < 3; ++i) {
    writer.appendScalar(*handle_or, static_cast<PJ::Timestamp>(i + 1) * kNs, values[static_cast<std::size_t>(i)]);
  }
  EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());
  return handle_or->topic_id;
}

QString keyForTopic(PJ::CatalogModel& catalog, PJ::TopicId topic_id) {
  for (const auto& curve : catalog.curves()) {
    if (const auto descriptor = catalog.curveDescriptor(curve.name); descriptor && descriptor->topic_id == topic_id) {
      return curve.name;
    }
  }
  return {};
}

}  // namespace

// The XY marker follows the cursor: it appears on the (x,y) sample at-or-before
// the tracker time, and hides when the cursor sits before the first sample.
TEST(PlotWidgetXyMarker, MarkerRidesCurveWithTracker) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key_x = keyForTopic(catalog, addScalarTopic(session, *dataset, "/pos/x", {10.0, 20.0, 30.0}));
  const QString key_y = keyForTopic(catalog, addScalarTopic(session, *dataset, "/pos/y", {100.0, 200.0, 300.0}));
  ASSERT_FALSE(key_x.isEmpty());
  ASSERT_FALSE(key_y.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  plot.setModeXY(true);
  auto* info = plot.addCurveXY(key_x, key_y, u"y vs x"_s);
  ASSERT_NE(info, nullptr);
  ASSERT_NE(info->marker, nullptr);

  // Cursor exactly on t = 2s -> the pair (x=20, y=200).
  plot.setTrackerPosition(2.0);
  EXPECT_TRUE(info->marker->isVisible());
  EXPECT_DOUBLE_EQ(info->marker->value().x(), 20.0);
  EXPECT_DOUBLE_EQ(info->marker->value().y(), 200.0);

  // Between samples -> the most recent at-or-before (t = 2s).
  plot.setTrackerPosition(2.5);
  EXPECT_TRUE(info->marker->isVisible());
  EXPECT_DOUBLE_EQ(info->marker->value().x(), 20.0);
  EXPECT_DOUBLE_EQ(info->marker->value().y(), 200.0);

  // Last sample.
  plot.setTrackerPosition(3.0);
  EXPECT_TRUE(info->marker->isVisible());
  EXPECT_DOUBLE_EQ(info->marker->value().x(), 30.0);
  EXPECT_DOUBLE_EQ(info->marker->value().y(), 300.0);

  // Before the first sample -> nothing to show, marker hidden.
  plot.setTrackerPosition(0.5);
  EXPECT_FALSE(info->marker->isVisible());
}

// A hidden curve carries no marker even when the cursor is over a valid sample.
TEST(PlotWidgetXyMarker, HiddenCurveHidesMarker) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key_x = keyForTopic(catalog, addScalarTopic(session, *dataset, "/pos/x", {10.0, 20.0, 30.0}));
  const QString key_y = keyForTopic(catalog, addScalarTopic(session, *dataset, "/pos/y", {100.0, 200.0, 300.0}));

  PJ::PlotWidget plot(&session, &catalog);
  plot.setModeXY(true);
  auto* info = plot.addCurveXY(key_x, key_y, u"y vs x"_s);
  ASSERT_NE(info, nullptr);
  ASSERT_NE(info->curve, nullptr);
  info->curve->setVisible(false);

  plot.setTrackerPosition(2.0);
  EXPECT_FALSE(info->marker->isVisible());
}

// The time-series tracker path must never reveal the per-curve markers (they are
// an XY-only affordance; time series get the vertical-line CurveTracker instead).
TEST(PlotWidgetXyMarker, TimeSeriesLeavesMarkerHidden) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key = keyForTopic(catalog, addScalarTopic(session, *dataset, "/pos/x", {10.0, 20.0, 30.0}));
  ASSERT_FALSE(key.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  auto* info = plot.addCurve(key);
  ASSERT_NE(info, nullptr);
  ASSERT_NE(info->marker, nullptr);

  plot.setTrackerPosition(2.0);
  EXPECT_FALSE(info->marker->isVisible());
}
