// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A curve's visibility and colour change through one path (PlotWidget::setCurveVisible / onChangeCurveColor): the
// tracker's value box follows at once, a legend click is an undoable step the curve editor hears, and the editor's eye
// and the legend agree.

#include <gtest/gtest.h>
#include <qwt_plot.h>
#include <qwt_plot_curve.h>
#include <qwt_series_data.h>
#include <qwt_text.h>

#include <QColor>
#include <QDomDocument>
#include <QListWidget>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>
#include <memory>
#include <string_view>

#include "pj_datastore/writer.hpp"
#include "pj_plotting/CurveEditor.h"
#include "pj_plotting/CurveTracker.h"
#include "pj_plotting/PlotLegend.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
using namespace Qt::StringLiterals;

namespace {

// Samples 0..10 s, one per second, so a tracker sits inside the plotted range.
PJ::TopicId addRampTopic(PJ::SessionManager& session, PJ::DatasetId dataset_id, std::string_view topic_name) {
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(dataset_id, topic_name, PJ::NumericType::kFloat64);
  EXPECT_TRUE(handle.has_value()) << (handle.has_value() ? "" : handle.error());
  if (!handle.has_value()) {
    return 0;
  }
  for (int k = 0; k <= 10; ++k) {
    writer.appendScalar(*handle, static_cast<PJ::Timestamp>(k) * 1'000'000'000, static_cast<double>(k));
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

// Re-exposes the protected Qwt plot and legend so a test can click a real legend entry through the canvas.
class VisibilityPlot final : public PJ::PlotWidget {
 public:
  using PJ::PlotWidget::PlotWidget;
  using PJ::PlotWidgetBase::legend;
  using PJ::PlotWidgetBase::qwtPlot;
};

// Two ramp curves "kept" and "toggled" on one plot with its tracker mid-range and the box naming every curve.
struct TrackerFixture {
  TrackerFixture() : catalog(&session) {
    auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
    EXPECT_TRUE(dataset.has_value()) << (dataset.has_value() ? "" : dataset.error());
    kept = keyForTopic(catalog, addRampTopic(session, *dataset, "/kept"));
    toggled = keyForTopic(catalog, addRampTopic(session, *dataset, "/toggled"));
    plot = std::make_unique<VisibilityPlot>(&session, &catalog);
    plot->setTrackerParameter(PJ::CurveTracker::kValueName);
    EXPECT_NE(plot->addCurve(kept), nullptr);
    EXPECT_NE(plot->addCurve(toggled), nullptr);
    plot->zoomOut(false);
    plot->setTrackerPosition(plot->maxZoomRect().center().x());
  }

  QString title(const QString& key) {
    return plot->curveFromTitle(key)->curve->title().text();
  }
  [[nodiscard]] bool boxNames(const QString& key) {
    return plot->trackerValueBoxText().contains(title(key));
  }

  PJ::SessionManager session;
  PJ::CatalogModel catalog;
  QString kept;
  QString toggled;
  std::unique_ptr<VisibilityPlot> plot;
};

// The centre of `curve`'s legend entry, in canvas coordinates; the plot must be shown and painted.
QPoint legendEntryCenter(VisibilityPlot& plot, QwtPlotItem* curve) {
  plot.qwtPlot()->canvas()->repaint();  // computes the legend entry geometry
  const auto geometries = plot.legend()->legendGeometries(curve);
  EXPECT_FALSE(geometries.empty()) << "legend entry was not laid out";
  return geometries.isEmpty() ? QPoint() : geometries.first().center();
}

}  // namespace

TEST(PlotCurveVisibility, HidingACurveDropsItsTrackerRowAtOnce) {
  TrackerFixture fixture;
  ASSERT_TRUE(fixture.boxNames(fixture.toggled)) << fixture.plot->trackerValueBoxText().toStdString();

  fixture.plot->setCurveVisible(fixture.toggled, false);
  EXPECT_FALSE(fixture.boxNames(fixture.toggled)) << fixture.plot->trackerValueBoxText().toStdString();
  EXPECT_TRUE(fixture.boxNames(fixture.kept));

  fixture.plot->setCurveVisible(fixture.toggled, true);
  EXPECT_TRUE(fixture.boxNames(fixture.toggled)) << fixture.plot->trackerValueBoxText().toStdString();
}

TEST(PlotCurveVisibility, ColourChangeRecoloursTheTrackerRowAtOnce) {
  TrackerFixture fixture;
  const QColor red(Qt::red);
  ASSERT_FALSE(fixture.plot->trackerValueBoxText().contains(u"color=%1"_s.arg(red.name())));

  fixture.plot->onChangeCurveColor(fixture.toggled, red);
  EXPECT_TRUE(fixture.plot->trackerValueBoxText().contains(u"color=%1"_s.arg(red.name())))
      << fixture.plot->trackerValueBoxText().toStdString();
}

TEST(PlotCurveVisibility, SetCurveVisibleIsOneUndoStepAnnouncedOnceAndRepeatingItIsNone) {
  TrackerFixture fixture;
  QSignalSpy undo(fixture.plot.get(), &PJ::PlotWidget::undoableChange);
  QSignalSpy announced(fixture.plot.get(), &PJ::PlotWidget::curveVisibilityChanged);

  fixture.plot->setCurveVisible(fixture.toggled, false);
  EXPECT_EQ(undo.count(), 1);
  ASSERT_EQ(announced.count(), 1);
  EXPECT_EQ(announced.front().at(0).toString(), fixture.toggled);
  EXPECT_FALSE(announced.front().at(1).toBool());

  fixture.plot->setCurveVisible(fixture.toggled, false);
  EXPECT_EQ(undo.count(), 1);
  EXPECT_EQ(announced.count(), 1);
}

TEST(PlotCurveVisibility, LegendClickIsAnUndoableVisibilityChangeThatDropsTheTrackerRow) {
  PJ::PlotWidgetBase::setGpuCanvasDisabledOverride(true);  // the raster canvas lays out the legend entries
  TrackerFixture fixture;
  fixture.plot->resize(800, 400);
  fixture.plot->show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(fixture.plot.get()));
  fixture.plot->replot();
  QwtPlotItem* curve = fixture.plot->curveFromTitle(fixture.toggled)->curve;
  const QPoint entry = legendEntryCenter(*fixture.plot, curve);
  ASSERT_TRUE(fixture.boxNames(fixture.toggled)) << fixture.plot->trackerValueBoxText().toStdString();

  QSignalSpy undo(fixture.plot.get(), &PJ::PlotWidget::undoableChange);
  QTest::mouseClick(fixture.plot->qwtPlot()->canvas(), Qt::LeftButton, Qt::NoModifier, entry);

  EXPECT_FALSE(curve->isVisible());
  EXPECT_EQ(undo.count(), 1);
  EXPECT_FALSE(fixture.boxNames(fixture.toggled)) << fixture.plot->trackerValueBoxText().toStdString();
}

TEST(CurveEditorCurveVisibility, LegendClickUnchecksTheEyeAndTheEyeBringsTheCurveBack) {
  PJ::PlotWidgetBase::setGpuCanvasDisabledOverride(true);
  TrackerFixture fixture;
  fixture.plot->resize(800, 400);
  fixture.plot->show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(fixture.plot.get()));
  fixture.plot->replot();

  PJ::CurveEditor editor;
  editor.setPlot(fixture.plot.get());
  auto* list = editor.findChild<QListWidget*>(u"listWidget"_s);
  ASSERT_NE(list, nullptr);
  ASSERT_EQ(list->count(), 2);
  auto* eye = list->itemWidget(list->item(1))->findChild<QToolButton*>(u"curveVisibilityToggle"_s);
  ASSERT_NE(eye, nullptr);
  ASSERT_TRUE(eye->isChecked());

  QwtPlotItem* curve = fixture.plot->curveFromTitle(fixture.toggled)->curve;
  QSignalSpy undo(fixture.plot.get(), &PJ::PlotWidget::undoableChange);
  QTest::mouseClick(
      fixture.plot->qwtPlot()->canvas(), Qt::LeftButton, Qt::NoModifier, legendEntryCenter(*fixture.plot, curve));
  ASSERT_FALSE(curve->isVisible());
  EXPECT_FALSE(eye->isChecked());
  EXPECT_EQ(undo.count(), 1) << "the eye follows the legend without a second undo step";

  // The next click on the eye shows the curve again instead of appearing to do nothing.
  eye->click();
  EXPECT_TRUE(curve->isVisible());
  EXPECT_TRUE(eye->isChecked());
  EXPECT_EQ(undo.count(), 2);
}

TEST(CurveEditorCurveVisibility, EyeClickIsOneUndoStep) {
  TrackerFixture fixture;
  PJ::CurveEditor editor;
  editor.setPlot(fixture.plot.get());
  auto* list = editor.findChild<QListWidget*>(u"listWidget"_s);
  ASSERT_NE(list, nullptr);
  auto* eye = list->itemWidget(list->item(1))->findChild<QToolButton*>(u"curveVisibilityToggle"_s);
  ASSERT_NE(eye, nullptr);

  QSignalSpy undo(fixture.plot.get(), &PJ::PlotWidget::undoableChange);
  eye->click();
  EXPECT_FALSE(fixture.plot->curveFromTitle(fixture.toggled)->curve->isVisible());
  EXPECT_EQ(undo.count(), 1);
  EXPECT_FALSE(fixture.boxNames(fixture.toggled)) << fixture.plot->trackerValueBoxText().toStdString();
}

// A restore that changes only a kept curve's visibility (an undo) refreshes the tracker too.
TEST(PlotCurveVisibility, RestoringAHiddenCurveDropsItsTrackerRow) {
  TrackerFixture fixture;
  fixture.plot->setCurveVisible(fixture.toggled, false);
  QDomDocument hidden_doc;
  QDomElement hidden_state = fixture.plot->xmlSaveState(hidden_doc);
  fixture.plot->stampLiveCurveKeys(hidden_state);
  fixture.plot->setCurveVisible(fixture.toggled, true);
  fixture.plot->setTrackerPosition(fixture.plot->maxZoomRect().center().x());
  ASSERT_TRUE(fixture.boxNames(fixture.toggled)) << fixture.plot->trackerValueBoxText().toStdString();

  ASSERT_TRUE(fixture.plot->xmlLoadState(hidden_state));
  EXPECT_FALSE(fixture.boxNames(fixture.toggled)) << fixture.plot->trackerValueBoxText().toStdString();
}

// The legend sends a curve's source_name; it must reach that curve even when an earlier curve's title spells the same.
TEST(PlotCurveVisibility, SourceNameWinsOverAnEarlierCurvesTitle) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotWidget plot(&session, &catalog);
  const auto series = [] { return new QwtPointSeriesData(QVector<QPointF>{{0.0, 0.0}, {1.0, 1.0}}); };
  PJ::PlotWidget::CurveInfo* first = plot.addCurve(u"first"_s, series(), Qt::red, u"first"_s);
  PJ::PlotWidget::CurveInfo* second = plot.addCurve(u"second"_s, series(), Qt::blue, u"second"_s);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  first->curve->setTitle(u"second"_s);

  plot.setCurveVisible(second->source_name, false);
  EXPECT_TRUE(first->curve->isVisible());
  EXPECT_FALSE(second->curve->isVisible());
}
