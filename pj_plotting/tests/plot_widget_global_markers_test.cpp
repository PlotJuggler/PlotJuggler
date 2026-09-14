// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A plot's dataset-scoped marker overlays (kDataset = __global__, kAllDatasets =
// __all__) are gated independently of every curve's own show_markers flag and
// of each other: a per-(dataset, scope) visible/hidden state, default
// visible, toggled via PlotWidget::setDatasetMarkerScopeVisible (the
// CurveEditor's "Dataset markers" / "Global markers" pseudo-rows) and
// persisted as <marker_scope scope="dataset|global"> children of <plot>.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QtGlobal>
#include <algorithm>
#include <string_view>
#include <vector>

#include "pj_base/builtin/plot_markers.hpp"
#include "pj_base/builtin/plot_markers_codec.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plotting/PlotMarkersItem.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/PlotWidgetBase.h"
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

QString scopeTopicName(PJ::MarkerScope scope) {
  const std::string_view topic = PJ::markerScopeTopic(scope);
  return QString::fromUtf8(topic.data(), static_cast<qsizetype>(topic.size()));
}

// Registers `scope`'s marker object topic on `dataset_id` the same way
// MarkerService::publishMarkerSet does (that method is private; this mirrors
// its ObjectStore calls) and notifies the plot overlay.
void publishScopeTopic(PJ::SessionManager& session, PJ::DatasetId dataset_id, PJ::MarkerScope scope) {
  const std::string object_topic = PJ::sdk::markerObjectTopicName(PJ::markerScopeTopic(scope));
  const auto registered = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{.dataset_id = dataset_id, .topic_name = object_topic, .metadata_json = {}});
  EXPECT_TRUE(registered.has_value()) << registered.error();
  if (!registered.has_value()) {
    return;
  }
  EXPECT_TRUE(session.objectStore()
                  .pushOwned(*registered, PJ::Timestamp{0}, PJ::serializePlotMarkers(PJ::sdk::PlotMarkers{}))
                  .has_value());
  session.notifyMarkersChanged();
}

bool hasScopeTarget(const std::vector<PJ::MarkerTarget>& targets, PJ::DatasetId dataset_id, PJ::MarkerScope scope) {
  const QString topic = scopeTopicName(scope);
  return std::any_of(targets.begin(), targets.end(), [&](const PJ::MarkerTarget& target) {
    return target.dataset == dataset_id && target.topic == topic;
  });
}

bool hasSeriesTarget(const std::vector<PJ::MarkerTarget>& targets, PJ::DatasetId dataset_id) {
  const QString dataset_topic = scopeTopicName(PJ::MarkerScope::kDataset);
  const QString global_topic = scopeTopicName(PJ::MarkerScope::kAllDatasets);
  return std::any_of(targets.begin(), targets.end(), [&](const PJ::MarkerTarget& target) {
    return target.dataset == dataset_id && target.topic != dataset_topic && target.topic != global_topic;
  });
}

}  // namespace

// The kDataset-scope overlay target is gated solely by the per-dataset flag: a
// curve's own show_markers toggles only its series target.
TEST(PlotWidgetGlobalMarkers, GlobalTargetGatedByDatasetFlagOnly) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/x"));
  ASSERT_FALSE(key.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  ASSERT_NE(plot.addCurve(key), nullptr);

  EXPECT_TRUE(hasScopeTarget(plot.markerTargets(), *dataset, PJ::MarkerScope::kDataset));
  EXPECT_TRUE(hasSeriesTarget(plot.markerTargets(), *dataset));

  plot.setCurveShowMarkers(key, false);
  EXPECT_TRUE(hasScopeTarget(plot.markerTargets(), *dataset, PJ::MarkerScope::kDataset));
  EXPECT_FALSE(hasSeriesTarget(plot.markerTargets(), *dataset));

  plot.setDatasetMarkerScopeVisible(*dataset, PJ::MarkerScope::kDataset, false);
  EXPECT_FALSE(hasScopeTarget(plot.markerTargets(), *dataset, PJ::MarkerScope::kDataset));
  EXPECT_FALSE(hasSeriesTarget(plot.markerTargets(), *dataset));

  plot.setCurveShowMarkers(key, true);
  EXPECT_TRUE(hasSeriesTarget(plot.markerTargets(), *dataset));
  EXPECT_FALSE(hasScopeTarget(plot.markerTargets(), *dataset, PJ::MarkerScope::kDataset));
}

// setDatasetMarkerScopeVisible fires undoableChange on an actual change and is
// a no-op (no extra emission) when re-applying the same value.
TEST(PlotWidgetGlobalMarkers, ToggleIsUndoableAndIdempotent) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/x"));
  ASSERT_FALSE(key.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  ASSERT_NE(plot.addCurve(key), nullptr);

  int undo_count = 0;
  QObject::connect(&plot, &PJ::PlotWidget::undoableChange, [&]() { ++undo_count; });

  plot.setDatasetMarkerScopeVisible(*dataset, PJ::MarkerScope::kDataset, false);
  EXPECT_EQ(undo_count, 1);
  EXPECT_FALSE(plot.datasetMarkerScopeVisible(*dataset, PJ::MarkerScope::kDataset));

  plot.setDatasetMarkerScopeVisible(*dataset, PJ::MarkerScope::kDataset, false);
  EXPECT_EQ(undo_count, 1);  // repeating the same value is a no-op
}

// Only hidden (dataset, scope) pairs get a <marker_scope> child; loading it
// into a fresh plot in the same session restores the flag, and an absent
// child (dataset_b, never hidden) defaults to visible.
TEST(PlotWidgetGlobalMarkers, XmlRoundTripDefaultsToVisible) {
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
  plot.setDatasetMarkerScopeVisible(*dataset_a, PJ::MarkerScope::kDataset, false);

  QDomDocument doc;
  const QDomElement plot_element = plot.xmlSaveState(doc);
  const QDomElement marker_scope_element = plot_element.firstChildElement(u"marker_scope"_s);
  ASSERT_FALSE(marker_scope_element.isNull());
  EXPECT_EQ(marker_scope_element.attribute(u"dataset_id"_s).toUInt(), *dataset_a);
  EXPECT_EQ(marker_scope_element.attribute(u"scope"_s), u"dataset"_s);
  EXPECT_EQ(marker_scope_element.attribute(u"visible"_s), u"false"_s);
  // Only the hidden (dataset, scope) pair is written.
  EXPECT_TRUE(marker_scope_element.nextSiblingElement(u"marker_scope"_s).isNull());

  PJ::PlotWidget plot2(&session, &catalog);
  ASSERT_TRUE(plot2.xmlLoadState(plot_element));
  EXPECT_FALSE(plot2.datasetMarkerScopeVisible(*dataset_a, PJ::MarkerScope::kDataset));
  EXPECT_TRUE(plot2.datasetMarkerScopeVisible(*dataset_a, PJ::MarkerScope::kAllDatasets));
  EXPECT_TRUE(plot2.datasetMarkerScopeVisible(*dataset_b, PJ::MarkerScope::kDataset));
  EXPECT_TRUE(plot2.datasetMarkerScopeVisible(*dataset_b, PJ::MarkerScope::kAllDatasets));
}

// Removing the last curve of a dataset resets every one of its scope flags to
// visible, so re-adding a curve of that dataset shows its markers again.
TEST(PlotWidgetGlobalMarkers, RemovingLastCurveResetsFlag) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/x"));
  ASSERT_FALSE(key.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  ASSERT_NE(plot.addCurve(key), nullptr);
  plot.setDatasetMarkerScopeVisible(*dataset, PJ::MarkerScope::kDataset, false);
  ASSERT_FALSE(plot.datasetMarkerScopeVisible(*dataset, PJ::MarkerScope::kDataset));

  plot.removeCurve(key);
  EXPECT_TRUE(plot.curveDatasets().empty());
  EXPECT_TRUE(plot.datasetMarkerScopeVisible(*dataset, PJ::MarkerScope::kDataset));
}

// markerScopeRows() (and the CurveEditor row it backs) follows the
// __global__ topic's existence, not a producer-triggered replot: the change
// signal fires once when the topic appears, and not again for an unrelated
// markersChanged with nothing new.
TEST(PlotWidgetGlobalMarkers, GlobalRowSetFollowsTopicExistence) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/x"));
  ASSERT_FALSE(key.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  ASSERT_NE(plot.addCurve(key), nullptr);
  EXPECT_TRUE(plot.markerScopeRows().empty());

  int changed_count = 0;
  QObject::connect(&plot, &PJ::PlotWidget::datasetMarkerScopesChanged, [&]() { ++changed_count; });

  publishScopeTopic(session, *dataset, PJ::MarkerScope::kDataset);
  const std::vector<PJ::MarkerScopeKey> rows = plot.markerScopeRows();
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows.front(), PJ::MarkerScopeKey(*dataset, PJ::MarkerScope::kDataset));
  EXPECT_EQ(changed_count, 1);

  session.notifyMarkersChanged();  // nothing changed this time
  EXPECT_EQ(changed_count, 1);
}

// The kAllDatasets-scope overlay target is gated by its own flag, independent of
// the kDataset-scope flag on the same dataset.
TEST(PlotWidgetGlobalMarkers, GlobalScopeTargetGatedIndependently) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/x"));
  ASSERT_FALSE(key.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  ASSERT_NE(plot.addCurve(key), nullptr);

  EXPECT_TRUE(hasScopeTarget(plot.markerTargets(), *dataset, PJ::MarkerScope::kDataset));
  EXPECT_TRUE(hasScopeTarget(plot.markerTargets(), *dataset, PJ::MarkerScope::kAllDatasets));

  plot.setDatasetMarkerScopeVisible(*dataset, PJ::MarkerScope::kAllDatasets, false);
  EXPECT_TRUE(hasScopeTarget(plot.markerTargets(), *dataset, PJ::MarkerScope::kDataset));
  EXPECT_FALSE(hasScopeTarget(plot.markerTargets(), *dataset, PJ::MarkerScope::kAllDatasets));

  plot.setDatasetMarkerScopeVisible(*dataset, PJ::MarkerScope::kDataset, false);
  EXPECT_FALSE(hasScopeTarget(plot.markerTargets(), *dataset, PJ::MarkerScope::kDataset));
  EXPECT_FALSE(hasScopeTarget(plot.markerTargets(), *dataset, PJ::MarkerScope::kAllDatasets));

  plot.setDatasetMarkerScopeVisible(*dataset, PJ::MarkerScope::kAllDatasets, true);
  EXPECT_FALSE(hasScopeTarget(plot.markerTargets(), *dataset, PJ::MarkerScope::kDataset));
  EXPECT_TRUE(hasScopeTarget(plot.markerTargets(), *dataset, PJ::MarkerScope::kAllDatasets));
}

// Hiding one scope on dataset A and the other scope on dataset B writes two
// <marker_scope> children, each carrying its own scope attribute, and a fresh
// plot loading them restores both independently.
TEST(PlotWidgetGlobalMarkers, XmlRoundTripWritesScopeAttribute) {
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
  plot.setDatasetMarkerScopeVisible(*dataset_a, PJ::MarkerScope::kAllDatasets, false);
  plot.setDatasetMarkerScopeVisible(*dataset_b, PJ::MarkerScope::kDataset, false);

  QDomDocument doc;
  const QDomElement plot_element = plot.xmlSaveState(doc);
  int marker_scope_count = 0;
  for (QDomElement element = plot_element.firstChildElement(u"marker_scope"_s); !element.isNull();
       element = element.nextSiblingElement(u"marker_scope"_s)) {
    ++marker_scope_count;
    EXPECT_EQ(element.attribute(u"visible"_s), u"false"_s);
    if (element.attribute(u"dataset_id"_s).toUInt() == *dataset_a) {
      EXPECT_EQ(element.attribute(u"scope"_s), u"global"_s);
    } else {
      EXPECT_EQ(element.attribute(u"dataset_id"_s).toUInt(), *dataset_b);
      EXPECT_EQ(element.attribute(u"scope"_s), u"dataset"_s);
    }
  }
  EXPECT_EQ(marker_scope_count, 2);

  PJ::PlotWidget plot2(&session, &catalog);
  ASSERT_TRUE(plot2.xmlLoadState(plot_element));
  EXPECT_TRUE(plot2.datasetMarkerScopeVisible(*dataset_a, PJ::MarkerScope::kDataset));
  EXPECT_FALSE(plot2.datasetMarkerScopeVisible(*dataset_a, PJ::MarkerScope::kAllDatasets));
  EXPECT_FALSE(plot2.datasetMarkerScopeVisible(*dataset_b, PJ::MarkerScope::kDataset));
  EXPECT_TRUE(plot2.datasetMarkerScopeVisible(*dataset_b, PJ::MarkerScope::kAllDatasets));
}

// A <marker_scope visible="false"> child with an absent or unrecognized
// `scope` attribute is ignored entirely — the entry never hides anything,
// rather than guessing which scope it meant.
TEST(PlotWidgetGlobalMarkers, XmlUnknownScopeIsIgnored) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/x"));
  ASSERT_FALSE(key.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  ASSERT_NE(plot.addCurve(key), nullptr);

  QDomDocument doc;
  QDomElement plot_element = plot.xmlSaveState(doc);
  QDomElement missing_scope_element = doc.createElement(u"marker_scope"_s);
  missing_scope_element.setAttribute(u"dataset_id"_s, QString::number(*dataset));
  missing_scope_element.setAttribute(u"dataset_source"_s, u"drive.mcap"_s);
  missing_scope_element.setAttribute(u"visible"_s, u"false"_s);
  plot_element.appendChild(missing_scope_element);
  QDomElement bogus_scope_element = doc.createElement(u"marker_scope"_s);
  bogus_scope_element.setAttribute(u"dataset_id"_s, QString::number(*dataset));
  bogus_scope_element.setAttribute(u"dataset_source"_s, u"drive.mcap"_s);
  bogus_scope_element.setAttribute(u"scope"_s, u"bogus"_s);
  bogus_scope_element.setAttribute(u"visible"_s, u"false"_s);
  plot_element.appendChild(bogus_scope_element);

  PJ::PlotWidget plot2(&session, &catalog);
  ASSERT_TRUE(plot2.xmlLoadState(plot_element));
  EXPECT_TRUE(plot2.datasetMarkerScopeVisible(*dataset, PJ::MarkerScope::kDataset));
  EXPECT_TRUE(plot2.datasetMarkerScopeVisible(*dataset, PJ::MarkerScope::kAllDatasets));
}

// markerScopeRows() tracks each scope's topic independently: with only
// __all__ published, the only row is the dataset's kAllDatasets one.
TEST(PlotWidgetGlobalMarkers, ScopeRowSetFollowsEachTopic) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/x"));
  ASSERT_FALSE(key.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  ASSERT_NE(plot.addCurve(key), nullptr);
  EXPECT_TRUE(plot.markerScopeRows().empty());

  int changed_count = 0;
  QObject::connect(&plot, &PJ::PlotWidget::datasetMarkerScopesChanged, [&]() { ++changed_count; });

  publishScopeTopic(session, *dataset, PJ::MarkerScope::kAllDatasets);
  const std::vector<PJ::MarkerScopeKey> rows = plot.markerScopeRows();
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows.front(), PJ::MarkerScopeKey(*dataset, PJ::MarkerScope::kAllDatasets));
  EXPECT_EQ(changed_count, 1);
}
