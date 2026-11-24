// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The Custom Topics panel through the real shell: which recipes it lists, how their rows are named, the owner
// gates on edit and delete (enforced again by the handlers), the "+" that follows the declared editor, the row
// glyph that follows the node state, and the refresh a burst of state changes coalesces into.

#include <gtest/gtest.h>

#include <QApplication>
#include <QPushButton>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidgetItem>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "MainWindow.h"
#include "dataset_test_helpers.h"
#include "pj_base/sdk/platform.hpp"
#include "pj_marketplace/qt_diagnostic_bridge.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/OnDemandService.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_widgets/CurveTreeView.h"
#include "support/modal_answerer.h"
#include "ui/CurveListPanel.h"

using namespace Qt::StringLiterals;

namespace PJ {

class MainWindowCustomTopicsTestPeer {
 public:
  [[nodiscard]] static AppSession& session(MainWindow& window) {
    return *window.session_;
  }
  // What the panel's Delete runs.
  static void deleteCustomTopics(MainWindow& window, const QStringList& catalog_keys) {
    window.deleteCustomTopics(catalog_keys);
  }
  static void deleteCustomTopic(MainWindow& window, const QString& catalog_key) {
    window.deleteCustomTopic(catalog_key);
  }
  static void syncCustomTopicsPanel(MainWindow& window) {
    window.syncCustomTopicsPanel();
  }
  // What a coalesced state change runs.
  static void scheduledRefresh(MainWindow& window) {
    window.refreshCustomTopicsPanel();
  }
  // These windows load no plugin, so the tests name the toolbox that would declare `custom_topics_editor`.
  static void setCustomTopicsEditorId(MainWindow& window, const QString& plugin_id, bool views = false) {
    window.session_->extensionCatalog().setCustomTopicsEditorIdForTest(plugin_id, views);
    window.syncCustomTopicsEditor();
  }
  [[nodiscard]] static int customTopicsSyncs(const MainWindow& window) {
    return window.custom_topics_syncs_;
  }
  [[nodiscard]] static int customTopicsTransformScans(const MainWindow& window) {
    return window.custom_topics_transform_scans_;
  }
  static void completeFileLoad(MainWindow& window, const QString& path) {
    window.onFileLoaded(path, {}, u"test-loader"_s, u"{}"_s, u"test-loader-id"_s);
  }
  static void undo(MainWindow& window) {
    window.onUndo();
  }
  [[nodiscard]] static std::size_t undoSize(const MainWindow& window) {
    return window.undo_states_.size();
  }
};

}  // namespace PJ

namespace {

using PJ::CurveTreeView;
using PJ::MainWindowCustomTopicsTestPeer;
using PJ::RecipeGraph;

constexpr const char* kEditor = "toolbox-transform-editor";
constexpr const char* kCloudTopic = "cloud";
constexpr const char* kScalarTopic = "sensor";

// A self-contained Luau transform class; `id` must match the transform's user id.
constexpr const char* kNegate = R"LUAU(-- pj-script: luau
return { id="neg", name="Negate", output="same",
  create = function(p) return { calculate = function(t, v) return -v end } end }
)LUAU";
constexpr const char* kNegate2 = R"LUAU(-- pj-script: luau
return { id="neg2", name="Negate", output="same",
  create = function(p) return { calculate = function(t, v) return -v end } end }
)LUAU";

PJ::scripting::OutputSpec cloudOutput(std::string name = "cropped") {
  return {std::move(name), PJ::scripting::OutputType::kObject, PJ::sdk::BuiltinObjectType::kPointCloud};
}

RecipeGraph::Recipe cropRecipe(const std::string& owner, const std::string& id) {
  RecipeGraph::Recipe recipe;
  recipe.owner_plugin = owner;
  recipe.user_id = id;
  recipe.inputs = {kCloudTopic};
  recipe.outputs = {cloudOutput()};
  recipe.script = "local inputs = ... return { cropped = inputs.cloud }";
  return recipe;
}

QTreeWidgetItem* findRowByKey(QTreeWidgetItem* item, const QString& key) {
  if (CurveTreeView::catalogKeyOf(item) == key) {
    return item;
  }
  for (int i = 0; i < item->childCount(); ++i) {
    if (QTreeWidgetItem* found = findRowByKey(item->child(i), key)) {
      return found;
    }
  }
  return nullptr;
}

// Presses button `button` (0 the destructive "Delete", 1 Cancel) of every themed message box that opens while it lives,
// after running `meanwhile` with the box still open: what a user changing the session behind a pending prompt does.
class MessageBoxAnswerer {
 public:
  explicit MessageBoxAnswerer(std::function<void()> meanwhile = {}, int button = 0) : answerer_(5) {
    answerer_.every(
        [button](QWidget* dialog) {
          return pj_app_test::isMessageBox(dialog) && pj_app_test::messageBoxButtons(dialog).size() > button;
        },
        [this, button, meanwhile = std::move(meanwhile)](QWidget* box) {
          ++answered;
          if (meanwhile) {
            meanwhile();
          }
          QMetaObject::invokeMethod(
              pj_app_test::messageBoxButtons(box).at(button), &QAbstractButton::click, Qt::QueuedConnection);
        });
  }
  int answered = 0;

 private:
  pj_app_test::ModalAnswerer answerer_;
};

// One window with a dataset holding an (empty) cloud topic and a scalar topic, and the editor named.
struct CustomTopicsFixture {
  QTemporaryDir extensions_dir;
  PJ::MainWindow window{extensions_dir.path()};
  PJ::CurveListPanel* panel = window.findChild<PJ::CurveListPanel*>(u"curveListPanel"_s);
  CurveTreeView* tree = panel != nullptr ? panel->findChild<CurveTreeView*>(u"treeView"_s) : nullptr;
  CurveTreeView* custom = panel != nullptr ? panel->findChild<CurveTreeView*>(u"customView"_s) : nullptr;
  QToolButton* edit = panel != nullptr ? panel->findChild<QToolButton*>(u"buttonEditCustom"_s) : nullptr;
  QToolButton* add = panel != nullptr ? panel->findChild<QToolButton*>(u"buttonAddCustom"_s) : nullptr;
  PJ::DatasetId dataset = 0;

  [[nodiscard]] PJ::AppSession& app() {
    return MainWindowCustomTopicsTestPeer::session(window);
  }
  [[nodiscard]] PJ::OnDemandService& service() {
    return app().sessionManager().onDemandService();
  }
  [[nodiscard]] bool setUp() {
    if (!extensions_dir.isValid() || panel == nullptr || tree == nullptr || custom == nullptr || edit == nullptr ||
        add == nullptr) {
      return false;
    }
    dataset = pj_test::createDataset(app(), "run.mcap", /*own_time_domain=*/true);
    if (dataset == 0U || pj_test::addScalarTopic(app(), dataset, kScalarTopic) == 0U) {
      return false;
    }
    if (!app()
             .sessionManager()
             .objectStore()
             .registerTopic({dataset, kCloudTopic, R"({"builtin_object_type":"kPointCloud"})"})
             .has_value()) {
      return false;
    }
    app().catalogModel().rebuildFromDatastore();
    MainWindowCustomTopicsTestPeer::setCustomTopicsEditorId(window, QString::fromLatin1(kEditor));
    QObject::connect(
        window.diagnosticBridge(), &PJ::QtDiagnosticBridge::diagnosticReported, &window,
        [this](int /*level*/, const QString& /*source*/, const QString& id, const QString& message) {
          reported.push_back(id + u": "_s + message);
        });
    return true;
  }
  QStringList reported;  ///< "<diagnostic id>: <message>"; the bridge delivers them queued
  // The production install: the session announces it, and the catalog and the panel follow.
  [[nodiscard]] bool install(const RecipeGraph::Recipe& recipe) {
    return service().install(recipe).has_value();
  }
  [[nodiscard]] QString keyOf(const std::string& topic) {
    for (const PJ::CatalogItem& item : app().catalogModel().items()) {
      if (item.topic_name == QString::fromStdString(topic)) {
        return item.key;
      }
    }
    return {};
  }
  [[nodiscard]] QTreeWidgetItem* customRow(const std::string& topic) {
    return findRowByKey(custom->invisibleRootItem(), keyOf(topic));
  }
  void select(const std::string& topic) {
    custom->clearSelection();
    if (QTreeWidgetItem* row = customRow(topic)) {
      row->setSelected(true);
    }
  }
};

// Persistent recipes (a finding included) list their object topics in Custom Topics and leave the Datasets
// tree; a preview is in neither, yet stays resolvable in the catalog; its removal leaves nothing behind.
TEST(MainWindowCustomTopicsTest, CustomTopicsListsPersistentRecipesOnly) {
  CustomTopicsFixture fx;
  ASSERT_TRUE(fx.setUp());
  auto pinned = cropRecipe("owner", "pin");
  pinned.pinned_t = 10'000'000'000;
  auto preview = cropRecipe("owner", "preview");
  preview.ephemeral = true;
  ASSERT_TRUE(fx.install(preview));  // first: the persistent installs below rebuild the Datasets tree around it
  ASSERT_TRUE(fx.install(cropRecipe("owner", "crop")));
  ASSERT_TRUE(fx.install(pinned));

  EXPECT_EQ(fx.panel->customTopicNames(), (QStringList{u"owner/crop/cropped"_s, u"owner/pin/cropped"_s}));
  fx.tree->expandAll();
  for (const char* topic : {"owner/crop/cropped", "owner/pin/cropped"}) {
    const QString key = fx.keyOf(topic);
    ASSERT_FALSE(key.isEmpty()) << topic;
    EXPECT_EQ(findRowByKey(fx.tree->invisibleRootItem(), key), nullptr) << topic << " must leave the Datasets tree";
    EXPECT_NE(fx.customRow(topic), nullptr) << topic;
  }
  const QString preview_key = fx.keyOf("owner/preview/cropped");
  ASSERT_FALSE(preview_key.isEmpty());
  EXPECT_EQ(findRowByKey(fx.tree->invisibleRootItem(), preview_key), nullptr) << "a preview is not a Datasets row";
  EXPECT_EQ(fx.customRow("owner/preview/cropped"), nullptr);
  EXPECT_TRUE(fx.app().catalogModel().ephemeralPreviewKeys().contains(preview_key));

  ASSERT_FALSE(fx.service().remove(preview.key()).empty());
  fx.app().sessionManager().notifyObjectTopicsChanged();
  EXPECT_TRUE(fx.app().catalogModel().ephemeralPreviewKeys().isEmpty());
  EXPECT_TRUE(fx.keyOf("owner/preview/cropped").isEmpty());
  EXPECT_EQ(fx.panel->customTopicNames().size(), 2);
}

// Use-only unless the declared editor made it: the pencil follows the row's owner.
TEST(MainWindowCustomTopicsTest, EditIsEnabledOnlyForEditorRows) {
  CustomTopicsFixture fx;
  ASSERT_TRUE(fx.setUp());
  ASSERT_TRUE(fx.install(cropRecipe("assistant", "crop")));
  ASSERT_TRUE(fx.install(cropRecipe(kEditor, "crop")));

  fx.select("assistant/crop/cropped");
  EXPECT_FALSE(fx.edit->isEnabled());
  EXPECT_EQ(fx.edit->toolTip(), u"Created by assistant; read-only"_s);
  fx.select("toolbox-transform-editor/crop/cropped");
  EXPECT_TRUE(fx.edit->isEnabled());
}

// An editor that declares `custom_topics_viewer` opens other owners' rows as a view: the pencil is enabled on them,
// and says so; Delete stays theirs.
TEST(MainWindowCustomTopicsTest, AViewingEditorOffersThePencilOnOtherOwnersRows) {
  CustomTopicsFixture fx;
  ASSERT_TRUE(fx.setUp());
  ASSERT_TRUE(fx.install(cropRecipe("assistant", "crop")));
  MainWindowCustomTopicsTestPeer::setCustomTopicsEditorId(fx.window, QString::fromLatin1(kEditor), /*views=*/true);

  fx.select("assistant/crop/cropped");
  EXPECT_TRUE(fx.edit->isEnabled());
  EXPECT_EQ(fx.edit->toolTip(), u"View the formula (created by assistant; read-only)"_s);

  MainWindowCustomTopicsTestPeer::setCustomTopicsEditorId(fx.window, QString::fromLatin1(kEditor), /*views=*/false);
  EXPECT_FALSE(fx.edit->isEnabled());
}

// The delete handler refuses another plugin's recipe on its own (the panel disables the action, but nothing
// else may reach it), and the editor's own recipe goes with its topic and rows.
TEST(MainWindowCustomTopicsTest, DeleteOfAnotherPluginsRecipeIsRefused) {
  CustomTopicsFixture fx;
  ASSERT_TRUE(fx.setUp());
  const auto foreign = cropRecipe("assistant", "crop");
  const auto mine = cropRecipe(kEditor, "crop");
  ASSERT_TRUE(fx.install(foreign));
  ASSERT_TRUE(fx.install(mine));
  const QString foreign_key = fx.keyOf("assistant/crop/cropped");
  const QString mine_key = fx.keyOf("toolbox-transform-editor/crop/cropped");
  ASSERT_FALSE(foreign_key.isEmpty());
  ASSERT_FALSE(mine_key.isEmpty());

  MainWindowCustomTopicsTestPeer::deleteCustomTopic(fx.window, foreign_key);
  EXPECT_NE(fx.service().graph().find(foreign.key()), nullptr);
  EXPECT_EQ(fx.panel->customTopicNames().size(), 2);

  MainWindowCustomTopicsTestPeer::deleteCustomTopic(fx.window, mine_key);
  EXPECT_EQ(fx.service().graph().find(mine.key()), nullptr);
  EXPECT_FALSE(fx.app().sessionManager().objectStore().findTopic(fx.dataset, "toolbox-transform-editor/crop/cropped"));
  EXPECT_TRUE(fx.keyOf("toolbox-transform-editor/crop/cropped").isEmpty()) << "gone from the catalog";
  EXPECT_EQ(fx.panel->customTopicNames(), QStringList{u"assistant/crop/cropped"_s});
}

// The same backstop for a transform output: another plugin's is use-only, with no editor declared nothing is
// deletable, and the editor's own goes.
TEST(MainWindowCustomTopicsTest, DeleteOfAnotherPluginsTransformIsRefused) {
  CustomTopicsFixture fx;
  ASSERT_TRUE(fx.setUp());
  PJ::DataProcessorService& transforms = fx.app().sessionManager().dataProcessorService();
  ASSERT_TRUE(transforms.upsertTransform("assistant", "neg", {"sensor/value"}, {"sensor/foreign"}, kNegate, "{}"));
  ASSERT_TRUE(transforms.upsertTransform(kEditor, "neg", {"sensor/value"}, {"sensor/mine"}, kNegate, "{}"));
  fx.app().catalogModel().rebuildFromDatastore();
  MainWindowCustomTopicsTestPeer::syncCustomTopicsPanel(fx.window);
  const QString foreign_key = fx.keyOf("sensor/foreign");
  const QString mine_key = fx.keyOf("sensor/mine");
  ASSERT_FALSE(foreign_key.isEmpty());
  ASSERT_FALSE(mine_key.isEmpty());

  MainWindowCustomTopicsTestPeer::deleteCustomTopic(fx.window, foreign_key);
  EXPECT_EQ(transforms.transformIdsForPlugin("assistant").size(), 1U) << "another plugin's transform is refused";

  MainWindowCustomTopicsTestPeer::setCustomTopicsEditorId(fx.window, QString{});
  MainWindowCustomTopicsTestPeer::deleteCustomTopic(fx.window, mine_key);
  EXPECT_EQ(transforms.transformIdsForPlugin(kEditor).size(), 1U) << "no editor declared";

  MainWindowCustomTopicsTestPeer::setCustomTopicsEditorId(fx.window, QString::fromLatin1(kEditor));
  MainWindowCustomTopicsTestPeer::deleteCustomTopic(fx.window, mine_key);
  EXPECT_TRUE(transforms.transformIdsForPlugin(kEditor).empty()) << "the editor's own goes";
}

// A text series is not plottable, but its Custom Topics row resolves to its transform like any other: Delete removes
// it.
TEST(MainWindowCustomTopicsTest, DeleteOfATextSeriesRemovesItsTransform) {
  constexpr const char* kLabel = R"LUAU(-- pj-script: luau
return { id="label", name="Label", output="string",
  create = function(p) return { calculate = function(t, v) return v > 0 and "up" or "down" end } end }
)LUAU";
  CustomTopicsFixture fx;
  ASSERT_TRUE(fx.setUp());
  PJ::DataProcessorService& transforms = fx.app().sessionManager().dataProcessorService();
  ASSERT_TRUE(transforms.upsertTransform(kEditor, "label", {"sensor/value"}, {"sensor/state"}, kLabel, "{}"));
  fx.app().catalogModel().rebuildFromDatastore();
  MainWindowCustomTopicsTestPeer::syncCustomTopicsPanel(fx.window);
  const QString key = fx.keyOf("sensor/state");
  ASSERT_FALSE(key.isEmpty());

  MainWindowCustomTopicsTestPeer::deleteCustomTopic(fx.window, key);
  EXPECT_TRUE(transforms.transformIdsForPlugin(kEditor).empty()) << "the text series' transform goes";
}

// A transform's row is its own output topic, found by topic id. A create refuses an output name already in use, but
// a topic of that name can arrive later in another dataset (a stream, a reload); in an earlier dataset a name lookup
// finds it first. That topic stays in the Datasets tree.
TEST(MainWindowCustomTopicsTest, ATransformRowIsItsOutputNotATopicOfTheSameName) {
  CustomTopicsFixture fx;
  ASSERT_TRUE(fx.setUp());
  const PJ::DatasetId second = pj_test::createDataset(fx.app(), "second.mcap", /*own_time_domain=*/true);
  ASSERT_NE(second, 0U);
  ASSERT_NE(pj_test::addScalarTopic(fx.app(), second, "other"), 0U);
  PJ::DataProcessorService& transforms = fx.app().sessionManager().dataProcessorService();
  const auto created = transforms.upsertTransform(kEditor, "neg", {"other/value"}, {"out"}, kNegate, "{}");
  ASSERT_TRUE(created.has_value()) << created.error();
  ASSERT_NE(pj_test::addScalarTopic(fx.app(), fx.dataset, "out"), 0U) << "a topic named like the output arrives";
  fx.app().catalogModel().rebuildFromDatastore();
  MainWindowCustomTopicsTestPeer::syncCustomTopicsPanel(fx.window);

  QString loaded_key;
  QString output_key;
  for (const PJ::CatalogItem& item : fx.app().catalogModel().items()) {
    if (item.topic_name == u"out"_s) {
      (item.dataset_id == second ? output_key : loaded_key) = item.key;
    }
  }
  ASSERT_FALSE(loaded_key.isEmpty());
  ASSERT_FALSE(output_key.isEmpty()) << "the transform writes into its input's dataset";
  EXPECT_NE(findRowByKey(fx.custom->invisibleRootItem(), output_key), nullptr) << "the transform's output is listed";
  EXPECT_EQ(findRowByKey(fx.custom->invisibleRootItem(), loaded_key), nullptr) << "the loaded topic is not";
  EXPECT_NE(findRowByKey(fx.tree->invisibleRootItem(), loaded_key), nullptr) << "it stays under Datasets";
  EXPECT_EQ(findRowByKey(fx.tree->invisibleRootItem(), output_key), nullptr);
}

// A grouped output ("t:a,b", one topic with a column per field) moves to Custom Topics whole: every column is listed
// there and none stays under Datasets.
TEST(MainWindowCustomTopicsTest, AGroupedTransformOutputMovesWholeToCustomTopics) {
  constexpr const char* kSplit = R"LUAU(-- pj-script: luau
return { id="split", name="Split",
  create = function(p) return { calculate = function(t, v) return v, -v end } end }
)LUAU";
  CustomTopicsFixture fx;
  ASSERT_TRUE(fx.setUp());
  PJ::DataProcessorService& transforms = fx.app().sessionManager().dataProcessorService();
  const auto created = transforms.upsertTransform(kEditor, "split", {"sensor/value"}, {"pair:a,b"}, kSplit, "{}");
  ASSERT_TRUE(created.has_value()) << created.error();
  fx.app().catalogModel().rebuildFromDatastore();
  MainWindowCustomTopicsTestPeer::syncCustomTopicsPanel(fx.window);

  QStringList keys;
  for (const PJ::CatalogItem& item : fx.app().catalogModel().items()) {
    if (item.topic_name == u"pair"_s) {
      keys << item.key;
    }
  }
  ASSERT_EQ(keys.size(), 2) << "one topic, a column per field";
  for (const QString& key : keys) {
    const QTreeWidgetItem* row = findRowByKey(fx.custom->invisibleRootItem(), key);
    ASSERT_NE(row, nullptr) << key.toStdString();
    EXPECT_EQ(findRowByKey(fx.tree->invisibleRootItem(), key), nullptr) << key.toStdString();
    EXPECT_TRUE(row->text(0) == u"pair/a"_s || row->text(0) == u"pair/b"_s) << row->text(0).toStdString();
  }

  // Deleting its rows together removes the transform once; the rows after the first are skipped.
  MainWindowCustomTopicsTestPeer::deleteCustomTopics(fx.window, keys);
  EXPECT_TRUE(transforms.transformIdsForPlugin(kEditor).empty());
  for (const QString& key : keys) {
    EXPECT_EQ(findRowByKey(fx.custom->invisibleRootItem(), key), nullptr) << key.toStdString();
  }
}

// Deleting the editor's transform never cascades into another plugin's: a reader owned elsewhere refuses the
// delete up front, by name, and nothing is removed.
TEST(MainWindowCustomTopicsTest, DeleteCascadingIntoAnotherPluginsTransformIsRefused) {
  CustomTopicsFixture fx;
  ASSERT_TRUE(fx.setUp());
  PJ::DataProcessorService& transforms = fx.app().sessionManager().dataProcessorService();
  ASSERT_TRUE(transforms.upsertTransform(kEditor, "neg", {"sensor/value"}, {"sensor/mine"}, kNegate, "{}"));
  ASSERT_TRUE(transforms.upsertTransform("assistant", "neg", {"sensor/mine"}, {"sensor/foreign"}, kNegate, "{}"));
  fx.app().catalogModel().rebuildFromDatastore();
  MainWindowCustomTopicsTestPeer::syncCustomTopicsPanel(fx.window);
  const QString mine_key = fx.keyOf("sensor/mine");
  ASSERT_FALSE(mine_key.isEmpty());

  const MessageBoxAnswerer answerer;  // a prompt would delete both
  MainWindowCustomTopicsTestPeer::deleteCustomTopic(fx.window, mine_key);
  QApplication::processEvents();
  EXPECT_EQ(answerer.answered, 0) << "refused before any prompt";
  EXPECT_EQ(transforms.transformIdsForPlugin(kEditor).size(), 1U);
  EXPECT_EQ(transforms.transformIdsForPlugin("assistant").size(), 1U) << "another plugin's transform stays";
  ASSERT_EQ(fx.reported.size(), 1) << fx.reported.join(u"; "_s).toStdString();
  EXPECT_EQ(
      fx.reported.front(), u"delete-cascade-read-only: 'neg' cannot be deleted: 'neg' of assistant reads its output"_s);
}

// The owner is checked again when the prompt is answered: an editor unloaded while the dependents prompt of an
// on-demand delete is open leaves its recipe in place.
TEST(MainWindowCustomTopicsTest, OnDemandDeleteRechecksTheOwnerAfterThePrompt) {
  CustomTopicsFixture fx;
  ASSERT_TRUE(fx.setUp());
  const auto upstream = cropRecipe(kEditor, "crop");
  auto reader = cropRecipe(kEditor, "reader");
  reader.inputs = {upstream.key() + "/cropped"};
  reader.script = "local inputs = ... return { cropped = inputs['" + upstream.key() + "/cropped'] }";
  ASSERT_TRUE(fx.install(upstream));
  ASSERT_TRUE(fx.install(reader));
  const QString key = fx.keyOf("toolbox-transform-editor/crop/cropped");
  ASSERT_FALSE(key.isEmpty());

  const MessageBoxAnswerer answerer(
      [&fx]() { MainWindowCustomTopicsTestPeer::setCustomTopicsEditorId(fx.window, QString{}); });
  MainWindowCustomTopicsTestPeer::deleteCustomTopic(fx.window, key);
  QApplication::processEvents();
  EXPECT_EQ(answerer.answered, 1);
  EXPECT_NE(fx.service().graph().find(upstream.key()), nullptr) << "no longer the editor's to delete";
  EXPECT_TRUE(fx.reported.join(u"; "_s).contains(u"delete-read-only"_s)) << fx.reported.join(u"; "_s).toStdString();
}

// Selecting several rows of one recipe (its `id/output` rows) and pressing Delete asks about the recipe once:
// a Cancel is not asked again for the next row, and a Delete removes it as one undo step.
TEST(MainWindowCustomTopicsTest, DeletingTwoRowsOfOneRecipeDeletesItOnce) {
  CustomTopicsFixture fx;
  ASSERT_TRUE(fx.setUp());
  RecipeGraph::Recipe pair = cropRecipe(kEditor, "pair");
  pair.outputs = {cloudOutput("left"), cloudOutput("right")};
  pair.script = "local inputs = ... return { left = inputs.cloud, right = inputs.cloud }";
  auto reader = cropRecipe(kEditor, "reader");
  reader.inputs = {pair.key() + "/left"};
  reader.script = "local inputs = ... return { cropped = inputs['" + pair.key() + "/left'] }";
  ASSERT_TRUE(fx.install(pair));
  ASSERT_TRUE(fx.install(reader));
  QTreeWidgetItem* left = fx.customRow("toolbox-transform-editor/pair/left");
  QTreeWidgetItem* right = fx.customRow("toolbox-transform-editor/pair/right");
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  fx.custom->clearSelection();
  left->setSelected(true);
  right->setSelected(true);
  QPushButton* remove = nullptr;
  for (QPushButton* button : fx.panel->findChildren<QPushButton*>()) {
    if (button->text() == u"Delete"_s) {
      remove = button;
    }
  }
  ASSERT_NE(remove, nullptr);
  ASSERT_TRUE(remove->isEnabled());
  const std::size_t before = MainWindowCustomTopicsTestPeer::undoSize(fx.window);

  {
    const MessageBoxAnswerer cancel({}, /*button=*/1);
    remove->click();
    QApplication::processEvents();
    EXPECT_EQ(cancel.answered, 1) << "one prompt for the recipe, not one per row";
    ASSERT_NE(fx.service().graph().find(pair.key()), nullptr);
  }
  ASSERT_TRUE(left->isSelected() && right->isSelected());
  const MessageBoxAnswerer answerer;
  remove->click();
  QApplication::processEvents();
  EXPECT_EQ(answerer.answered, 1);
  EXPECT_EQ(fx.service().graph().find(pair.key()), nullptr);
  EXPECT_EQ(MainWindowCustomTopicsTestPeer::undoSize(fx.window), before + 1) << "one undo step";
}

// The same recheck on the transform path, before the dependents go: unloading the editor behind the prompt
// leaves the whole chain.
TEST(MainWindowCustomTopicsTest, TransformDeleteRechecksTheOwnerBeforeRemovingDependents) {
  CustomTopicsFixture fx;
  ASSERT_TRUE(fx.setUp());
  PJ::DataProcessorService& transforms = fx.app().sessionManager().dataProcessorService();
  ASSERT_TRUE(transforms.upsertTransform(kEditor, "neg", {"sensor/value"}, {"sensor/mine"}, kNegate, "{}"));
  ASSERT_TRUE(transforms.upsertTransform(kEditor, "neg2", {"sensor/mine"}, {"sensor/chain"}, kNegate2, "{}"));
  fx.app().catalogModel().rebuildFromDatastore();
  MainWindowCustomTopicsTestPeer::syncCustomTopicsPanel(fx.window);
  const QString mine_key = fx.keyOf("sensor/mine");
  ASSERT_FALSE(mine_key.isEmpty());

  const MessageBoxAnswerer answerer(
      [&fx]() { MainWindowCustomTopicsTestPeer::setCustomTopicsEditorId(fx.window, QString{}); });
  MainWindowCustomTopicsTestPeer::deleteCustomTopic(fx.window, mine_key);
  QApplication::processEvents();
  EXPECT_EQ(answerer.answered, 1);
  EXPECT_EQ(transforms.transformIdsForPlugin(kEditor).size(), 2U) << "neither the transform nor its dependent goes";
}

// Deleting every column of a grouped output asks once: the columns are one transform, and a cancelled prompt does not
// come back for the next column.
TEST(MainWindowCustomTopicsTest, DeletingAGroupedOutputsColumnsAsksOnce) {
  constexpr const char* kSplit = R"LUAU(-- pj-script: luau
return { id="split", name="Split",
  create = function(p) return { calculate = function(t, v) return v, -v end } end }
)LUAU";
  CustomTopicsFixture fx;
  ASSERT_TRUE(fx.setUp());
  PJ::DataProcessorService& transforms = fx.app().sessionManager().dataProcessorService();
  ASSERT_TRUE(transforms.upsertTransform(kEditor, "split", {"sensor/value"}, {"pair:a,b"}, kSplit, "{}"));
  ASSERT_TRUE(transforms.upsertTransform(kEditor, "neg2", {"pair/a"}, {"chain"}, kNegate2, "{}"));
  fx.app().catalogModel().rebuildFromDatastore();
  MainWindowCustomTopicsTestPeer::syncCustomTopicsPanel(fx.window);
  QStringList keys;
  for (const PJ::CatalogItem& item : fx.app().catalogModel().items()) {
    if (item.topic_name == u"pair"_s) {
      keys << item.key;
    }
  }
  ASSERT_EQ(keys.size(), 2);

  const MessageBoxAnswerer cancel({}, /*button=*/1);
  MainWindowCustomTopicsTestPeer::deleteCustomTopics(fx.window, keys);
  QApplication::processEvents();
  EXPECT_EQ(cancel.answered, 1) << "one prompt for the one transform";
  EXPECT_EQ(transforms.transformIdsForPlugin(kEditor).size(), 2U) << "cancelled: nothing goes";
}

// A delete never strands a history-exempt reader: the delete is an undo step, and its redo would be refused
// for the reader it strands. Refused up front, naming the reader, and nothing is removed.
TEST(MainWindowCustomTopicsTest, DeleteReadByAHistoryExemptRecipeIsRefused) {
  CustomTopicsFixture fx;
  ASSERT_TRUE(fx.setUp());
  const auto mine = cropRecipe(kEditor, "crop");
  auto finding = cropRecipe("assistant", "finding");
  finding.inputs = {mine.key() + "/cropped"};
  finding.script = "local inputs = ... return { cropped = inputs['" + mine.key() + "/cropped'] }";
  finding.history_exempt = true;
  ASSERT_TRUE(fx.install(mine));
  ASSERT_TRUE(fx.install(finding));

  const MessageBoxAnswerer answerer;  // a prompt would delete it
  MainWindowCustomTopicsTestPeer::deleteCustomTopic(fx.window, fx.keyOf("toolbox-transform-editor/crop/cropped"));
  QApplication::processEvents();
  EXPECT_EQ(answerer.answered, 0) << "refused before any prompt";
  EXPECT_NE(fx.service().graph().find(mine.key()), nullptr);
  ASSERT_EQ(fx.reported.size(), 1) << fx.reported.join(u"; "_s).toStdString();
  EXPECT_EQ(
      fx.reported.front(),
      u"delete-exempt-reader: 'crop' cannot be deleted: the protected 'assistant/finding/cropped' reads its output"_s);
}

// Across a file load: a derived topic is no part of the data universe history is checked against, so the load
// after a delete keeps the undo step, and the undo brings the recipe back.
TEST(MainWindowCustomTopicsTest, ADeleteKeepsItsUndoAcrossTheNextFileLoad) {
  CustomTopicsFixture fx;
  ASSERT_TRUE(fx.setUp());
  const auto mine = cropRecipe(kEditor, "crop");
  ASSERT_TRUE(fx.install(mine));
  // A load while the derived topic exists: history is reconciled with the session's data.
  MainWindowCustomTopicsTestPeer::completeFileLoad(fx.window, u"/tmp/run.mcap"_s);
  const std::size_t before = MainWindowCustomTopicsTestPeer::undoSize(fx.window);

  MainWindowCustomTopicsTestPeer::deleteCustomTopic(fx.window, fx.keyOf("toolbox-transform-editor/crop/cropped"));
  ASSERT_EQ(fx.service().graph().find(mine.key()), nullptr);
  ASSERT_EQ(MainWindowCustomTopicsTestPeer::undoSize(fx.window), before + 1);

  const PJ::DatasetId second = pj_test::createDataset(fx.app(), "second.mcap", /*own_time_domain=*/true);
  ASSERT_NE(pj_test::addScalarTopic(fx.app(), second, "other"), 0U);
  fx.app().catalogModel().rebuildFromDatastore();
  MainWindowCustomTopicsTestPeer::completeFileLoad(fx.window, u"/tmp/second.mcap"_s);
  EXPECT_EQ(MainWindowCustomTopicsTestPeer::undoSize(fx.window), before + 1) << "the load keeps the history";
  MainWindowCustomTopicsTestPeer::undo(fx.window);
  EXPECT_NE(fx.service().graph().find(mine.key()), nullptr) << "the undo brings the recipe back";
}

// The scheduled refresh (a node state, an offset) rebuilds the on-demand rows only. The transform rows come from
// the last full sync, rebuilt once more after the catalog changed.
TEST(MainWindowCustomTopicsTest, AScheduledRefreshReusesTheTransformRows) {
  CustomTopicsFixture fx;
  ASSERT_TRUE(fx.setUp());
  PJ::DataProcessorService& transforms = fx.app().sessionManager().dataProcessorService();
  ASSERT_TRUE(transforms.upsertTransform(kEditor, "neg", {"sensor/value"}, {"sensor/mine"}, kNegate, "{}"));
  fx.app().catalogModel().rebuildFromDatastore();
  MainWindowCustomTopicsTestPeer::syncCustomTopicsPanel(fx.window);
  const auto recipe = cropRecipe(kEditor, "crop");
  ASSERT_TRUE(fx.install(recipe));
  QApplication::processEvents();
  const int scans = MainWindowCustomTopicsTestPeer::customTopicsTransformScans(fx.window);
  const int syncs = MainWindowCustomTopicsTestPeer::customTopicsSyncs(fx.window);

  for (int i = 0; i < 10; ++i) {
    emit fx.app().sessionManager().derivedStateChanged(QString::fromStdString(recipe.key()), {});
  }
  QApplication::processEvents();
  EXPECT_EQ(MainWindowCustomTopicsTestPeer::customTopicsSyncs(fx.window), syncs + 1);
  EXPECT_EQ(MainWindowCustomTopicsTestPeer::customTopicsTransformScans(fx.window), scans) << "no transform is read";
  EXPECT_EQ(fx.panel->customTopicNames(), (QStringList{u"sensor/mine"_s, u"toolbox-transform-editor/crop/cropped"_s}));

  ASSERT_TRUE(transforms.upsertTransform(kEditor, "neg2", {"sensor/value"}, {"sensor/chain"}, kNegate2, "{}"));
  fx.app().catalogModel().rebuildFromDatastore();
  emit fx.app().sessionManager().derivedStateChanged(QString::fromStdString(recipe.key()), {});
  QApplication::processEvents();
  EXPECT_EQ(MainWindowCustomTopicsTestPeer::customTopicsTransformScans(fx.window), scans + 1) << "the catalog changed";
  EXPECT_TRUE(fx.panel->customTopicNames().contains(u"sensor/chain"_s));
}

// "+" exists only while a toolbox declares `custom_topics_editor`; with none every row is read-only.
TEST(MainWindowCustomTopicsTest, NoDeclaredEditorHidesPlusAndMakesRowsReadOnly) {
  CustomTopicsFixture fx;
  ASSERT_TRUE(fx.setUp());
  ASSERT_TRUE(fx.install(cropRecipe(kEditor, "crop")));
  fx.select("toolbox-transform-editor/crop/cropped");
  EXPECT_FALSE(fx.add->isHidden());
  EXPECT_TRUE(fx.edit->isEnabled());

  MainWindowCustomTopicsTestPeer::setCustomTopicsEditorId(fx.window, QString{});
  EXPECT_TRUE(fx.add->isHidden());
  EXPECT_FALSE(fx.edit->isEnabled());
}

// Rows are flat and named by recipe (the id, or `id/output` for several object outputs); an external owner
// is badged, by its id when it is not loaded; the tooltip names the dataset.
TEST(MainWindowCustomTopicsTest, CustomTopicsRowsAreFlatNamedByRecipe) {
  CustomTopicsFixture fx;
  ASSERT_TRUE(fx.setUp());
  auto multi = cropRecipe("assistant", "pair");
  multi.outputs = {cloudOutput("left"), cloudOutput("right")};
  multi.script = "local inputs = ... return { left = inputs.cloud, right = inputs.cloud }";
  ASSERT_TRUE(fx.install(cropRecipe("assistant", "lidar_top_crop")));
  ASSERT_TRUE(fx.install(multi));
  ASSERT_TRUE(fx.install(cropRecipe(kEditor, "mine")));

  QTreeWidgetItem* crop = fx.customRow("assistant/lidar_top_crop/cropped");
  QTreeWidgetItem* left = fx.customRow("assistant/pair/left");
  QTreeWidgetItem* right = fx.customRow("assistant/pair/right");
  QTreeWidgetItem* mine = fx.customRow("toolbox-transform-editor/mine/cropped");
  ASSERT_NE(crop, nullptr);
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  ASSERT_NE(mine, nullptr);
  EXPECT_EQ(fx.custom->topLevelItemCount(), 4) << "no dataset / owner / recipe groups";
  EXPECT_EQ(crop->text(0), u"lidar_top_crop"_s);
  EXPECT_EQ(left->text(0), u"pair/left"_s);
  EXPECT_EQ(right->text(0), u"pair/right"_s);
  EXPECT_EQ(mine->text(0), u"mine"_s);
  EXPECT_EQ(CurveTreeView::badgeOf(crop), u"assistant"_s) << "an owner not loaded shows its id";
  EXPECT_TRUE(CurveTreeView::badgeOf(mine).isEmpty());
  EXPECT_TRUE(crop->toolTip(0).contains(u"dataset run.mcap"_s)) << crop->toolTip(0).toStdString();
}

// Removing a finding, announced by whoever removed it, takes its row out of Custom Topics
// and the catalog with no further call from the shell.
TEST(MainWindowCustomTopicsTest, RemovingAFindingRemovesItsRowEverywhere) {
  CustomTopicsFixture fx;
  ASSERT_TRUE(fx.setUp());
  auto pin = cropRecipe("assistant", "pin");
  pin.pinned_t = 10'000'000'000;
  pin.history_exempt = true;
  ASSERT_TRUE(fx.install(pin));
  ASSERT_EQ(fx.panel->customTopicNames(), QStringList{u"assistant/pin/cropped"_s});

  ASSERT_FALSE(fx.service().remove(pin.key()).empty());
  fx.app().sessionManager().notifyObjectTopicsChanged();
  EXPECT_TRUE(fx.panel->customTopicNames().isEmpty());
  EXPECT_TRUE(fx.keyOf("assistant/pin/cropped").isEmpty());
}

// Through the session: the row glyph follows the node state, and a burst of state changes is one refresh
// at the next event-loop turn, not one per change.
TEST(MainWindowCustomTopicsTest, RowGlyphFollowsTheNodeStateAndBurstsCoalesce) {
  CustomTopicsFixture fx;
  ASSERT_TRUE(fx.setUp());
  const auto recipe = cropRecipe(kEditor, "crop");
  ASSERT_TRUE(fx.install(recipe));
  QApplication::processEvents();

  fx.service().requestAt(recipe.key(), 1'000'000'000);  // the cloud topic has no entry: a missing input
  QApplication::processEvents();
  CurveTreeView::CurvePath spec;
  spec.dataset = u"run.mcap"_s;
  spec.topic = u"toolbox-transform-editor/crop/cropped"_s;
  const QString path = CurveTreeView::treePathFromCurvePath(spec);
  EXPECT_EQ(fx.custom->derivedGlyphStateAt(path), CurveTreeView::DerivedGlyphState::kMissingInput);
  QTreeWidgetItem* row = fx.customRow("toolbox-transform-editor/crop/cropped");
  ASSERT_NE(row, nullptr);
  EXPECT_TRUE(row->toolTip(0).contains(u"Missing input"_s)) << row->toolTip(0).toStdString();

  const int before = MainWindowCustomTopicsTestPeer::customTopicsSyncs(fx.window);
  for (int i = 0; i < 100; ++i) {
    emit fx.app().sessionManager().derivedStateChanged(QString::fromStdString(recipe.key()), {});
  }
  EXPECT_EQ(MainWindowCustomTopicsTestPeer::customTopicsSyncs(fx.window), before) << "nothing runs synchronously";
  QApplication::processEvents();
  EXPECT_EQ(MainWindowCustomTopicsTestPeer::customTopicsSyncs(fx.window), before + 1);
}

// Through the shell: a finding's "Pinned at" follows the Source Timeline: a drag of its dataset (the
// per-dataset offset signal) re-words it although no recipe row changed.
TEST(MainWindowCustomTopicsTest, PinnedTooltipFollowsTheDatasetOffset) {
  CustomTopicsFixture fx;
  ASSERT_TRUE(fx.setUp());
  auto pin = cropRecipe("assistant", "pin");
  pin.pinned_t = 10'000'000'000;
  ASSERT_TRUE(fx.install(pin));
  QApplication::processEvents();
  const QRegularExpression pinned(u"Pinned at (-?[0-9.]+) s"_s);
  const auto pinned_seconds = [&]() -> double {
    QTreeWidgetItem* row = fx.customRow("assistant/pin/cropped");
    const QRegularExpressionMatch match = pinned.match(row != nullptr ? row->toolTip(0) : QString{});
    return match.hasMatch() ? match.captured(1).toDouble() : -1e9;
  };
  const double before = pinned_seconds();
  ASSERT_GT(before, -1e9);

  fx.app().sessionManager().setDisplayOffset(fx.dataset, PJ::DisplayOffset{PJ::Duration{2'000'000'000}});
  QApplication::processEvents();
  EXPECT_DOUBLE_EQ(pinned_seconds(), before - 2.0);
}

// Median microseconds of `runs` calls of `work`.
template <typename Work>
double medianMicros(int runs, const Work& work) {
  std::vector<double> samples;
  for (int run = 0; run < runs; ++run) {
    const auto start = std::chrono::steady_clock::now();
    work(run);
    samples.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
  }
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

// PJ_BENCH=1: the Custom Topics refresh at 1,000 derived rows plus 20 transform outputs. The full sync (an install, a
// restore, a toolbox write) derives every row from the services; the scheduled refresh is what every coalesced state
// change costs during streaming or a Timeline drag when nothing visible changed (the on-demand rows, the transform rows
// of the last full sync, the panel's compare); a state-only change repaints the glyphs and tooltips of every row; a
// renamed set refills the view.
TEST(MainWindowCustomTopicsTest, ThousandRowRefreshBench) {
  if (!PJ::sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  constexpr int kRows = 1000;
  constexpr int kTransforms = 20;
  CustomTopicsFixture fx;
  ASSERT_TRUE(fx.setUp());
  for (int i = 0; i < kRows; ++i) {
    ASSERT_TRUE(fx.install(cropRecipe(kEditor, "r" + std::to_string(i))));
  }
  PJ::DataProcessorService& transforms = fx.app().sessionManager().dataProcessorService();
  for (int i = 0; i < kTransforms; ++i) {
    const std::string id = "t" + std::to_string(i);
    const std::string script = "-- pj-script: luau\nreturn { id=\"" + id +
                               "\", name=\"Negate\", output=\"same\",\n"
                               "  create = function(p) return { calculate = function(t, v) return -v end } end }\n";
    ASSERT_TRUE(transforms.upsertTransform(kEditor, id, {"sensor/value"}, {"sensor/" + id}, script, "{}"));
  }
  fx.app().catalogModel().rebuildFromDatastore();
  MainWindowCustomTopicsTestPeer::syncCustomTopicsPanel(fx.window);
  ASSERT_EQ(fx.panel->customTopicNames().size(), kRows + kTransforms);

  const double full = medianMicros(21, [&](int) { MainWindowCustomTopicsTestPeer::syncCustomTopicsPanel(fx.window); });
  const double unchanged = medianMicros(21, [&](int) { MainWindowCustomTopicsTestPeer::scheduledRefresh(fx.window); });

  std::vector<PJ::CurveListPanel::CustomTopicRow> rows;
  for (const PJ::CatalogItem& item : fx.app().catalogModel().items()) {
    if (PJ::isObjectTopic(item) && item.topic_name.startsWith(QString::fromLatin1(kEditor))) {
      rows.push_back({.key = item.key, .owner = QString::fromLatin1(kEditor), .name = item.topic_name});
    }
  }
  ASSERT_EQ(rows.size(), static_cast<size_t>(kRows));
  const double states = medianMicros(21, [&](int run) {
    PJ::NodeScheduler::NodeState state;
    state.status =
        run % 2 == 0 ? PJ::NodeScheduler::NodeState::Status::kPending : PJ::NodeScheduler::NodeState::Status::kError;
    state.detail = "detail";
    for (auto& row : rows) {
      row.state = state;
    }
    fx.panel->setCustomTopics(rows);
  });
  const double refill = medianMicros(11, [&](int run) {
    for (auto& row : rows) {
      row.name = row.key + QString::number(run);
    }
    fx.panel->setCustomTopics(rows);
  });
  std::printf(
      "[PJ_BENCH] Custom Topics, %d rows + %d transforms: full sync %.0f us, scheduled refresh %.0f us (%.0f ns/row), "
      "state-only repaint %.0f us (%.0f ns/row), refill %.0f us (%.0f ns/row)\n",
      kRows, kTransforms, full, unchanged, unchanged * 1000.0 / kRows, states, states * 1000.0 / kRows, refill,
      refill * 1000.0 / kRows);
}

}  // namespace
