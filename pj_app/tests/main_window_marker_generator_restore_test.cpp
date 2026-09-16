// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// A marker generator saved in a layout is replayed by restoreDataProcessors, which
// runs its script immediately. That happens on a session where no toolbox panel has
// ever been opened — the case these tests pin, because the catalog-backed series
// resolver the script reads through used to be installed only when a toolbox was
// launched, so every restored rule ran against no data and failed.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <optional>
#include <string>
#include <vector>

#include "MainWindow.h"
#include "dataset_test_helpers.h"
#include "pj_base/builtin/plot_markers.hpp"
#include "pj_base/builtin/plot_markers_codec.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/MarkerService.h"
#include "pj_runtime/MarkerTopics.h"
#include "pj_runtime/SessionManager.h"

using namespace Qt::StringLiterals;

namespace PJ {

class MainWindowMarkerGeneratorTestPeer {
 public:
  [[nodiscard]] static AppSession& session(MainWindow& window) {
    return *window.session_;
  }

  [[nodiscard]] static bool restoreDataProcessors(MainWindow& window, const QDomElement& root) {
    return window.restoreDataProcessors(root);
  }

  [[nodiscard]] static auto captureWorkspace(MainWindow& window) {
    return window.captureWorkspace();
  }

  [[nodiscard]] static bool restoreWorkspace(MainWindow& window, const MainWindow::CapturedWorkspace& captured) {
    return window.restoreWorkspaceState(
               captured, MainWindow::MissingCurvePolicy::kExact, MainWindow::TimelineRestoreMode::kExact) ==
           MainWindow::RestoreResult::kApplied;
  }
};

}  // namespace PJ

namespace {

constexpr const char* kTopic = "sensor";
constexpr const char* kSeriesKey = "sensor/value";
constexpr const char* kGeneratorId = "toolbox-anomaly-detector/rule/sensor";

// A layout carrying one marker generator over `series_key`, shaped exactly as
// MainWindow::saveDataProcessors writes it.
QDomDocument layoutWithGenerator(PJ::DatasetId dataset_id, const QString& source_name, const QString& script) {
  QDomDocument doc;
  QDomElement root = doc.createElement(u"root"_s);
  doc.appendChild(root);
  QDomElement processors = doc.createElement(u"data_processors"_s);
  root.appendChild(processors);

  QDomElement gen = doc.createElement(u"generator"_s);
  gen.setAttribute(u"id"_s, QString::fromLatin1(kGeneratorId));
  gen.setAttribute(u"language"_s, u"luau"_s);
  gen.setAttribute(u"all_datasets"_s, u"0"_s);
  gen.setAttribute(u"dataset_id"_s, QString::number(dataset_id));
  gen.setAttribute(u"dataset_source"_s, source_name);
  processors.appendChild(gen);

  QDomElement input = doc.createElement(u"input"_s);
  input.setAttribute(u"name"_s, QString::fromLatin1(kSeriesKey));
  gen.appendChild(input);
  QDomElement output = doc.createElement(u"output"_s);
  output.setAttribute(u"name"_s, QString::fromLatin1(kSeriesKey));
  gen.appendChild(output);

  QDomElement script_el = doc.createElement(u"script"_s);
  script_el.appendChild(doc.createCDATASection(script));
  gen.appendChild(script_el);
  return doc;
}

// Markers the layout's generator published for its output topic on `dataset`, if any.
std::optional<PJ::sdk::PlotMarkers> publishedMarkers(PJ::AppSession& app, PJ::DatasetId dataset) {
  return pj_test::publishedMarkers(app, dataset, kSeriesKey, kGeneratorId);
}

// The whole point: no toolbox is ever launched here, yet the restored rule must see
// its input series and publish. Before the resolver moved to MainWindow construction,
// series() returned nil and the restore failed.
TEST(MainWindowMarkerGeneratorRestoreTest, RestoresGeneratorWithoutLaunchingAToolbox) {
  QTemporaryDir extensions_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  PJ::MainWindow window(extensions_dir.path());
  PJ::AppSession& app = PJ::MainWindowMarkerGeneratorTestPeer::session(window);

  const PJ::DatasetId dataset = pj_test::createDataset(app, "run.csv");
  ASSERT_NE(dataset, 0U);
  ASSERT_NE(pj_test::addScalarTopic(app, dataset, kTopic), 0U);

  const QDomDocument doc = layoutWithGenerator(
      dataset, u"run.csv"_s,
      uR"(
        local s = series("sensor/value")
        for i = 0, s:size() - 1 do
          createMarker(s:at(i).t, s:at(i).v, {label="sample"})
        end
      )"_s);

  EXPECT_TRUE(PJ::MainWindowMarkerGeneratorTestPeer::restoreDataProcessors(window, doc.documentElement()));

  const std::optional<PJ::sdk::PlotMarkers> markers = publishedMarkers(app, dataset);
  ASSERT_TRUE(markers.has_value()) << "the restored generator published nothing";
  EXPECT_EQ(markers->markers.size(), 2U) << "one marker per sample of the two-sample fixture";
}

// A rule whose input series is absent (the layout was reopened against other data) is
// an ordinary outcome, not a corrupt layout: it is reported and skipped, and the
// restore still succeeds so the caller does not roll the whole workspace back.
TEST(MainWindowMarkerGeneratorRestoreTest, UnresolvableGeneratorDoesNotFailTheRestore) {
  QTemporaryDir extensions_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  PJ::MainWindow window(extensions_dir.path());
  PJ::AppSession& app = PJ::MainWindowMarkerGeneratorTestPeer::session(window);

  const PJ::DatasetId dataset = pj_test::createDataset(app, "run.csv");
  ASSERT_NE(dataset, 0U);
  ASSERT_NE(pj_test::addScalarTopic(app, dataset, kTopic), 0U);

  // Reads a series this dataset does not have -> series() is nil -> script error.
  const QDomDocument doc = layoutWithGenerator(
      dataset, u"run.csv"_s,
      uR"(
        local s = series("not/here")
        createMarker(s:at(0).t, s:at(0).v, {label="never"})
      )"_s);

  EXPECT_TRUE(PJ::MainWindowMarkerGeneratorTestPeer::restoreDataProcessors(window, doc.documentElement()));
  EXPECT_FALSE(publishedMarkers(app, dataset).has_value());
}

// The layout stamps the generator's dataset by numeric id AND source label. Ids are
// minted by load order, so reopening the same files in another order hands the saved
// id to a different source: the restore must rebind by label, not trust the id.
TEST(MainWindowMarkerGeneratorRestoreTest, RestoresGeneratorBySourceWhenIdsSwap) {
  QTemporaryDir extensions_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  PJ::MainWindow window(extensions_dir.path());
  PJ::AppSession& app = PJ::MainWindowMarkerGeneratorTestPeer::session(window);

  // Saved session: "a" then "b", generator on "b". Reopened session: "b" then "a",
  // so "a" now owns the id the layout recorded for "b". Both carry the same series.
  const PJ::DatasetId dataset_b = pj_test::createDataset(app, "b");
  const PJ::DatasetId dataset_a = pj_test::createDataset(app, "a");
  ASSERT_NE(dataset_b, 0U);
  ASSERT_NE(dataset_a, 0U);
  ASSERT_NE(pj_test::addScalarTopic(app, dataset_b, kTopic), 0U);
  ASSERT_NE(pj_test::addScalarTopic(app, dataset_a, kTopic), 0U);

  const QDomDocument doc = layoutWithGenerator(
      dataset_a, u"b"_s,
      uR"(
        local s = series("sensor/value")
        createMarker(s:at(0).t, s:at(0).v, {label="sample"})
      )"_s);

  EXPECT_TRUE(PJ::MainWindowMarkerGeneratorTestPeer::restoreDataProcessors(window, doc.documentElement()));

  const std::vector<PJ::MarkerService::GeneratorRecipe> recipes = app.sessionManager().markerService().recipes();
  ASSERT_EQ(recipes.size(), 1U);
  EXPECT_EQ(recipes.front().dataset_id, dataset_b);
  EXPECT_TRUE(publishedMarkers(app, dataset_b).has_value());
  EXPECT_FALSE(publishedMarkers(app, dataset_a).has_value());
}

TEST(MainWindowMarkerGeneratorRestoreTest, ReplayAnnouncesMarkersChangedOnce) {
  QTemporaryDir extensions_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  PJ::MainWindow window(extensions_dir.path());
  PJ::AppSession& app = PJ::MainWindowMarkerGeneratorTestPeer::session(window);
  const PJ::DatasetId dataset = pj_test::createDataset(app, "run.csv");
  ASSERT_NE(dataset, 0U);
  ASSERT_NE(pj_test::addScalarTopic(app, dataset, kTopic), 0U);

  QSignalSpy changed(&app.sessionManager(), &PJ::SessionManager::markersChanged);
  const QDomDocument doc = layoutWithGenerator(dataset, u"run.csv"_s, u"createMarker(100)"_s);
  ASSERT_TRUE(PJ::MainWindowMarkerGeneratorTestPeer::restoreDataProcessors(window, doc.documentElement()));
  EXPECT_EQ(changed.count(), 1);

  ASSERT_TRUE(PJ::MainWindowMarkerGeneratorTestPeer::restoreDataProcessors(window, QDomElement{}));
  EXPECT_EQ(changed.count(), 2);
  ASSERT_TRUE(PJ::MainWindowMarkerGeneratorTestPeer::restoreDataProcessors(window, QDomElement{}));
  EXPECT_EQ(changed.count(), 2);
}

TEST(MainWindowMarkerGeneratorRestoreTest, GeneratorReplaysUnderTheSnapshotsDisplayOffsets) {
  constexpr PJ::Timestamp kSecond = 1'000'000'000LL;
  QTemporaryDir extensions_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  PJ::MainWindow window(extensions_dir.path());
  PJ::AppSession& app = PJ::MainWindowMarkerGeneratorTestPeer::session(window);
  const PJ::DatasetId dataset_a = pj_test::createDataset(app, "a", /*own_time_domain=*/true);
  const PJ::DatasetId dataset_b = pj_test::createDataset(app, "b", /*own_time_domain=*/true);
  ASSERT_NE(dataset_a, 0U);
  ASSERT_NE(dataset_b, 0U);
  ASSERT_NE(pj_test::addScalarTopic(app, dataset_a, kTopic, 100 * kSecond, 200 * kSecond), 0U);
  ASSERT_NE(pj_test::addScalarTopic(app, dataset_b, "other", 100 * kSecond, 200 * kSecond), 0U);
  app.sessionManager().setDisplayOffset(dataset_a, PJ::DisplayOffset{PJ::Duration{20 * kSecond}});
  auto captured = PJ::MainWindowMarkerGeneratorTestPeer::captureWorkspace(window);

  QDomDocument doc;
  ASSERT_TRUE(static_cast<bool>(doc.setContent(captured.xml)));
  QDomElement processors = doc.documentElement().firstChildElement(u"data_processors"_s);
  if (processors.isNull()) {
    processors = doc.createElement(u"data_processors"_s);
    doc.documentElement().appendChild(processors);
  }
  QDomElement gen = doc.createElement(u"generator"_s);
  gen.setAttribute(u"id"_s, u"test/global-rule"_s);
  gen.setAttribute(u"language"_s, u"luau"_s);
  gen.setAttribute(u"all_datasets"_s, u"1"_s);
  processors.appendChild(gen);
  QDomElement input = doc.createElement(u"input"_s);
  input.setAttribute(u"name"_s, u"sensor/value"_s);
  gen.appendChild(input);
  QDomElement output = doc.createElement(u"output"_s);
  output.setAttribute(u"name"_s, QString::fromStdString(std::string(PJ::kAllDatasetsMarkerTopic)));
  gen.appendChild(output);
  QDomElement script = doc.createElement(u"script"_s);
  script.appendChild(
      doc.createCDATASection(uR"(
    local s = series("sensor/value")
    if s ~= nil then
      for i = 0, s:size() - 1 do createMarker(s:at(i).t) end
    end
  )"_s));
  gen.appendChild(script);
  captured.xml = doc.toByteArray();

  app.sessionManager().setDisplayOffset(dataset_a, PJ::DisplayOffset{PJ::Duration{0}});
  ASSERT_TRUE(PJ::MainWindowMarkerGeneratorTestPeer::restoreWorkspace(window, captured));
  EXPECT_EQ(app.sessionManager().sourceDisplayOffset(dataset_a).value.count(), 20 * kSecond);
  // Only a carries the input; replayed under its restored 20 s offset, raw 100/200 sit at 80/180.
  EXPECT_EQ(
      pj_test::markerTimes(app, PJ::kAllDatasetsMarkerDataset, PJ::kAllDatasetsMarkerTopic, "test/global-rule"),
      (std::vector<PJ::Timestamp>{80 * kSecond, 180 * kSecond}));
}

}  // namespace
