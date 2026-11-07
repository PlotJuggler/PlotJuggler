// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A plot shows a "computing markers" note while a rule it shows computes, keeps its
// markers drawn meanwhile, lists a failed rule's error as the note's tooltip, and
// repaints only when the note changes.

#include <gtest/gtest.h>
#include <qwt_legend_data.h>
#include <qwt_plot_curve.h>
#include <qwt_text.h>

#include <QApplication>
#include <QEvent>
#include <QHelpEvent>
#include <QObject>
#include <QSignalSpy>
#include <QTest>
#include <QToolTip>
#include <string>
#include <string_view>

#include "pj_base/builtin/plot_markers_codec.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plotting/PlotMarkersItem.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/PlotWidgetBase.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/MarkerTopics.h"
#include "pj_runtime/SessionManager.h"

using namespace Qt::StringLiterals;

namespace {

// A scalar topic on `dataset_id` so the plot has a curve, and so a dataset to draw markers for.
QString addCurveKey(PJ::SessionManager& session, PJ::CatalogModel& catalog, PJ::DatasetId dataset_id) {
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(dataset_id, "/imu/x", PJ::NumericType::kFloat64);
  EXPECT_TRUE(handle.has_value());
  writer.appendScalar(*handle, 100, 1.0);
  writer.appendScalar(*handle, 200, 2.0);
  EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());
  for (const auto& curve : catalog.curves()) {
    if (const auto descriptor = catalog.curveDescriptor(curve.name);
        descriptor && descriptor->topic_id == handle->topic_id) {
      return curve.name;
    }
  }
  return {};
}

// What a generator publishes for its dataset-scope set: one marker, under the owner's own topic.
void publishRuleMarkers(PJ::SessionManager& session, PJ::DatasetId dataset_id, std::string_view owner) {
  PJ::MarkerTopicInfo info = PJ::markerInfoForScope(PJ::MarkerScope::kDataset);
  info.owner = std::string(owner);
  const auto topic = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = dataset_id,
          .topic_name = PJ::markerOwnerTopicName(PJ::markerScopeTopic(PJ::MarkerScope::kDataset), owner),
          .metadata_json = PJ::markerTopicMetadataJson(info)});
  ASSERT_TRUE(topic.has_value());
  PJ::sdk::PlotMarkers markers;
  markers.markers.push_back(PJ::sdk::PlotMarker{});
  ASSERT_TRUE(session.objectStore().pushOwned(*topic, PJ::Timestamp{0}, PJ::serializePlotMarkers(markers)).has_value());
  session.notifyMarkersChanged();
}

PJ::MarkerRuleStatus computing(QString rule_id) {
  return PJ::MarkerRuleStatus{.rule_id = std::move(rule_id), .computing = true, .error = {}};
}

PJ::MarkerRuleStatus failed(QString rule_id, QString error) {
  return PJ::MarkerRuleStatus{.rule_id = std::move(rule_id), .computing = false, .error = std::move(error)};
}

// Counts the paint events of one widget.
class PaintCounter : public QObject {
 public:
  explicit PaintCounter(QWidget& widget) {
    widget.installEventFilter(this);
  }
  bool eventFilter(QObject* /*watched*/, QEvent* event) override {
    count += event->type() == QEvent::Paint ? 1 : 0;
    return false;
  }
  int count = 0;
};

class PlotWidgetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto dataset = session_.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
    ASSERT_TRUE(dataset.has_value());
    dataset_ = *dataset;
    key_ = addCurveKey(session_, catalog_, dataset_);
    ASSERT_FALSE(key_.isEmpty());
    ASSERT_NE(plot_.addCurve(key_), nullptr);
  }

  // Declared first: the raster canvas is chosen when the plot is built, and a GPU canvas
  // that fails to start would be swapped out (with the paint counters) mid-test.
  struct RasterCanvas {
    RasterCanvas() {
      PJ::PlotWidgetBase::setGpuCanvasDisabledOverride(true);
    }
  } raster_canvas_;
  PJ::SessionManager session_;
  PJ::CatalogModel catalog_{&session_};
  PJ::PlotWidget plot_{&session_, &catalog_};
  PJ::DatasetId dataset_ = 0;
  QString key_;
};

}  // namespace

TEST_F(PlotWidgetTest, MarkerStatusShowsComputingAndKeepsTheMarkers) {
  publishRuleMarkers(session_, dataset_, "rule/a");
  const auto drawn_topics = [&] {
    const PJ::MarkerTarget target{dataset_, PJ::markerInfoForScope(PJ::MarkerScope::kDataset)};
    return PJ::markerTargetTopics(
        target, PJ::scanMarkerTopics(session_.objectStore(), dataset_),
        PJ::scanMarkerTopics(session_.objectStore(), PJ::kAllDatasetsMarkerDataset));
  };
  ASSERT_EQ(drawn_topics().size(), 1U);
  EXPECT_TRUE(plot_.markerStatusNote().isEmpty());

  plot_.setMarkerStatus({computing(u"rule/a"_s), computing(u"rule/b"_s)});
  EXPECT_EQ(plot_.markerStatusNote(), u"Computing markers…"_s);
  EXPECT_EQ(drawn_topics().size(), 1U) << "the previous markers stay drawn while the rule computes";

  plot_.setMarkerStatus({computing(u"rule/b"_s)});
  EXPECT_EQ(plot_.markerStatusNote(), u"Computing markers…"_s) << "another rule still computes";

  plot_.setMarkerStatus({});
  EXPECT_TRUE(plot_.markerStatusNote().isEmpty());
  EXPECT_EQ(drawn_topics().size(), 1U);

  plot_.setMarkerStatus({PJ::MarkerRuleStatus{u"rule/a"_s, false, {}}});
  EXPECT_TRUE(plot_.markerStatusNote().isEmpty()) << "a rule that neither computes nor failed says nothing";
}

TEST_F(PlotWidgetTest, MarkerErrorIsTheNotesTooltip) {
  plot_.resize(400, 300);
  plot_.show();
  QWidget* canvas = plot_.canvasWidget();
  // The tooltip a hover at `pos` leaves; empty when the plot showed none (a sentinel stays untouched).
  const auto tooltip_at = [&](QPoint pos) {
    QToolTip::showText(canvas->mapToGlobal(pos), u"sentinel"_s, canvas);
    QHelpEvent help(QEvent::ToolTip, pos, canvas->mapToGlobal(pos));
    QApplication::sendEvent(canvas, &help);
    return QToolTip::text() == u"sentinel"_s ? QString() : QToolTip::text();
  };
  const auto tooltip_at_note = [&] { return tooltip_at(plot_.markerStatusNoteRect().center().toPoint()); };

  plot_.setMarkerStatus({failed(u"rule/a"_s, u"script error: boom"_s)});
  ASSERT_EQ(plot_.markerStatusNote(), u"Marker rule failed"_s);
  const QString tip = tooltip_at_note();
  EXPECT_TRUE(tip.contains(u"boom"_s)) << tip.toStdString();
  EXPECT_TRUE(tip.contains(u"rule/a"_s)) << tip.toStdString();

  // A rule computing again while its last run failed: the note says computing, the error stays listed.
  plot_.setMarkerStatus({PJ::MarkerRuleStatus{u"rule/a"_s, true, u"script error: boom"_s}});
  EXPECT_EQ(plot_.markerStatusNote(), u"Computing markers…"_s);
  EXPECT_TRUE(tooltip_at_note().contains(u"boom"_s));

  // Away from the note there is no tooltip, and a computing-only note has none.
  EXPECT_TRUE(tooltip_at(QPoint(390, 290)).isEmpty());
  plot_.setMarkerStatus({computing(u"rule/a"_s)});
  EXPECT_TRUE(tooltip_at_note().isEmpty());

  plot_.setMarkerStatus({});
  EXPECT_TRUE(plot_.markerStatusNoteRect().isEmpty());
}

TEST_F(PlotWidgetTest, UnchangedStatusDoesNotRepaint) {
  plot_.resize(400, 300);
  plot_.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&plot_));
  QTest::qWait(200);  // the canvas settles (a failed GPU canvas is swapped once)
  PaintCounter canvas_paints(*plot_.canvasWidget());

  plot_.setMarkerStatus({computing(u"rule/a"_s)});
  QTest::qWait(100);
  ASSERT_GT(canvas_paints.count, 0) << "a changed note repaints the canvas";

  canvas_paints.count = 0;
  plot_.setMarkerStatus({computing(u"rule/a"_s)});
  plot_.setMarkerStatus({computing(u"rule/a"_s), PJ::MarkerRuleStatus{u"rule/idle"_s, false, {}}});
  plot_.setMarkerStatus({computing(u"rule/a"_s), computing(u"rule/b"_s)});
  QTest::qWait(100);
  EXPECT_EQ(canvas_paints.count, 0) << "the same status, an idle rule and another computing rule repaint nothing";
}

// The host maps rules to plots through the plot's marker targets, so every change of them is announced: a curve
// added or removed, a curve's own markers shown or hidden, a dataset's marker scope shown or hidden.
TEST_F(PlotWidgetTest, MarkerTargetChangesAreAnnounced) {
  QSignalSpy changed(&plot_, &PJ::PlotWidget::markerTargetsChanged);
  plot_.setCurveShowMarkers(key_, false);
  EXPECT_EQ(changed.count(), 1) << "a curve's markers hidden";
  plot_.setDatasetMarkerScopeVisible(dataset_, PJ::MarkerScope::kDataset, false);
  EXPECT_EQ(changed.count(), 2) << "a marker scope hidden";
  plot_.setDatasetMarkerScopeVisible(dataset_, PJ::MarkerScope::kDataset, false);
  EXPECT_EQ(changed.count(), 2) << "no change, no announcement";
  plot_.removeAllCurves();
  EXPECT_GE(changed.count(), 3) << "the curves removed";
}

// The legend takes the top-left corner when it is aligned there; the note moves to the opposite side.
TEST_F(PlotWidgetTest, TheNoteStaysClearOfALeftAlignedLegend) {
  plot_.resize(400, 300);
  plot_.show();
  plot_.setMarkerStatus({computing(u"rule/a"_s)});
  const double middle = plot_.canvasWidget()->contentsRect().width() / 2.0;
  plot_.setLegendVisible(true);

  plot_.setLegendAlignment(Qt::AlignTop | Qt::AlignRight);
  EXPECT_LT(plot_.markerStatusNoteRect().center().x(), middle) << "a legend at the right leaves the left";
  plot_.setLegendAlignment(Qt::AlignTop | Qt::AlignLeft);
  EXPECT_GT(plot_.markerStatusNoteRect().center().x(), middle) << "a legend at the left leaves the right";
  plot_.setLegendAlignment(Qt::AlignBottom | Qt::AlignLeft);
  EXPECT_LT(plot_.markerStatusNoteRect().center().x(), middle) << "a bottom legend does not meet a top note";
}

// GAP-40: a plotted series that is not computed says so after its legend title, "not computed" or "computing N%", as
// its host reports it; the curve's title, its identity, stays; a series of another dataset, or none, says nothing.
TEST_F(PlotWidgetTest, UncomputedSeriesLegendSaysSo) {
  const PJ::PlotWidgetBase::CurveInfo& info = plot_.curveList().front();
  const auto legend = [&] { return info.curve->legendData().front().title().text(); };
  const QString title = info.curve->title().text();
  ASSERT_EQ(legend(), title);

  plot_.setSeriesStatus({PJ::SeriesCurveStatus{.dataset_id = dataset_, .topic_name = u"/imu/x"_s}});
  EXPECT_EQ(legend(), title + u" (not computed)"_s);
  plot_.setSeriesStatus(
      {PJ::SeriesCurveStatus{.dataset_id = dataset_, .topic_name = u"/imu/x"_s, .computing_percent = 34}});
  EXPECT_EQ(legend(), title + u" (computing 34%)"_s);
  EXPECT_EQ(info.curve->title().text(), title);
  EXPECT_EQ(plot_.curveFromTitle(title), &info);

  plot_.setSeriesStatus({PJ::SeriesCurveStatus{.dataset_id = dataset_ + 1, .topic_name = u"/imu/x"_s}});
  EXPECT_EQ(legend(), title) << "another dataset's series";
  plot_.setSeriesStatus({PJ::SeriesCurveStatus{.dataset_id = dataset_, .topic_name = u"/imu/y"_s}});
  EXPECT_EQ(legend(), title) << "another topic's series in the same dataset";
  plot_.setSeriesStatus({PJ::SeriesCurveStatus{.dataset_id = dataset_, .topic_name = u"/imu/x"_s}});
  plot_.setSeriesStatus({});
  EXPECT_EQ(legend(), title) << "computed";
}
