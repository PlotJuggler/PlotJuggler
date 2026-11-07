// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Clicking a legend entry toggles only that curve's visibility: the user's
// current X/Y view must survive the click instead of snapping back to the full
// data range.

#include <gtest/gtest.h>
#include <qwt_plot.h>
#include <qwt_plot_curve.h>
#include <qwt_scale_div.h>

#include <QTest>
#include <string_view>

#include "pj_datastore/writer.hpp"
#include "pj_plotting/PlotLegend.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"

namespace {

PJ::TopicId addScalarTopic(PJ::SessionManager& session, PJ::DatasetId dataset_id, std::string_view topic_name) {
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(dataset_id, topic_name, PJ::NumericType::kFloat64);
  EXPECT_TRUE(handle.has_value()) << (handle.has_value() ? "" : handle.error());
  if (!handle.has_value()) {
    return 0;
  }
  for (int i = 0; i <= 10; ++i) {
    writer.appendScalar(*handle, PJ::Timestamp{i} * 1'000'000'000, static_cast<double>(i));
  }
  EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());
  return handle->topic_id;
}

QString keyForTopic(PJ::CatalogModel& catalog, PJ::TopicId topic_id) {
  for (const auto& curve : catalog.curves()) {
    if (const auto descriptor = catalog.curveDescriptor(curve.name); descriptor && descriptor->topic_id == topic_id) {
      return curve.name;
    }
  }
  return {};
}

// Re-exposes the protected Qwt plot and legend so the test can click a real
// legend entry through the canvas event filter.
class LegendTogglePlot final : public PJ::PlotWidget {
 public:
  using PJ::PlotWidget::PlotWidget;
  using PJ::PlotWidgetBase::legend;
  using PJ::PlotWidgetBase::qwtPlot;
};

}  // namespace

TEST(PlotWidgetLegendToggle, TogglingCurveKeepsCurrentZoom) {
  // The raster canvas paints through QPainter, which lays out the legend entries.
  PJ::PlotWidgetBase::setGpuCanvasDisabledOverride(true);

  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "run.csv"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key_a = keyForTopic(catalog, addScalarTopic(session, *dataset, "/a"));
  const QString key_b = keyForTopic(catalog, addScalarTopic(session, *dataset, "/b"));

  LegendTogglePlot widget(&session, &catalog);
  ASSERT_NE(widget.addCurve(key_a), nullptr);
  ASSERT_NE(widget.addCurve(key_b), nullptr);
  widget.resize(800, 400);
  widget.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&widget));

  QwtPlot* plot = widget.qwtPlot();
  plot->setAxisScale(QwtPlot::xBottom, 2.0, 4.0);
  plot->setAxisScale(QwtPlot::yLeft, 1.0, 5.0);
  plot->replot();
  plot->canvas()->repaint();  // computes the legend entry geometry

  QwtPlotItem* curve_b = nullptr;
  for (auto& info : widget.curveList()) {
    if (info.source_name == key_b) {
      curve_b = info.curve;
    }
  }
  ASSERT_NE(curve_b, nullptr);
  const auto geometries = widget.legend()->legendGeometries(curve_b);
  ASSERT_FALSE(geometries.empty()) << "legend entry was not laid out";

  const auto expect_view_unchanged = [plot] {
    EXPECT_DOUBLE_EQ(plot->axisScaleDiv(QwtPlot::xBottom).lowerBound(), 2.0);
    EXPECT_DOUBLE_EQ(plot->axisScaleDiv(QwtPlot::xBottom).upperBound(), 4.0);
    EXPECT_DOUBLE_EQ(plot->axisScaleDiv(QwtPlot::yLeft).lowerBound(), 1.0);
    EXPECT_DOUBLE_EQ(plot->axisScaleDiv(QwtPlot::yLeft).upperBound(), 5.0);
  };
  const QPoint entry = geometries.first().center();

  QTest::mouseClick(plot->canvas(), Qt::LeftButton, Qt::NoModifier, entry);
  EXPECT_FALSE(curve_b->isVisible());
  expect_view_unchanged();

  QTest::mouseClick(plot->canvas(), Qt::LeftButton, Qt::NoModifier, entry);
  EXPECT_TRUE(curve_b->isVisible());
  expect_view_unchanged();
}
