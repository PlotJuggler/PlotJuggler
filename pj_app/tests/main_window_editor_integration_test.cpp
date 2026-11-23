// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The Custom Topics editor through MainWindow: the toolbox whose manifest declares the `custom_topics_editor`
// capability is the editor, the panel's "+" opens it, its preview frame's scene view is embedded with the requested
// topic attached, and a curve-tree drop onto the panel hands the plugin names it can address. The toolbox is a test
// fixture (custom_topics_editor_fixture.cpp), staged into the extensions directory before the window scans it.

#include <gtest/gtest.h>

#include <QDomDocument>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QLabel>
#include <QMimeData>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidgetItemIterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>

#include "MainWindow.h"
#include "dataset_test_helpers.h"
#include "event_pump_test_utils.h"
#include "pj_base/sdk/object_topic_metadata.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plugins/host_qt/drop_event_filter.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/OnDemandService.h"
#include "pj_runtime/SessionManager.h"
#include "pj_scene_common/scene_dock_widget.h"
#include "pj_widgets/CurveTreeView.h"
#include "ui/CurveListPanel.h"

#ifndef PJ_CUSTOM_TOPICS_EDITOR_FIXTURE_PATH
#error "PJ_CUSTOM_TOPICS_EDITOR_FIXTURE_PATH must be defined"
#endif

using namespace Qt::StringLiterals;

namespace PJ {

class MainWindowEditorIntegrationTestPeer {
 public:
  [[nodiscard]] static AppSession& session(MainWindow& window) {
    return *window.session_;
  }
  static void syncCustomTopicsPanel(MainWindow& window) {
    window.syncCustomTopicsPanel();
  }
  [[nodiscard]] static bool isLaunched(const MainWindow& window, const QString& plugin_id) {
    return window.findToolbox(plugin_id) != nullptr;
  }
  // The user pins the launched panel as a tab, which is what puts it in a saved layout.
  static void pin(MainWindow& window, const QString& plugin_id) {
    window.withToolbox(plugin_id, [](ToolboxPresenter& live) { live.moveToTab(/*transient=*/false); });
  }
  [[nodiscard]] static QString savedPinnedToolboxesXml(const MainWindow& window) {
    QDomDocument doc;
    doc.appendChild(window.savePinnedToolboxes(doc));
    return doc.toString();
  }
};

}  // namespace PJ

namespace {

using Peer = PJ::MainWindowEditorIntegrationTestPeer;

// Puts the custom-topics editor fixture where a MainWindow built on `extensions_dir` finds it.
void stageEditorFixture(const QTemporaryDir& extensions_dir) {
  ASSERT_TRUE(extensions_dir.isValid());
  const QString fixture = QString::fromUtf8(PJ_CUSTOM_TOPICS_EDITOR_FIXTURE_PATH);
  ASSERT_TRUE(QFile::copy(fixture, extensions_dir.filePath(QFileInfo(fixture).fileName())));
}

PJ::Expected<PJ::ObjectTopicId> registerCloud(PJ::AppSession& app, PJ::DatasetId dataset) {
  const auto metadata =
      PJ::sdk::ObjectTopicMetadataBuilder().builtinObjectType(PJ::sdk::BuiltinObjectType::kPointCloud).build();
  if (!metadata) {
    return PJ::unexpected(metadata.error());
  }
  return app.sessionManager().objectStore().registerTopic({dataset, "/cloud", *metadata});
}

// The "+" of the curve list panel, which launches the declared editor.
QToolButton* addCustomButton(PJ::MainWindow& window) {
  auto* panel = window.findChild<PJ::CurveListPanel*>(u"curveListPanel"_s);
  return panel != nullptr ? panel->findChild<QToolButton*>(u"buttonAddCustom"_s) : nullptr;
}

// "+" launches the declared editor, and the `scene_view` of its preview frame is a 3D scene embedded in that frame,
// showing the topic it names.
TEST(MainWindowEditorIntegrationTest, PlusOpensTheDeclaredEditorAndItsPreviewEmbedsAScene) {
#ifndef PJ_WITH_SCENE3D
  GTEST_SKIP() << "this build embeds no 3D scene view";
#else
  QTemporaryDir extensions_dir;
  stageEditorFixture(extensions_dir);
  ASSERT_FALSE(HasFatalFailure());
  PJ::MainWindow window(extensions_dir.path());
  PJ::AppSession& app = Peer::session(window);
  ASSERT_EQ(app.extensionCatalog().customTopicsEditorId(), u"custom-topics-editor-fixture"_s)
      << "the editor is the toolbox declaring the capability";

  const PJ::DatasetId dataset = pj_test::createDataset(app, "drive", true);
  ASSERT_NE(dataset, 0U);
  ASSERT_TRUE(registerCloud(app, dataset).has_value());
  app.catalogModel().rebuildFromDatastore();

  QToolButton* add = addCustomButton(window);
  ASSERT_NE(add, nullptr);
  ASSERT_FALSE(add->isHidden()) << "the declared editor shows the \"+\"";
  add->click();

  QFrame* preview = nullptr;
  PJ::SceneDockWidget* scene = nullptr;
  PJ::test::pumpUntil([&] {
    preview = window.findChild<QFrame*>(u"scenePreview"_s);
    scene = preview != nullptr ? preview->findChild<PJ::SceneDockWidget*>() : nullptr;
    return scene != nullptr && scene->layers().size() == 1;
  });
  ASSERT_NE(preview, nullptr) << "the \"+\" opened the editor's panel";
  ASSERT_NE(scene, nullptr) << "its preview frame embeds a scene view";
  ASSERT_EQ(scene->layers().size(), 1U) << "showing the topic the frame names";
  const auto cloud = app.sessionManager().objectStore().findTopic(dataset, "/cloud");
  ASSERT_TRUE(cloud.has_value());
  EXPECT_EQ(scene->layers().front().topic_id.id, cloud->id);
#endif
}

// A drop onto a plugin panel carries opaque catalog keys; the plugin receives an object topic by its topic name and a
// scalar field by its series key, the names it addresses them by, each with its data source: two datasets' same-named
// series differ only there.
TEST(MainWindowEditorIntegrationTest, DropOnThePanelDeliversNamesAndDataSources) {
  QTemporaryDir extensions_dir;
  stageEditorFixture(extensions_dir);
  ASSERT_FALSE(HasFatalFailure());
  PJ::MainWindow window(extensions_dir.path());
  PJ::AppSession& app = Peer::session(window);
  // Shown, so the panel is laid out: the drop filter finds its target by geometry.
  window.show();

  const PJ::DatasetId dataset = pj_test::createDataset(app, "drive", true);
  ASSERT_NE(dataset, 0U);
  const auto cloud = registerCloud(app, dataset);
  ASSERT_TRUE(cloud.has_value());
  const PJ::TopicId speed = pj_test::addScalarTopic(app, dataset, "/speed", 1'000, 2'000);
  ASSERT_NE(speed, 0U);
  const std::optional<QString> cloud_key = app.catalogModel().objectTopicKey(dataset, *cloud);
  const std::optional<QString> speed_key = app.catalogModel().scalarColumnKey(dataset, speed, 0);
  ASSERT_TRUE(cloud_key.has_value());
  ASSERT_TRUE(speed_key.has_value());
  const PJ::DatasetId other_dataset = pj_test::createDataset(app, "other", true);
  ASSERT_NE(other_dataset, 0U);
  const PJ::TopicId other_speed = pj_test::addScalarTopic(app, other_dataset, "/speed", 1'000, 2'000);
  ASSERT_NE(other_speed, 0U);
  const std::optional<QString> other_speed_key = app.catalogModel().scalarColumnKey(other_dataset, other_speed, 0);
  ASSERT_TRUE(other_speed_key.has_value());

  QToolButton* add = addCustomButton(window);
  ASSERT_NE(add, nullptr);
  add->click();

  QWidget* root = nullptr;
  QLabel* target = nullptr;
  PJ::test::pumpUntil([&] {
    root = window.findChild<QWidget*>(u"EditorFixture"_s);
    target = root != nullptr ? root->findChild<QLabel*>(u"droppedItems"_s) : nullptr;
    return target != nullptr && target->isVisible();
  });
  ASSERT_NE(target, nullptr) << "the \"+\" opened the editor's panel";
  ASSERT_TRUE(target->isVisible());

  QMimeData mime;
  mime.setData(
      PJ::CurveTreeView::catalogItemsMimeType(),
      PJ::CurveTreeView::encodeCatalogKeys({*cloud_key, *speed_key, *other_speed_key}));
  QDropEvent drop(
      QPointF(target->mapTo(root, target->rect().center())), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
  // Qt delivers a Drop only to the target of a drag in progress, so the synthesized one goes to the panel's filter.
  auto* drop_filter = root->findChild<PJ::DropEventFilter*>(Qt::FindDirectChildrenOnly);
  ASSERT_NE(drop_filter, nullptr) << "the panel declares a drop target";
  EXPECT_TRUE(static_cast<QObject*>(drop_filter)->eventFilter(root, &drop));

  const QString expected = u"/cloud@%1\n/speed/value@%1\n/speed/value@%2"_s.arg(dataset).arg(other_dataset);
  PJ::test::pumpUntil([&] { return target->text() == expected; });
  EXPECT_EQ(target->text().toStdString(), expected.toStdString());
}

// A self-contained Luau transform class; its id is the transform's user id, escaped the way an MCP agent's series
// "speed/neg" is.
constexpr const char* kNegate = R"LUAU(-- pj-script: luau
return { id="speed_2fneg", name="Negate", output="same",
  create = function(p) return { calculate = function(t, v) return -v end } end }
)LUAU";

// Selects the Custom Topics row of `topic`, presses the pencil and returns the config the editor was launched with.
std::optional<std::string> pencilConfig(PJ::MainWindow& window, const QString& topic) {
  PJ::AppSession& app = Peer::session(window);
  QString key;
  for (const PJ::CatalogItem& item : app.catalogModel().items()) {
    if (item.topic_name == topic) {
      key = item.key;
    }
  }
  auto* panel = window.findChild<PJ::CurveListPanel*>(u"curveListPanel"_s);
  auto* custom = panel != nullptr ? panel->findChild<PJ::CurveTreeView*>(u"customView"_s) : nullptr;
  auto* edit = panel != nullptr ? panel->findChild<QToolButton*>(u"buttonEditCustom"_s) : nullptr;
  if (key.isEmpty() || custom == nullptr || edit == nullptr) {
    return std::nullopt;
  }
  custom->clearSelection();
  QTreeWidgetItemIterator it(custom);
  for (; *it != nullptr; ++it) {
    if (PJ::CurveTreeView::catalogKeyOf(*it) == key) {
      (*it)->setSelected(true);
      break;
    }
  }
  if (!edit->isEnabled()) {
    return std::nullopt;
  }
  edit->click();
  QLabel* loaded = nullptr;
  PJ::test::pumpUntil([&] {
    loaded = window.findChild<QLabel*>(u"loadedConfig"_s);
    return loaded != nullptr && !loaded->text().isEmpty();
  });
  if (loaded == nullptr) {
    return std::nullopt;
  }
  return loaded->text().toStdString();
}

constexpr const char* kEditorId = "custom-topics-editor-fixture";

// The recipe of an agent's on-demand object: a point cloud cropped from /cloud.
PJ::RecipeGraph::Recipe agentCropRecipe() {
  PJ::RecipeGraph::Recipe recipe;
  recipe.owner_plugin = "agent-test";
  recipe.user_id = "crop";
  recipe.script = "local inputs = ... return { cropped = inputs.cloud }";
  recipe.params_json = R"({"radius":3})";
  recipe.inputs = {"/cloud"};
  recipe.outputs = {
      {"cropped", PJ::scripting::OutputType::kObject, PJ::sdk::BuiltinObjectType::kPointCloud},
      {"rest", PJ::scripting::OutputType::kObject, PJ::sdk::BuiltinObjectType::kPointCloud}};
  return recipe;
}

// A window with the fixture editor, a dataset holding `/speed`, and an agent's transform over it.
struct AgentTransformWindow {
  QTemporaryDir extensions_dir;
  std::unique_ptr<PJ::MainWindow> window;

  [[nodiscard]] bool setUp() {
    stageEditorFixture(extensions_dir);
    if (::testing::Test::HasFatalFailure()) {
      return false;
    }
    window = std::make_unique<PJ::MainWindow>(extensions_dir.path());
    PJ::AppSession& app = Peer::session(*window);
    app.extensionCatalog().registerAdHocOwner(u"agent-test"_s, u"AI agent (agent-test)"_s);
    const PJ::DatasetId dataset = pj_test::createDataset(app, "drive", true);
    if (dataset == 0U || pj_test::addScalarTopic(app, dataset, "/speed", 1'000, 2'000) == 0U) {
      return false;
    }
    if (!app.sessionManager().dataProcessorService().upsertTransform(
            "agent-test", "speed_2fneg", {"/speed/value"}, {"/speed/neg"}, kNegate, R"({"k":2})")) {
      return false;
    }
    app.catalogModel().rebuildFromDatastore();
    Peer::syncCustomTopicsPanel(*window);
    return true;
  }
};

// The pencil on the editor's own row hands it the recipe's params_json; on a row an AI agent made, an editor that
// declares `custom_topics_viewer` gets a view of the recipe instead (script, inputs, outputs, params, the owner's
// name), which it shows read-only. The view's name is the one the row shows (the output topic), not the transform's
// escaped id.
TEST(MainWindowEditorIntegrationTest, PencilOpensTheEditorOnAViewOfAnotherOwnersTransform) {
  AgentTransformWindow fx;
  ASSERT_TRUE(fx.setUp());
  ASSERT_TRUE(Peer::session(*fx.window).extensionCatalog().customTopicsEditorViews())
      << "the fixture declares custom_topics_viewer";

  const std::optional<std::string> config = pencilConfig(*fx.window, u"/speed/neg"_s);
  ASSERT_TRUE(config.has_value()) << "the pencil is enabled on the agent's row and launches the editor";
  const auto parsed = nlohmann::json::parse(*config, nullptr, false);
  ASSERT_TRUE(parsed.is_object()) << *config;
  ASSERT_TRUE(parsed.contains("view")) << *config;
  const nlohmann::json& view = parsed["view"];
  EXPECT_EQ(view["owner"], "AI agent (agent-test)");
  EXPECT_EQ(view["name"], "/speed/neg");
  EXPECT_EQ(view["kind"], "transform");
  EXPECT_EQ(view["language"], "luau");
  EXPECT_EQ(view["script"], kNegate);
  EXPECT_EQ(view["inputs"], nlohmann::json::array({"/speed/value"}));
  EXPECT_EQ(view["outputs"], nlohmann::json::array({"/speed/neg"}));
  EXPECT_EQ(view["params"], R"({"k":2})");
}

// An agent's on-demand object opens the same way: kind "on_demand", the recipe's language, and its outputs by name.
TEST(MainWindowEditorIntegrationTest, PencilOpensTheEditorOnAViewOfAnotherOwnersOnDemandRecipe) {
  QTemporaryDir extensions_dir;
  stageEditorFixture(extensions_dir);
  ASSERT_FALSE(HasFatalFailure());
  PJ::MainWindow window(extensions_dir.path());
  PJ::AppSession& app = Peer::session(window);
  app.extensionCatalog().registerAdHocOwner(u"agent-test"_s, u"AI agent (agent-test)"_s);
  const PJ::DatasetId dataset = pj_test::createDataset(app, "drive", true);
  ASSERT_NE(dataset, 0U);
  ASSERT_TRUE(registerCloud(app, dataset).has_value());
  app.catalogModel().rebuildFromDatastore();
  const PJ::RecipeGraph::Recipe recipe = agentCropRecipe();
  ASSERT_TRUE(app.sessionManager().onDemandService().install(recipe).has_value());

  const std::optional<std::string> config = pencilConfig(window, u"agent-test/crop/cropped"_s);
  ASSERT_TRUE(config.has_value()) << "the pencil is enabled on the agent's row and launches the editor";
  const auto parsed = nlohmann::json::parse(*config, nullptr, false);
  ASSERT_TRUE(parsed.is_object() && parsed.contains("view")) << *config;
  const nlohmann::json& view = parsed["view"];
  EXPECT_EQ(view["owner"], "AI agent (agent-test)");
  EXPECT_EQ(view["name"], "crop");
  EXPECT_EQ(view["kind"], "on_demand");
  EXPECT_EQ(view["language"], "luau");
  EXPECT_EQ(view["script"], recipe.script);
  EXPECT_EQ(view["inputs"], nlohmann::json::array({"/cloud"}));
  EXPECT_EQ(view["outputs"], nlohmann::json::array({"cropped", "rest"}));
  EXPECT_EQ(view["params"], recipe.params_json);
}

TEST(MainWindowEditorIntegrationTest, PencilOnTheEditorsOwnRowPassesItsParams) {
  QTemporaryDir extensions_dir;
  stageEditorFixture(extensions_dir);
  ASSERT_FALSE(HasFatalFailure());
  PJ::MainWindow window(extensions_dir.path());
  PJ::AppSession& app = Peer::session(window);

  const PJ::DatasetId dataset = pj_test::createDataset(app, "drive", true);
  ASSERT_NE(dataset, 0U);
  ASSERT_NE(pj_test::addScalarTopic(app, dataset, "/speed", 1'000, 2'000), 0U);
  ASSERT_TRUE(app.sessionManager().dataProcessorService().upsertTransform(
      kEditorId, "speed_2fneg", {"/speed/value"}, {"/speed/neg"}, kNegate, R"({"output_name":"neg"})"));
  app.catalogModel().rebuildFromDatastore();
  Peer::syncCustomTopicsPanel(window);

  const std::optional<std::string> config = pencilConfig(window, u"/speed/neg"_s);
  ASSERT_TRUE(config.has_value());
  EXPECT_EQ(*config, R"({"output_name":"neg"})");

  // The editor's own recipe is workspace state: a pinned panel on it is written into the layout.
  Peer::pin(window, QString::fromLatin1(kEditorId));
  EXPECT_TRUE(Peer::savedPinnedToolboxesXml(window).contains(u"output_name"_s));
}

// A view is a snapshot of a recipe another owner can change or remove, so a pinned editor showing one saves nothing
// into the layout: a later load would hand the editor a stale recipe.
TEST(MainWindowEditorIntegrationTest, APinnedViewIsNotSavedIntoTheLayout) {
  AgentTransformWindow fx;
  ASSERT_TRUE(fx.setUp());
  ASSERT_TRUE(pencilConfig(*fx.window, u"/speed/neg"_s).has_value());
  Peer::pin(*fx.window, QString::fromLatin1(kEditorId));
  ASSERT_TRUE(Peer::isLaunched(*fx.window, QString::fromLatin1(kEditorId)));

  const QString saved = Peer::savedPinnedToolboxesXml(*fx.window);
  EXPECT_FALSE(saved.contains(u"<toolbox"_s)) << saved.toStdString();
  EXPECT_FALSE(saved.contains(u"\"view\""_s)) << saved.toStdString();
}

// The panel enables the pencil on another owner's row only for an editor that shows views, and the handler decides
// again: an editor without the tag is never launched on a row it does not own.
TEST(MainWindowEditorIntegrationTest, TheHandlerRefusesAViewWhenTheEditorLacksTheTag) {
  AgentTransformWindow fx;
  ASSERT_TRUE(fx.setUp());
  PJ::AppSession& app = Peer::session(*fx.window);
  auto* panel = fx.window->findChild<PJ::CurveListPanel*>(u"curveListPanel"_s);
  ASSERT_NE(panel, nullptr);
  QString key;
  for (const PJ::CatalogItem& item : app.catalogModel().items()) {
    if (item.topic_name == u"/speed/neg"_s) {
      key = item.key;
    }
  }
  ASSERT_FALSE(key.isEmpty());
  const QString editor = QString::fromLatin1(kEditorId);

  app.extensionCatalog().setCustomTopicsEditorIdForTest(editor, /*views=*/false);
  emit panel->editCustomTopicRequested(key);
  PJ::test::pumpFor(100);
  EXPECT_FALSE(Peer::isLaunched(*fx.window, editor));

  app.extensionCatalog().setCustomTopicsEditorIdForTest(editor, /*views=*/true);
  emit panel->editCustomTopicRequested(key);
  PJ::test::pumpUntil([&] { return Peer::isLaunched(*fx.window, editor); });
  EXPECT_TRUE(Peer::isLaunched(*fx.window, editor)) << "the same request launches the editor once it shows views";
}

}  // namespace
