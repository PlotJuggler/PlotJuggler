// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>
#include <qwt_plot_curve.h>

#include <QApplication>
#include <QColor>
#include <QDomDocument>
#include <QDomElement>
#include <QHash>
#include <QPen>
#include <QSet>
#include <QTemporaryDir>
#include <QtGlobal>
#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

#include "LayoutXml.h"
#include "PendingDisplayBinder.h"
#include "dataset_test_helpers.h"
#include "pj_plotting/PointSeriesXY.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/TopicDemandTracker.h"
#include "pj_scene_common/scene_dock_widget.h"
using namespace Qt::StringLiterals;

namespace {

using PJ::layout_xml::SeriesPath;
using pj_test::addScalarTopic;
using pj_test::createDataset;

QDomElement addPlot(QDomDocument& doc, const QString& id, const QString& mode) {
  QDomElement root = doc.documentElement();
  if (root.isNull()) {
    root = doc.createElement(u"root"_s);
    doc.appendChild(root);
  }
  QDomElement plot = doc.createElement(u"plot"_s);
  plot.setAttribute(u"id"_s, id);
  plot.setAttribute(u"mode"_s, mode);
  root.appendChild(plot);
  return plot;
}

// Writes a SeriesPath's dataset qualifiers onto a curve element, using the given
// attribute-name prefix ("" for time series, "x_"/"y_" for XY axes). Only set
// qualifiers are written so an unqualified path leaves a clean element.
void writeDatasetQualifiers(QDomElement& curve, const QString& prefix, const SeriesPath& path) {
  if (path.dataset_id != 0) {
    curve.setAttribute(prefix + u"dataset_id"_s, QString::number(path.dataset_id));
  }
  if (!path.dataset_source.isEmpty()) {
    curve.setAttribute(prefix + u"dataset_source"_s, path.dataset_source);
  }
  if (!path.dataset_path.isEmpty()) {
    curve.setAttribute(prefix + u"dataset_path"_s, path.dataset_path);
  }
}

QDomElement addTimeSeriesCurve(QDomDocument& doc, QDomElement& plot, const SeriesPath& path) {
  QDomElement curve = doc.createElement(u"curve"_s);
  curve.setAttribute(u"topic"_s, path.topic);
  curve.setAttribute(u"field"_s, path.field);
  writeDatasetQualifiers(curve, QString(), path);
  plot.appendChild(curve);
  return curve;
}

QDomElement addXyCurve(QDomDocument& doc, QDomElement& plot, const SeriesPath& x_path, const SeriesPath& y_path) {
  QDomElement curve = doc.createElement(u"curve"_s);
  curve.setAttribute(u"x_topic"_s, x_path.topic);
  curve.setAttribute(u"x_field"_s, x_path.field);
  curve.setAttribute(u"y_topic"_s, y_path.topic);
  curve.setAttribute(u"y_field"_s, y_path.field);
  writeDatasetQualifiers(curve, u"x_"_s, x_path);
  writeDatasetQualifiers(curve, u"y_"_s, y_path);
  plot.appendChild(curve);
  return curve;
}

// Resolves a SeriesPath to the DatasetId of the concrete curve it binds to, or
// nullopt when unresolvable/ambiguous. The identity assertion most resolver tests
// need.
std::optional<PJ::DatasetId> resolvedDataset(PJ::CatalogModel& catalog, const SeriesPath& path) {
  const auto key = PJ::resolveSeriesPath(catalog, path);
  if (!key.has_value()) {
    return std::nullopt;
  }
  const auto descriptor = catalog.curveDescriptor(*key);
  return descriptor.has_value() ? std::optional<PJ::DatasetId>{descriptor->dataset_id} : std::nullopt;
}

void rebindAgainstCatalog(QDomDocument& doc, PJ::CatalogModel& catalog) {
  const QList<SeriesPath> unresolved = PJ::layout_xml::rebindCurveKeys(
      doc, [&catalog](const SeriesPath& path) { return PJ::resolveSeriesPath(catalog, path); });
  (void)unresolved;
}

QHash<QString, PJ::PlotWidget*> indexByStateId(PJ::PlotWidget& plot) {
  QHash<QString, PJ::PlotWidget*> plots;
  plots.insert(plot.stateId(), &plot);
  return plots;
}

}  // namespace

TEST(PendingDisplayBinderTest, RestoredEntryReleasesItsReferenceWhenThePlotDies) {
  PJ::AppSession app_session;
  const PJ::DatasetId dataset_id = createDataset(app_session);
  PJ::TopicDemandTracker tracker;
  PJ::PendingDisplayBinder binder(app_session.catalogModel(), &tracker);

  // The topic is advertised (nameable) but has no data — a layout restore over
  // a live demand stream stages a pending curve holding a demand reference.
  app_session.catalogModel().setAdvertisedTopics(
      dataset_id, {PJ::AdvertisedTopic{u"/speed"_s, PJ::sdk::BuiltinObjectType::kNone}});
  QDomDocument doc;
  auto plot = std::make_unique<PJ::PlotWidget>(&app_session.sessionManager(), &app_session.catalogModel());
  QDomElement plot_element = addPlot(doc, plot->stateId(), u"time"_s);
  addTimeSeriesCurve(doc, plot_element, SeriesPath{u"/speed"_s, u"value"_s});
  binder.collect(doc, indexByStateId(*plot));
  ASSERT_EQ(binder.size(), 1);
  const auto held = tracker.activeTopics(dataset_id);
  ASSERT_NE(std::find(held.begin(), held.end(), u"/speed"_s), held.end());

  // The plot dies before the topic ever materializes: the reference must
  // release AT destruction — on a quiet stream no later flush ever runs, and a
  // lazy release would keep the topic subscribed forever.
  plot.reset();
  const auto after = tracker.activeTopics(dataset_id);
  EXPECT_EQ(std::find(after.begin(), after.end(), u"/speed"_s), after.end());
  EXPECT_EQ(binder.size(), 0);
}

TEST(PendingDisplayBinderTest, DuplicatePlaceholderDropStagesOneEntry) {
  PJ::AppSession app_session;
  const PJ::DatasetId dataset_id = createDataset(app_session);
  PJ::TopicDemandTracker tracker;
  PJ::PendingDisplayBinder binder(app_session.catalogModel(), &tracker);
  PJ::PlotWidget plot(&app_session.sessionManager(), &app_session.catalogModel());

  // Double-dropping the same placeholder on the same plot must not stage a
  // second intent: on promotion the first entry binds every field, the
  // duplicate's addCurve() calls all return null, and it would sit forever
  // holding its demand reference (and re-add ghost curves after a manual
  // curve delete on a later flush).
  binder.addPendingCurve(&plot, SeriesPath{u"/speed"_s, QString()}, dataset_id);
  binder.addPendingCurve(&plot, SeriesPath{u"/speed"_s, QString()}, dataset_id);
  EXPECT_EQ(binder.size(), 1);
}

TEST(PendingDisplayBinderTest, PreferredPlaceholderWaitsForItsDatasetInsteadOfStealingSibling) {
  PJ::AppSession app_session;
  const PJ::DatasetId dataset_a = createDataset(app_session);
  const PJ::DatasetId dataset_b = createDataset(app_session);
  ASSERT_NE(addScalarTopic(app_session, dataset_a, "/speed"), 0U);
  app_session.catalogModel().setAdvertisedTopics(
      dataset_b, {PJ::AdvertisedTopic{u"/speed"_s, PJ::sdk::BuiltinObjectType::kNone}});

  PJ::PendingDisplayBinder binder(app_session.catalogModel());
  PJ::PlotWidget plot(&app_session.sessionManager(), &app_session.catalogModel());
  binder.addPendingCurve(&plot, SeriesPath{u"/speed"_s, QString()}, dataset_b);

  EXPECT_EQ(binder.flush({}), 0);
  EXPECT_TRUE(plot.curveList().empty()) << "dataset A must not satisfy dataset B's explicit drop";
  ASSERT_NE(addScalarTopic(app_session, dataset_b, "/speed"), 0U);
  EXPECT_EQ(binder.flush(QSet<QString>{u"/speed"_s}), 1);
  ASSERT_EQ(plot.curveList().size(), 1U);
  const auto descriptor = app_session.catalogModel().curveDescriptor(plot.curveList().front().source_name);
  ASSERT_TRUE(descriptor.has_value());
  EXPECT_EQ(descriptor->dataset_id, dataset_b);
}

TEST(PendingDisplayBinderTest, SameTopicDropsFromDifferentDatasetsRemainDistinctIntents) {
  PJ::AppSession app_session;
  const PJ::DatasetId dataset_a = createDataset(app_session);
  const PJ::DatasetId dataset_b = createDataset(app_session);
  PJ::PendingDisplayBinder binder(app_session.catalogModel());
  PJ::PlotWidget plot(&app_session.sessionManager(), &app_session.catalogModel());

  const SeriesPath topic{u"/speed"_s, QString()};
  binder.addPendingCurve(&plot, topic, dataset_a);
  binder.addPendingCurve(&plot, topic, dataset_b);
  EXPECT_EQ(binder.size(), 2) << "preferred DatasetId participates in pending-entry identity";
}

TEST(PendingDisplayBinderTest, PlaceholderCompletionPreservesPopulatedPlotZoom) {
  PJ::AppSession app_session;
  const PJ::DatasetId ready_dataset = createDataset(app_session);
  const PJ::DatasetId late_dataset = createDataset(app_session);
  ASSERT_NE(addScalarTopic(app_session, ready_dataset, "/ready"), 0U);
  app_session.catalogModel().setAdvertisedTopics(
      late_dataset, {PJ::AdvertisedTopic{u"/late"_s, PJ::sdk::BuiltinObjectType::kNone}});

  PJ::PlotWidget plot(&app_session.sessionManager(), &app_session.catalogModel());
  const auto ready_key =
      PJ::resolveSeriesPath(app_session.catalogModel(), SeriesPath{u"/ready"_s, u"value"_s, ready_dataset});
  ASSERT_TRUE(ready_key.has_value());
  ASSERT_NE(plot.addCurve(*ready_key), nullptr);
  plot.setZoomRectangle(QRectF(12.0, -4.0, 3.0, 8.0), /*emit_signal=*/false);
  const QRectF user_zoom = plot.currentBoundingRect();

  PJ::PendingDisplayBinder binder(app_session.catalogModel());
  binder.addPendingCurve(&plot, SeriesPath{u"/late"_s, QString()}, late_dataset);
  ASSERT_NE(addScalarTopic(app_session, late_dataset, "/late"), 0U);
  ASSERT_EQ(binder.flush({}), 1);
  EXPECT_EQ(plot.currentBoundingRect(), user_zoom);
}

TEST(PendingDisplayBinderTest, FreshPlaceholderCompletionZoomsToFirstCurve) {
  PJ::AppSession app_session;
  const PJ::DatasetId dataset_id = createDataset(app_session);
  app_session.catalogModel().setAdvertisedTopics(
      dataset_id, {PJ::AdvertisedTopic{u"/late"_s, PJ::sdk::BuiltinObjectType::kNone}});
  PJ::PlotWidget plot(&app_session.sessionManager(), &app_session.catalogModel());
  PJ::PendingDisplayBinder binder(app_session.catalogModel());
  binder.addPendingCurve(&plot, SeriesPath{u"/late"_s, QString()}, dataset_id);

  ASSERT_NE(addScalarTopic(app_session, dataset_id, "/late"), 0U);
  ASSERT_EQ(binder.flush({}), 1);
  EXPECT_EQ(plot.currentBoundingRect(), plot.maxZoomRect());
}

TEST(PendingDisplayBinderTest, FinalizedSavedViewportSurvivesLateOnlyCurveMaterialization) {
  PJ::AppSession app_session;
  const PJ::DatasetId dataset_id = createDataset(app_session);
  app_session.catalogModel().setAdvertisedTopics(
      dataset_id, {PJ::AdvertisedTopic{u"/late"_s, PJ::sdk::BuiltinObjectType::kNone}});

  QDomDocument doc;
  PJ::PlotWidget plot(&app_session.sessionManager(), &app_session.catalogModel());
  QDomElement plot_element = addPlot(doc, plot.stateId(), u"TimeSeries"_s);
  QDomElement range = doc.createElement(u"range"_s);
  range.setAttribute(u"bottom"_s, u"-5"_s);
  range.setAttribute(u"top"_s, u"5"_s);
  range.setAttribute(u"left"_s, u"10"_s);
  range.setAttribute(u"right"_s, u"20"_s);
  range.setAttribute(u"left_ns"_s, u"10000000000"_s);
  range.setAttribute(u"right_ns"_s, u"20000000000"_s);
  range.setAttribute(u"x_basis"_s, u"absolute"_s);
  plot_element.appendChild(range);
  QDomElement pending = addTimeSeriesCurve(doc, plot_element, SeriesPath{u"/late"_s, QString(), dataset_id, u"test"_s});
  pending.setAttribute(u"pending_intent"_s, u"true"_s);

  ASSERT_TRUE(plot.xmlLoadState(plot_element));
  PJ::PendingDisplayBinder binder(app_session.catalogModel());
  binder.collect(doc, indexByStateId(plot));
  ASSERT_EQ(binder.size(), 1);
  plot.applySavedViewportOrZoom(/*clear_after=*/true);
  const QRectF finalized = plot.currentBoundingRect();
  ASSERT_FALSE(plot.hasSavedViewport());

  ASSERT_NE(addScalarTopic(app_session, dataset_id, "/late"), 0U);
  ASSERT_EQ(binder.flush({}), 1);
  EXPECT_EQ(plot.currentBoundingRect(), finalized);
  EXPECT_EQ(plot.curveList().size(), 1U);
}

TEST(PendingDisplayBinderTest, PlaceholderIntentRoundTripsAsOrdinaryKeepAliveCurve) {
  PJ::AppSession app_session;
  const PJ::DatasetId dataset_id = createDataset(app_session);
  app_session.catalogModel().setAdvertisedTopics(
      dataset_id, {PJ::AdvertisedTopic{u"/speed"_s, PJ::sdk::BuiltinObjectType::kNone}});
  PJ::PendingDisplayBinder binder(app_session.catalogModel());
  PJ::PlotWidget plot(&app_session.sessionManager(), &app_session.catalogModel());
  binder.addPendingCurve(&plot, SeriesPath{u"/speed"_s, QString()}, dataset_id);

  QDomDocument saved_doc;
  const QDomElement saved = plot.xmlSaveState(saved_doc);
  EXPECT_TRUE(saved.firstChildElement(u"pending_curve"_s).isNull());
  const QDomElement intent = saved.firstChildElement(u"curve"_s);
  ASSERT_FALSE(intent.isNull());
  EXPECT_EQ(intent.attribute(u"pending_intent"_s), u"true"_s);
  EXPECT_EQ(intent.attribute(u"dataset_id"_s).toUInt(), dataset_id);

  QDomDocument stripped_doc;
  stripped_doc.appendChild(stripped_doc.importNode(saved, /*deep=*/true));
  PJ::layout_xml::removeUnvalidatedDatasetIds(stripped_doc);
  PJ::layout_xml::stripUnresolvedCurves(stripped_doc);
  const QDomElement stripped_intent = stripped_doc.documentElement().firstChildElement(u"curve"_s);
  ASSERT_FALSE(stripped_intent.isNull());
  EXPECT_FALSE(stripped_intent.hasAttribute(u"dataset_id"_s));
  EXPECT_EQ(stripped_intent.attribute(u"pending_intent"_s), u"true"_s);

  PJ::PlotWidget restored(&app_session.sessionManager(), &app_session.catalogModel());
  ASSERT_TRUE(restored.xmlLoadState(saved));
  ASSERT_EQ(restored.pendingCurveIntentCount(), 1U);
  QDomDocument resaved_doc;
  const QDomElement resaved = restored.xmlSaveState(resaved_doc);
  EXPECT_TRUE(resaved.firstChildElement(u"pending_curve"_s).isNull());
  EXPECT_EQ(resaved.firstChildElement(u"curve"_s).attribute(u"pending_intent"_s), u"true"_s);
}

TEST(PendingDisplayBinderTest, LateXyHalvesKeepTheirIndependentDatasetIdentities) {
  QTemporaryDir extensions_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  PJ::AppSession app_session(extensions_dir.path());
  auto& engine = app_session.sessionManager().dataEngine();
  const auto dataset_a = engine.createDataset(PJ::DatasetDescriptor{.source_name = "same.mcap"});
  const auto dataset_b = engine.createDataset(PJ::DatasetDescriptor{.source_name = "same.mcap"});
  ASSERT_TRUE(dataset_a.has_value());
  ASSERT_TRUE(dataset_b.has_value());
  ASSERT_NE(addScalarTopic(app_session, *dataset_a, "/x"), 0U);
  ASSERT_NE(addScalarTopic(app_session, *dataset_a, "/y"), 0U);

  // x is qualified to dataset A, y to dataset B (both share the raw source
  // label "same.mcap"): y is not materialized yet, so the curve waits.
  const SeriesPath x_path{u"/x"_s, u"value"_s, *dataset_a, u"same.mcap"_s};
  const SeriesPath y_path{u"/y"_s, u"value"_s, *dataset_b, u"same.mcap"_s};
  QDomDocument doc;
  QDomElement plot_element = addPlot(doc, u"plot_xy"_s, u"XYPlot"_s);
  QDomElement curve = addXyCurve(doc, plot_element, x_path, y_path);
  curve.setAttribute(u"name"_s, u"cross-source"_s);
  rebindAgainstCatalog(doc, app_session.catalogModel());

  PJ::PlotWidget plot(&app_session.sessionManager(), &app_session.catalogModel());
  ASSERT_TRUE(plot.xmlLoadState(plot_element));
  ASSERT_TRUE(plot.curveList().empty());

  PJ::PendingDisplayBinder binder(app_session.catalogModel());
  binder.collect(doc, indexByStateId(plot));
  ASSERT_EQ(binder.size(), 1);

  ASSERT_NE(addScalarTopic(app_session, *dataset_b, "/y"), 0U);
  EXPECT_EQ(binder.flush(QSet<QString>{u"/y"_s}), 1);
  ASSERT_EQ(plot.curveList().size(), 1U);
  const auto* series = dynamic_cast<const PJ::PointSeriesXY*>(plot.curveList().front().curve->data());
  ASSERT_NE(series, nullptr);
  EXPECT_EQ(series->xSource().dataset_id, *dataset_a);
  EXPECT_EQ(series->ySource().dataset_id, *dataset_b);
}

TEST(PendingDisplayBinderTest, CollectsUnresolvedCurvesAndBindsThemWhenTopicArrives) {
  QTemporaryDir extensions_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  PJ::AppSession app_session(extensions_dir.path());
  const PJ::DatasetId dataset_id = createDataset(app_session);
  ASSERT_NE(dataset_id, 0U);

  const SeriesPath ready{u"/ready"_s, u"value"_s};
  const SeriesPath late{u"/late"_s, u"value"_s};
  ASSERT_NE(addScalarTopic(app_session, dataset_id, ready.topic.toStdString()), 0U);

  QDomDocument doc;
  QDomElement plot_element = addPlot(doc, u"plot_ts"_s, u"TimeSeries"_s);
  // Style and line width are plot-level (every curve inherits them); colour and
  // visibility are per-curve.
  plot_element.setAttribute(u"style"_s, u"Dots"_s);
  plot_element.setAttribute(u"line_width"_s, u"3.0"_s);
  addTimeSeriesCurve(doc, plot_element, ready);
  QDomElement late_curve = addTimeSeriesCurve(doc, plot_element, late);
  late_curve.setAttribute(u"color"_s, u"#123456"_s);
  late_curve.setAttribute(u"visible"_s, u"false"_s);
  rebindAgainstCatalog(doc, app_session.catalogModel());

  PJ::PlotWidget plot(&app_session.sessionManager(), &app_session.catalogModel());
  ASSERT_TRUE(plot.xmlLoadState(plot_element));
  ASSERT_EQ(plot.curveList().size(), 1U);

  PJ::PendingDisplayBinder binder(app_session.catalogModel());
  binder.collect(doc, indexByStateId(plot));
  ASSERT_EQ(binder.size(), 1);
  ASSERT_EQ(binder.unresolved().size(), 1);
  EXPECT_EQ(binder.unresolved().front(), late);
  EXPECT_EQ(binder.flush(QSet<QString>{u"/unrelated"_s}), 0);
  EXPECT_EQ(binder.size(), 1);

  ASSERT_NE(addScalarTopic(app_session, dataset_id, late.topic.toStdString()), 0U);
  EXPECT_EQ(binder.flush(QSet<QString>{late.topic}), 1);
  EXPECT_TRUE(binder.empty());
  ASSERT_EQ(plot.curveList().size(), 2U);

  const auto late_key = PJ::resolveSeriesPath(app_session.catalogModel(), late);
  ASSERT_TRUE(late_key.has_value());
  PJ::PlotWidget::CurveInfo* info = plot.curveFromTitle(*late_key);
  ASSERT_NE(info, nullptr);
  ASSERT_NE(info->curve, nullptr);
  EXPECT_EQ(info->curve->pen().color(), QColor(u"#123456"_s));
  // Style and width are inherited from the plot (Dots at the plot's line width).
  EXPECT_EQ(info->curve->style(), QwtPlotCurve::Dots);
  EXPECT_DOUBLE_EQ(info->curve->pen().widthF(), PJ::dotWidthValue(PJ::LineWidth::kPoints30));
  EXPECT_FALSE(info->curve->isVisible());

  EXPECT_EQ(binder.flush(QSet<QString>{late.topic}), 0);
  EXPECT_EQ(plot.curveList().size(), 2U);
}

TEST(PendingDisplayBinderTest, XyCurveWaitsForBothHalvesBeforeBinding) {
  QTemporaryDir extensions_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  PJ::AppSession app_session(extensions_dir.path());
  const PJ::DatasetId dataset_id = createDataset(app_session);
  ASSERT_NE(dataset_id, 0U);

  const SeriesPath x_path{u"/pose_x"_s, u"value"_s};
  const SeriesPath y_path{u"/pose_y"_s, u"value"_s};
  ASSERT_NE(addScalarTopic(app_session, dataset_id, x_path.topic.toStdString()), 0U);

  QDomDocument doc;
  QDomElement plot_element = addPlot(doc, u"plot_xy"_s, u"XYPlot"_s);
  addXyCurve(doc, plot_element, x_path, y_path);
  rebindAgainstCatalog(doc, app_session.catalogModel());

  PJ::PlotWidget plot(&app_session.sessionManager(), &app_session.catalogModel());
  ASSERT_TRUE(plot.xmlLoadState(plot_element));
  ASSERT_TRUE(plot.curveList().empty());

  PJ::PendingDisplayBinder binder(app_session.catalogModel());
  binder.collect(doc, indexByStateId(plot));
  ASSERT_EQ(binder.size(), 1);
  // x is already in the catalog, so only the missing half (y) is reported.
  const QList<SeriesPath> unresolved = binder.unresolved();
  ASSERT_EQ(unresolved.size(), 1);
  EXPECT_EQ(unresolved[0], y_path);

  EXPECT_EQ(binder.flush(QSet<QString>{x_path.topic}), 0);
  EXPECT_EQ(binder.size(), 1);
  EXPECT_TRUE(plot.curveList().empty());

  ASSERT_NE(addScalarTopic(app_session, dataset_id, y_path.topic.toStdString()), 0U);
  EXPECT_EQ(binder.flush(QSet<QString>{y_path.topic}), 1);
  EXPECT_TRUE(binder.empty());
  EXPECT_EQ(plot.curveList().size(), 1U);

  EXPECT_EQ(binder.flush({}), 0);
  EXPECT_EQ(plot.curveList().size(), 1U);
}

// ---------- strict reject-ambiguity resolver -------------------------------

TEST(ResolveSeriesPathTest, UnqualifiedUniquePathBinds) {
  PJ::AppSession app_session;
  const PJ::DatasetId only = createDataset(app_session);
  ASSERT_NE(addScalarTopic(app_session, only, "/speed"), 0U);

  const SeriesPath legacy{u"/speed"_s, u"value"_s};
  EXPECT_EQ(resolvedDataset(app_session.catalogModel(), legacy), only)
      << "a genuinely unqualified legacy path may use its sole candidate";
}

TEST(ResolveSeriesPathTest, UnqualifiedAmbiguousPathIsRejected) {
  PJ::AppSession app_session;
  auto& engine = app_session.sessionManager().dataEngine();
  const auto dataset_a = engine.createDataset(PJ::DatasetDescriptor{.source_name = "same.mcap"});
  const auto dataset_b = engine.createDataset(PJ::DatasetDescriptor{.source_name = "same.mcap"});
  ASSERT_TRUE(dataset_a.has_value());
  ASSERT_TRUE(dataset_b.has_value());
  ASSERT_NE(addScalarTopic(app_session, *dataset_a, "/speed"), 0U);
  ASSERT_NE(addScalarTopic(app_session, *dataset_b, "/speed"), 0U);

  const SeriesPath ambiguous{u"/speed"_s, u"value"_s};
  EXPECT_FALSE(PJ::resolveSeriesPath(app_session.catalogModel(), ambiguous).has_value())
      << "an unqualified duplicate path must stay unresolved instead of binding the first dataset";
}

TEST(ResolveSeriesPathTest, ExactDatasetHintPreventsDuplicatePathMisbind) {
  PJ::AppSession app_session;
  auto& engine = app_session.sessionManager().dataEngine();
  const auto dataset_a = engine.createDataset(PJ::DatasetDescriptor{.source_name = "same.mcap"});
  const auto dataset_b = engine.createDataset(PJ::DatasetDescriptor{.source_name = "same.mcap"});
  ASSERT_TRUE(dataset_a.has_value());
  ASSERT_TRUE(dataset_b.has_value());
  ASSERT_NE(addScalarTopic(app_session, *dataset_a, "/speed"), 0U);
  ASSERT_NE(addScalarTopic(app_session, *dataset_b, "/speed"), 0U);

  const SeriesPath exact{u"/speed"_s, u"value"_s, *dataset_b, u"same.mcap"_s};
  EXPECT_EQ(resolvedDataset(app_session.catalogModel(), exact), *dataset_b)
      << "the exact in-session id must win among same-named datasets";
}

TEST(ResolveSeriesPathTest, IntendedDatasetMissingSeriesNeverStealsSibling) {
  PJ::AppSession app_session;
  auto& engine = app_session.sessionManager().dataEngine();
  const auto dataset_a = engine.createDataset(PJ::DatasetDescriptor{.source_name = "same.mcap"});
  const auto dataset_b = engine.createDataset(PJ::DatasetDescriptor{.source_name = "same.mcap"});
  ASSERT_TRUE(dataset_a.has_value());
  ASSERT_TRUE(dataset_b.has_value());
  // Only dataset A has /speed; the saved curve names dataset B exactly.
  ASSERT_NE(addScalarTopic(app_session, *dataset_a, "/speed"), 0U);

  const SeriesPath saved_b{u"/speed"_s, u"value"_s, *dataset_b, u"same.mcap"_s};
  EXPECT_FALSE(PJ::resolveSeriesPath(app_session.catalogModel(), saved_b).has_value())
      << "the intended dataset exists but lacks the series - never steal dataset A's field";
}

// FIX B: a data-processor recipe saved from dataset 2 must restore its input onto
// dataset 2, never dataset 1's same-named field. restoreDataProcessors resolves the
// input through the same resolveSeriesPath that a plotted curve uses, keyed by the
// saved input_dataset_id/source; this pins the three cases MainWindow relies on.
TEST(ResolveSeriesPathTest, ProcessorInputQualifierBindsToItsOwnDataset) {
  PJ::AppSession app_session;
  auto& engine = app_session.sessionManager().dataEngine();
  const auto dataset1 = engine.createDataset(PJ::DatasetDescriptor{.source_name = "run1.mcap"});
  const auto dataset2 = engine.createDataset(PJ::DatasetDescriptor{.source_name = "run2.mcap"});
  ASSERT_TRUE(dataset1.has_value());
  ASSERT_TRUE(dataset2.has_value());
  ASSERT_NE(addScalarTopic(app_session, *dataset1, "/imu"), 0U);
  ASSERT_NE(addScalarTopic(app_session, *dataset2, "/imu"), 0U);

  // Recipe saved from dataset 2 restores onto dataset 2 (not dataset 1's /imu).
  const SeriesPath saved_input2{u"/imu"_s, u"value"_s, *dataset2, u"run2.mcap"_s};
  EXPECT_EQ(resolvedDataset(app_session.catalogModel(), saved_input2), *dataset2)
      << "the processor input qualifier must bind dataset 2, not dataset 1's same-topic field";

  // Legacy (qualifiers stripped) with BOTH datasets loaded: ambiguous -> no restore.
  const SeriesPath legacy{u"/imu"_s, u"value"_s};
  EXPECT_FALSE(PJ::resolveSeriesPath(app_session.catalogModel(), legacy).has_value())
      << "a legacy unqualified processor input must not first-match onto either dataset";
}

TEST(ResolveSeriesPathTest, ProcessorInputLegacyUnqualifiedBindsWhenSingleDataset) {
  PJ::AppSession app_session;
  const PJ::DatasetId only = createDataset(app_session);
  ASSERT_NE(addScalarTopic(app_session, only, "/imu"), 0U);

  // With only one dataset present the unqualified input has a unique candidate,
  // so a legacy layout still restores its filter.
  const SeriesPath legacy{u"/imu"_s, u"value"_s};
  EXPECT_EQ(resolvedDataset(app_session.catalogModel(), legacy), only)
      << "an unqualified processor input binds when it is the sole candidate";
}

// FIX D: a dataset whose source file is deleted from disk after loading must still
// resolve by its full path. isSamePath()/canonicalFilePath() return empty for a
// missing file, so both resolveSeriesPath's path scan and SessionManager's
// path_matches now short-circuit on normalized-string equality first.
TEST(ResolveSeriesPathTest, DeletedSourceFileStillResolvesByPath) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession app_session;
  auto& session = app_session.sessionManager();
  auto& engine = session.dataEngine();
  // Two same-basename datasets so only the full path disambiguates; the exact id
  // is deliberately reminted (points at the wrong dataset) to force the path leg.
  const auto reminted_collision = engine.createDataset(PJ::DatasetDescriptor{.source_name = "run.mcap"});
  const auto intended = engine.createDataset(PJ::DatasetDescriptor{.source_name = "run.mcap"});
  ASSERT_TRUE(reminted_collision.has_value());
  ASSERT_TRUE(intended.has_value());

  const QString intended_path = dir.filePath(u"wanted/run.mcap"_s);
  ASSERT_TRUE(QDir(dir.path()).mkpath(u"wanted"_s));
  {
    QFile file(intended_path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("mcap");
  }
  // Register the path WHILE the file exists (stored as its canonical form), then
  // delete it — the pre-fix canonical-based comparison would now fail to match.
  session.setDatasetSourcePath(*intended, intended_path);
  session.setDatasetSourcePath(*reminted_collision, dir.filePath(u"other/run.mcap"_s));
  ASSERT_TRUE(QFile::remove(intended_path));
  ASSERT_FALSE(QFileInfo::exists(intended_path));

  ASSERT_NE(addScalarTopic(app_session, *reminted_collision, "/speed"), 0U);
  ASSERT_NE(addScalarTopic(app_session, *intended, "/speed"), 0U);

  const SeriesPath by_path{u"/speed"_s, u"value"_s, *reminted_collision, u"run.mcap"_s, intended_path};
  EXPECT_EQ(resolvedDataset(app_session.catalogModel(), by_path), *intended)
      << "a deleted-on-disk source must still resolve by its registered full path";
}

// FIX D, binder path-scan leg: after a fan-out reload the saved raw source label no
// longer names any dataset, so resolveDatasetIdentity returns nullopt and
// resolveSeriesPath falls to its own physical-path scan. That scan used isSamePath,
// which fails on a deleted file; the normalized-equality short-circuit rescues it.
TEST(ResolveSeriesPathTest, DeletedSourceFileResolvesThroughBinderPathScan) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession app_session;
  auto& session = app_session.sessionManager();
  auto& engine = session.dataEngine();
  // Both datasets carry NEW fan-out labels; the saved label ("old_run.mcap") is gone.
  const auto sibling = engine.createDataset(PJ::DatasetDescriptor{.source_name = "run_0.mcap"});
  const auto intended = engine.createDataset(PJ::DatasetDescriptor{.source_name = "run_1.mcap"});
  ASSERT_TRUE(sibling.has_value());
  ASSERT_TRUE(intended.has_value());

  const QString intended_path = dir.filePath(u"wanted/run.mcap"_s);
  ASSERT_TRUE(QDir(dir.path()).mkpath(u"wanted"_s));
  {
    QFile file(intended_path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("mcap");
  }
  session.setDatasetSourcePath(*intended, intended_path);
  session.setDatasetSourcePath(*sibling, dir.filePath(u"other/run.mcap"_s));
  ASSERT_TRUE(QFile::remove(intended_path));

  ASSERT_NE(addScalarTopic(app_session, *sibling, "/speed"), 0U);
  ASSERT_NE(addScalarTopic(app_session, *intended, "/speed"), 0U);

  // id=0 (stripped), source no longer exists -> identity resolver returns nullopt,
  // exercising resolveSeriesPath's own path-scan leg, which must match by path.
  const SeriesPath by_path{u"/speed"_s, u"value"_s, 0, u"old_run.mcap"_s, intended_path};
  EXPECT_EQ(resolvedDataset(app_session.catalogModel(), by_path), *intended)
      << "the binder path scan must match a deleted-on-disk source by normalized-path equality";
}

TEST(ResolveSeriesPathTest, RemintedIdFallsBackToUniqueRawSourceIdentity) {
  PJ::AppSession app_session;
  auto& engine = app_session.sessionManager().dataEngine();
  const auto reminted_collision = engine.createDataset(PJ::DatasetDescriptor{.source_name = "other.mcap"});
  const auto intended = engine.createDataset(PJ::DatasetDescriptor{.source_name = "logs/drive.mcap"});
  ASSERT_TRUE(reminted_collision.has_value());
  ASSERT_TRUE(intended.has_value());
  ASSERT_NE(addScalarTopic(app_session, *reminted_collision, "/speed"), 0U);
  ASSERT_NE(addScalarTopic(app_session, *intended, "/speed"), 0U);

  // The UI label is deliberately unrelated to the raw source identity. The saved
  // id now names other.mcap (as can happen in a fresh session), so the resolver
  // must reject that collision and find the single raw-source match.
  app_session.catalogModel().setDatasetDisplayName(*intended, u"Pretty recording"_s);
  const SeriesPath portable{u"/speed"_s, u"value"_s, *reminted_collision, u"logs/drive.mcap"_s};
  EXPECT_EQ(resolvedDataset(app_session.catalogModel(), portable), *intended);
}

TEST(ResolveSeriesPathTest, DuplicateRawSourceFallbackIsAmbiguousBeforePathLookup) {
  PJ::AppSession app_session;
  auto& engine = app_session.sessionManager().dataEngine();
  const auto first = engine.createDataset(PJ::DatasetDescriptor{.source_name = "same.mcap"});
  const auto second = engine.createDataset(PJ::DatasetDescriptor{.source_name = "same.mcap"});
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  ASSERT_NE(addScalarTopic(app_session, *first, "/speed"), 0U);

  // External XML strips the unvalidated id (id=0); its duplicate source-only
  // fallback must remain unresolved rather than pick the first named match.
  const SeriesPath portable{u"/speed"_s, u"value"_s, 0, u"same.mcap"_s};
  EXPECT_FALSE(PJ::resolveSeriesPath(app_session.catalogModel(), portable).has_value());
}

TEST(ResolveSeriesPathTest, QualifiedIdentityNeverFallsBackToAnUnrelatedUniquePath) {
  PJ::AppSession app_session;
  auto& engine = app_session.sessionManager().dataEngine();
  const auto unrelated = engine.createDataset(PJ::DatasetDescriptor{.source_name = "other.mcap"});
  ASSERT_TRUE(unrelated.has_value());
  ASSERT_NE(addScalarTopic(app_session, *unrelated, "/speed"), 0U);

  const SeriesPath missing_source{u"/speed"_s, u"value"_s, 999, u"missing.mcap"_s};
  EXPECT_FALSE(PJ::resolveSeriesPath(app_session.catalogModel(), missing_source).has_value())
      << "a qualified identity whose source/path no longer exists must not steal an unrelated unique candidate";

  const SeriesPath stale_id_without_source{u"/speed"_s, u"value"_s, 999, QString()};
  EXPECT_FALSE(PJ::resolveSeriesPath(app_session.catalogModel(), stale_id_without_source).has_value())
      << "a bare stale id (no portable qualifier) resolves to nothing once reminted";

  const SeriesPath legacy_unqualified{u"/speed"_s, u"value"_s};
  EXPECT_EQ(resolvedDataset(app_session.catalogModel(), legacy_unqualified), *unrelated)
      << "a genuinely unqualified legacy path may still use its sole candidate";
}

TEST(ResolveSeriesPathTest, FullPathOverridesRemintedCollisionForTimeSeriesAndBothXyAxes) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession app_session;
  auto& session = app_session.sessionManager();
  auto& engine = session.dataEngine();
  const auto reminted_collision = engine.createDataset(PJ::DatasetDescriptor{.source_name = "run.mcap"});
  const auto intended = engine.createDataset(PJ::DatasetDescriptor{.source_name = "run.mcap"});
  ASSERT_TRUE(reminted_collision.has_value());
  ASSERT_TRUE(intended.has_value());
  const QString collision_path = dir.filePath(u"other/run.mcap"_s);
  const QString intended_path = dir.filePath(u"wanted/run.mcap"_s);
  session.setDatasetSourcePath(*reminted_collision, collision_path);
  session.setDatasetSourcePath(*intended, intended_path);

  ASSERT_NE(addScalarTopic(app_session, *reminted_collision, "/x"), 0U);
  ASSERT_NE(addScalarTopic(app_session, *reminted_collision, "/y"), 0U);
  ASSERT_NE(addScalarTopic(app_session, *intended, "/x"), 0U);
  ASSERT_NE(addScalarTopic(app_session, *intended, "/y"), 0U);

  const SeriesPath x{u"/x"_s, u"value"_s, *reminted_collision, u"run.mcap"_s, intended_path};
  const SeriesPath y{u"/y"_s, u"value"_s, *reminted_collision, u"run.mcap"_s, intended_path};
  EXPECT_EQ(resolvedDataset(app_session.catalogModel(), x), *intended);

  QDomDocument doc;
  QDomElement plot = addPlot(doc, u"xy"_s, u"XYPlot"_s);
  QDomElement curve = addXyCurve(doc, plot, x, y);
  curve.setAttribute(u"name"_s, u"trajectory"_s);
  rebindAgainstCatalog(doc, app_session.catalogModel());
  const auto x_descriptor = app_session.catalogModel().curveDescriptor(curve.attribute(u"curve_x"_s));
  const auto y_descriptor = app_session.catalogModel().curveDescriptor(curve.attribute(u"curve_y"_s));
  ASSERT_TRUE(x_descriptor.has_value());
  ASSERT_TRUE(y_descriptor.has_value());
  EXPECT_EQ(x_descriptor->dataset_id, *intended);
  EXPECT_EQ(y_descriptor->dataset_id, *intended);
}

TEST(PendingDisplayBinderTest, LateDuplicatePathBindsToSavedDatasetId) {
  QTemporaryDir extensions_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  PJ::AppSession app_session(extensions_dir.path());
  auto& engine = app_session.sessionManager().dataEngine();
  const auto dataset_a = engine.createDataset(PJ::DatasetDescriptor{.source_name = "same.mcap"});
  const auto dataset_b = engine.createDataset(PJ::DatasetDescriptor{.source_name = "same.mcap"});
  ASSERT_TRUE(dataset_a.has_value());
  ASSERT_TRUE(dataset_b.has_value());
  ASSERT_NE(addScalarTopic(app_session, *dataset_a, "/speed"), 0U);

  // The saved curve names dataset B, which has not materialized /speed yet, so
  // the entry stays pending instead of stealing dataset A's field.
  const SeriesPath saved{u"/speed"_s, u"value"_s, *dataset_b, u"same.mcap"_s};
  QDomDocument doc;
  QDomElement plot_element = addPlot(doc, u"plot_ts"_s, u"TimeSeries"_s);
  addTimeSeriesCurve(doc, plot_element, saved);
  rebindAgainstCatalog(doc, app_session.catalogModel());

  PJ::PlotWidget plot(&app_session.sessionManager(), &app_session.catalogModel());
  ASSERT_TRUE(plot.xmlLoadState(plot_element));
  ASSERT_TRUE(plot.curveList().empty());

  PJ::PendingDisplayBinder binder(app_session.catalogModel());
  binder.collect(doc, indexByStateId(plot));
  ASSERT_EQ(binder.size(), 1);

  ASSERT_NE(addScalarTopic(app_session, *dataset_b, "/speed"), 0U);
  EXPECT_EQ(binder.flush(QSet<QString>{u"/speed"_s}), 1);
  ASSERT_EQ(plot.curveList().size(), 1U);
  const auto descriptor = app_session.catalogModel().curveDescriptor(plot.curveList().front().source_name);
  ASSERT_TRUE(descriptor.has_value());
  EXPECT_EQ(descriptor->dataset_id, *dataset_b) << "the late duplicate path binds its saved dataset, not the sibling";
}

// Minimal concrete SceneDockWidget: the binder only needs a live target.
class BinderStubSceneDock : public PJ::SceneDockWidget {
 public:
  [[nodiscard]] int configConsumeAttempts() const {
    return config_consume_attempts_;
  }

 protected:
  QWidget* createSceneView() override {
    return new QWidget();
  }
  std::unique_ptr<PJ::SceneLayerContext> makeContext() override {
    return std::make_unique<PJ::SceneLayerContext>();
  }
  [[nodiscard]] bool acceptsObjectType(PJ::sdk::BuiltinObjectType /*object_type*/) const override {
    return true;
  }
  bool handleSceneConfigTopic(
      PJ::ObjectTopicId /*topic_id*/, PJ::sdk::BuiltinObjectType /*object_type*/, const QString& /*title*/) override {
    ++config_consume_attempts_;
    return true;
  }
  void syncViewLayers(const std::vector<PJ::ISceneLayer*>& /*ordered_layers*/) override {}

 private:
  int config_consume_attempts_ = 0;
};

TEST(PendingDisplayBinderTest, SceneDemandReferenceCannotMaterializeIntentByItself) {
  PJ::AppSession app_session;
  const PJ::DatasetId dataset_id = createDataset(app_session);
  PJ::PendingDisplayBinder binder(app_session.catalogModel());
  BinderStubSceneDock dock;
  dock.setSessionManager(&app_session.sessionManager());
  ASSERT_TRUE(
      dock.deferTopicIntent(dataset_id, u"/scene_topic"_s, PJ::sdk::BuiltinObjectType::kPointCloud, u"scene_topic"_s));
  binder.addPendingSceneLayer(&dock, u"/scene_topic"_s, dataset_id);
  ASSERT_EQ(binder.size(), 1);

  const auto object_topic = app_session.sessionManager().objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = dataset_id,
          .topic_name = "/scene_topic",
          .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
      });
  ASSERT_TRUE(object_topic.has_value()) << object_topic.error();
  app_session.catalogModel().rebuildFromDatastore();

  static_cast<void>(binder.flush(QSet<QString>{u"/scene_topic"_s}));
  EXPECT_EQ(dock.configConsumeAttempts(), 0) << "the binder must never call the dock's addTopic path";
  EXPECT_EQ(binder.size(), 1) << "only dock-queue completion may release the scene demand reference";
}

TEST(PendingDisplayBinderTest, SceneDemandRefsRebuildFromDockQueueAndReleaseOnCompletion) {
  PJ::AppSession app_session;
  const PJ::DatasetId dataset_id = createDataset(app_session);
  app_session.catalogModel().setAdvertisedTopics(
      dataset_id, {PJ::AdvertisedTopic{u"/scene_topic"_s, PJ::sdk::BuiltinObjectType::kPointCloud}});
  PJ::TopicDemandTracker tracker;
  PJ::PendingDisplayBinder binder(app_session.catalogModel(), &tracker);
  BinderStubSceneDock dock;
  dock.setSessionManager(&app_session.sessionManager());
  ASSERT_TRUE(
      dock.deferTopicIntent(dataset_id, u"/scene_topic"_s, PJ::sdk::BuiltinObjectType::kPointCloud, u"scene_topic"_s));
  binder.addPendingSceneLayer(&dock, u"/scene_topic"_s, dataset_id);
  ASSERT_EQ(binder.size(), 1);
  const auto is_referenced = [&tracker, dataset_id]() {
    const auto active = tracker.activeTopics(dataset_id);
    return std::find(active.cbegin(), active.cend(), u"/scene_topic"_s) != active.cend();
  };
  EXPECT_TRUE(is_referenced());

  // collect rebuilds the binder from authoritative widget-owned state.
  QDomDocument doc;
  binder.collect(doc, {});
  EXPECT_FALSE(is_referenced());
  binder.addPendingSceneLayer(&dock, u"/scene_topic"_s, dataset_id);
  EXPECT_TRUE(is_referenced());

  dock.clearPendingRestores();
  EXPECT_EQ(binder.flush({}), 1);
  EXPECT_FALSE(is_referenced());
}
