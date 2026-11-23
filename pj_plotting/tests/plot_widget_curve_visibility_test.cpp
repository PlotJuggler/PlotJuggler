// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A curve's visibility and colour change through one path (PlotWidget::setCurveVisible / onChangeCurveColor): the
// tracker's value box follows at once, a legend click is an undoable step the curve editor hears, and the editor's eye
// and the legend agree.

#include <gtest/gtest.h>
#include <qwt_plot.h>
#include <qwt_plot_curve.h>
#include <qwt_plot_marker.h>
#include <qwt_scale_map.h>
#include <qwt_series_data.h>
#include <qwt_text.h>

#include <QApplication>
#include <QColor>
#include <QDomDocument>
#include <QListWidget>
#include <QPalette>
#include <QPen>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>
#include <algorithm>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "pj_datastore/writer.hpp"
#include "pj_plotting/CurveEditor.h"
#include "pj_plotting/CurveTracker.h"
#include "pj_plotting/PlotLegend.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
#include "pj_widgets/ColorContrast.h"
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
  // The ink of the value box's row for `key`.
  [[nodiscard]] std::optional<QColor> rowInk(const QString& key) {
    for (const PJ::ValueBoxRow& row : plot->trackerValueBoxRows()) {
      if (row.text.endsWith(u": "_s + title(key))) {
        return row.ink;
      }
    }
    return std::nullopt;
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
  ASSERT_NE(fixture.rowInk(fixture.toggled), red);

  fixture.plot->onChangeCurveColor(fixture.toggled, red);
  EXPECT_EQ(fixture.rowInk(fixture.toggled), red) << fixture.plot->trackerValueBoxText().toStdString();
}

// The default blue and red curve colours are 3.3:1 and 3.2:1 on the dark backdrop: read as text they are lifted
// to 4.5:1, while the curve and its marker keep the colour the user picked.
TEST(PlotCurveVisibility, DarkThemeTrackerInkReadsOnTheBackdropAndTheCurveKeepsItsColour) {
  QPalette dark = QApplication::palette();
  dark.setColor(QPalette::Window, QColor("#373743"));  // theme::appTheme() reads the window lightness
  QApplication::setPalette(dark);
  QApplication::processEvents();

  TrackerFixture fixture;
  const QColor blue("#1F77B4");
  fixture.plot->onChangeCurveColor(fixture.toggled, blue);

  const QColor backdrop = fixture.plot->qwtPlot()->canvasBackground().color();
  ASSERT_LT(PJ::contrastRatio(blue, backdrop), 4.5) << "the raw colour must be the failing case";
  const std::optional<QColor> row_ink = fixture.rowInk(fixture.toggled);
  ASSERT_TRUE(row_ink.has_value()) << fixture.plot->trackerValueBoxText().toStdString();
  const QColor ink = *row_ink;
  EXPECT_GE(PJ::contrastRatio(ink, backdrop), 4.5) << ink.name().toStdString();
  EXPECT_NE(ink, blue);
  EXPECT_EQ(fixture.plot->curveFromTitle(fixture.toggled)->curve->pen().color(), blue);
}

// A live theme switch redraws the tracker box in the new theme at once, not at the next cursor move.
TEST(PlotCurveVisibility, ThemeSwitchRedrawsTheTrackerBox) {
  TrackerFixture fixture;
  const QColor blue("#1F77B4");
  fixture.plot->onChangeCurveColor(fixture.toggled, blue);

  QPalette dark = QApplication::palette();
  dark.setColor(QPalette::Window, QColor("#373743"));
  QApplication::setPalette(dark);
  QApplication::processEvents();

  const QColor backdrop = fixture.plot->qwtPlot()->canvasBackground().color();
  ASSERT_LT(PJ::contrastRatio(blue, backdrop), 4.5) << "the canvas did not take the dark backdrop";
  const std::optional<QColor> row_ink = fixture.rowInk(fixture.toggled);
  ASSERT_TRUE(row_ink.has_value()) << fixture.plot->trackerValueBoxText().toStdString();
  EXPECT_GE(PJ::contrastRatio(*row_ink, backdrop), 4.5) << "the box kept the light theme's ink";
}

// Where the tracker's value box is drawn, in canvas pixels, with the tracker line at each pixel column in turn.
struct BoxSpan {
  double left = 0.0;
  double right = 0.0;
  double line = 0.0;
};
// The plot is sized so that its canvas is `canvas_per_box` times as wide as the box: the box's font is the system's
// fixed-pitch one, so the box and the axes differ in width between platforms.
std::vector<BoxSpan> boxSpansAcrossTheCanvas(
    TrackerFixture& fixture, double canvas_per_box, double* canvas_left, double* canvas_right) {
  fixture.plot->resize(520, 240);
  fixture.plot->show();
  QApplication::processEvents();
  QwtPlot* qwt = fixture.plot->qwtPlot();
  double box_width = 0.0;
  for (const QwtPlotItem* item : qwt->itemList(QwtPlotItem::Rtti_PlotMarker)) {
    const auto* marker = static_cast<const QwtPlotMarker*>(item);
    if (!marker->label().isEmpty()) {
      box_width = marker->label().textSize().width();
    }
  }
  EXPECT_GT(box_width, 0.0);
  const double axes_width = qwt->width() - std::abs(qwt->canvasMap(QwtPlot::xBottom).pDist());
  fixture.plot->resize(static_cast<int>(canvas_per_box * box_width + axes_width), 240);
  QApplication::processEvents();
  const QwtScaleMap x_map = qwt->canvasMap(QwtPlot::xBottom);
  *canvas_left = std::min(x_map.p1(), x_map.p2());
  *canvas_right = std::max(x_map.p1(), x_map.p2());
  EXPECT_LT(*canvas_right - *canvas_left, 0.95 * qwt->width()) << "the axes must take part of the plot's width";

  std::vector<BoxSpan> spans;
  for (double pixel = *canvas_left + 2.0; pixel < *canvas_right - 2.0; pixel += 1.0) {
    const double time = x_map.invTransform(pixel);
    fixture.plot->setTrackerPosition(time);
    EXPECT_TRUE(fixture.plot->trackerValueBoxVisible()) << pixel;
    for (const QwtPlotItem* item : qwt->itemList(QwtPlotItem::Rtti_PlotMarker)) {
      const auto* marker = static_cast<const QwtPlotMarker*>(item);
      if (!marker->label().isEmpty()) {
        // QwtPlotMarker::drawLabel starts a right-aligned label half a pixel plus `spacing` after its position.
        const double left = x_map.transform(marker->xValue()) + marker->spacing() + 0.5;
        spans.push_back({.left = left, .right = left + marker->label().textSize().width(), .line = pixel});
      }
    }
  }
  return spans;
}

// The value box stays inside the canvas and clear of the tracker line wherever the line is: the box's width is
// converted to plot units over the canvas. Over the whole plot widget, which also holds the axes, it came out too
// small, so near the right edge the box overflowed the canvas or covered the line.
TEST(PlotCurveVisibility, ValueBoxStaysInsideTheCanvasAndClearOfTheLine) {
  TrackerFixture fixture;
  double canvas_left = 0.0;
  double canvas_right = 0.0;
  const std::vector<BoxSpan> spans = boxSpansAcrossTheCanvas(fixture, 2.6, &canvas_left, &canvas_right);
  ASSERT_GT(spans.size(), 100u);
  constexpr double kSlack = 1.0;  // pixel rounding
  for (const BoxSpan& box : spans) {
    ASSERT_LT(2.0 * (box.right - box.left), canvas_right - canvas_left) << "the box must fit on one side of the line";
    EXPECT_GE(box.left, canvas_left - kSlack) << "line at " << box.line;
    EXPECT_LE(box.right, canvas_right + kSlack) << "line at " << box.line;
    EXPECT_TRUE(box.left >= box.line - kSlack || box.right <= box.line + kSlack)
        << "box " << box.left << ".." << box.right << " covers the line at " << box.line;
  }
}

// A box wider than the canvas cannot fit on either side: it keeps its first column (the time and the values) inside
// the canvas and loses the end of its rows.
TEST(PlotCurveVisibility, ValueBoxWiderThanTheCanvasKeepsItsLeftEdgeInside) {
  TrackerFixture fixture;
  double canvas_left = 0.0;
  double canvas_right = 0.0;
  const std::vector<BoxSpan> spans = boxSpansAcrossTheCanvas(fixture, 0.7, &canvas_left, &canvas_right);
  ASSERT_FALSE(spans.empty());
  ASSERT_GT(spans.front().right - spans.front().left, canvas_right - canvas_left) << "the box must not fit";
  for (const BoxSpan& box : spans) {
    EXPECT_GE(box.left, canvas_left - 1.0) << "line at " << box.line;
  }
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
