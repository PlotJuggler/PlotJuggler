// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// History-exempt processors, marker generators, and owned plot tabs persist in
// full layouts while remaining outside undo/redo authority.

#include <gtest/gtest.h>

#include <QDomDocument>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTreeWidgetItem>
#include <algorithm>
#include <limits>
#include <memory>
#include <string_view>

#include "LayoutXml.h"
#include "MainWindow.h"
#include "dataset_test_helpers.h"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/MarkerService.h"
#include "pj_runtime/SessionManager.h"
#include "pj_widgets/CurveTreeView.h"
#include "ui/CurveListPanel.h"

using namespace Qt::StringLiterals;

namespace PJ {

class MainWindowHistoryExemptTestPeer {
 public:
  using CapturedWorkspace = MainWindow::CapturedWorkspace;
  using MissingCurvePolicy = MainWindow::MissingCurvePolicy;
  using RestoreIntent = MainWindow::RestoreIntent;
  using RestoreResult = MainWindow::RestoreResult;
  using SnapshotScope = MainWindow::SnapshotScope;
  using TimelineRestoreMode = MainWindow::TimelineRestoreMode;

  [[nodiscard]] static AppSession& appSession(MainWindow& window) {
    return *window.session_;
  }

  [[nodiscard]] static DataProcessorService& processors(MainWindow& window) {
    return window.session_->sessionManager().dataProcessorService();
  }

  [[nodiscard]] static MarkerService& markers(MainWindow& window) {
    return window.session_->sessionManager().markerService();
  }

  [[nodiscard]] static TabbedPlotWidget& tabs(MainWindow& window) {
    return *window.findChild<TabbedPlotWidget*>();
  }

  [[nodiscard]] static Status createOwnedTab(
      MainWindow& window, const QString& plugin_id, const QString& tab_id, const QString& title) {
    return window.createOwnedPlotTab(plugin_id, tab_id, title);
  }

  [[nodiscard]] static Status closeOwnedTab(MainWindow& window, const QString& plugin_id, const QString& tab_id) {
    return window.closeOwnedPlotTab(plugin_id, tab_id);
  }

  [[nodiscard]] static PlotDocker* ownedTab(const MainWindow& window, const QString& plugin_id, const QString& tab_id) {
    return window.ownedPlotTab(plugin_id, tab_id);
  }

  static void resetHistory(MainWindow& window) {
    window.resetUndoHistory();
  }

  static void pushUndoState(MainWindow& window, bool force_new_state) {
    window.pushUndoState(force_new_state);
  }

  static void undo(MainWindow& window) {
    window.onUndo();
  }

  static void redo(MainWindow& window) {
    window.onRedo();
  }

  [[nodiscard]] static std::size_t undoSize(const MainWindow& window) {
    return window.undo_states_.size();
  }

  [[nodiscard]] static std::size_t redoSize(const MainWindow& window) {
    return window.redo_states_.size();
  }

  [[nodiscard]] static CapturedWorkspace captureWorkspace(const MainWindow& window, SnapshotScope scope) {
    return window.captureWorkspace(scope);
  }

  [[nodiscard]] static QDomDocument xmlSaveState(const MainWindow& window, SnapshotScope scope) {
    return window.xmlSaveState(scope);
  }

  [[nodiscard]] static QByteArray liveState(const MainWindow& window) {
    return window.xmlSaveState().toByteArray(2);
  }

  [[nodiscard]] static RestoreResult restoreWorkspaceState(
      MainWindow& window, const CapturedWorkspace& target, MissingCurvePolicy policy, TimelineRestoreMode timeline_mode,
      const CapturedWorkspace* rollback_to, RestoreIntent intent, QString* out_reason = nullptr) {
    return window.restoreWorkspaceState(target, policy, timeline_mode, rollback_to, intent, out_reason);
  }

  [[nodiscard]] static RestoreResult restoreWorkspaceStateDoc(
      MainWindow& window, QDomDocument& doc, MissingCurvePolicy policy, const CapturedWorkspace* rollback_to,
      RestoreIntent intent) {
    return window.restoreWorkspaceState(doc, policy, rollback_to, intent);
  }
};

}  // namespace PJ

namespace {

QTreeWidgetItem* findRowByCatalogKey(QTreeWidgetItem* item, const QString& catalog_key) {
  if (PJ::CurveTreeView::catalogKeyOf(item) == catalog_key) {
    return item;
  }
  for (int index = 0; index < item->childCount(); ++index) {
    if (QTreeWidgetItem* found = findRowByCatalogKey(item->child(index), catalog_key)) {
      return found;
    }
  }
  return nullptr;
}

QString catalogKeyForTopic(const PJ::CatalogModel& catalog, const QString& topic_name) {
  const auto items = catalog.items();
  const auto item = std::find_if(
      items.begin(), items.end(), [&](const PJ::CatalogItem& candidate) { return candidate.topic_name == topic_name; });
  return item != items.end() ? item->key : QString{};
}

PJ::CurveTreeView* customSeriesView(PJ::MainWindow& window) {
  return window.findChild<PJ::CurveTreeView*>(u"customView"_s);
}

PJ::PlotWidget* ensureCurrentPlot(PJ::MainWindow& window) {
  auto* tabbed = window.findChild<PJ::TabbedPlotWidget*>(u"tabbedPlotWidget"_s);
  if (tabbed == nullptr || tabbed->currentTab() == nullptr || tabbed->currentTab()->plotCount() == 0) {
    return nullptr;
  }
  PJ::DockWidget* dock = tabbed->currentTab()->plotAt(0);
  if (dock == nullptr) {
    return nullptr;
  }
  if (dock->plotWidget() == nullptr) {
    dock->setPlotWidget(new PJ::PlotWidget(nullptr, nullptr, dock));
  }
  return dock->plotWidget();
}

PJ::PlotWidget* ensurePlot(PJ::PlotDocker* docker) {
  if (docker == nullptr || docker->plotCount() == 0) {
    return nullptr;
  }
  PJ::DockWidget* dock = docker->plotAt(0);
  if (dock == nullptr) {
    return nullptr;
  }
  if (dock->plotWidget() == nullptr) {
    dock->setPlotWidget(new PJ::PlotWidget(nullptr, nullptr, dock));
  }
  return dock->plotWidget();
}

// Mirrors main_window_history_test.cpp's helper: appends (or creates) a
// <data_processors><processor> entry whose input cannot resolve against any
// loaded dataset, so restoreDataProcessors fails that entry and the whole
// restore is rejected under MissingCurvePolicy::kExact.
QDomElement addUnresolvableProcessor(QDomDocument& doc) {
  QDomElement processors = doc.documentElement().firstChildElement(u"data_processors"_s);
  if (processors.isNull()) {
    processors = doc.createElement(u"data_processors"_s);
    doc.documentElement().appendChild(processors);
  }
  QDomElement processor = doc.createElement(u"processor"_s);
  processor.setAttribute(u"input_topic"_s, u"/missing_processor_input"_s);
  processor.setAttribute(u"input_field"_s, u"value"_s);
  processor.setAttribute(u"input_dataset_id"_s, QString::number(std::numeric_limits<PJ::DatasetId>::max()));
  processor.setAttribute(u"input_dataset_source"_s, u"missing-processor-source"_s);
  processor.setAttribute(u"processor_id"_s, u"absolute"_s);
  processor.setAttribute(u"output_name"_s, u"missing[Absolute]"_s);
  PJ::layout_xml::appendJsonAsCdata(doc, processor, u"{}"_s);
  processors.appendChild(processor);
  return processor;
}

// Like ensurePlot, but the plot is bound to the session so addCurve(catalog key)
// resolves; owned tabs start with an empty dock in this fixture.
PJ::PlotWidget* ensureBoundPlot(PJ::AppSession& app_session, PJ::PlotDocker* docker) {
  if (docker == nullptr || docker->plotCount() == 0) {
    return nullptr;
  }
  PJ::DockWidget* dock = docker->plotAt(0);
  if (dock == nullptr) {
    return nullptr;
  }
  if (dock->plotWidget() == nullptr) {
    dock->setPlotWidget(new PJ::PlotWidget(&app_session.sessionManager(), &app_session.catalogModel(), dock));
  }
  return dock->plotWidget();
}

constexpr const char* kNegateScript = R"LUAU(-- pj-script: luau
return { id="negate", name="Negate", output="same",
  create = function(p) return { calculate = function(t, v) return -v end } end }
)LUAU";

constexpr const char* kTimes10Script = R"LUAU(-- pj-script: luau
return { id="negate", name="Negate", output="same",
  create = function(p) return { calculate = function(t, v) return v * 10.0 end } end }
)LUAU";

constexpr const char* kMarkerScript = "createMarker(0.0)\n";

class MainWindowHistoryExemptFixture : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    extensions_dir_ = std::make_unique<QTemporaryDir>();
    ASSERT_TRUE(extensions_dir_->isValid());
    window_ = std::make_unique<PJ::MainWindow>(extensions_dir_->path());
  }

  static void TearDownTestSuite() {
    window_.reset();
    extensions_dir_.reset();
  }

  [[nodiscard]] PJ::MainWindow& mainWindow() const {
    return *window_;
  }

  // Every case starts from a clean slate: no live processors/generators, and a
  // fresh undo history baselined at the current (empty) state — the shell is
  // shared across TEST_F cases in this binary (its dtor leaks process state).
  void SetUp() override {
    PJ::DataProcessorService& service = PJ::MainWindowHistoryExemptTestPeer::processors(mainWindow());
    service.clearAllFilters();
    service.clearAllTransforms();
    PJ::MainWindowHistoryExemptTestPeer::markers(mainWindow()).clearAllGenerators();
    PJ::AppSession& app_session = PJ::MainWindowHistoryExemptTestPeer::appSession(mainWindow());
    PJ::SessionManager& session = app_session.sessionManager();
    for (const PJ::DatasetId dataset : session.dataEngine().listDatasets()) {
      app_session.catalogModel().removeDataset(dataset, /*tombstone=*/false);
      session.evictDatasetObjects(dataset);
      session.removeDataset(dataset);
    }
    PJ::TabbedPlotWidget& tabs = PJ::MainWindowHistoryExemptTestPeer::tabs(mainWindow());
    for (int index = tabs.dockerCount() - 1; index >= 0; --index) {
      PJ::PlotDocker* docker = tabs.dockerAt(index);
      if (docker != nullptr && docker->isHistoryExempt()) {
        tabs.closeTab(docker);
      }
    }
    app_session.catalogModel().rebuildFromDatastore();
    PJ::MainWindowHistoryExemptTestPeer::resetHistory(mainWindow());
  }

 private:
  inline static std::unique_ptr<QTemporaryDir> extensions_dir_;
  inline static std::unique_ptr<PJ::MainWindow> window_;
};

// (a) An exempt series survives an unrelated undo: creating it, then undoing a
// later, unrelated edit, must not touch it.
TEST_F(MainWindowHistoryExemptFixture, ExemptSeriesSurvivesUnrelatedUndo) {
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app_session = PJ::MainWindowHistoryExemptTestPeer::appSession(window);
  PJ::DataProcessorService& service = PJ::MainWindowHistoryExemptTestPeer::processors(window);

  const PJ::DatasetId dataset = pj_test::createDataset(app_session, "exempt-survives-undo", /*own_time_domain=*/true);
  ASSERT_NE(dataset, 0U);
  ASSERT_NE(pj_test::addScalarTopic(app_session, dataset, "/x"), 0U);

  const auto created = service.upsertTransform(
      "assistant-test", "negate", {"/x"}, {"E_out"}, kNegateScript, "{}", /*ephemeral=*/false,
      /*input_column_index=*/0, /*history_exempt=*/true);
  ASSERT_TRUE(created.has_value()) << created.error();
  app_session.catalogModel().rebuildFromDatastore();

  PJ::PlotWidget* plot = ensureCurrentPlot(window);
  ASSERT_NE(plot, nullptr);
  plot->setStateId(u"exempt-survives-undo-after"_s);
  PJ::MainWindowHistoryExemptTestPeer::pushUndoState(window, /*force_new_state=*/true);
  ASSERT_GE(PJ::MainWindowHistoryExemptTestPeer::undoSize(window), 2U);

  PJ::MainWindowHistoryExemptTestPeer::undo(window);

  const auto recipes = service.transformRecipes();
  const auto it =
      std::find_if(recipes.begin(), recipes.end(), [](const auto& r) { return r.key == "assistant-test/negate"; });
  ASSERT_NE(it, recipes.end()) << "the exempt transform must survive an unrelated undo";
  EXPECT_TRUE(it->history_exempt);

  const auto items = app_session.catalogModel().items();
  EXPECT_TRUE(std::any_of(items.begin(), items.end(), [](const auto& item) { return item.topic_name == u"E_out"_s; }))
      << "the exempt transform's output must remain in the catalog";
}

// (b) redo must not resurrect an exempt series a plugin deliberately removed:
// undo/redo across an UNRELATED edit that happened while it was alive leaves it
// gone, because no history document — before or after its creation — ever named
// it.
TEST_F(MainWindowHistoryExemptFixture, RedoDoesNotResurrectRemovedExemptSeries) {
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app_session = PJ::MainWindowHistoryExemptTestPeer::appSession(window);
  PJ::DataProcessorService& service = PJ::MainWindowHistoryExemptTestPeer::processors(window);

  const PJ::DatasetId dataset = pj_test::createDataset(app_session, "exempt-no-resurrect", /*own_time_domain=*/true);
  ASSERT_NE(dataset, 0U);
  ASSERT_NE(pj_test::addScalarTopic(app_session, dataset, "/x"), 0U);

  PJ::PlotWidget* plot = ensureCurrentPlot(window);
  ASSERT_NE(plot, nullptr);
  plot->setStateId(u"exempt-no-resurrect-s1"_s);
  PJ::MainWindowHistoryExemptTestPeer::pushUndoState(window, /*force_new_state=*/true);
  ASSERT_GE(PJ::MainWindowHistoryExemptTestPeer::undoSize(window), 2U);

  const auto created = service.upsertTransform(
      "assistant-test", "negate", {"/x"}, {"E_out"}, kNegateScript, "{}", /*ephemeral=*/false,
      /*input_column_index=*/0, /*history_exempt=*/true);
  ASSERT_TRUE(created.has_value()) << created.error();
  ASSERT_TRUE(service.removeTransform("assistant-test/negate").has_value());

  PJ::MainWindowHistoryExemptTestPeer::undo(window);
  PJ::MainWindowHistoryExemptTestPeer::redo(window);

  const auto recipes = service.transformRecipes();
  EXPECT_TRUE(std::none_of(recipes.begin(), recipes.end(), [](const auto& r) {
    return r.key == "assistant-test/negate";
  })) << "redo must not resurrect a processor no history document ever carried";
}

// (c) An exempt transform round-trips through a kFull layout document
// (history_exempt="1" written and restored) but is entirely absent from a
// kHistory document.
TEST_F(MainWindowHistoryExemptFixture, ExemptTransformRoundTripsThroughFullNotHistoryScope) {
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app_session = PJ::MainWindowHistoryExemptTestPeer::appSession(window);
  PJ::DataProcessorService& service = PJ::MainWindowHistoryExemptTestPeer::processors(window);

  const PJ::DatasetId dataset = pj_test::createDataset(app_session, "exempt-roundtrip", /*own_time_domain=*/true);
  ASSERT_NE(dataset, 0U);
  ASSERT_NE(pj_test::addScalarTopic(app_session, dataset, "/x"), 0U);

  const auto created = service.upsertTransform(
      "assistant-test", "negate", {"/x"}, {"E_out"}, kNegateScript, "{}", /*ephemeral=*/false,
      /*input_column_index=*/0, /*history_exempt=*/true);
  ASSERT_TRUE(created.has_value()) << created.error();
  app_session.catalogModel().rebuildFromDatastore();
  const QString initial_output_key = catalogKeyForTopic(app_session.catalogModel(), u"E_out"_s);
  ASSERT_FALSE(initial_output_key.isEmpty());
  PJ::CurveTreeView* custom_view = customSeriesView(window);
  ASSERT_NE(custom_view, nullptr);
  EXPECT_EQ(findRowByCatalogKey(custom_view->invisibleRootItem(), initial_output_key), nullptr)
      << "a direct service install has not registered a Custom Series row";

  const QDomDocument full_doc = PJ::MainWindowHistoryExemptTestPeer::xmlSaveState(
      window, PJ::MainWindowHistoryExemptTestPeer::SnapshotScope::kFull);
  const QString full_xml = QString::fromUtf8(full_doc.toByteArray(2));
  EXPECT_TRUE(full_xml.contains(u"history_exempt=\"1\""_s));
  EXPECT_TRUE(full_xml.contains(u"id=\"negate\""_s));

  const QDomDocument history_doc = PJ::MainWindowHistoryExemptTestPeer::xmlSaveState(
      window, PJ::MainWindowHistoryExemptTestPeer::SnapshotScope::kHistory);
  const QString history_xml = QString::fromUtf8(history_doc.toByteArray(2));
  EXPECT_FALSE(history_xml.contains(u"history_exempt"_s));
  EXPECT_FALSE(history_xml.contains(u"id=\"negate\""_s))
      << "a kHistory document must omit a history-exempt transform outright";

  // Restore the kFull document (kReplace intent) onto the live session: the
  // exempt entry must come back tagged history_exempt == true.
  ASSERT_TRUE(service.removeTransform("assistant-test/negate").has_value());
  QDomDocument reload_doc = full_doc;
  const PJ::MainWindowHistoryExemptTestPeer::RestoreResult result =
      PJ::MainWindowHistoryExemptTestPeer::restoreWorkspaceStateDoc(
          window, reload_doc, PJ::MainWindowHistoryExemptTestPeer::MissingCurvePolicy::kExact,
          /*rollback_to=*/nullptr, PJ::MainWindowHistoryExemptTestPeer::RestoreIntent::kReplace);
  ASSERT_EQ(result, PJ::MainWindowHistoryExemptTestPeer::RestoreResult::kApplied);

  const auto recipes = service.transformRecipes();
  const auto it =
      std::find_if(recipes.begin(), recipes.end(), [](const auto& r) { return r.key == "assistant-test/negate"; });
  ASSERT_NE(it, recipes.end());
  EXPECT_TRUE(it->history_exempt);
  const QString restored_output_key = catalogKeyForTopic(app_session.catalogModel(), u"E_out"_s);
  ASSERT_FALSE(restored_output_key.isEmpty());
  EXPECT_NE(findRowByCatalogKey(custom_view->invisibleRootItem(), restored_output_key), nullptr)
      << "full restore must register the persisted transform in Custom Series";
}

TEST_F(MainWindowHistoryExemptFixture, FullReplaceDropsReplacedTransformFromCustomSeries) {
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app_session = PJ::MainWindowHistoryExemptTestPeer::appSession(window);
  PJ::DataProcessorService& service = PJ::MainWindowHistoryExemptTestPeer::processors(window);

  const PJ::DatasetId dataset = pj_test::createDataset(app_session, "custom-series-full-replace", true);
  ASSERT_NE(dataset, 0U);
  ASSERT_NE(pj_test::addScalarTopic(app_session, dataset, "/x"), 0U);
  const QDomDocument without_transform = PJ::MainWindowHistoryExemptTestPeer::xmlSaveState(
      window, PJ::MainWindowHistoryExemptTestPeer::SnapshotScope::kFull);

  const auto created = service.upsertTransform(
      "obsolete-owner", "negate", {"/x"}, {"obsolete_output"}, kNegateScript, "{}", /*ephemeral=*/false,
      /*input_column_index=*/0, /*history_exempt=*/true);
  ASSERT_TRUE(created.has_value()) << created.error();
  app_session.catalogModel().rebuildFromDatastore();
  const QString output_key = catalogKeyForTopic(app_session.catalogModel(), u"obsolete_output"_s);
  ASSERT_FALSE(output_key.isEmpty());
  auto* panel = window.findChild<PJ::CurveListPanel*>();
  PJ::CurveTreeView* custom_view = customSeriesView(window);
  ASSERT_NE(panel, nullptr);
  ASSERT_NE(custom_view, nullptr);
  panel->setCustomCurves({output_key});
  ASSERT_NE(findRowByCatalogKey(custom_view->invisibleRootItem(), output_key), nullptr);

  QDomDocument reload_doc = without_transform;
  ASSERT_EQ(
      PJ::MainWindowHistoryExemptTestPeer::restoreWorkspaceStateDoc(
          window, reload_doc, PJ::MainWindowHistoryExemptTestPeer::MissingCurvePolicy::kExact,
          /*rollback_to=*/nullptr, PJ::MainWindowHistoryExemptTestPeer::RestoreIntent::kReplace),
      PJ::MainWindowHistoryExemptTestPeer::RestoreResult::kApplied);

  EXPECT_TRUE(service.transformRecipes().empty());
  EXPECT_EQ(findRowByCatalogKey(custom_view->invisibleRootItem(), output_key), nullptr)
      << "full replacement must remove the replaced transform's Custom Series row";
}

// (d) A history restore that would strand an exempt dependent is rejected
// atomically: neither the user's transform nor the assistant's dependent
// exempt one is touched, and the reason names the exempt output.
TEST_F(MainWindowHistoryExemptFixture, HistoryRestoreRejectsWhenExemptDependentWouldStrand) {
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app_session = PJ::MainWindowHistoryExemptTestPeer::appSession(window);
  PJ::DataProcessorService& service = PJ::MainWindowHistoryExemptTestPeer::processors(window);

  const PJ::DatasetId dataset =
      pj_test::createDataset(app_session, "exempt-dependency-reject", /*own_time_domain=*/true);
  ASSERT_NE(dataset, 0U);
  ASSERT_NE(pj_test::addScalarTopic(app_session, dataset, "/x"), 0U);

  // Baseline BEFORE the user's transform U exists — the history target that
  // will lack U.
  const PJ::MainWindowHistoryExemptTestPeer::CapturedWorkspace before_u =
      PJ::MainWindowHistoryExemptTestPeer::captureWorkspace(
          window, PJ::MainWindowHistoryExemptTestPeer::SnapshotScope::kHistory);

  const auto u = service.upsertTransform(
      "user-plugin", "negate", {"/x"}, {"U_out"}, kNegateScript, "{}", /*ephemeral=*/false,
      /*input_column_index=*/0, /*history_exempt=*/false);
  ASSERT_TRUE(u.has_value()) << u.error();
  const auto e = service.upsertTransform(
      "assistant-test", "negate", {"U_out"}, {"E_out"}, kNegateScript, "{}",
      /*ephemeral=*/false,
      /*input_column_index=*/0, /*history_exempt=*/true);
  ASSERT_TRUE(e.has_value()) << e.error();

  const PJ::MainWindowHistoryExemptTestPeer::CapturedWorkspace current =
      PJ::MainWindowHistoryExemptTestPeer::captureWorkspace(
          window, PJ::MainWindowHistoryExemptTestPeer::SnapshotScope::kHistory);

  QString reason;
  const PJ::MainWindowHistoryExemptTestPeer::RestoreResult result =
      PJ::MainWindowHistoryExemptTestPeer::restoreWorkspaceState(
          window, before_u, PJ::MainWindowHistoryExemptTestPeer::MissingCurvePolicy::kExact,
          PJ::MainWindowHistoryExemptTestPeer::TimelineRestoreMode::kExact, &current,
          PJ::MainWindowHistoryExemptTestPeer::RestoreIntent::kHistory, &reason);

  EXPECT_EQ(result, PJ::MainWindowHistoryExemptTestPeer::RestoreResult::kFailed);
  EXPECT_TRUE(reason.contains(u"E_out"_s)) << reason.toStdString();

  const auto recipes = service.transformRecipes();
  EXPECT_TRUE(std::any_of(recipes.begin(), recipes.end(), [](const auto& r) { return r.key == "user-plugin/negate"; }))
      << "the user's transform U must still be live after a rejected history restore";
  EXPECT_TRUE(std::any_of(recipes.begin(), recipes.end(), [](const auto& r) {
    return r.key == "assistant-test/negate";
  })) << "the exempt dependent E must still be live after a rejected history restore";
}

TEST_F(MainWindowHistoryExemptFixture, ExemptMarkerRoundTripsThroughFullNotHistoryScope) {
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app_session = PJ::MainWindowHistoryExemptTestPeer::appSession(window);
  PJ::MarkerService& markers = PJ::MainWindowHistoryExemptTestPeer::markers(window);

  const PJ::DatasetId dataset = pj_test::createDataset(app_session, "exempt-marker-roundtrip", true);
  ASSERT_NE(dataset, 0U);
  ASSERT_NE(pj_test::addScalarTopic(app_session, dataset, "/marker_input"), 0U);

  PJ::MarkerService::GeneratorRecipe recipe;
  recipe.id = "assistant-test/marker";
  recipe.dataset_id = dataset;
  recipe.inputs = {"/marker_input"};
  recipe.outputs = {"assistant_markers"};
  recipe.script = kMarkerScript;
  recipe.history_exempt = true;
  ASSERT_TRUE(markers.upsertGenerator(recipe).has_value());

  const QDomDocument full_doc = PJ::MainWindowHistoryExemptTestPeer::xmlSaveState(
      window, PJ::MainWindowHistoryExemptTestPeer::SnapshotScope::kFull);
  const QString full_xml = QString::fromUtf8(full_doc.toByteArray(2));
  EXPECT_TRUE(full_xml.contains(u"id=\"assistant-test/marker\""_s));
  EXPECT_TRUE(full_xml.contains(u"history_exempt=\"1\""_s));

  const QDomDocument history_doc = PJ::MainWindowHistoryExemptTestPeer::xmlSaveState(
      window, PJ::MainWindowHistoryExemptTestPeer::SnapshotScope::kHistory);
  const QString history_xml = QString::fromUtf8(history_doc.toByteArray(2));
  EXPECT_FALSE(history_xml.contains(u"id=\"assistant-test/marker\""_s));

  ASSERT_TRUE(markers.removeGenerator("assistant-test/marker").has_value());
  QDomDocument reload_doc = full_doc;
  ASSERT_EQ(
      PJ::MainWindowHistoryExemptTestPeer::restoreWorkspaceStateDoc(
          window, reload_doc, PJ::MainWindowHistoryExemptTestPeer::MissingCurvePolicy::kExact,
          /*rollback_to=*/nullptr, PJ::MainWindowHistoryExemptTestPeer::RestoreIntent::kReplace),
      PJ::MainWindowHistoryExemptTestPeer::RestoreResult::kApplied);

  const auto recipes = markers.recipes();
  const auto restored = std::find_if(
      recipes.begin(), recipes.end(), [](const auto& candidate) { return candidate.id == "assistant-test/marker"; });
  ASSERT_NE(restored, recipes.end());
  EXPECT_TRUE(restored->history_exempt);
}

TEST_F(MainWindowHistoryExemptFixture, UndoRejectsExemptMarkerDependencyWithoutMovingHistory) {
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app_session = PJ::MainWindowHistoryExemptTestPeer::appSession(window);
  PJ::DataProcessorService& processors = PJ::MainWindowHistoryExemptTestPeer::processors(window);
  PJ::MarkerService& markers = PJ::MainWindowHistoryExemptTestPeer::markers(window);

  const PJ::DatasetId dataset = pj_test::createDataset(app_session, "exempt-marker-dependency", true);
  ASSERT_NE(dataset, 0U);
  ASSERT_NE(pj_test::addScalarTopic(app_session, dataset, "/marker_source"), 0U);
  PJ::MainWindowHistoryExemptTestPeer::resetHistory(window);

  const auto user_transform = processors.upsertTransform(
      "user-plugin", "negate", {"/marker_source"}, {"marker_source_derived"}, kNegateScript, "{}",
      /*ephemeral=*/false, /*input_column_index=*/0, /*history_exempt=*/false);
  ASSERT_TRUE(user_transform.has_value()) << user_transform.error();
  app_session.catalogModel().rebuildFromDatastore();

  PJ::MarkerService::GeneratorRecipe marker;
  marker.id = "assistant-test/dependent-marker";
  marker.dataset_id = user_transform->dataset_id;
  marker.inputs = {"marker_source_derived/value"};
  marker.outputs = {"dependent_markers"};
  marker.script = kMarkerScript;
  marker.history_exempt = true;
  ASSERT_TRUE(markers.upsertGenerator(marker).has_value());

  PJ::MainWindowHistoryExemptTestPeer::pushUndoState(window, true);
  const std::size_t undo_size = PJ::MainWindowHistoryExemptTestPeer::undoSize(window);
  const std::size_t redo_size = PJ::MainWindowHistoryExemptTestPeer::redoSize(window);
  ASSERT_GE(undo_size, 2U);

  PJ::MainWindowHistoryExemptTestPeer::undo(window);

  EXPECT_EQ(PJ::MainWindowHistoryExemptTestPeer::undoSize(window), undo_size);
  EXPECT_EQ(PJ::MainWindowHistoryExemptTestPeer::redoSize(window), redo_size);
  EXPECT_TRUE(window.statusBar()->currentMessage().contains(u"dependent_markers"_s));
  const auto transforms_after = processors.transformRecipes();
  EXPECT_TRUE(std::any_of(transforms_after.begin(), transforms_after.end(), [](const auto& candidate) {
    return candidate.key == "user-plugin/negate";
  }));
  const auto markers_after = markers.recipes();
  EXPECT_TRUE(std::any_of(markers_after.begin(), markers_after.end(), [](const auto& candidate) {
    return candidate.id == "assistant-test/dependent-marker";
  }));
}

TEST_F(MainWindowHistoryExemptFixture, OwnedTabSurvivesUndoAsSameLivePage) {
  PJ::MainWindow& window = mainWindow();
  PJ::TabbedPlotWidget& tabs = PJ::MainWindowHistoryExemptTestPeer::tabs(window);
  PJ::MainWindowHistoryExemptTestPeer::resetHistory(window);

  ASSERT_TRUE(
      PJ::MainWindowHistoryExemptTestPeer::createOwnedTab(
          window, u"missing-assistant"_s, u"analysis"_s, u"Assistant analysis"_s));
  PJ::PlotDocker* owned = PJ::MainWindowHistoryExemptTestPeer::ownedTab(window, u"missing-assistant"_s, u"analysis"_s);
  ASSERT_NE(owned, nullptr);
  ASSERT_TRUE(owned->isHistoryExempt());
  PJ::PlotWidget* owned_plot = ensurePlot(owned);
  ASSERT_NE(owned_plot, nullptr);
  owned_plot->setStateId(u"assistant-live-content"_s);
  QPointer<PJ::PlotDocker> owned_guard(owned);

  const QDomDocument history_doc = PJ::MainWindowHistoryExemptTestPeer::xmlSaveState(
      window, PJ::MainWindowHistoryExemptTestPeer::SnapshotScope::kHistory);
  EXPECT_FALSE(QString::fromUtf8(history_doc.toByteArray()).contains(u"missing-assistant/analysis"_s));

  PJ::PlotDocker* ordinary = tabs.dockerAt(0);
  ASSERT_NE(ordinary, nullptr);
  ASSERT_FALSE(ordinary->isHistoryExempt());
  // The shell's first tab starts with an empty dock, so give it a plot of our
  // own rather than depending on an earlier case in this suite having left one
  // there: each case now runs in its own process.
  PJ::PlotWidget* ordinary_plot = ensurePlot(ordinary);
  ASSERT_NE(ordinary_plot, nullptr);
  ordinary_plot->setStateId(u"ordinary-after"_s);
  PJ::MainWindowHistoryExemptTestPeer::pushUndoState(window, true);

  PJ::MainWindowHistoryExemptTestPeer::undo(window);

  ASSERT_FALSE(owned_guard.isNull());
  EXPECT_EQ(
      PJ::MainWindowHistoryExemptTestPeer::ownedTab(window, u"missing-assistant"_s, u"analysis"_s), owned_guard.data());
  ASSERT_NE(owned_guard->plotAt(0), nullptr);
  ASSERT_NE(owned_guard->plotAt(0)->plotWidget(), nullptr);
  EXPECT_EQ(owned_guard->plotAt(0)->plotWidget()->stateId(), u"assistant-live-content"_s);
}

TEST_F(MainWindowHistoryExemptFixture, RedoDoesNotResurrectClosedOwnedTab) {
  PJ::MainWindow& window = mainWindow();
  PJ::TabbedPlotWidget& tabs = PJ::MainWindowHistoryExemptTestPeer::tabs(window);
  PJ::MainWindowHistoryExemptTestPeer::resetHistory(window);

  ASSERT_TRUE(
      PJ::MainWindowHistoryExemptTestPeer::createOwnedTab(
          window, u"missing-assistant"_s, u"removed"_s, u"Temporary analysis"_s));
  PJ::PlotDocker* ordinary = tabs.dockerAt(0);
  ASSERT_NE(ordinary, nullptr);
  ASSERT_FALSE(ordinary->isHistoryExempt());
  ordinary->setName(u"ordinary-after"_s);
  PJ::MainWindowHistoryExemptTestPeer::pushUndoState(window, true);

  ASSERT_TRUE(PJ::MainWindowHistoryExemptTestPeer::closeOwnedTab(window, u"missing-assistant"_s, u"removed"_s));
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  ASSERT_EQ(PJ::MainWindowHistoryExemptTestPeer::ownedTab(window, u"missing-assistant"_s, u"removed"_s), nullptr);

  PJ::MainWindowHistoryExemptTestPeer::undo(window);
  PJ::MainWindowHistoryExemptTestPeer::redo(window);

  EXPECT_EQ(PJ::MainWindowHistoryExemptTestPeer::ownedTab(window, u"missing-assistant"_s, u"removed"_s), nullptr);
}

// (e) A failed kReplace restore's rollback preimage is captured at kFull scope:
// an exempt transform present only in the live state (not in the failing
// document) survives because the rollback replays the FULL live snapshot, not a
// history-scoped one.
TEST_F(MainWindowHistoryExemptFixture, FailedReplaceRestoreRollbackKeepsExemptWorkAlive) {
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app_session = PJ::MainWindowHistoryExemptTestPeer::appSession(window);
  PJ::DataProcessorService& service = PJ::MainWindowHistoryExemptTestPeer::processors(window);

  const PJ::DatasetId dataset = pj_test::createDataset(app_session, "exempt-rollback-kfull", /*own_time_domain=*/true);
  ASSERT_NE(dataset, 0U);
  ASSERT_NE(pj_test::addScalarTopic(app_session, dataset, "/x"), 0U);

  // Captured BEFORE E exists, so bad_doc (built from it) never mentions E —
  // any post-restore reappearance of E can only come from the restore's own
  // freshly captured (kFull, live-at-call-time) rollback preimage.
  const PJ::MainWindowHistoryExemptTestPeer::CapturedWorkspace before_e =
      PJ::MainWindowHistoryExemptTestPeer::captureWorkspace(
          window, PJ::MainWindowHistoryExemptTestPeer::SnapshotScope::kFull);

  const auto created = service.upsertTransform(
      "assistant-test", "negate", {"/x"}, {"E_out"}, kNegateScript, "{}", /*ephemeral=*/false,
      /*input_column_index=*/0, /*history_exempt=*/true);
  ASSERT_TRUE(created.has_value()) << created.error();
  ASSERT_TRUE(
      PJ::MainWindowHistoryExemptTestPeer::createOwnedTab(
          window, u"missing-assistant"_s, u"rollback"_s, u"Rollback analysis"_s));

  QDomDocument bad_doc;
  ASSERT_TRUE(bad_doc.setContent(before_e.xml));
  addUnresolvableProcessor(bad_doc);

  const PJ::MainWindowHistoryExemptTestPeer::RestoreResult result =
      PJ::MainWindowHistoryExemptTestPeer::restoreWorkspaceStateDoc(
          window, bad_doc, PJ::MainWindowHistoryExemptTestPeer::MissingCurvePolicy::kExact,
          /*rollback_to=*/nullptr, PJ::MainWindowHistoryExemptTestPeer::RestoreIntent::kReplace);
  ASSERT_EQ(result, PJ::MainWindowHistoryExemptTestPeer::RestoreResult::kFailed);

  const auto recipes = service.transformRecipes();
  EXPECT_TRUE(std::any_of(recipes.begin(), recipes.end(), [](const auto& r) {
    return r.key == "assistant-test/negate";
  })) << "the exempt transform must be recreated by the rollback's kFull preimage";
  PJ::PlotDocker* restored_tab =
      PJ::MainWindowHistoryExemptTestPeer::ownedTab(window, u"missing-assistant"_s, u"rollback"_s);
  ASSERT_NE(restored_tab, nullptr);
  EXPECT_TRUE(restored_tab->isHistoryExempt());
  EXPECT_EQ(restored_tab->name(), u"Rollback analysis"_s);
}

// (f) A name collision under a history restore is atomic: the live exempt
// series occupying the name is untouched, and the restore fails rather than
// partially applying.
TEST_F(MainWindowHistoryExemptFixture, NameCollisionUnderHistoryRestoreIsAtomic) {
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app_session = PJ::MainWindowHistoryExemptTestPeer::appSession(window);
  PJ::DataProcessorService& service = PJ::MainWindowHistoryExemptTestPeer::processors(window);

  const PJ::DatasetId dataset = pj_test::createDataset(app_session, "exempt-name-collision", /*own_time_domain=*/true);
  ASSERT_NE(dataset, 0U);
  ASSERT_NE(pj_test::addScalarTopic(app_session, dataset, "/x"), 0U);

  const auto user_x = service.upsertTransform(
      "user-plugin", "negate", {"/x"}, {"X"}, kNegateScript, "{}", /*ephemeral=*/false,
      /*input_column_index=*/0, /*history_exempt=*/false);
  ASSERT_TRUE(user_x.has_value()) << user_x.error();

  // S1: the user's "X" transform is live — this is the undo TARGET below.
  const PJ::MainWindowHistoryExemptTestPeer::CapturedWorkspace snapshot_with_user_x =
      PJ::MainWindowHistoryExemptTestPeer::captureWorkspace(
          window, PJ::MainWindowHistoryExemptTestPeer::SnapshotScope::kHistory);

  // The user removes their transform, freeing the "X" output name.
  ASSERT_TRUE(service.removeTransform("user-plugin/negate").has_value());
  // The assistant claims the now-free name as an EXEMPT transform.
  const auto assistant_x = service.upsertTransform(
      "assistant-test", "negate", {"/x"}, {"X"}, kNegateScript, "{}", /*ephemeral=*/false,
      /*input_column_index=*/0, /*history_exempt=*/true);
  ASSERT_TRUE(assistant_x.has_value()) << assistant_x.error();

  const PJ::MainWindowHistoryExemptTestPeer::CapturedWorkspace current =
      PJ::MainWindowHistoryExemptTestPeer::captureWorkspace(
          window, PJ::MainWindowHistoryExemptTestPeer::SnapshotScope::kHistory);

  const PJ::MainWindowHistoryExemptTestPeer::RestoreResult result =
      PJ::MainWindowHistoryExemptTestPeer::restoreWorkspaceState(
          window, snapshot_with_user_x, PJ::MainWindowHistoryExemptTestPeer::MissingCurvePolicy::kExact,
          PJ::MainWindowHistoryExemptTestPeer::TimelineRestoreMode::kExact, &current,
          PJ::MainWindowHistoryExemptTestPeer::RestoreIntent::kHistory);

  EXPECT_EQ(result, PJ::MainWindowHistoryExemptTestPeer::RestoreResult::kFailed);

  const auto recipes = service.transformRecipes();
  const auto it =
      std::find_if(recipes.begin(), recipes.end(), [](const auto& r) { return r.key == "assistant-test/negate"; });
  ASSERT_NE(it, recipes.end()) << "the live exempt X must be untouched by a rejected/failed history restore";
  EXPECT_TRUE(it->history_exempt);
  EXPECT_EQ(it->outputs, std::vector<std::string>{"X"});
}

// Regression [Codex #4]: history replay checks exemption on the SNAPSHOT entry,
// not the live one, so undoing to a state that carried X as non-exempt overwrites
// the live exempt X (script B) with the older non-exempt X (script A).
TEST_F(MainWindowHistoryExemptFixture, UndoDoesNotOverwriteLiveExemptTransformUnderSameId) {
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app_session = PJ::MainWindowHistoryExemptTestPeer::appSession(window);
  PJ::DataProcessorService& service = PJ::MainWindowHistoryExemptTestPeer::processors(window);

  const PJ::DatasetId dataset =
      pj_test::createDataset(app_session, "exempt-same-id-overwrite", /*own_time_domain=*/true);
  ASSERT_NE(dataset, 0U);
  ASSERT_NE(pj_test::addScalarTopic(app_session, dataset, "/x"), 0U);

  const auto user_x = service.upsertTransform(
      "user-plugin", "negate", {"/x"}, {"X_out"}, kNegateScript, "{}", /*ephemeral=*/false,
      /*input_column_index=*/0, /*history_exempt=*/false);
  ASSERT_TRUE(user_x.has_value()) << user_x.error();
  PJ::MainWindowHistoryExemptTestPeer::pushUndoState(window, /*force_new_state=*/true);  // S1: X non-exempt, script A

  const auto exempt_x = service.upsertTransform(
      "user-plugin", "negate", {"/x"}, {"X_out"}, kTimes10Script, "{}", /*ephemeral=*/false,
      /*input_column_index=*/0, /*history_exempt=*/true);
  ASSERT_TRUE(exempt_x.has_value()) << exempt_x.error();

  PJ::PlotWidget* plot = ensureCurrentPlot(window);
  ASSERT_NE(plot, nullptr);
  plot->setStateId(u"exempt-same-id-after"_s);
  PJ::MainWindowHistoryExemptTestPeer::pushUndoState(window, /*force_new_state=*/true);  // S2: X exempt (omitted)
  ASSERT_GE(PJ::MainWindowHistoryExemptTestPeer::undoSize(window), 3U);

  PJ::MainWindowHistoryExemptTestPeer::undo(window);

  const auto recipes = service.transformRecipes();
  const auto it =
      std::find_if(recipes.begin(), recipes.end(), [](const auto& r) { return r.key == "user-plugin/negate"; });
  ASSERT_NE(it, recipes.end());
  EXPECT_TRUE(it->history_exempt) << "history replay must not overwrite a live exempt processor";
  EXPECT_EQ(it->script, kTimes10Script) << "the live exempt script must survive an unrelated undo";
}

// Regression [Codex #4], marker mirror: the same overwrite through MarkerService::upsertGenerator.
TEST_F(MainWindowHistoryExemptFixture, UndoDoesNotOverwriteLiveExemptMarkerUnderSameId) {
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app_session = PJ::MainWindowHistoryExemptTestPeer::appSession(window);
  PJ::MarkerService& markers = PJ::MainWindowHistoryExemptTestPeer::markers(window);

  const PJ::DatasetId dataset = pj_test::createDataset(app_session, "exempt-marker-same-id", /*own_time_domain=*/true);
  ASSERT_NE(dataset, 0U);
  ASSERT_NE(pj_test::addScalarTopic(app_session, dataset, "/marker_input"), 0U);

  PJ::MarkerService::GeneratorRecipe recipe;
  recipe.id = "user-plugin/marker";
  recipe.dataset_id = dataset;
  recipe.inputs = {"/marker_input"};
  recipe.outputs = {"same_id_markers"};
  recipe.script = kMarkerScript;
  ASSERT_TRUE(markers.upsertGenerator(recipe).has_value());
  PJ::MainWindowHistoryExemptTestPeer::pushUndoState(window, /*force_new_state=*/true);  // S1: non-exempt, script A

  recipe.script = "createMarker(1.0)\n";
  recipe.history_exempt = true;
  ASSERT_TRUE(markers.upsertGenerator(recipe).has_value());

  PJ::PlotWidget* plot = ensureCurrentPlot(window);
  ASSERT_NE(plot, nullptr);
  plot->setStateId(u"exempt-marker-same-id-after"_s);
  PJ::MainWindowHistoryExemptTestPeer::pushUndoState(window, /*force_new_state=*/true);  // S2: exempt (omitted)
  ASSERT_GE(PJ::MainWindowHistoryExemptTestPeer::undoSize(window), 3U);

  PJ::MainWindowHistoryExemptTestPeer::undo(window);

  const auto recipes = markers.recipes();
  const auto it =
      std::find_if(recipes.begin(), recipes.end(), [](const auto& r) { return r.id == "user-plugin/marker"; });
  ASSERT_NE(it, recipes.end());
  EXPECT_TRUE(it->history_exempt) << "history replay must not overwrite a live exempt generator";
  EXPECT_EQ(it->script, "createMarker(1.0)\n");
}

// Regression [Codex #7]: restore finalization calls applySavedViewportOrZoom on
// EVERY plot, including preserved owned tabs, which have no saved viewport and
// therefore auto-fit — an unrelated undo resets the owned plot's zoom.
TEST_F(MainWindowHistoryExemptFixture, UnrelatedUndoKeepsOwnedPlotViewport) {
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app_session = PJ::MainWindowHistoryExemptTestPeer::appSession(window);
  PJ::TabbedPlotWidget& tabs = PJ::MainWindowHistoryExemptTestPeer::tabs(window);

  const PJ::DatasetId dataset = pj_test::createDataset(app_session, "owned-viewport", /*own_time_domain=*/true);
  ASSERT_NE(dataset, 0U);
  ASSERT_NE(pj_test::addScalarTopic(app_session, dataset, "/x"), 0U);
  app_session.catalogModel().rebuildFromDatastore();
  const QString key = catalogKeyForTopic(app_session.catalogModel(), u"/x"_s);
  ASSERT_FALSE(key.isEmpty());
  PJ::MainWindowHistoryExemptTestPeer::resetHistory(window);

  ASSERT_TRUE(
      PJ::MainWindowHistoryExemptTestPeer::createOwnedTab(
          window, u"missing-assistant"_s, u"viewport"_s, u"Viewport analysis"_s));
  PJ::PlotDocker* owned = PJ::MainWindowHistoryExemptTestPeer::ownedTab(window, u"missing-assistant"_s, u"viewport"_s);
  ASSERT_NE(owned, nullptr);
  PJ::PlotWidget* owned_plot = ensureBoundPlot(app_session, owned);
  ASSERT_NE(owned_plot, nullptr);
  ASSERT_NE(owned_plot->addCurve(key), nullptr);
  owned_plot->setZoomRectangle(QRectF(0.25, 0.75, 0.5, -0.5), /*emit_signal=*/false);
  const QRectF zoomed = owned_plot->currentBoundingRect();
  ASSERT_NEAR(zoomed.left(), 0.25, 1e-9);
  ASSERT_NEAR(zoomed.right(), 0.75, 1e-9);

  PJ::PlotDocker* ordinary = tabs.dockerAt(0);
  ASSERT_NE(ordinary, nullptr);
  ASSERT_FALSE(ordinary->isHistoryExempt());
  PJ::PlotWidget* ordinary_plot = ensurePlot(ordinary);
  ASSERT_NE(ordinary_plot, nullptr);
  ordinary_plot->setStateId(u"ordinary-viewport-after"_s);
  PJ::MainWindowHistoryExemptTestPeer::pushUndoState(window, /*force_new_state=*/true);

  PJ::MainWindowHistoryExemptTestPeer::undo(window);

  ASSERT_EQ(PJ::MainWindowHistoryExemptTestPeer::ownedTab(window, u"missing-assistant"_s, u"viewport"_s), owned);
  const QRectF after = owned_plot->currentBoundingRect();
  EXPECT_NEAR(after.left(), zoomed.left(), 1e-9) << "owned plot was auto-fitted by an unrelated undo";
  EXPECT_NEAR(after.right(), zoomed.right(), 1e-9);
  EXPECT_NEAR(after.top(), zoomed.top(), 1e-9);
  EXPECT_NEAR(after.bottom(), zoomed.bottom(), 1e-9);
}

// Regression [Codex #5]: an unrelated undo clears and recreates ordinary
// transforms, retiring their output TopicIds; the catalog removal handler then
// prunes the (preserved) owned tab's curve bound to the old output key.
TEST_F(MainWindowHistoryExemptFixture, UnrelatedUndoKeepsOwnedPlotCurveOfOrdinaryTransformOutput) {
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app_session = PJ::MainWindowHistoryExemptTestPeer::appSession(window);
  PJ::DataProcessorService& service = PJ::MainWindowHistoryExemptTestPeer::processors(window);
  PJ::TabbedPlotWidget& tabs = PJ::MainWindowHistoryExemptTestPeer::tabs(window);

  const PJ::DatasetId dataset = pj_test::createDataset(app_session, "owned-derived-curve", /*own_time_domain=*/true);
  ASSERT_NE(dataset, 0U);
  ASSERT_NE(pj_test::addScalarTopic(app_session, dataset, "/x"), 0U);
  const auto user_t = service.upsertTransform(
      "user-plugin", "negate", {"/x"}, {"U_out"}, kNegateScript, "{}", /*ephemeral=*/false,
      /*input_column_index=*/0, /*history_exempt=*/false);
  ASSERT_TRUE(user_t.has_value()) << user_t.error();
  app_session.catalogModel().rebuildFromDatastore();
  const QString key = catalogKeyForTopic(app_session.catalogModel(), u"U_out"_s);
  ASSERT_FALSE(key.isEmpty());
  PJ::MainWindowHistoryExemptTestPeer::resetHistory(window);  // baseline S0 already holds U

  ASSERT_TRUE(
      PJ::MainWindowHistoryExemptTestPeer::createOwnedTab(
          window, u"missing-assistant"_s, u"derived"_s, u"Derived analysis"_s));
  PJ::PlotDocker* owned = PJ::MainWindowHistoryExemptTestPeer::ownedTab(window, u"missing-assistant"_s, u"derived"_s);
  ASSERT_NE(owned, nullptr);
  PJ::PlotWidget* owned_plot = ensureBoundPlot(app_session, owned);
  ASSERT_NE(owned_plot, nullptr);
  ASSERT_NE(owned_plot->addCurve(key), nullptr);
  ASSERT_EQ(owned_plot->curveList().size(), 1U);

  PJ::PlotDocker* ordinary = tabs.dockerAt(0);
  ASSERT_NE(ordinary, nullptr);
  PJ::PlotWidget* ordinary_plot = ensurePlot(ordinary);
  ASSERT_NE(ordinary_plot, nullptr);
  ordinary_plot->setStateId(u"ordinary-derived-after"_s);
  PJ::MainWindowHistoryExemptTestPeer::pushUndoState(window, /*force_new_state=*/true);

  PJ::MainWindowHistoryExemptTestPeer::undo(window);

  ASSERT_EQ(PJ::MainWindowHistoryExemptTestPeer::ownedTab(window, u"missing-assistant"_s, u"derived"_s), owned);
  EXPECT_EQ(owned_plot->curveList().size(), 1U)
      << "owned tab lost its curve when U_out was replayed with a new TopicId";
}

TEST_F(MainWindowHistoryExemptFixture, UnrelatedUndoKeepsOwnedPlotCurveOfOrdinaryFilterOutput) {
  GTEST_SKIP() << "known gap: per-curve filters still clear+replay on a history restore, so an owned tab's curve "
                  "bound to a filter output is pruned by an unrelated undo (follow-up: filter diff-reconcile)";
  auto& window = mainWindow();
  auto& app = PJ::MainWindowHistoryExemptTestPeer::appSession(window);
  auto& service = PJ::MainWindowHistoryExemptTestPeer::processors(window);
  const auto dataset = pj_test::createDataset(app, "owned-filter-curve", true);
  const auto input = pj_test::addScalarTopic(app, dataset, "/filter-input");
  ASSERT_NE(input, 0U);
  const auto filter = service.applyFilter(input, dataset, "absolute", "filtered");
  ASSERT_TRUE(filter) << filter.error();
  app.catalogModel().rebuildFromDatastore();
  const QString key = catalogKeyForTopic(app.catalogModel(), u"filtered"_s);
  ASSERT_FALSE(key.isEmpty());
  ASSERT_TRUE(PJ::MainWindowHistoryExemptTestPeer::createOwnedTab(window, u"assistant"_s, u"filter"_s, u"Filter"_s));
  auto* owned = PJ::MainWindowHistoryExemptTestPeer::ownedTab(window, u"assistant"_s, u"filter"_s);
  auto* plot = ensureBoundPlot(app, owned);
  ASSERT_NE(plot, nullptr);
  ASSERT_NE(plot->addCurve(key), nullptr);
  PJ::MainWindowHistoryExemptTestPeer::resetHistory(window);
  auto* ordinary = ensurePlot(PJ::MainWindowHistoryExemptTestPeer::tabs(window).dockerAt(0));
  ASSERT_NE(ordinary, nullptr);
  ordinary->setStateId(u"unrelated-filter-edit"_s);
  PJ::MainWindowHistoryExemptTestPeer::pushUndoState(window, true);

  PJ::MainWindowHistoryExemptTestPeer::undo(window);

  ASSERT_EQ(PJ::MainWindowHistoryExemptTestPeer::ownedTab(window, u"assistant"_s, u"filter"_s), owned);
  EXPECT_EQ(PJ::MainWindowHistoryExemptTestPeer::redoSize(window), 1U);
  EXPECT_EQ(plot->curveList().size(), 1U) << "owned tab lost its curve when an unchanged filter was replayed";
}

// Claim: an exempt dependent must not block undo when its ordinary producer is unchanged.
TEST_F(MainWindowHistoryExemptFixture, UnrelatedUndoWithExemptTransformDependentKeepsProducerAndAdvancesHistory) {
  auto& window = mainWindow();
  auto& app = PJ::MainWindowHistoryExemptTestPeer::appSession(window);
  auto& service = PJ::MainWindowHistoryExemptTestPeer::processors(window);
  const auto dataset = pj_test::createDataset(app, "unchanged-producer", true);
  ASSERT_NE(pj_test::addScalarTopic(app, dataset, "/x"), 0U);
  const auto producer = service.upsertTransform("user", "negate", {"/x"}, {"U"}, kNegateScript, "{}");
  ASSERT_TRUE(producer) << producer.error();
  const auto dependent =
      service.upsertTransform("assistant", "negate", {"U"}, {"E"}, kNegateScript, "{}", false, 0, true);
  ASSERT_TRUE(dependent) << dependent.error();
  app.catalogModel().rebuildFromDatastore();
  PJ::MainWindowHistoryExemptTestPeer::resetHistory(window);
  ASSERT_NE(ensureCurrentPlot(window), nullptr);
  ensureCurrentPlot(window)->setStateId(u"unrelated-edit"_s);
  PJ::MainWindowHistoryExemptTestPeer::pushUndoState(window, true);
  const auto undo_size = PJ::MainWindowHistoryExemptTestPeer::undoSize(window);
  ASSERT_GE(undo_size, 2U);

  PJ::MainWindowHistoryExemptTestPeer::undo(window);

  EXPECT_EQ(PJ::MainWindowHistoryExemptTestPeer::undoSize(window), undo_size - 1)
      << window.statusBar()->currentMessage().toStdString();
  EXPECT_EQ(PJ::MainWindowHistoryExemptTestPeer::redoSize(window), 1U);
  const auto recipes = service.transformRecipes();
  ASSERT_EQ(recipes.size(), 2U);
  for (const auto& recipe : recipes) {
    EXPECT_EQ(
        recipe.output_topic_ids,
        recipe.key == "user/negate" ? producer->output_topic_ids : dependent->output_topic_ids);
  }
}

// Claim: an exempt marker must not block undo when its ordinary producer is unchanged.
TEST_F(MainWindowHistoryExemptFixture, UnrelatedUndoWithExemptMarkerDependentKeepsProducerAndAdvancesHistory) {
  auto& window = mainWindow();
  auto& app = PJ::MainWindowHistoryExemptTestPeer::appSession(window);
  auto& service = PJ::MainWindowHistoryExemptTestPeer::processors(window);
  auto& markers = PJ::MainWindowHistoryExemptTestPeer::markers(window);
  const auto dataset = pj_test::createDataset(app, "unchanged-marker-producer", true);
  ASSERT_NE(pj_test::addScalarTopic(app, dataset, "/x"), 0U);
  const auto producer = service.upsertTransform("user", "negate", {"/x"}, {"U"}, kNegateScript, "{}");
  ASSERT_TRUE(producer) << producer.error();
  app.catalogModel().rebuildFromDatastore();
  PJ::MarkerService::GeneratorRecipe marker;
  marker.id = "assistant/marker";
  marker.dataset_id = producer->dataset_id;
  marker.inputs = {"U/value"};
  marker.outputs = {"protected_markers"};
  marker.script = kMarkerScript;
  marker.history_exempt = true;
  ASSERT_TRUE(markers.upsertGenerator(marker));
  PJ::MainWindowHistoryExemptTestPeer::resetHistory(window);
  ASSERT_NE(ensureCurrentPlot(window), nullptr);
  ensureCurrentPlot(window)->setStateId(u"unrelated-marker-edit"_s);
  PJ::MainWindowHistoryExemptTestPeer::pushUndoState(window, true);
  const auto undo_size = PJ::MainWindowHistoryExemptTestPeer::undoSize(window);
  ASSERT_GE(undo_size, 2U);

  PJ::MainWindowHistoryExemptTestPeer::undo(window);

  EXPECT_EQ(PJ::MainWindowHistoryExemptTestPeer::undoSize(window), undo_size - 1)
      << window.statusBar()->currentMessage().toStdString();
  EXPECT_EQ(PJ::MainWindowHistoryExemptTestPeer::redoSize(window), 1U);
  const auto recipes = service.transformRecipes();
  ASSERT_EQ(recipes.size(), 1U);
  EXPECT_EQ(recipes.front().output_topic_ids, producer->output_topic_ids);
  const auto remaining_markers = markers.recipes();
  ASSERT_EQ(remaining_markers.size(), 1U);
  EXPECT_EQ(remaining_markers.front().id, marker.id);
}

}  // namespace
