// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// On-demand recipes through MainWindow's history (docs/derived_objects/ARCHITECTURE.md §13): undo and redo restore
// what a step removed, its topic and the scene layer bound to it; they never touch a history-exempt recipe or
// an open panel's previews; removing every dataset ends the previews. Deletion goes through OnDemandService
// directly (the plugin remove slot is slice 8a-2b's). The fake script run (`test::FakeRun`) computes.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEvent>
#include <QTemporaryDir>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "MainWindow.h"
#include "dataset_test_helpers.h"
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
#include "pj_runtime/OnDemandService.h"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
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

}  // namespace
