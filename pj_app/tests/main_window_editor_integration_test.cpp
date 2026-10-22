// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The Custom Topics editor through MainWindow (the host half of slice 13): the toolbox whose manifest declares the
// `custom_topics_editor` capability is the editor, the panel's "+" opens it, and the scene view its preview frame asks
// for is embedded in its panel with the requested topic attached. The toolbox is a test fixture
// (custom_topics_editor_fixture.cpp), staged into the extensions directory before the window scans it.

#include <gtest/gtest.h>

#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QTemporaryDir>
#include <QToolButton>

#include "MainWindow.h"
#include "dataset_test_helpers.h"
#include "event_pump_test_utils.h"
#include "pj_base/sdk/object_topic_metadata.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/SessionManager.h"
#include "pj_scene_common/scene_dock_widget.h"
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
};

}  // namespace PJ

namespace {

using Peer = PJ::MainWindowEditorIntegrationTestPeer;

// GAP-45's plugin-visible half: "+" launches the declared editor, and the `scene_view` of its preview frame is a 3D
// scene embedded in that frame, showing the topic it names.
TEST(MainWindowEditorIntegrationTest, PlusOpensTheDeclaredEditorAndItsPreviewEmbedsAScene) {
#ifndef PJ_WITH_SCENE3D
  GTEST_SKIP() << "this build embeds no 3D scene view";
#else
  QTemporaryDir extensions_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  const QString fixture = QString::fromUtf8(PJ_CUSTOM_TOPICS_EDITOR_FIXTURE_PATH);
  ASSERT_TRUE(QFile::copy(fixture, extensions_dir.filePath(QFileInfo(fixture).fileName())));
  PJ::MainWindow window(extensions_dir.path());
  PJ::AppSession& app = Peer::session(window);
  ASSERT_EQ(app.extensionCatalog().customTopicsEditorId(), u"custom-topics-editor-fixture"_s)
      << "the editor is the toolbox declaring the capability";

  const PJ::DatasetId dataset = pj_test::createDataset(app, "drive", true);
  ASSERT_NE(dataset, 0U);
  const auto metadata =
      PJ::sdk::ObjectTopicMetadataBuilder().builtinObjectType(PJ::sdk::BuiltinObjectType::kPointCloud).build();
  ASSERT_TRUE(metadata.has_value());
  ASSERT_TRUE(app.sessionManager().objectStore().registerTopic({dataset, "/cloud", *metadata}).has_value());
  app.catalogModel().rebuildFromDatastore();

  auto* panel = window.findChild<PJ::CurveListPanel*>(u"curveListPanel"_s);
  ASSERT_NE(panel, nullptr);
  auto* add = panel->findChild<QToolButton*>(u"buttonAddCustom"_s);
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

}  // namespace
