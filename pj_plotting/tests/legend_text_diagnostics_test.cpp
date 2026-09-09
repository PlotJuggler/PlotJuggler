// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Verifies the opt-in PJ_PLOT_TEXT_DEBUG diagnostics (PlotLegend / CurveTracker)
// actually emit on a real legend paint. This is the tool used to chase the
// intermittent "canvas text disappears" bug, so we must know it fires — a silent
// diagnostic is worse than none. The test does NOT reproduce the bug; it proves
// the instrument is live so the field log can be trusted.

#include <gtest/gtest.h>
#include <qwt_plot.h>
#include <qwt_plot_renderer.h>

#include <QApplication>
#include <QImage>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <string_view>

#include "pj_datastore/writer.hpp"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
using namespace Qt::StringLiterals;

namespace {

// Keep opt-in diagnostics and raster settings local to the selected legend suite.
class LegendTextDiagnosticsEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    const auto* unit_test = ::testing::UnitTest::GetInstance();
    for (int i = 0; i < unit_test->total_test_suite_count(); ++i) {
      const auto* suite = unit_test->GetTestSuite(i);
      if (suite->should_run() && std::string_view(suite->name()) == "LegendTextDiagnostics") {
        // Must be set before the first legend paint: PlotLegend reads it once (static).
        qputenv("PJ_PLOT_TEXT_DEBUG", "1");
        // Force the raster canvas so the test needs no GL context; QwtPlotRenderer
        // draws via QPainter regardless, exercising the same drawLegendData path.
        QSettings().setValue(u"Preferences::use_opengl"_s, false);
        return;
      }
    }
  }
};

static auto* const kEnv = ::testing::AddGlobalTestEnvironment(new LegendTextDiagnosticsEnvironment);

QStringList g_messages;

void captureHandler(QtMsgType /*type*/, const QMessageLogContext& /*ctx*/, const QString& msg) {
  g_messages << msg;
}

PJ::TopicId addScalarTopic(PJ::SessionManager& session, PJ::DatasetId dataset_id, std::string_view topic_name) {
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle_or = writer.registerScalarSeries(dataset_id, topic_name, PJ::NumericType::kFloat64);
  EXPECT_TRUE(handle_or.has_value()) << (handle_or.has_value() ? "" : handle_or.error());
  if (!handle_or.has_value()) {
    return 0;
  }
  writer.appendScalar(*handle_or, 100, 1.0);
  writer.appendScalar(*handle_or, 200, 2.0);
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

// PlotWidgetBase keeps its backing QwtPlot behind a protected accessor. A thin
// test-only subclass promotes it so QwtPlotRenderer can draw the in-canvas legend
// synchronously, without widening the production API surface.
class TestablePlotWidget : public PJ::PlotWidget {
 public:
  using PJ::PlotWidget::PlotWidget;   // inherit the (SessionManager*, CatalogModel*) ctor
  using PJ::PlotWidgetBase::qwtPlot;  // re-expose the protected accessor to the test
};

}  // namespace

// A legend paint with the env var set must emit a "[PJ_PLOT_TEXT_DEBUG] legend"
// line naming the entry's title and failure flags. QwtPlotRenderer draws the
// in-canvas legend item synchronously, so this does not depend on paint-event
// timing or the (GL) canvas backing store.
TEST(LegendTextDiagnostics, FiresOnLegendPaint) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/x"));
  ASSERT_FALSE(key.isEmpty());

  TestablePlotWidget plot(&session, &catalog);
  ASSERT_NE(plot.addCurve(key), nullptr);
  plot.qwtPlot()->setAxisScale(QwtPlot::xBottom, 0.0, 1.0);
  plot.qwtPlot()->setAxisScale(QwtPlot::yLeft, 0.0, 1.0);

  g_messages.clear();
  QtMessageHandler previous = qInstallMessageHandler(captureHandler);
  QImage image(400, 300, QImage::Format_ARGB32);
  QwtPlotRenderer renderer;
  renderer.renderTo(plot.qwtPlot(), image);
  qInstallMessageHandler(previous);

  bool legend_logged = false;
  for (const QString& msg : g_messages) {
    if (msg.startsWith("[PJ_PLOT_TEXT_DEBUG] legend"_L1)) {
      legend_logged = true;
      // Healthy render: the title is present, so EMPTY must be 0.
      EXPECT_TRUE(msg.contains("EMPTY=0"_L1)) << msg.toStdString();
    }
  }
  EXPECT_TRUE(legend_logged) << "No legend diagnostic emitted; captured " << g_messages.size() << " messages";
}
