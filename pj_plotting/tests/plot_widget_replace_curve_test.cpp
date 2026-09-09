// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>
#include <qwt_plot_curve.h>

#include <QApplication>
#include <QColor>
#include <QPen>
#include <QtGlobal>
#include <string_view>

#include "pj_datastore/writer.hpp"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/CurveDescriptor.h"
#include "pj_runtime/SessionManager.h"
using namespace Qt::StringLiterals;

namespace {

PJ::TopicId addScalarTopic(PJ::SessionManager& session, PJ::DatasetId dataset_id, std::string_view topic_name) {
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle_or = writer.registerScalarSeries(dataset_id, topic_name, PJ::NumericType::kFloat64);
  EXPECT_TRUE(handle_or.has_value()) << handle_or.error();
  if (!handle_or.has_value()) {
    return 0;
  }
  writer.appendScalar(*handle_or, 100, 1.0);
  writer.appendScalar(*handle_or, 200, 2.0);
  EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());
  return handle_or->topic_id;
}

// Catalog key (opaque "dataset:N/topic:M/column:K") for a given topic id.
QString keyForTopic(PJ::CatalogModel& catalog, PJ::TopicId topic_id) {
  for (const auto& curve : catalog.curves()) {
    if (const auto descriptor = catalog.curveDescriptor(curve.name); descriptor && descriptor->topic_id == topic_id) {
      return curve.name;
    }
  }
  return {};
}

}  // namespace

// The Filter Editor's core behavior: applying a filter to a plotted curve makes
// the filtered output REPLACE the source (not add alongside) and INHERIT the
// source's colour.
TEST(PlotWidgetReplaceCurve, OutputInheritsSourceColourAndReplacesSource) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();

  const PJ::TopicId src = addScalarTopic(session, *dataset, "/imu/accel");
  const PJ::TopicId out = addScalarTopic(session, *dataset, "/imu/accel[Filtered]");
  ASSERT_NE(src, 0U);
  ASSERT_NE(out, 0U);

  const QString src_key = keyForTopic(catalog, src);
  const QString out_key = keyForTopic(catalog, out);
  ASSERT_FALSE(src_key.isEmpty());
  ASSERT_FALSE(out_key.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  ASSERT_NE(plot.addCurve(src_key), nullptr);

  const QColor chosen(Qt::red);
  plot.onChangeCurveColor(src_key, chosen);
  ASSERT_EQ(plot.curveList().size(), 1U);

  plot.replaceCurve(src_key, out_key);

  // Source gone, output present, count unchanged (replaced, not added).
  ASSERT_EQ(plot.curveList().size(), 1U);
  const auto& only = plot.curveList().front();
  EXPECT_EQ(only.source_name, out_key);
  ASSERT_NE(only.curve, nullptr);
  EXPECT_EQ(only.curve->pen().color(), chosen);
}

// A source not currently plotted is just added (no spurious removal/crash).
TEST(PlotWidgetReplaceCurve, AbsentSourceJustAddsOutput) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const PJ::TopicId out = addScalarTopic(session, *dataset, "/imu/accel[Filtered]");
  ASSERT_NE(out, 0U);
  const QString out_key = keyForTopic(catalog, out);
  ASSERT_FALSE(out_key.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  plot.replaceCurve(u"dataset:1/topic:999/column:0"_s, out_key);

  ASSERT_EQ(plot.curveList().size(), 1U);
  EXPECT_EQ(plot.curveList().front().source_name, out_key);
}

// [i] replaceCurve must be a NO-OP when the output is not in the catalog (e.g. a stale key): it
// must NOT drop the existing source curve just because the replacement cannot be added.
TEST(PlotWidgetReplaceCurve, NoOpWhenOutputMissing) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const PJ::TopicId src = addScalarTopic(session, *dataset, "/imu/accel");
  ASSERT_NE(src, 0U);
  const QString src_key = keyForTopic(catalog, src);
  ASSERT_FALSE(src_key.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  ASSERT_NE(plot.addCurve(src_key), nullptr);
  ASSERT_EQ(plot.curveList().size(), 1U);

  // The output key references a topic that does not exist in the catalog.
  plot.replaceCurve(src_key, u"dataset:1/topic:999/column:0"_s);

  // No-op: the source curve stays; nothing is dropped.
  ASSERT_EQ(plot.curveList().size(), 1U);
  EXPECT_EQ(plot.curveList().front().source_name, src_key);
}
