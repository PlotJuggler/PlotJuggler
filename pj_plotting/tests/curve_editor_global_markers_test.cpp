// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The CurveEditor's per-dataset marker-scope pseudo-rows: appended after a
// plot's curve rows, up to two per dataset (Dataset then Global, one per
// marker scope whose object topic exists on this plot), each carrying only a
// markers toggle (no swatch/eye/trash) and driving
// PlotWidget::setDatasetMarkerScopeVisible directly.

#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QToolButton>
#include <QtGlobal>
#include <string_view>

#include "pj_base/builtin/plot_markers.hpp"
#include "pj_base/builtin/plot_markers_codec.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plotting/CurveEditor.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/CatalogModel.h"
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

QString keyForTopic(PJ::CatalogModel& catalog, PJ::TopicId topic_id) {
  for (const auto& curve : catalog.curves()) {
    if (const auto descriptor = catalog.curveDescriptor(curve.name); descriptor && descriptor->topic_id == topic_id) {
      return curve.name;
    }
  }
  return {};
}

// Registers `scope`'s bare marker object topic where the overlay reads it for
// `dataset_id` (the dataset itself, or the shared dataset for the ALL-DATASETS
// scope) the same way MarkerService::publishMarkerSet does (that method is
// private; this mirrors its ObjectStore calls) and notifies the plot overlay.
void publishScopeTopic(PJ::SessionManager& session, PJ::DatasetId dataset_id, PJ::MarkerScope scope) {
  const std::string object_topic = PJ::sdk::markerObjectTopicName(PJ::markerScopeTopic(scope));
  const auto registered = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = PJ::markerScopeDataset(scope, dataset_id), .topic_name = object_topic, .metadata_json = {}});
  EXPECT_TRUE(registered.has_value()) << registered.error();
  if (!registered.has_value()) {
    return;
  }
  EXPECT_TRUE(session.objectStore()
                  .pushOwned(*registered, PJ::Timestamp{0}, PJ::serializePlotMarkers(PJ::sdk::PlotMarkers{}))
                  .has_value());
  session.notifyMarkersChanged();
}

QList<QWidget*> footerRows(QWidget* footer) {
  return footer->findChildren<QWidget*>(u"markerScopeRow"_s, Qt::FindDirectChildrenOnly);
}

// A footer row's label (the ElidingLabel's tooltip carries the full text).
QString rowLabel(QWidget* row) {
  auto* name_label = row->findChild<QLabel*>(u"curveNameLabel"_s);
  return name_label != nullptr ? name_label->toolTip() : QString();
}

}  // namespace

// With a curve but no marker topic, only the curve row shows and the footer
// is hidden; once the Dataset-scope topic is registered, the footer shows one
// row (markers toggle only) tagged "dataset", while the list itself keeps
// just the curve.
TEST(CurveEditorGlobalMarkers, AppendsOneRowPerDatasetAfterCurves) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key_a = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/x"));
  const QString key_b = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/y"));
  ASSERT_FALSE(key_a.isEmpty());
  ASSERT_FALSE(key_b.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  ASSERT_NE(plot.addCurve(key_a), nullptr);
  ASSERT_NE(plot.addCurve(key_b), nullptr);

  PJ::CurveEditor editor;
  editor.setPlot(&plot);
  auto* list = editor.findChild<QListWidget*>(u"listWidget"_s);
  ASSERT_NE(list, nullptr);

  auto* footer = editor.findChild<QWidget*>(u"markerScopesFooter"_s);
  ASSERT_NE(footer, nullptr);

  // No marker topic yet: only the two curve rows, and no footer.
  EXPECT_EQ(list->count(), 2);
  EXPECT_TRUE(footer->isHidden());
  EXPECT_TRUE(footerRows(footer).isEmpty());

  publishScopeTopic(session, *dataset, PJ::MarkerScope::kDataset);
  EXPECT_EQ(list->count(), 2);
  EXPECT_FALSE(footer->isHidden());
  const auto rows = footerRows(footer);
  ASSERT_EQ(rows.size(), 1);
  QWidget* row = rows.front();
  EXPECT_EQ(rowLabel(row), u"Dataset markers"_s);
  EXPECT_EQ(row->findChildren<QToolButton*>(u"curveMarkersToggle"_s).size(), 1);
  EXPECT_TRUE(row->findChildren<QToolButton*>(u"curveVisibilityToggle"_s).isEmpty());
  EXPECT_TRUE(row->findChildren<QToolButton*>(u"curveTrashToggle"_s).isEmpty());
}

// When both scope topics exist on a dataset, the footer carries exactly two
// rows in Dataset-then-Global order, each tagged with its own scope.
TEST(CurveEditorGlobalMarkers, AppendsDatasetThenGlobalRowPerDataset) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/x"));
  ASSERT_FALSE(key.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  ASSERT_NE(plot.addCurve(key), nullptr);
  publishScopeTopic(session, *dataset, PJ::MarkerScope::kDataset);
  publishScopeTopic(session, *dataset, PJ::MarkerScope::kAllDatasets);

  PJ::CurveEditor editor;
  editor.setPlot(&plot);
  auto* footer = editor.findChild<QWidget*>(u"markerScopesFooter"_s);
  ASSERT_NE(footer, nullptr);
  const auto rows = footerRows(footer);
  ASSERT_EQ(rows.size(), 2);
  EXPECT_EQ(rowLabel(rows[0]), u"Dataset markers"_s);
  EXPECT_EQ(rowLabel(rows[1]), u"Global markers"_s);
}

// With curves from two datasets, a scope row's label disambiguates by
// appending the dataset name; with only one dataset on the plot (the other
// tests) the plain scope label is used instead.
TEST(CurveEditorGlobalMarkers, RowLabelsCarryDatasetNameWithTwoDatasets) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset_a = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "a.mcap"});
  auto dataset_b = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "b.mcap"});
  ASSERT_TRUE(dataset_a.has_value()) << dataset_a.error();
  ASSERT_TRUE(dataset_b.has_value()) << dataset_b.error();
  const QString key_a = keyForTopic(catalog, addScalarTopic(session, *dataset_a, "/imu/x"));
  const QString key_b = keyForTopic(catalog, addScalarTopic(session, *dataset_b, "/imu/x"));
  ASSERT_FALSE(key_a.isEmpty());
  ASSERT_FALSE(key_b.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  ASSERT_NE(plot.addCurve(key_a), nullptr);
  ASSERT_NE(plot.addCurve(key_b), nullptr);
  publishScopeTopic(session, *dataset_a, PJ::MarkerScope::kDataset);

  PJ::CurveEditor editor;
  editor.setPlot(&plot);
  auto* footer = editor.findChild<QWidget*>(u"markerScopesFooter"_s);
  ASSERT_NE(footer, nullptr);
  const auto rows = footerRows(footer);
  ASSERT_EQ(rows.size(), 1);
  // QLabel has Q_OBJECT (ElidingLabel itself does not, so it must be looked up
  // through its Q_OBJECT base — see pj_plotting/CLAUDE.md).
  auto* name_label = rows.front()->findChild<QLabel*>(u"curveNameLabel"_s);
  ASSERT_NE(name_label, nullptr);
  const QString expected = QStringLiteral("Dataset markers — %1").arg(plot.datasetDisplayName(*dataset_a));
  EXPECT_EQ(name_label->toolTip(), expected);
}

// Clicking the Global row's toggle drives only the plot's kAllDatasets flag,
// leaving the same dataset's kDataset flag untouched; a plot-side change (the
// undo/redo path) fires datasetMarkerScopesChanged, which refresh() turns
// into a rebuilt, correctly-checked row.
TEST(CurveEditorGlobalMarkers, ToggleDrivesPlotFlagAndUndoRefreshes) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/x"));
  ASSERT_FALSE(key.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  ASSERT_NE(plot.addCurve(key), nullptr);
  publishScopeTopic(session, *dataset, PJ::MarkerScope::kDataset);
  publishScopeTopic(session, *dataset, PJ::MarkerScope::kAllDatasets);

  PJ::CurveEditor editor;
  editor.setPlot(&plot);
  auto* footer = editor.findChild<QWidget*>(u"markerScopesFooter"_s);
  ASSERT_NE(footer, nullptr);

  const auto rows = footerRows(footer);
  ASSERT_EQ(rows.size(), 2);
  QWidget* global_row = rows[1];  // Dataset row first, Global second
  const auto toggles = global_row->findChildren<QToolButton*>(u"curveMarkersToggle"_s);
  ASSERT_EQ(toggles.size(), 1);
  ASSERT_TRUE(toggles.front()->isChecked());  // default: visible
  toggles.front()->click();

  EXPECT_FALSE(plot.datasetMarkerScopeVisible(*dataset, PJ::MarkerScope::kAllDatasets));
  EXPECT_TRUE(plot.datasetMarkerScopeVisible(*dataset, PJ::MarkerScope::kDataset));  // untouched by the Global toggle

  plot.setDatasetMarkerScopeVisible(*dataset, PJ::MarkerScope::kAllDatasets, true);
  const auto rows_after = footerRows(footer);
  ASSERT_EQ(rows_after.size(), 2);
  const auto toggles_after = rows_after[1]->findChildren<QToolButton*>(u"curveMarkersToggle"_s);
  ASSERT_EQ(toggles_after.size(), 1);
  EXPECT_TRUE(toggles_after.front()->isChecked());
}
