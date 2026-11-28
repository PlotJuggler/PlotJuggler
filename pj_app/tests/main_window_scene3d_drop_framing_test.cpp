// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A hand-made 3D view whose first layer arrives by a user drop frames that
// content (fixed frame + camera), because the default fixed frame can be far from
// the data. A restored layout keeps its saved frame and camera instead.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QStringList>
#include <QTemporaryDir>
#include <memory>

#include "MainWindow.h"
#include "dataset_test_helpers.h"
#include "pj_datastore/object_store.hpp"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"

using namespace Qt::StringLiterals;

namespace PJ {

class MainWindowScene3dDropFramingTestPeer {
 public:
  /// Registers a canonical PointCloud object topic and returns its catalog key.
  static QString registerCloud(MainWindow& window) {
    AppSession& app = *window.session_;
    const auto dataset = pj_test::createDataset(app, "drop-framing", true);
    EXPECT_NE(dataset, 0u);
    const auto metadata =
        sdk::ObjectTopicMetadataBuilder().builtinObjectType(sdk::BuiltinObjectType::kPointCloud).build();
    EXPECT_TRUE(metadata.has_value());
    const auto topic = app.sessionManager().objectStore().registerTopic(
        ObjectTopicDescriptor{.dataset_id = dataset, .topic_name = "/cloud", .metadata_json = *metadata});
    EXPECT_TRUE(topic.has_value());
    app.catalogModel().rebuildFromDatastore();
    const auto items = app.catalogModel().items();
    return items.empty() ? QString() : items.front().key;
  }

  static PlotDocker* freshDocker(MainWindow& window) {
    auto* tabs = window.findChild<TabbedPlotWidget*>(u"tabbedPlotWidget"_s);
    return tabs != nullptr ? tabs->addTab(u"drop"_s) : nullptr;
  }

  /// What the shell reports as "Layout" issues while `restore` runs.
  template <typename Restore>
  static QStringList layoutIssuesOf(MainWindow& window, Restore&& restore) {
    QStringList issues;
    window.layout_issue_capture_ = &issues;
    restore();
    window.layout_issue_capture_ = nullptr;
    return issues;
  }

  static bool restoreExactly(MainWindow& window, QDomDocument& doc, RestoreIntent intent) {
    return window.restoreWorkspaceState(doc, MainWindow::MissingCurvePolicy::kExact, nullptr, intent) ==
           MainWindow::RestoreResult::kApplied;
  }

  static QDomDocument workspace(const MainWindow& window) {
    return window.xmlSaveState();
  }

#ifdef PJ_WITH_SCENE3D
  static Scene3DDockWidget* sceneOf(PlotDocker* docker) {
    DockWidget* dock = docker != nullptr ? docker->plotAt(0) : nullptr;
    IDataWidget* widget = dock != nullptr ? dock->objectWidget() : nullptr;
    return widget != nullptr ? qobject_cast<Scene3DDockWidget*>(widget->widget()) : nullptr;
  }
#endif
};

}  // namespace PJ

namespace {

class MainWindowScene3dDropFramingTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    extensions_ = std::make_unique<QTemporaryDir>();
    ASSERT_TRUE(extensions_->isValid());
    window_ = std::make_unique<PJ::MainWindow>(extensions_->path());
  }
  static void TearDownTestSuite() {
    window_.reset();
    extensions_.reset();
  }
  inline static std::unique_ptr<QTemporaryDir> extensions_;
  inline static std::unique_ptr<PJ::MainWindow> window_;
};

#ifdef PJ_WITH_SCENE3D

// A drop on an empty placeholder builds the dock through the seeded factory.
TEST_F(MainWindowScene3dDropFramingTest, DropOnEmptyPlaceholderArmsFraming) {
  using Peer = PJ::MainWindowScene3dDropFramingTestPeer;
  const QString key = Peer::registerCloud(*window_);
  ASSERT_FALSE(key.isEmpty());
  PJ::PlotDocker* docker = Peer::freshDocker(*window_);
  ASSERT_NE(docker, nullptr);
  PJ::DockWidget* dock = docker->plotAt(0);
  ASSERT_NE(dock, nullptr);

  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{key})));

  PJ::Scene3DDockWidget* scene = Peer::sceneOf(docker);
  ASSERT_NE(scene, nullptr);
  EXPECT_TRUE(scene->initialContentFramingPending());
}

// The 3D icon creates an empty view; only its first dropped topic frames it.
TEST_F(MainWindowScene3dDropFramingTest, FirstTopicDroppedIntoEmptyViewArmsFraming) {
  using Peer = PJ::MainWindowScene3dDropFramingTestPeer;
  const QString key = Peer::registerCloud(*window_);
  ASSERT_FALSE(key.isEmpty());
  PJ::PlotDocker* docker = Peer::freshDocker(*window_);
  ASSERT_NE(docker, nullptr);
  PJ::DockWidget* dock = docker->plotAt(0);
  ASSERT_NE(dock, nullptr);

  emit docker->objectFamilyRequested(dock, PJ::VisualizationKind::kScene3D);
  PJ::Scene3DDockWidget* scene = Peer::sceneOf(docker);
  ASSERT_NE(scene, nullptr);
  EXPECT_FALSE(scene->initialContentFramingPending());

  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{key})));
  EXPECT_TRUE(scene->initialContentFramingPending());
}

// Restoring a saved layout goes through the seedless factory path and must keep
// the saved fixed frame and camera, so nothing is armed.
TEST_F(MainWindowScene3dDropFramingTest, LayoutRestoreDoesNotArmFraming) {
  using Peer = PJ::MainWindowScene3dDropFramingTestPeer;
  const QString key = Peer::registerCloud(*window_);
  ASSERT_FALSE(key.isEmpty());
  PJ::PlotDocker* source = Peer::freshDocker(*window_);
  ASSERT_NE(source, nullptr);
  PJ::DockWidget* dock = source->plotAt(0);
  ASSERT_NE(dock, nullptr);
  emit source->objectFamilyRequested(dock, PJ::VisualizationKind::kScene3D);
  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{key})));
  ASSERT_TRUE(Peer::sceneOf(source)->initialContentFramingPending());

  QDomDocument doc;
  const QDomElement saved = source->xmlSaveState(doc);
  PJ::PlotDocker* restored = Peer::freshDocker(*window_);
  ASSERT_NE(restored, nullptr);
  ASSERT_TRUE(restored->xmlLoadState(saved));

  PJ::Scene3DDockWidget* scene = Peer::sceneOf(restored);
  ASSERT_NE(scene, nullptr);
  EXPECT_FALSE(scene->initialContentFramingPending());
}

// A scene layer with a value its reader refuses is dropped; the rest of the tab restores, and the layout's issues
// (which MCP load_layout returns as warnings) name the layer, the attribute and the values it takes.
TEST_F(MainWindowScene3dDropFramingTest, SkippedSceneLayerIsReportedAsALayoutIssue) {
  using Peer = PJ::MainWindowScene3dDropFramingTestPeer;
  const QString key = Peer::registerCloud(*window_);
  ASSERT_FALSE(key.isEmpty());
  PJ::PlotDocker* source = Peer::freshDocker(*window_);
  ASSERT_NE(source, nullptr);
  PJ::DockWidget* dock = source->plotAt(0);
  ASSERT_NE(dock, nullptr);
  emit source->objectFamilyRequested(dock, PJ::VisualizationKind::kScene3D);
  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{key})));

  dock->setName(u"Lidar view"_s);
  QDomDocument doc;
  const QDomElement saved = source->xmlSaveState(doc);
  const QDomNodeList clouds = saved.elementsByTagName(u"pointcloud"_s);
  ASSERT_EQ(clouds.size(), 1);
  clouds.at(0).toElement().setAttribute(u"shape"_s, u"square"_s);

  PJ::PlotDocker* restored = Peer::freshDocker(*window_);
  ASSERT_NE(restored, nullptr);
  const QStringList issues = Peer::layoutIssuesOf(
      *window_, [&] { ASSERT_TRUE(restored->xmlLoadState(saved)) << "one bad layer must not fail the tab"; });
  PJ::Scene3DDockWidget* scene = Peer::sceneOf(restored);
  ASSERT_NE(scene, nullptr);
  EXPECT_TRUE(scene->layers().empty());
  ASSERT_EQ(issues.size(), 1) << issues.join(u" | "_s).toStdString();
  EXPECT_TRUE(issues.front().startsWith(u"Lidar view: "_s))
      << "named by the dock title the user sees: " << issues.front().toStdString();
  EXPECT_TRUE(issues.front().contains(u"/cloud"_s)) << issues.front().toStdString();
  EXPECT_TRUE(issues.front().contains(u"shape=\"square\""_s)) << issues.front().toStdString();
  EXPECT_TRUE(issues.front().contains(u"sphere, point, cube"_s)) << issues.front().toStdString();
}

// A layout applied exactly (the browser's layout load) keeps the rest of the workspace past a skipped layer; only a
// history replay, which must reproduce its snapshot, fails on it.
TEST_F(MainWindowScene3dDropFramingTest, OnlyAHistoryRestoreFailsOnASkippedSceneLayer) {
  using Peer = PJ::MainWindowScene3dDropFramingTestPeer;
  const QString key = Peer::registerCloud(*window_);
  ASSERT_FALSE(key.isEmpty());
  PJ::PlotDocker* source = Peer::freshDocker(*window_);
  ASSERT_NE(source, nullptr);
  PJ::DockWidget* dock = source->plotAt(0);
  ASSERT_NE(dock, nullptr);
  emit source->objectFamilyRequested(dock, PJ::VisualizationKind::kScene3D);
  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{key})));
  QDomDocument doc = Peer::workspace(*window_);
  const QDomNodeList clouds = doc.elementsByTagName(u"pointcloud"_s);
  ASSERT_EQ(clouds.size(), 1);
  clouds.at(0).toElement().setAttribute(u"shape"_s, u"square"_s);

  QDomDocument layout = doc.cloneNode(true).toDocument();
  const QStringList issues = Peer::layoutIssuesOf(
      *window_, [&] { EXPECT_TRUE(Peer::restoreExactly(*window_, layout, PJ::RestoreIntent::kReplace)); });
  EXPECT_EQ(issues.size(), 1) << issues.join(u" | "_s).toStdString();

  QDomDocument history = doc.cloneNode(true).toDocument();
  EXPECT_FALSE(Peer::restoreExactly(*window_, history, PJ::RestoreIntent::kHistory));
}

#endif  // PJ_WITH_SCENE3D

}  // namespace
