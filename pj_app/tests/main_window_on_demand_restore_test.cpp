// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// On-demand recipes through MainWindow's history (docs/derived_objects/ARCHITECTURE.md §13): undo and redo restore
// what a step removed, its topic and the scene layer bound to it; they never touch a history-exempt recipe or
// an open panel's previews; removing every dataset ends the previews. Deletion goes through OnDemandService
// directly (the plugin remove slot is slice 8a-2b's) or through the Custom Topics delete, which is its own undo
// step; a restore fills Custom Topics. A series (a number output over an object input) is a Custom Topics row too,
// whose replay draws a stoppable bar. The fake script run (`test::FakeRun`) computes.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDomDocument>
#include <QEvent>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidgetItem>
#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "MainWindow.h"
#include "SeriesProgressController.h"
#include "dataset_test_helpers.h"
#include "event_pump_test_utils.h"
#include "on_demand_test_utils.h"
#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/BatchEngine.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/DerivedObjects.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/OnDemandService.h"
#include "pj_runtime/SeriesRunner.h"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_widgets/CurveTreeView.h"
#include "pj_widgets/VisualizationKind.h"
#include "ui/CurveListPanel.h"

using namespace Qt::StringLiterals;

namespace PJ {

class MainWindowOnDemandRestoreTestPeer {
 public:
  [[nodiscard]] static AppSession& session(MainWindow& window) {
    return *window.session_;
  }
  static void resetHistory(MainWindow& window) {
    window.resetUndoHistory();
  }
  static void pushUndoState(MainWindow& window) {
    window.pushUndoState(/*force_new_state=*/true);
  }
  static void undo(MainWindow& window) {
    window.onUndo();
  }
  static void redo(MainWindow& window) {
    window.onRedo();
  }
  // What the Custom Topics panel's Delete runs.
  static void deleteCustomTopic(MainWindow& window, const QString& catalog_key) {
    window.deleteCustomTopic(catalog_key);
  }
  // These windows load no plugin: names the toolbox that would declare `custom_topics_editor`.
  static void setCustomTopicsEditorId(MainWindow& window, const QString& plugin_id) {
    window.session_->extensionCatalog().setCustomTopicsEditorIdForTest(plugin_id);
    window.syncCustomTopicsEditor();
  }
  [[nodiscard]] static QDomElement saveDataProcessors(MainWindow& window, QDomDocument& doc) {
    return window.derivedObjects().save(doc, SnapshotScope::kFull);
  }
  [[nodiscard]] static bool restoreDataProcessors(MainWindow& window, const QDomElement& root) {
    return window.restoreDataProcessors(root);
  }
  // A 3D scene dock in a new plot tab, built the way the family picker builds it.
  [[nodiscard]] static Scene3DDockWidget* addSceneDock(MainWindow& window) {
    auto* tabs = window.findChild<TabbedPlotWidget*>();
    PlotDocker* docker = tabs != nullptr ? tabs->addTab(u"Scene"_s) : nullptr;
    DockWidget* dock = docker != nullptr ? docker->plotAt(0) : nullptr;
    if (dock == nullptr) {
      return nullptr;
    }
    window.onObjectFamilyRequested(dock, VisualizationKind::kScene3D);
    return dock->objectWidget() != nullptr ? qobject_cast<Scene3DDockWidget*>(dock->objectWidget()->widget()) : nullptr;
  }
};

}  // namespace PJ

namespace {

using Peer = PJ::MainWindowOnDemandRestoreTestPeer;
using PJ::RecipeGraph;

constexpr PJ::Timestamp kSec = 1'000'000'000;

RecipeGraph::Recipe cropRecipe(std::string owner, std::string id) {
  RecipeGraph::Recipe recipe = PJ::test::onDemandRecipe(
      std::move(id), {"cloud"},
      {{"cropped", PJ::scripting::OutputType::kObject, PJ::sdk::BuiltinObjectType::kPointCloud}});
  recipe.owner_plugin = std::move(owner);
  return recipe;
}

std::string topicOf(const RecipeGraph::Recipe& recipe) {
  return recipe.key() + "/cropped";
}

// A transform class whose id is `id`, negating its input.
std::string negate(const std::string& id) {
  return "-- pj-script: luau\nreturn { id=\"" + id + "\", name=\"" + id +
         "\", output=\"same\",\n  create = function(p) return { calculate = function(t, v) return -v end } end }\n";
}

// A window with the dataset `run.mcap` holding a cloud topic `cloud` (entries at 5 and 10 s) and a scalar
// `threshold`; its on-demand engine runs the fake.
struct Fixture {
  ~Fixture() {
    fake.allow(1 << 20);  // a gated run left waiting must not hold the pool's join
  }
  Fixture() {
    dataset = pj_test::createDataset(app, "run.mcap");
    EXPECT_NE(pj_test::addScalarTopic(app, dataset, "threshold"), 0U);
    PJ::ObjectStore& store = manager.objectStore();
    cloud = store.registerTopic({dataset, "cloud", R"({"builtin_object_type":"kPointCloud"})"}).value();
    for (const PJ::Timestamp t : {5 * kSec, 10 * kSec}) {
      const auto bytes = PJ::serializeBuiltinObject(PJ::sdk::BuiltinObject(PJ::test::makeCloud(4, t)));
      EXPECT_TRUE(store.pushOwned(cloud, t, *bytes).has_value());
    }
    app.catalogModel().rebuildFromDatastore();
  }

  [[nodiscard]] PJ::OnDemandService& service() {
    return manager.onDemandService();
  }
  // Installs and announces, the way a recipe-creating path does.
  void install(const RecipeGraph::Recipe& recipe) {
    const auto installed = service().install(recipe);
    ASSERT_TRUE(installed.has_value()) << installed.error();
    manager.notifyObjectTopicsChanged();
    app.catalogModel().rebuildFromDatastore();
  }
  // Removes and announces; the catalog rebuild prunes what showed the retired topics.
  void remove(const std::string& key) {
    ASSERT_FALSE(service().remove(key).empty());
    manager.notifyObjectTopicsChanged();
    app.catalogModel().rebuildFromDatastore();
  }
  [[nodiscard]] std::optional<PJ::ObjectTopicId> topic(const std::string& name) const {
    return manager.objectStore().findTopic(dataset, name);
  }
  [[nodiscard]] QStringList customTopics() {
    auto* panel = window.findChild<PJ::CurveListPanel*>(u"curveListPanel"_s);
    return panel != nullptr ? panel->customTopicNames() : QStringList{u"<no panel>"_s};
  }
  [[nodiscard]] QString catalogKey(const std::string& name) {
    for (const PJ::CatalogItem& item : app.catalogModel().items()) {
      if (item.topic_name == QString::fromStdString(name)) {
        return item.key;
      }
    }
    return {};
  }
  [[nodiscard]] bool installed(const std::string& key) const {
    return manager.onDemandService().graph().find(key) != nullptr;
  }
  // Layers across every 3D scene dock: an undo rebuilds the docks, so no pointer is held across one.
  [[nodiscard]] size_t sceneLayers() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    size_t count = 0;
    for (auto* dock : window.findChildren<PJ::Scene3DDockWidget*>()) {
      count += dock->layers().size();
    }
    return count;
  }
  void showInScene(const std::string& name) {
    PJ::Scene3DDockWidget* dock = Peer::addSceneDock(window);
    ASSERT_NE(dock, nullptr);
    ASSERT_TRUE(dock->addTopic(*topic(name), PJ::sdk::BuiltinObjectType::kPointCloud, u"layer"_s));
    QCoreApplication::processEvents();
  }

  PJ::test::FakeRun fake;  ///< outlives the window, whose session's pool joins the runs
  PJ::test::ScopedRun scoped_run{fake};
  QTemporaryDir extensions_dir;
  PJ::MainWindow window{extensions_dir.path()};
  PJ::AppSession& app = Peer::session(window);
  PJ::SessionManager& manager = app.sessionManager();
  PJ::DatasetId dataset = 0;
  PJ::ObjectTopicId cloud;
};

// LAY-12: deleting an ordinary recipe retires its topic and the scene layer bound to it; undo brings the recipe,
// its topic and the layer back, redo removes them again.
TEST(MainWindowOnDemandRestoreTest, DeleteOfOrdinaryRecipeRetiresTopicAndLayerAndIsUndoable) {
  Fixture fx;
  const RecipeGraph::Recipe crop = cropRecipe("toolbox", "crop");
  fx.install(crop);
  fx.showInScene(topicOf(crop));
  ASSERT_EQ(fx.sceneLayers(), 1U);
  Peer::resetHistory(fx.window);

  fx.remove(crop.key());
  Peer::pushUndoState(fx.window);
  EXPECT_FALSE(fx.topic(topicOf(crop)).has_value());
  EXPECT_EQ(fx.sceneLayers(), 0U) << "the layer goes with its topic";

  Peer::undo(fx.window);
  EXPECT_TRUE(fx.installed(crop.key())) << "undo brings the recipe back";
  EXPECT_TRUE(fx.topic(topicOf(crop)).has_value()) << "and its topic";
  EXPECT_EQ(fx.sceneLayers(), 1U) << "and the layer bound to it";

  Peer::redo(fx.window);
  EXPECT_FALSE(fx.installed(crop.key()));
  EXPECT_FALSE(fx.topic(topicOf(crop)).has_value());
  EXPECT_EQ(fx.sceneLayers(), 0U);
}

// LAY-12 through the Custom Topics delete: the editor's ordinary recipe goes with its topic, its row and the scene
// layer bound to it as one undo step of its own; undo brings all of them back, redo removes them again.
TEST(MainWindowOnDemandRestoreTest, CustomTopicsDeleteOfOrdinaryRecipeIsOneUndoStep) {
  Fixture fx;
  Peer::setCustomTopicsEditorId(fx.window, u"toolbox"_s);
  const RecipeGraph::Recipe crop = cropRecipe("toolbox", "crop");
  fx.install(crop);
  fx.showInScene(topicOf(crop));
  ASSERT_EQ(fx.sceneLayers(), 1U);
  Peer::resetHistory(fx.window);
  const QStringList row{QString::fromStdString(topicOf(crop))};
  ASSERT_EQ(fx.customTopics(), row);
  const QString key = fx.catalogKey(topicOf(crop));
  ASSERT_FALSE(key.isEmpty());

  Peer::deleteCustomTopic(fx.window, key);  // no dependents: no prompt
  EXPECT_FALSE(fx.installed(crop.key()));
  EXPECT_FALSE(fx.topic(topicOf(crop)).has_value());
  EXPECT_TRUE(fx.catalogKey(topicOf(crop)).isEmpty());
  EXPECT_TRUE(fx.customTopics().isEmpty());
  EXPECT_EQ(fx.sceneLayers(), 0U) << "the layer goes with its topic";

  Peer::undo(fx.window);
  EXPECT_TRUE(fx.installed(crop.key())) << "undo brings the recipe back";
  EXPECT_TRUE(fx.topic(topicOf(crop)).has_value()) << "and its topic";
  EXPECT_EQ(fx.customTopics(), row) << "and its row";
  EXPECT_EQ(fx.sceneLayers(), 1U) << "and the layer bound to it";

  Peer::redo(fx.window);
  EXPECT_FALSE(fx.installed(crop.key()));
  EXPECT_FALSE(fx.topic(topicOf(crop)).has_value());
  EXPECT_TRUE(fx.customTopics().isEmpty());
  EXPECT_EQ(fx.sceneLayers(), 0U);
}

// LAY-12 without a bound view: no layout change marks the step, and the install pushed no state of its own, so the
// delete itself must refresh the baseline and push the step, or undo would find nothing to undo.
TEST(MainWindowOnDemandRestoreTest, CustomTopicsDeleteIsAnUndoStepWithoutABoundView) {
  Fixture fx;
  Peer::setCustomTopicsEditorId(fx.window, u"toolbox"_s);
  Peer::resetHistory(fx.window);  // before the install, which pushes nothing
  const RecipeGraph::Recipe crop = cropRecipe("toolbox", "crop");
  fx.install(crop);

  Peer::deleteCustomTopic(fx.window, fx.catalogKey(topicOf(crop)));
  ASSERT_FALSE(fx.installed(crop.key()));
  Peer::undo(fx.window);
  EXPECT_TRUE(fx.installed(crop.key()));
  EXPECT_EQ(fx.customTopics(), QStringList{QString::fromStdString(topicOf(crop))});
  Peer::redo(fx.window);
  EXPECT_FALSE(fx.installed(crop.key()));
  EXPECT_TRUE(fx.customTopics().isEmpty());
}

// LAY-15: a restore fills Custom Topics itself (its installs announce nothing while it runs): the restored recipe's
// row is there when it returns.
TEST(MainWindowOnDemandRestoreTest, RestoreFillsCustomTopics) {
  Fixture fx;
  const RecipeGraph::Recipe crop = cropRecipe("owner", "crop");
  fx.install(crop);
  QDomDocument doc;
  QDomElement root = doc.createElement(u"root"_s);
  doc.appendChild(root);
  root.appendChild(Peer::saveDataProcessors(fx.window, doc));
  fx.remove(crop.key());
  ASSERT_TRUE(fx.customTopics().isEmpty());

  ASSERT_TRUE(Peer::restoreDataProcessors(fx.window, doc.documentElement()));
  EXPECT_EQ(fx.customTopics(), QStringList{u"owner/crop/cropped"_s});
}

// LAY-13: an undo of an unrelated step leaves a history-exempt recipe and its scene layer as they were: the same
// topic id, no run.
TEST(MainWindowOnDemandRestoreTest, UndoLeavesExemptRecipeAndItsSceneLayerIntact) {
  Fixture fx;
  RecipeGraph::Recipe finding = cropRecipe("assistant", "finding");
  finding.history_exempt = true;
  fx.install(finding);
  const RecipeGraph::Recipe mine = cropRecipe("toolbox", "mine");
  fx.install(mine);
  const PJ::ObjectTopicId exempt_topic = *fx.topic(topicOf(finding));
  const uint64_t revision = fx.service().graph().find(finding.key())->revision;
  fx.showInScene(topicOf(finding));
  Peer::resetHistory(fx.window);

  fx.remove(mine.key());
  Peer::pushUndoState(fx.window);
  Peer::undo(fx.window);

  EXPECT_TRUE(fx.installed(mine.key())) << "the undo really moved history";
  ASSERT_TRUE(fx.installed(finding.key()));
  EXPECT_EQ(fx.service().graph().find(finding.key())->revision, revision) << "not re-created";
  EXPECT_EQ(fx.topic(topicOf(finding)), exempt_topic) << "same topic id";
  EXPECT_EQ(fx.sceneLayers(), 1U);
}

// LAY-14: history snapshots do not carry an exempt recipe, so deleting one is not undone by an unrelated undo or
// redo.
TEST(MainWindowOnDemandRestoreTest, DeleteOfExemptRecipeIsNotResurrectedByUndoRedo) {
  Fixture fx;
  Peer::resetHistory(fx.window);  // S0: nothing
  const RecipeGraph::Recipe ordinary = cropRecipe("toolbox", "ordinary");
  fx.install(ordinary);
  RecipeGraph::Recipe exempt = cropRecipe("assistant", "exempt");
  exempt.history_exempt = true;
  fx.install(exempt);
  Peer::pushUndoState(fx.window);  // S1: the ordinary recipe
  fx.remove(exempt.key());

  Peer::undo(fx.window);
  EXPECT_FALSE(fx.installed(ordinary.key())) << "the undo really moved history";
  EXPECT_FALSE(fx.installed(exempt.key()));
  Peer::redo(fx.window);
  EXPECT_TRUE(fx.installed(ordinary.key()));
  EXPECT_FALSE(fx.installed(exempt.key()));
  EXPECT_FALSE(fx.topic(topicOf(exempt)).has_value());
}

// OWN-12: an undo and a redo leave an open panel's previews in every engine (on-demand, transform, marker rule),
// and still undo the persisted state of the step.
TEST(MainWindowOnDemandRestoreTest, UndoKeepsAnOpenPanelsPreviewsInEveryEngine) {
  Fixture fx;
  Peer::resetHistory(fx.window);  // S0: nothing persisted
  RecipeGraph::Recipe preview = cropRecipe("tbx", "__preview__");
  preview.ephemeral = true;
  fx.install(preview);
  const RecipeGraph::Recipe ordinary = cropRecipe("owner", "ordinary");
  fx.install(ordinary);
  PJ::DataProcessorService& transforms = fx.manager.dataProcessorService();
  ASSERT_TRUE(transforms.upsertTransform("tbx", "p", {"threshold"}, {"threshold_preview"}, negate("p"), "{}", true)
                  .has_value());
  ASSERT_TRUE(transforms.upsertTransform("owner", "t", {"threshold"}, {"threshold_kept"}, negate("t"), "{}", false)
                  .has_value());
  PJ::BatchEngine::GeneratorRecipe marker;
  marker.id = "tbx/__p__";
  marker.dataset_id = fx.dataset;
  marker.inputs = {"threshold/value"};
  marker.script = "createMarker(0.0)\n";
  marker.ephemeral = true;
  ASSERT_TRUE(fx.manager.batchEngine().upsertGenerator(marker).has_value());
  fx.app.catalogModel().rebuildFromDatastore();
  Peer::pushUndoState(fx.window);  // S1: the ordinary recipe and owner/t

  Peer::undo(fx.window);
  EXPECT_FALSE(fx.installed(ordinary.key())) << "the undo really moved history";
  EXPECT_TRUE(transforms.transformIdsForPlugin("owner").empty());
  EXPECT_TRUE(fx.installed(preview.key())) << "the on-demand preview survives";
  EXPECT_TRUE(fx.topic(topicOf(preview)).has_value());
  EXPECT_EQ(transforms.transformIdsForPlugin("tbx").size(), 1U) << "the transform preview survives";
  EXPECT_NE(fx.manager.batchEngine().firstBinding("tbx/__p__"), nullptr) << "the marker preview survives";

  Peer::redo(fx.window);
  EXPECT_TRUE(fx.installed(ordinary.key()));
  EXPECT_TRUE(fx.installed(preview.key()));
  EXPECT_EQ(transforms.transformIdsForPlugin("tbx").size(), 1U);
}

// OWN-13: removing every dataset removes the data an on-demand preview reads: every preview ends, an open panel's
// or not.
TEST(MainWindowOnDemandRestoreTest, RemoveAllDatasetsEndsTheOpenPanelsPreviews) {
  Fixture fx;
  RecipeGraph::Recipe preview = cropRecipe("tbx", "__preview__");
  preview.ephemeral = true;
  fx.install(preview);
  RecipeGraph::Recipe orphan = cropRecipe("closed", "__preview__");
  orphan.ephemeral = true;
  fx.install(orphan);
  auto* panel = fx.window.findChild<PJ::CurveListPanel*>(u"curveListPanel"_s);
  ASSERT_NE(panel, nullptr);

  emit panel->clearAllCurvesRequested();

  EXPECT_FALSE(fx.installed(preview.key()));
  EXPECT_FALSE(fx.installed(orphan.key()));
  EXPECT_FALSE(fx.topic(topicOf(preview)).has_value());
}

// `id`: one number output `n` over the fixture's cloud topic, so a series whose topic is named like its key.
RecipeGraph::Recipe seriesRecipe(std::string owner, std::string id = "points") {
  RecipeGraph::Recipe recipe = PJ::test::onDemandRecipe(
      std::move(id), {"cloud"}, {{"n", PJ::scripting::OutputType::kNumber, PJ::sdk::BuiltinObjectType::kNone}});
  recipe.owner_plugin = std::move(owner);
  return recipe;
}

// The panel's two trees and its pencil.
struct PanelViews {
  explicit PanelViews(PJ::MainWindow& window)
      : panel(window.findChild<PJ::CurveListPanel*>(u"curveListPanel"_s)),
        tree(panel != nullptr ? panel->findChild<PJ::CurveTreeView*>(u"treeView"_s) : nullptr),
        custom(panel != nullptr ? panel->findChild<PJ::CurveTreeView*>(u"customView"_s) : nullptr),
        edit(panel != nullptr ? panel->findChild<QToolButton*>(u"buttonEditCustom"_s) : nullptr) {}
  [[nodiscard]] bool valid() const {
    return panel != nullptr && tree != nullptr && custom != nullptr && edit != nullptr;
  }

  PJ::CurveListPanel* panel;
  PJ::CurveTreeView* tree;
  PJ::CurveTreeView* custom;
  QToolButton* edit;
};

QTreeWidgetItem* findRowByKey(QTreeWidgetItem* parent, const QString& key) {
  for (int index = 0; index < parent->childCount(); ++index) {
    QTreeWidgetItem* child = parent->child(index);
    if (PJ::CurveTreeView::catalogKeyOf(child) == key) {
      return child;
    }
    if (QTreeWidgetItem* found = findRowByKey(child, key)) {
      return found;
    }
  }
  return nullptr;
}

constexpr int kProgressRoleForTest = Qt::UserRole + 13;  // CurveTreeView's kDatasetProgressRole

// The progress decoration on the Custom Topics row of `catalog_key`, if any.
std::optional<PJ::CurveTreeView::DatasetProgress> rowProgress(PanelViews& views, const QString& catalog_key) {
  const QTreeWidgetItem* row = findRowByKey(views.custom->invisibleRootItem(), catalog_key);
  if (row == nullptr || !row->data(0, kProgressRoleForTest).isValid()) {
    return std::nullopt;
  }
  return row->data(0, kProgressRoleForTest).value<PJ::CurveTreeView::DatasetProgress>();
}

// SER-16: a series is one flat Custom Topics row per number output, named `recipe/output`, owned like the object rows
// (the editor's own: editable, no badge) and gone from Datasets; deleting the row deletes the recipe and retires the
// topic as one undo step.
TEST(MainWindowOnDemandRestoreTest, SeriesRowListsInCustomTopicsAndDeleteRemovesTheRecipe) {
  Fixture fx;
  PanelViews views(fx.window);
  ASSERT_TRUE(views.valid());
  Peer::setCustomTopicsEditorId(fx.window, u"toolbox"_s);
  Peer::resetHistory(fx.window);
  const RecipeGraph::Recipe recipe = seriesRecipe("toolbox");
  fx.install(recipe);

  const QString key = fx.catalogKey(recipe.key());
  ASSERT_FALSE(key.isEmpty()) << "the series topic is a catalog row";
  EXPECT_EQ(fx.customTopics(), QStringList{QString::fromStdString(recipe.key())});
  views.tree->expandAll();
  QTreeWidgetItem* row = findRowByKey(views.custom->invisibleRootItem(), key);
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->text(0), u"points/n"_s);
  EXPECT_TRUE(PJ::CurveTreeView::badgeOf(row).isEmpty()) << "the editor's own rows carry no badge";
  EXPECT_EQ(findRowByKey(views.tree->invisibleRootItem(), key), nullptr) << "a series row leaves Datasets";
  views.custom->clearSelection();
  row->setSelected(true);
  EXPECT_TRUE(views.edit->isEnabled());

  const PJ::TopicId topic = fx.service().series().state(recipe.key())->topic;
  Peer::deleteCustomTopic(fx.window, key);
  EXPECT_FALSE(fx.installed(recipe.key()));
  EXPECT_TRUE(fx.customTopics().isEmpty());
  const auto listed = fx.manager.dataEngine().listTopics(fx.dataset);
  EXPECT_EQ(std::count(listed.begin(), listed.end(), topic), 0) << "the topic is retired";
  EXPECT_TRUE(fx.catalogKey(recipe.key()).isEmpty());

  Peer::undo(fx.window);
  EXPECT_TRUE(fx.installed(recipe.key())) << "undo brings the recipe back";
  EXPECT_EQ(fx.customTopics(), QStringList{QString::fromStdString(recipe.key())}) << "and its row";
}

// SER-17: a replaying series draws a stoppable bar on its row; the row's stop keeps the rows computed and leaves the
// series incomplete and installed; the bar goes after its linger.
TEST(MainWindowOnDemandRestoreTest, SeriesReplayShowsProgressAndStopKeepsRows) {
  Fixture fx;
  PanelViews views(fx.window);
  ASSERT_TRUE(views.valid());
  auto* controller = fx.window.findChild<PJ::SeriesProgressController*>();
  ASSERT_NE(controller, nullptr);
  controller->setLingerMs(10);
  PJ::ObjectStore& store = fx.manager.objectStore();
  for (int second = 11; second <= 210; ++second) {  // more than one chunk of instants
    const PJ::Timestamp t = second * kSec;
    const auto bytes = PJ::serializeBuiltinObject(PJ::sdk::BuiltinObject(PJ::test::makeCloud(4, t)));
    ASSERT_TRUE(store.pushOwned(fx.cloud, t, *bytes).has_value());
  }
  fx.fake.gated = true;
  const RecipeGraph::Recipe recipe = seriesRecipe("toolbox");
  fx.install(recipe);
  const QString key = fx.catalogKey(recipe.key());
  ASSERT_FALSE(key.isEmpty());
  fx.service().series().request(recipe.key());
  fx.fake.allow(static_cast<int>(PJ::SeriesRunner::kInstantsPerChunk));

  std::optional<PJ::CurveTreeView::DatasetProgress> progress;
  PJ::test::pumpUntil([&] {
    progress = rowProgress(views, key);
    return progress.has_value() && fx.service().series().state(recipe.key())->rows > 0;
  });
  ASSERT_TRUE(progress.has_value()) << "the replay's row carries a bar";
  EXPECT_EQ(progress->state, PJ::CurveTreeView::DatasetProgress::State::kLoading);
  EXPECT_TRUE(progress->cancellable);
  EXPECT_FALSE(progress->discardable) << "stopping a series keeps what it computed";
  const PJ::SeriesRunner::State running = *fx.service().series().state(recipe.key());
  ASSERT_FALSE(running.complete);
  ASSERT_GT(running.rows, 0U);

  emit views.custom->cancelRequested(static_cast<quint64>(running.topic) << 16, /*keep_partial=*/false);
  const PJ::SeriesRunner::State stopped = *fx.service().series().state(recipe.key());
  EXPECT_TRUE(stopped.stopped);
  EXPECT_FALSE(stopped.complete);
  EXPECT_EQ(stopped.rows, running.rows);
  EXPECT_TRUE(fx.installed(recipe.key())) << "the recipe stays installed";
  PJ::test::pumpUntil([&] { return !rowProgress(views, key).has_value(); });
  EXPECT_FALSE(rowProgress(views, key).has_value()) << "the bar goes after its linger";
  fx.fake.allow(1 << 20);
  PJ::test::pumpFor(50);
  EXPECT_EQ(fx.service().series().state(recipe.key())->rows, running.rows) << "a stopped run computes nothing more";
}

// SER-17: a series that finishes shows its end briefly, then its bar is retired; the row stays.
TEST(MainWindowOnDemandRestoreTest, CompletedSeriesRetiresItsBar) {
  Fixture fx;
  PanelViews views(fx.window);
  ASSERT_TRUE(views.valid());
  auto* controller = fx.window.findChild<PJ::SeriesProgressController*>();
  ASSERT_NE(controller, nullptr);
  controller->setLingerMs(10);
  std::vector<PJ::CurveTreeView::DatasetProgress::State> seen;
  QObject::connect(
      controller, &PJ::SeriesProgressController::progressUpdated, controller,
      [&seen](const QHash<quint64, PJ::CurveTreeView::DatasetProgress>& by_topic) {
        for (const auto& progress : by_topic) {
          seen.push_back(progress.state);
        }
      });
  const RecipeGraph::Recipe recipe = seriesRecipe("toolbox");
  fx.install(recipe);
  const QString key = fx.catalogKey(recipe.key());
  ASSERT_FALSE(key.isEmpty());

  fx.service().series().request(recipe.key());
  const auto done = [&] {
    const auto state = fx.service().series().state(recipe.key());
    return state && state->complete && !state->replaying && !seen.empty() && !rowProgress(views, key).has_value();
  };
  PJ::test::pumpUntil(done);
  ASSERT_TRUE(done());
  EXPECT_EQ(seen.front(), PJ::CurveTreeView::DatasetProgress::State::kLoading);
  EXPECT_NE(std::find(seen.begin(), seen.end(), PJ::CurveTreeView::DatasetProgress::State::kCompleted), seen.end());
  EXPECT_NE(findRowByKey(views.custom->invisibleRootItem(), key), nullptr) << "the row stays";
}

// SER-16: another plugin's series row is use-only: its Delete is refused and nothing is removed.
TEST(MainWindowOnDemandRestoreTest, SeriesRowOfAnotherPluginCannotBeDeleted) {
  Fixture fx;
  Peer::setCustomTopicsEditorId(fx.window, u"toolbox"_s);
  const RecipeGraph::Recipe recipe = seriesRecipe("assistant");
  fx.install(recipe);
  const QString key = fx.catalogKey(recipe.key());
  ASSERT_FALSE(key.isEmpty());
  ASSERT_EQ(fx.customTopics(), QStringList{QString::fromStdString(recipe.key())});

  Peer::deleteCustomTopic(fx.window, key);
  EXPECT_TRUE(fx.installed(recipe.key()));
  EXPECT_EQ(fx.customTopics(), QStringList{QString::fromStdString(recipe.key())});
}

}  // namespace
