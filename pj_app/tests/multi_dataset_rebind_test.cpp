// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Two datasets loaded side by side (comparing two runs of the same robot)
// usually share topic names. A layout / undo-redo snapshot identifies each
// curve by its stable (topic, field) path plus dataset qualifiers (dataset_id
// + dataset_source, written by PlotWidget::xmlSaveState), and the restore path
// (MainWindow::rebindCurvesToLoadedDatasets -> layout_xml::rebindCurveKeys ->
// PJ::resolveSeriesPath) re-resolves that qualified path against the loaded
// datasets. Rebinding must keep every curve on the dataset it was plotted
// from — not collapse all of them onto the first dataset in load order,
// which silently swaps the plotted data (and its display offset) on every
// undo/redo — and must reject ambiguity rather than guess when the saved
// identity no longer pins down exactly one dataset.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QDomElement>
#include <QString>
#include <QtGlobal>

#include "LayoutXml.h"
#include "PendingDisplayBinder.h"
#include "dataset_test_helpers.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
using namespace Qt::StringLiterals;

namespace {

using PJ::layout_xml::SeriesPath;
using pj_test::addScalarTopic;
using pj_test::createDataset;

// Concrete catalog key of `topic` (a plain scalar series, whose single column
// materializes as field "value") within one specific dataset.
QString keyInDataset(PJ::CatalogModel& catalog, PJ::DatasetId dataset_id, const QString& topic) {
  const auto descriptor = catalog.descriptorForPath(dataset_id, topic, u"value"_s);
  return descriptor.has_value() ? descriptor->name : QString();
}

// Runs the snapshot-restore rebind exactly like MainWindow::rebindCurvesToLoadedDatasets.
QList<SeriesPath> rebind(QDomDocument& doc, PJ::CatalogModel& catalog) {
  return PJ::layout_xml::rebindCurveKeys(
      doc, [&catalog](const SeriesPath& path) { return PJ::resolveSeriesPath(catalog, path); });
}

// Serializes `plot` into a fresh snapshot document, like MainWindow::xmlSaveState.
QDomDocument saveSnapshot(const PJ::PlotWidget& plot) {
  QDomDocument doc;
  QDomElement root = doc.createElement(u"root"_s);
  doc.appendChild(root);
  root.appendChild(plot.xmlSaveState(doc));
  return doc;
}

QDomElement firstCurve(const QDomDocument& doc) {
  return doc.documentElement().firstChildElement(u"plot"_s).firstChildElement(u"curve"_s);
}

}  // namespace

// The undo/redo reproduction: a curve plotted from the SECOND dataset, saved
// and rebound while a first dataset with the same topic is also loaded, must
// resolve back to the second dataset's key. xmlSaveState stamps dataset_id +
// dataset_source (the raw source label) on the curve, so the exact-id branch
// of resolveSeriesPath pins the rebind to dataset 2.
TEST(MultiDatasetRebind, TimeSeriesCurveStaysOnItsDataset) {
  PJ::AppSession app_session;
  const PJ::DatasetId run1 = createDataset(app_session, "run1");
  const PJ::DatasetId run2 = createDataset(app_session, "run2");
  addScalarTopic(app_session, run1, "/imu/accel");
  addScalarTopic(app_session, run2, "/imu/accel");

  PJ::CatalogModel& catalog = app_session.catalogModel();
  const QString run2_key = keyInDataset(catalog, run2, u"/imu/accel"_s);
  ASSERT_FALSE(run2_key.isEmpty());
  ASSERT_NE(run2_key, keyInDataset(catalog, run1, u"/imu/accel"_s));

  PJ::PlotWidget plot(&app_session.sessionManager(), &catalog);
  ASSERT_NE(plot.addCurve(run2_key), nullptr);

  QDomDocument doc = saveSnapshot(plot);
  const QDomElement saved_curve = firstCurve(doc);
  ASSERT_FALSE(saved_curve.isNull());
  EXPECT_EQ(saved_curve.attribute(u"dataset_id"_s).toUInt(), run2);
  EXPECT_EQ(saved_curve.attribute(u"dataset_source"_s), u"run2"_s);

  EXPECT_TRUE(rebind(doc, catalog).isEmpty());

  const QDomElement curve = firstCurve(doc);
  ASSERT_FALSE(curve.isNull());
  EXPECT_EQ(curve.attribute(u"name"_s), run2_key);
}

// Same guarantee for an XY curve: both axis series were picked from the second
// dataset and must rebind there, each axis carrying its own x_/y_ qualifiers.
TEST(MultiDatasetRebind, XyCurveStaysOnItsDataset) {
  PJ::AppSession app_session;
  const PJ::DatasetId run1 = createDataset(app_session, "run1");
  const PJ::DatasetId run2 = createDataset(app_session, "run2");
  for (const PJ::DatasetId id : {run1, run2}) {
    addScalarTopic(app_session, id, "/gps/x");
    addScalarTopic(app_session, id, "/gps/y");
  }

  PJ::CatalogModel& catalog = app_session.catalogModel();
  const QString x_key = keyInDataset(catalog, run2, u"/gps/x"_s);
  const QString y_key = keyInDataset(catalog, run2, u"/gps/y"_s);
  ASSERT_FALSE(x_key.isEmpty());
  ASSERT_FALSE(y_key.isEmpty());

  PJ::PlotWidget plot(&app_session.sessionManager(), &catalog);
  ASSERT_NE(plot.addCurveXY(x_key, y_key, u"trajectory"_s), nullptr);

  QDomDocument doc = saveSnapshot(plot);
  const QDomElement saved_curve = firstCurve(doc);
  ASSERT_FALSE(saved_curve.isNull());
  EXPECT_EQ(saved_curve.attribute(u"x_dataset_id"_s).toUInt(), run2);
  EXPECT_EQ(saved_curve.attribute(u"x_dataset_source"_s), u"run2"_s);
  EXPECT_EQ(saved_curve.attribute(u"y_dataset_id"_s).toUInt(), run2);
  EXPECT_EQ(saved_curve.attribute(u"y_dataset_source"_s), u"run2"_s);

  EXPECT_TRUE(rebind(doc, catalog).isEmpty());

  const QDomElement curve = firstCurve(doc);
  ASSERT_FALSE(curve.isNull());
  EXPECT_EQ(curve.attribute(u"curve_x"_s), x_key);
  EXPECT_EQ(curve.attribute(u"curve_y"_s), y_key);
}

// A qualified curve whose saved dataset id is gone (e.g. the layout was
// reopened in a fresh session) must still resolve when the raw source label
// (dataset_source) names exactly ONE loaded dataset — the source-fallback leg
// of resolveSeriesPath / SessionManager::resolveDatasetIdentity.
TEST(MultiDatasetRebind, GoneDatasetFallsBackToUniqueSameSourceCandidate) {
  PJ::AppSession app_session;
  const PJ::DatasetId run2 = createDataset(app_session, "run2");
  addScalarTopic(app_session, run2, "/imu/accel");

  PJ::CatalogModel& catalog = app_session.catalogModel();
  const QString run2_key = keyInDataset(catalog, run2, u"/imu/accel"_s);
  ASSERT_FALSE(run2_key.isEmpty());

  PJ::PlotWidget plot(&app_session.sessionManager(), &catalog);
  ASSERT_NE(plot.addCurve(run2_key), nullptr);
  QDomDocument doc = saveSnapshot(plot);

  // Rebind against a DIFFERENT session where the saved DatasetId no longer
  // exists, but exactly one loaded dataset shares the saved source label.
  PJ::AppSession other_session;
  const PJ::DatasetId other_run = createDataset(other_session, "run2");
  addScalarTopic(other_session, other_run, "/imu/accel");
  PJ::CatalogModel& other_catalog = other_session.catalogModel();

  EXPECT_TRUE(rebind(doc, other_catalog).isEmpty());
  const QDomElement curve = firstCurve(doc);
  EXPECT_EQ(curve.attribute(u"name"_s), keyInDataset(other_catalog, other_run, u"/imu/accel"_s));
}

// Same scenario, but the target session has TWO datasets sharing the saved
// source label. The source-fallback must reject the ambiguity rather than
// pick one by load order: rebindCurveKeys clears the curve's key and reports
// it unresolved.
TEST(MultiDatasetRebind, GoneDatasetWithTwoSameSourceCandidatesStaysUnresolved) {
  PJ::AppSession app_session;
  const PJ::DatasetId run2 = createDataset(app_session, "run2");
  addScalarTopic(app_session, run2, "/imu/accel");

  PJ::CatalogModel& catalog = app_session.catalogModel();
  const QString run2_key = keyInDataset(catalog, run2, u"/imu/accel"_s);
  ASSERT_FALSE(run2_key.isEmpty());

  PJ::PlotWidget plot(&app_session.sessionManager(), &catalog);
  ASSERT_NE(plot.addCurve(run2_key), nullptr);
  QDomDocument doc = saveSnapshot(plot);
  const QDomElement saved_curve = firstCurve(doc);
  ASSERT_EQ(saved_curve.attribute(u"dataset_source"_s), u"run2"_s);

  // Target session: TWO datasets both named "run2" (a same-named reload/fan-out).
  // A leading decoy dataset with a DIFFERENT source name absorbs the saved
  // dataset_id (a fresh DataEngine mints ids from 1), so the saved identity's
  // exact-id branch genuinely misses (source label mismatch) and resolution
  // must fall through to the source-only scan across BOTH same-source datasets.
  PJ::AppSession other_session;
  createDataset(other_session, "unrelated.mcap");
  const PJ::DatasetId other_a = createDataset(other_session, "run2");
  const PJ::DatasetId other_b = createDataset(other_session, "run2");
  addScalarTopic(other_session, other_a, "/imu/accel");
  addScalarTopic(other_session, other_b, "/imu/accel");
  PJ::CatalogModel& other_catalog = other_session.catalogModel();

  const QList<SeriesPath> unresolved = rebind(doc, other_catalog);
  ASSERT_EQ(unresolved.size(), 1);
  EXPECT_EQ(unresolved.front().topic, u"/imu/accel"_s);

  const QDomElement curve = firstCurve(doc);
  ASSERT_FALSE(curve.isNull());
  EXPECT_TRUE(curve.attribute(u"name"_s).isEmpty())
      << "ambiguous source fallback must clear the key rather than guess by load order";
}

// A fully unqualified curve (no dataset hint at all — an old/generic layout)
// binds when the topic+field is unique across the loaded datasets.
TEST(MultiDatasetRebind, UnqualifiedCurveBindsWhenTopicIsUnique) {
  PJ::AppSession app_session;
  const PJ::DatasetId only = createDataset(app_session, "run1");
  addScalarTopic(app_session, only, "/imu/accel");
  PJ::CatalogModel& catalog = app_session.catalogModel();

  QDomDocument doc;
  QDomElement root = doc.createElement(u"root"_s);
  doc.appendChild(root);
  QDomElement plot_element = doc.createElement(u"plot"_s);
  root.appendChild(plot_element);
  QDomElement curve = doc.createElement(u"curve"_s);
  curve.setAttribute(u"topic"_s, u"/imu/accel"_s);
  curve.setAttribute(u"field"_s, u"value"_s);
  plot_element.appendChild(curve);

  EXPECT_TRUE(rebind(doc, catalog).isEmpty());
  const QDomElement rebound = firstCurve(doc);
  EXPECT_EQ(rebound.attribute(u"name"_s), keyInDataset(catalog, only, u"/imu/accel"_s));
}

// Same fully-unqualified curve, but two datasets provide the topic: strict
// semantics reject the ambiguity instead of collapsing onto load order.
TEST(MultiDatasetRebind, UnqualifiedCurveStaysUnresolvedWhenTopicIsAmbiguous) {
  PJ::AppSession app_session;
  const PJ::DatasetId run1 = createDataset(app_session, "run1");
  const PJ::DatasetId run2 = createDataset(app_session, "run2");
  addScalarTopic(app_session, run1, "/imu/accel");
  addScalarTopic(app_session, run2, "/imu/accel");
  PJ::CatalogModel& catalog = app_session.catalogModel();

  QDomDocument doc;
  QDomElement root = doc.createElement(u"root"_s);
  doc.appendChild(root);
  QDomElement plot_element = doc.createElement(u"plot"_s);
  root.appendChild(plot_element);
  QDomElement curve = doc.createElement(u"curve"_s);
  curve.setAttribute(u"topic"_s, u"/imu/accel"_s);
  curve.setAttribute(u"field"_s, u"value"_s);
  plot_element.appendChild(curve);

  const QList<SeriesPath> unresolved = rebind(doc, catalog);
  ASSERT_EQ(unresolved.size(), 1);
  EXPECT_EQ(unresolved.front().topic, u"/imu/accel"_s);
  const QDomElement rebound = firstCurve(doc);
  EXPECT_TRUE(rebound.attribute(u"name"_s).isEmpty());
}
