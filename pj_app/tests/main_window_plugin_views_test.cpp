// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QDataStream>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFrame>
#include <QMimeData>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "MainWindow.h"
#include "PluginPlotTabsController.h"
#include "PluginSceneViewsController.h"
#include "dataset_test_helpers.h"
#include "pj_base/builtin/image.hpp"
#include "pj_base/builtin/image_codec.hpp"
#include "pj_base/sdk/object_topic_metadata.hpp"
#include "pj_base/sdk/platform.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/PlotTabsRuntimeHost.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_runtime/TopicDemandTracker.h"
#include "pj_scene_common/scene_dock_widget.h"
#include "pj_widgets/ToastNotification.h"
#ifdef PJ_WITH_SCENE3D
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#endif

using namespace Qt::StringLiterals;

namespace PJ {

/// Exercise the shell callbacks that enforce plugin identity and dataset liveness.
class MainWindowPluginViewsTestPeer {
 public:
  [[nodiscard]] static PluginPlotTabsController& pluginTabs(MainWindow& window) {
    return *window.plugin_plot_tabs_;
  }

  static void checkNewPlotLinkedZoom(MainWindow& window) {
    auto& app = *window.session_;
    app.sessionManager().setUseTimeOffset(false);
    const auto dataset = pj_test::createDataset(app, "linked-zoom", true);
    ASSERT_NE(pj_test::addScalarTopic(app, dataset, "/signal", 0, 10'000'000'000), 0U);
    const auto key = app.catalogModel().resolveCurveKey(0, u"linked-zoom"_s, {}, u"/signal"_s, u"value"_s);
    ASSERT_TRUE(key);
    auto* tabs = window.findChild<TabbedPlotWidget*>();
    auto* docker = tabs->addTab(u"Linked zoom"_s);
    auto* source_dock = docker->plotAt(0);
    auto* source = source_dock->ensurePlotWidget();
    ASSERT_NE(source->addCurve(*key), nullptr);
    source->setZoomRectangle(QRectF(2.0, 8.0, 2.0, -10.0), false);
    const auto original = source->currentBoundingRect();
    auto* link = window.findChild<QAbstractButton*>(u"buttonLink"_s);
    ASSERT_NE(link, nullptr);
    link->setChecked(true);

    auto* target_dock = source_dock->splitVertical();
    ASSERT_NE(target_dock, nullptr);
    ASSERT_TRUE(
        QMetaObject::invokeMethod(
            target_dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{*key})));
    auto* target = target_dock->plotWidget();
    ASSERT_NE(target, nullptr);
    EXPECT_DOUBLE_EQ(target->currentBoundingRect().left(), 2.0);
    EXPECT_DOUBLE_EQ(target->currentBoundingRect().right(), 4.0);
    EXPECT_EQ(source->currentBoundingRect(), original);

    auto* canvas_dock = target_dock->splitVertical();
    ASSERT_NE(canvas_dock, nullptr);
    auto* canvas = canvas_dock->ensurePlotWidget();
    const auto drop_curve = [](PlotWidget* plot, const QString& curve_key) {
      QMimeData mime;
      QByteArray encoded;
      QDataStream stream(&encoded, QIODevice::WriteOnly);
      stream << curve_key;
      mime.setData(u"curveslist/add_curve"_s, encoded);
      QDragEnterEvent enter(QPointF(1, 1), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
      if (!QMetaObject::invokeMethod(plot, "onDragEnterEvent", Qt::DirectConnection, Q_ARG(QDragEnterEvent*, &enter)) ||
          !enter.isAccepted()) {
        return false;
      }
      QDropEvent drop(QPointF(1, 1), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
      return QMetaObject::invokeMethod(plot, "onDropEvent", Qt::DirectConnection, Q_ARG(QDropEvent*, &drop)) &&
             drop.isAccepted();
    };
    ASSERT_TRUE(drop_curve(canvas, *key));
    EXPECT_DOUBLE_EQ(canvas->currentBoundingRect().left(), 2.0);
    EXPECT_DOUBLE_EQ(canvas->currentBoundingRect().right(), 4.0);
    EXPECT_EQ(source->currentBoundingRect(), original);

    ASSERT_NE(pj_test::addScalarTopic(app, dataset, "/other", 0, 10'000'000'000), 0U);
    const auto other = app.catalogModel().resolveCurveKey(0, u"linked-zoom"_s, {}, u"/other"_s, u"value"_s);
    ASSERT_TRUE(other);
    link->setChecked(false);
    canvas->setZoomRectangle(QRectF(6.0, 8.0, 2.0, -10.0), false);
    link->setChecked(true);
    ASSERT_TRUE(drop_curve(canvas, *other));
    EXPECT_DOUBLE_EQ(canvas->currentBoundingRect().left(), 6.0);
    EXPECT_DOUBLE_EQ(canvas->currentBoundingRect().right(), 8.0);
    EXPECT_EQ(source->currentBoundingRect(), original);

    link->setChecked(false);
    auto* unlinked_dock = target_dock->splitVertical();
    ASSERT_NE(unlinked_dock, nullptr);
    ASSERT_TRUE(
        QMetaObject::invokeMethod(
            unlinked_dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{*key})));
    auto* unlinked = unlinked_dock->plotWidget();
    ASSERT_NE(unlinked, nullptr);
    EXPECT_LE(unlinked->currentBoundingRect().left(), 0.0);
    EXPECT_GE(unlinked->currentBoundingRect().right(), 10.0);
    EXPECT_EQ(source->currentBoundingRect(), original);
  }

  /// A plugin's pj.plot_tabs.v1 binding, wired exactly as launchToolbox wires it:
  /// the real runtime bridge over the shell's per-plugin callbacks.
  struct TabsApi {
    std::unique_ptr<PlotTabsRuntimeHost> host;
    sdk::PlotTabHostView view;
  };
  [[nodiscard]] static TabsApi tabsApi(PluginPlotTabsController& controller, const QString& plugin_id) {
    TabsApi api;
    api.host = std::make_unique<PlotTabsRuntimeHost>(controller.plotTabsCallbacks(plugin_id));
    api.view = sdk::PlotTabHostView(api.host->raw());
    return api;
  }
  [[nodiscard]] static TabsApi tabsApi(MainWindow& window, const QString& plugin_id) {
    return tabsApi(pluginTabs(window), plugin_id);
  }

  /// A controller over the window's workspace whose scene-widget factory is `make_dock`.
  [[nodiscard]] static std::unique_ptr<PluginPlotTabsController> makeController(
      MainWindow& window, std::function<IDataWidget*(const QString& kind, QWidget* parent)> make_dock) {
    AppSession& app = *window.session_;
    return std::make_unique<PluginPlotTabsController>(
        *window.findChild<TabbedPlotWidget*>(), app.catalogModel(), app.sessionManager(), app.extensionCatalog(),
        std::move(make_dock));
  }
  [[nodiscard]] static std::function<IDataWidget*(const QString& kind, QWidget* parent)> windowDockFactory(
      MainWindow& window) {
    return [&window](const QString& kind, QWidget* parent) { return window.makeSeededEmptyObjectDock(kind, parent); };
  }

  /// Registers object topics of `type` directly on the ObjectStore (no parser), the
  /// registerCanonicalTopic idiom of pj_scene3D/widgets/tests/mock_parser_support.h.
  static void registerObjectTopics(
      AppSession& app, DatasetId dataset, const std::vector<std::string>& names,
      sdk::BuiltinObjectType type = sdk::BuiltinObjectType::kPointCloud) {
    const auto metadata = sdk::ObjectTopicMetadataBuilder().builtinObjectType(type).build();
    ASSERT_TRUE(metadata.has_value());
    for (const std::string& name : names) {
      ASSERT_TRUE(app.sessionManager()
                      .objectStore()
                      .registerTopic(
                          ObjectTopicDescriptor{.dataset_id = dataset, .topic_name = name, .metadata_json = *metadata})
                      .has_value());
    }
    app.catalogModel().rebuildFromDatastore();
  }

  // ABI-38: create a 3D tab, attach a canonical PointCloud topic, read it back,
  // detach it, focus and close the tab.
  static void checkSceneViewsLifecycle(MainWindow& window) {
    const QString owner = u"scene-view-plugin"_s;
    AppSession& app = *window.session_;
    const auto dataset = pj_test::createDataset(app, "scene-views-lifecycle", true);
    ASSERT_NE(dataset, 0u);
    registerObjectTopics(app, dataset, {"/cloud"});

    TabsApi api = tabsApi(window, owner);
    ASSERT_TRUE(api.view.createTabV2("view", "3d", "3D View"));
    PlotDocker* docker = pluginTabs(window).ownedTab(owner, u"view"_s);
    ASSERT_NE(docker, nullptr);
    EXPECT_EQ(sceneKindOf(docker), u"3d"_s);

    ASSERT_TRUE(api.view.attachTopic("view", "/cloud", "scene-views-lifecycle"));
    auto config = api.view.configOf("view");
    ASSERT_TRUE(config);
    EXPECT_EQ(
        *config,
        R"({"kind":"3d","title":"3D View","topics":[{"dataset":"scene-views-lifecycle","topic":"/cloud","type":"kPointCloud","visible":true}]})");

    // Attaching an attached topic changes nothing, whether named bare or qualified.
    EXPECT_TRUE(api.view.attachTopic("view", "/cloud", "scene-views-lifecycle"));
    EXPECT_TRUE(api.view.attachTopic("view", "/cloud"));
    EXPECT_EQ(sceneDockOf(docker)->layers().size(), 1U);

    const auto missing = api.view.attachTopic("view", "/no-such-cloud");
    ASSERT_FALSE(missing);
    EXPECT_NE(missing.error().find("no object topic '/no-such-cloud'"), std::string::npos) << missing.error();

    ASSERT_TRUE(api.view.detachTopic("view", "/cloud", "scene-views-lifecycle"));
    config = api.view.configOf("view");
    ASSERT_TRUE(config);
    EXPECT_EQ(config->find("/cloud"), std::string::npos) << *config;

    const auto absent = api.view.detachTopic("view", "/cloud", "scene-views-lifecycle");
    ASSERT_FALSE(absent);
    EXPECT_NE(absent.error().find("'/cloud' is not attached"), std::string::npos) << absent.error();

    // Another plugin can neither reach nor confirm the tab.
    TabsApi intruder = tabsApi(window, u"scene-view-intruder"_s);
    EXPECT_FALSE(intruder.view.attachTopic("view", "/cloud"));
    EXPECT_FALSE(intruder.view.focusTab("view"));

    // A 2D tab refuses a type it cannot show, naming it.
    ASSERT_TRUE(api.view.createTabV2("image", "2d", "2D View"));
    const auto wrong_type = api.view.attachTopic("image", "/cloud");
    ASSERT_FALSE(wrong_type);
    EXPECT_NE(wrong_type.error().find("2D scene tabs do not accept topics of type 'kPointCloud'"), std::string::npos)
        << wrong_type.error();

    ASSERT_TRUE(api.view.focusTab("view"));
    EXPECT_EQ(window.findChild<TabbedPlotWidget*>()->currentTab(), docker);
    ASSERT_TRUE(api.view.close("view"));
    ASSERT_TRUE(api.view.close("image"));
    EXPECT_EQ(pluginTabs(window).ownedTab(owner, u"view"_s), nullptr);
  }

#ifdef PJ_WITH_SCENE3D
  // DEM-06: a view embedded in a plugin panel is built by the shell, demands the
  // topics it shows, follows the playback cursor and dies with its frame.
  static void checkEmbeddedSceneView(MainWindow& window) {
    AppSession& app = *window.session_;
    const auto dataset = pj_test::createDataset(app, "embedded-scene-view", true);
    ASSERT_NE(dataset, 0u);
    registerObjectTopics(app, dataset, {"/embedded-cloud"});

    const EmbeddedViewHost host = window.makeEmbeddedViewHost();
    ASSERT_TRUE(static_cast<bool>(host));
    const auto ticked = [&window](QWidget* view) {
      return std::any_of(
          window.embedded_scene_views_.begin(), window.embedded_scene_views_.end(),
          [view](const QPointer<SceneDockWidget>& embedded) { return embedded == view; });
    };

    auto frame = std::make_unique<QFrame>();
    QWidget* view = host.create(u"3d"_s, frame.get());
    ASSERT_NE(view, nullptr);
    EXPECT_EQ(view->parent(), frame.get());
    EXPECT_TRUE(ticked(view));
    EXPECT_EQ(host.create(u"bogus"_s, frame.get()), nullptr);

    bool demanded = false;
    const QMetaObject::Connection demand_connection = QObject::connect(
        &app.topicDemandTracker(), &TopicDemandTracker::activeTopicsChanged, frame.get(),
        [&demanded](DatasetId, const std::vector<QString>& active) {
          demanded = demanded || std::find(active.begin(), active.end(), u"/embedded-cloud"_s) != active.end();
        });
    std::string error;
    ASSERT_TRUE(host.attach(view, u"/embedded-cloud"_s, u"embedded-scene-view"_s, &error)) << error;
    auto* scene = qobject_cast<SceneDockWidget*>(view);
    ASSERT_NE(scene, nullptr);
    EXPECT_EQ(scene->layers().size(), 1U);
    QObject::disconnect(demand_connection);
    EXPECT_TRUE(demanded) << "an attached topic must become demanded";

    EXPECT_FALSE(host.attach(view, u"/missing"_s, u"embedded-scene-view"_s, &error));
    EXPECT_NE(error.find("no object topic '/missing'"), std::string::npos) << error;

    host.detach(view, u"/embedded-cloud"_s, u"embedded-scene-view"_s);
    EXPECT_EQ(scene->layers().size(), 0U);

    // The protected tracker accessor, reached through a member pointer formed in a
    // derived class (the test hooks accessor is not compiled into this runner).
    struct TrackerPeek : SceneDockWidget {
      static auto accessor() {
        return &TrackerPeek::lastTrackerNs;
      }
    };
    const auto last_tracker_ns = TrackerPeek::accessor();
    window.broadcastTrackerTime(7.0);
    EXPECT_EQ((scene->*last_tracker_ns)(), std::optional<int64_t>{7'000'000'000}) << "the cursor reaches the view";
    window.broadcastTrackerTimeToVisible(9.0);
    EXPECT_EQ((scene->*last_tracker_ns)(), std::optional<int64_t>{7'000'000'000})
        << "a hidden view skips per-tick moves";

    ASSERT_TRUE(host.attach(view, u"/embedded-cloud"_s, u"embedded-scene-view"_s, &error)) << error;
    EXPECT_TRUE(app.topicDemandTracker().hasAnyReference(dataset));
    frame.reset();  // the view dies with its frame, and its demand with it
    EXPECT_FALSE(app.topicDemandTracker().hasAnyReference(dataset));
    EXPECT_FALSE(ticked(view));
    window.broadcastTrackerTime(0.0);
    EXPECT_TRUE(window.embedded_scene_views_.isEmpty());
  }
#endif

#ifdef PJ_WITH_SCENE3D
  /// The playhead a scene widget last received (SceneDockWidget's protected accessor,
  /// reached through a member pointer formed in a derived class).
  [[nodiscard]] static std::optional<int64_t> lastTrackerNs(const SceneDockWidget& scene) {
    struct TrackerPeek : SceneDockWidget {
      static auto accessor() {
        return &TrackerPeek::lastTrackerNs;
      }
    };
    return (scene.*TrackerPeek::accessor())();
  }

  // Catalog removal revalidates an embedded view as it does a plugin's scene tab:
  // the evicted topic's layer and demand go, the emptied view stays in its frame.
  static void checkEmbeddedSceneViewDropsAnEvictedTopic(MainWindow& window) {
    AppSession& app = *window.session_;
    const auto dataset = pj_test::createDataset(app, "embedded-evict", true);
    registerObjectTopics(app, dataset, {"/embedded-evict-cloud"});
    const EmbeddedViewHost host = window.makeEmbeddedViewHost();
    auto frame = std::make_unique<QFrame>();
    QPointer<SceneDockWidget> scene = qobject_cast<SceneDockWidget*>(host.create(u"3d"_s, frame.get()));
    ASSERT_NE(scene, nullptr);
    std::string error;
    ASSERT_TRUE(host.attach(scene, u"/embedded-evict-cloud"_s, {}, &error)) << error;
    ASSERT_TRUE(app.topicDemandTracker().hasAnyReference(dataset));

    unloadDataset(app, dataset);
    window.syncWidgetsToCatalog();
    ASSERT_NE(scene, nullptr) << "the emptied view stays";
    EXPECT_EQ(scene->parent(), frame.get());
    EXPECT_TRUE(scene->layers().empty());
    EXPECT_FALSE(app.topicDemandTracker().hasAnyReference(dataset));
  }

  // A detach undoes the attach it pairs with even when the name it was attached by
  // has since become ambiguous across datasets.
  static void checkEmbeddedSceneViewDetachesTheTopicItAttached(MainWindow& window) {
    AppSession& app = *window.session_;
    const auto first = pj_test::createDataset(app, "embedded-dup-a", true);
    registerObjectTopics(app, first, {"/embedded-dup"});
    const EmbeddedViewHost host = window.makeEmbeddedViewHost();
    auto frame = std::make_unique<QFrame>();
    auto* scene = qobject_cast<SceneDockWidget*>(host.create(u"3d"_s, frame.get()));
    ASSERT_NE(scene, nullptr);
    std::string error;
    ASSERT_TRUE(host.attach(scene, u"/embedded-dup"_s, {}, &error)) << error;
    ASSERT_TRUE(app.topicDemandTracker().hasAnyReference(first));

    const auto second = pj_test::createDataset(app, "embedded-dup-b", true);
    registerObjectTopics(app, second, {"/embedded-dup"});
    ASSERT_FALSE(host.attach(scene, u"/embedded-dup"_s, {}, &error)) << "the bare name is now ambiguous";
    host.detach(scene, u"/embedded-dup"_s, {});
    EXPECT_TRUE(scene->layers().empty());
    EXPECT_FALSE(app.topicDemandTracker().hasAnyReference(first));
    unloadDataset(app, first);
    unloadDataset(app, second);
  }

  // A view hidden while the playhead moves shows the current playhead once revealed.
  static void checkEmbeddedSceneViewCatchesUpWhenRevealed(MainWindow& window) {
    PlaybackEngine& playback = window.session_->playbackEngine();
    const EmbeddedViewHost host = window.makeEmbeddedViewHost();
    auto frame = std::make_unique<QFrame>();
    auto* scene = qobject_cast<SceneDockWidget*>(host.create(u"3d"_s, frame.get()));
    ASSERT_NE(scene, nullptr);
    frame->show();
    playback.setRangeAndCurrentTime(displayRange(0.0, 20.0), displaySeconds(3.0));
    window.broadcastTrackerTime(3.0);
    ASSERT_EQ(lastTrackerNs(*scene), std::optional<int64_t>{3'000'000'000});

    frame->hide();
    playback.setCurrentTime(displaySeconds(11.0));  // a seek, then paused
    window.broadcastTrackerTimeToVisible(11.0);
    ASSERT_EQ(lastTrackerNs(*scene), std::optional<int64_t>{3'000'000'000}) << "a hidden view skips the move";
    frame->show();
    EXPECT_EQ(lastTrackerNs(*scene), std::optional<int64_t>{11'000'000'000});
    playback.setRangeAndCurrentTime(displayRange(0.0, 0.0), displaySeconds(0.0));
  }

  /// "Remove all Datasets" as the shell runs it: the store is cleared (its ids restart)
  /// before the catalog, whose cleared() revalidates every widget.
  static void removeAllDatasets(AppSession& app) {
    app.sessionManager().clearAllObjects();
    app.catalogModel().clearAll(/*tombstone=*/false);
  }

  /// A 3D view embedded in a frame the caller owns.
  [[nodiscard]] static SceneDockWidget* embedded3dView(const EmbeddedViewHost& host, QFrame& frame) {
    return qobject_cast<SceneDockWidget*>(host.create(u"3d"_s, &frame));
  }

  // Removing every dataset and loading the topic again shows it again: the frame still
  // asks for it, and its unchanged request makes the binding no call.
  static void checkEmbeddedSceneViewReattachesAfterReload(MainWindow& window) {
    AppSession& app = *window.session_;
    registerObjectTopics(app, pj_test::createDataset(app, "embedded-reload-a", true), {"/embedded-reload"});
    const EmbeddedViewHost host = window.makeEmbeddedViewHost();
    QFrame frame;
    SceneDockWidget* scene = embedded3dView(host, frame);
    ASSERT_NE(scene, nullptr);
    std::string error;
    ASSERT_TRUE(host.attach(scene, u"/embedded-reload"_s, {}, &error)) << error;

    removeAllDatasets(app);
    ASSERT_TRUE(scene->layers().empty());
    const auto reloaded = pj_test::createDataset(app, "embedded-reload-b", true);
    registerObjectTopics(app, reloaded, {"/embedded-reload"});
    window.syncWidgetsToCatalog();  // as a finished load does
    const auto topic = app.sessionManager().objectStore().findTopic(reloaded, "/embedded-reload");
    ASSERT_TRUE(topic.has_value());
    ASSERT_EQ(scene->layers().size(), 1U) << "the reloaded topic must show again";
    EXPECT_EQ(scene->layers().front().topic_id.id, topic->id);
  }

  // ObjectStore::clear() restarts the ids, so the id an evicted topic was attached by
  // may name another topic later: that topic neither hides the evicted one's return
  // nor goes when the evicted one is detached.
  static void checkEmbeddedSceneViewNeverDetachesARecycledId(MainWindow& window) {
    AppSession& app = *window.session_;
    ObjectStore& store = app.sessionManager().objectStore();
    const auto first = pj_test::createDataset(app, "embedded-recycle-a", true);
    registerObjectTopics(app, first, {"/embedded-old"});
    const auto old_topic = store.findTopic(first, "/embedded-old");
    ASSERT_TRUE(old_topic.has_value());
    const EmbeddedViewHost host = window.makeEmbeddedViewHost();
    QFrame frame;
    SceneDockWidget* scene = embedded3dView(host, frame);
    ASSERT_NE(scene, nullptr);
    std::string error;
    ASSERT_TRUE(host.attach(scene, u"/embedded-old"_s, {}, &error)) << error;

    removeAllDatasets(app);
    const auto second = pj_test::createDataset(app, "embedded-recycle-b", true);
    registerObjectTopics(app, second, {"/embedded-new", "/embedded-old"});
    const auto new_topic = store.findTopic(second, "/embedded-new");
    ASSERT_TRUE(new_topic.has_value());
    ASSERT_EQ(new_topic->id, old_topic->id) << "the fixture relies on clear() recycling the id";
    ASSERT_TRUE(host.attach(scene, u"/embedded-new"_s, {}, &error)) << error;
    window.syncWidgetsToCatalog();  // as a finished load does
    EXPECT_EQ(scene->layers().size(), 2U) << "the returning topic must show beside the one holding its old id";

    host.detach(scene, u"/embedded-old"_s, {});
    ASSERT_EQ(scene->layers().size(), 1U) << "detaching the evicted topic removed another one";
    EXPECT_EQ(scene->layers().front().topic_id.id, new_topic->id);
  }

  // Two entries naming one topic, by bare name and by dataset, share its layer: it
  // stays until the last of them is detached.
  static void checkEmbeddedSceneViewKeepsATopicAnotherEntryHolds(MainWindow& window) {
    AppSession& app = *window.session_;
    registerObjectTopics(app, pj_test::createDataset(app, "embedded-twice", true), {"/embedded-twice"});
    const EmbeddedViewHost host = window.makeEmbeddedViewHost();
    QFrame frame;
    SceneDockWidget* scene = embedded3dView(host, frame);
    ASSERT_NE(scene, nullptr);
    std::string error;
    ASSERT_TRUE(host.attach(scene, u"/embedded-twice"_s, {}, &error)) << error;
    ASSERT_TRUE(host.attach(scene, u"/embedded-twice"_s, u"embedded-twice"_s, &error)) << error;
    ASSERT_EQ(scene->layers().size(), 1U);

    host.detach(scene, u"/embedded-twice"_s, {});
    EXPECT_EQ(scene->layers().size(), 1U) << "the dataset-qualified entry still holds the topic";
    host.detach(scene, u"/embedded-twice"_s, u"embedded-twice"_s);
    EXPECT_TRUE(scene->layers().empty());
  }

  // An embedded view's changes are the plugin's, not the workspace's: no workspace undo
  // snapshot, and a refusal reaches the plugin as scene_view_error instead of a toast.
  static void checkEmbeddedSceneViewHasNoMainWindowSideEffects(MainWindow& window) {
    AppSession& app = *window.session_;
    registerObjectTopics(app, pj_test::createDataset(app, "embedded-quiet", true), {"/embedded-quiet"});
    const EmbeddedViewHost host = window.makeEmbeddedViewHost();
    QFrame frame;
    SceneDockWidget* scene = embedded3dView(host, frame);
    ASSERT_NE(scene, nullptr);
    window.scene_undo_debounce_.stop();
    QSignalSpy workspace_changes(scene, &SceneDockWidget::workspaceChanged);
    std::string error;
    ASSERT_TRUE(host.attach(scene, u"/embedded-quiet"_s, {}, &error)) << error;
    ASSERT_GT(workspace_changes.count(), 0) << "the fixture relies on an attach changing the workspace";
    EXPECT_FALSE(window.scene_undo_debounce_.isActive()) << "an embedded attach must not snapshot the workspace";

    const qsizetype toasts = window.findChildren<ToastNotification*>().size();
    emit scene->topicRefused(u"another dataset's topic"_s);
    EXPECT_EQ(window.findChildren<ToastNotification*>().size(), toasts) << "an embedded refusal must not toast";
  }
#endif

#ifdef PJ_WITH_SCENE2D
  /// Registers a 2D background topic of `type` with a bound parser, as an image source has.
  static void registerParsedImageTopic(
      AppSession& app, DatasetId dataset, const std::string& name, sdk::BuiltinObjectType type) {
    const auto metadata = sdk::ObjectTopicMetadataBuilder().builtinObjectType(type).build();
    ASSERT_TRUE(metadata);
    const auto topic = app.sessionManager().objectStore().registerTopic({dataset, name, *metadata});
    ASSERT_TRUE(topic);
    auto parser = std::make_unique<MessageParserHandle>(MessageParserPluginBase::vtableWithCreate(
        []() noexcept -> void* { return new MessageParserPluginBase(); },
        R"({"id":"image-fixture","name":"Image Fixture","version":"1.0.0","encoding":["fixture"]})"));
    ASSERT_TRUE(parser->valid());
    app.sessionManager().registerObjectTopicParser(*topic, std::move(parser));
    app.catalogModel().rebuildFromDatastore();
  }

  // An embedded 2D view refuses a second background instead of swapping it in: the
  // panel's binding holds every topic it attached, so a swap would leave it stale.
  static void checkEmbedded2dViewRefusesASecondBackground(MainWindow& window) {
    AppSession& app = *window.session_;
    const auto dataset = pj_test::createDataset(app, "embedded-2d", true);
    registerParsedImageTopic(app, dataset, "/embedded-bg-a", sdk::BuiltinObjectType::kImage);
    registerParsedImageTopic(app, dataset, "/embedded-bg-b", sdk::BuiltinObjectType::kImage);
    const EmbeddedViewHost host = window.makeEmbeddedViewHost();
    auto frame = std::make_unique<QFrame>();
    auto* scene = qobject_cast<SceneDockWidget*>(host.create(u"2d"_s, frame.get()));
    ASSERT_NE(scene, nullptr);
    std::string error;
    ASSERT_TRUE(host.attach(scene, u"/embedded-bg-a"_s, u"embedded-2d"_s, &error)) << error;
    const auto first = app.sessionManager().objectStore().findTopic(dataset, "/embedded-bg-a");
    ASSERT_TRUE(first.has_value());

    EXPECT_FALSE(host.attach(scene, u"/embedded-bg-b"_s, u"embedded-2d"_s, &error));
    EXPECT_NE(error.find("a 2D view shows one background"), std::string::npos) << error;
    ASSERT_EQ(scene->layers().size(), 1U);
    EXPECT_EQ(scene->layers().front().topic_id.id, first->id) << "the first background stays";
    EXPECT_TRUE(host.attach(scene, u"/embedded-bg-a"_s, u"embedded-2d"_s, &error)) << "re-attaching it is success";

    // A plugin's 2D tab keeps replacing its background.
    TabsApi api = tabsApi(window, u"embedded-2d-plugin"_s);
    ASSERT_TRUE(api.view.createTabV2("image", "2d", "Image"));
    ASSERT_TRUE(api.view.attachTopic("image", "/embedded-bg-a", "embedded-2d"));
    const auto replaced = api.view.attachTopic("image", "/embedded-bg-b", "embedded-2d");
    EXPECT_TRUE(replaced) << replaced.error();
    const auto config = api.view.configOf("image");
    ASSERT_TRUE(config);
    EXPECT_EQ(config->find("/embedded-bg-a"), std::string::npos) << *config;
    EXPECT_NE(config->find("/embedded-bg-b"), std::string::npos) << *config;
    ASSERT_TRUE(api.view.close("image"));
    unloadDataset(app, dataset);
  }
#endif

#ifdef PJ_HAS_FFMPEG
  // ABI-40: a 2D tab takes a video topic as its background.
  static void checkVideoSceneView(MainWindow& window) {
    auto& app = *window.session_;
    const auto dataset = pj_test::createDataset(app, "video-scene-view", true);
    const auto metadata =
        sdk::ObjectTopicMetadataBuilder().builtinObjectType(sdk::BuiltinObjectType::kVideoFrame).build();
    ASSERT_TRUE(metadata);
    const auto topic = app.sessionManager().objectStore().registerTopic({dataset, "/video", *metadata});
    ASSERT_TRUE(topic);
    auto parser = std::make_unique<MessageParserHandle>(MessageParserPluginBase::vtableWithCreate(
        []() noexcept -> void* { return new MessageParserPluginBase(); },
        R"({"id":"video-fixture","name":"Video Fixture","version":"1.0.0","encoding":["fixture"]})"));
    ASSERT_TRUE(parser->valid());
    app.sessionManager().registerObjectTopicParser(*topic, std::move(parser));
    app.catalogModel().rebuildFromDatastore();
    TabsApi api = tabsApi(window, u"video-plugin"_s);
    ASSERT_TRUE(api.view.createTabV2("video", "2d", "Video"));
    ASSERT_TRUE(api.view.attachTopic("video", "/video", "video-scene-view"));
    const auto config = api.view.configOf("video");
    ASSERT_TRUE(config);
    EXPECT_NE(config->find("\"kind\":\"2d\""), std::string::npos) << *config;
    EXPECT_NE(config->find("kVideoFrame"), std::string::npos) << *config;
    EXPECT_TRUE(api.view.close("video"));
  }
#endif

  // LAY-16: a full-layout save/restore rebuilds an owned scene tab as a scene, found
  // by (plugin, id) from a fresh controller, history-exempt, with its topic kept.
  static void checkSceneViewsLayoutRoundTrip(MainWindow& window) {
    const QString owner = u"scene-view-roundtrip-plugin"_s;
    AppSession& app = *window.session_;
    const auto dataset = pj_test::createDataset(app, "scene-views-roundtrip", true);
    ASSERT_NE(dataset, 0u);
    registerObjectTopics(app, dataset, {"/roundtrip-cloud"});

    {
      TabsApi api = tabsApi(window, owner);
      ASSERT_TRUE(api.view.createTabV2("view", "3d", "Round-trip View"));
      ASSERT_TRUE(api.view.attachTopic("view", "/roundtrip-cloud", "scene-views-roundtrip"));
    }
    PlotDocker* before_restore = pluginTabs(window).ownedTab(owner, u"view"_s);
    ASSERT_NE(sceneDockOf(before_restore), nullptr);
    EXPECT_EQ(sceneDockOf(before_restore)->layers().size(), 1U);

    const QDomDocument saved = window.xmlSaveState(MainWindow::SnapshotScope::kFull);
    const QString saved_xml = QString::fromUtf8(saved.toByteArray());
    EXPECT_TRUE(saved_xml.contains(u"owner_plugin=\"scene-view-roundtrip-plugin\""_s));
    window.plugin_plot_tabs_ = makeController(window, windowDockFactory(window));
    ASSERT_TRUE(window.xmlLoadState(saved, MainWindow::RestoreIntent::kReplace));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    PlotDocker* restored = pluginTabs(window).ownedTab(owner, u"view"_s);
    ASSERT_NE(restored, nullptr) << "an owned scene tab must still be found by (plugin_id, tab_id) after restore";
    EXPECT_TRUE(restored->isHistoryExempt());
    SceneDockWidget* restored_scene = sceneDockOf(restored);
    ASSERT_NE(restored_scene, nullptr) << "the restored owned tab must rebuild its scene3d widget, not a plot";
    TabsApi restored_api = tabsApi(window, owner);
    const auto ids = restored_api.view.list();
    ASSERT_TRUE(ids);
    EXPECT_EQ(*ids, (std::vector<std::string>{"view"}));
    const auto config = restored_api.view.configOf("view");
    ASSERT_TRUE(config);
    EXPECT_NE(config->find("\"kind\":\"3d\""), std::string::npos) << *config;
    EXPECT_EQ(restored_scene->layers().size(), 1U) << "the attached topic must survive the round trip";
  }

  // ABI-39: one id namespace per plugin across kinds. Another kind closes the old tab
  // and starts empty; the same kind clears a plot tab's curves but only retitles a
  // scene tab (its topics stay); v1 create is the "plot" kind.
  static void checkSharedIdNamespace(MainWindow& window) {
    const QString owner = u"namespace-plugin"_s;
    AppSession& app = *window.session_;
    const auto dataset = pj_test::createDataset(app, "namespace-run", true);
    ASSERT_NE(pj_test::addScalarTopic(app, dataset, "/ns-speed"), 0U);
    registerObjectTopics(app, dataset, {"/ns-cloud"});
    TabsApi api = tabsApi(window, owner);

    ASSERT_TRUE(api.view.createTabV2("a", "plot", "Plot A"));
    QPointer<PlotDocker> plot_tab(pluginTabs(window).ownedTab(owner, u"a"_s));
    ASSERT_FALSE(plot_tab.isNull());
    EXPECT_TRUE(sceneKindOf(plot_tab).isEmpty());
    ASSERT_TRUE(api.view.addCurve("a", "/ns-speed", "value", "namespace-run"));
    ASSERT_TRUE(api.view.createTabV2("a", "plot", "Plot A again"));
    EXPECT_EQ(pluginTabs(window).ownedTab(owner, u"a"_s), plot_tab.data());
    EXPECT_EQ(plot_tab->name(), u"Plot A again"_s);
    auto config = api.view.configOf("a");
    ASSERT_TRUE(config);
    EXPECT_EQ(config->find("/ns-speed"), std::string::npos) << "same kind clears a plot tab: " << *config;

    ASSERT_TRUE(api.view.createTabV2("a", "3d", "Scene A"));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_TRUE(plot_tab.isNull()) << "the plot tab must be closed when its id is reused as a 3d tab";
    PlotDocker* scene_tab = pluginTabs(window).ownedTab(owner, u"a"_s);
    EXPECT_EQ(sceneKindOf(scene_tab), u"3d"_s);
    auto ids = api.view.list();
    ASSERT_TRUE(ids);
    EXPECT_EQ(*ids, (std::vector<std::string>{"a"}));

    ASSERT_TRUE(api.view.attachTopic("a", "/ns-cloud", "namespace-run"));
    ASSERT_TRUE(api.view.createTabV2("a", "3d", "Renamed"));
    EXPECT_EQ(pluginTabs(window).ownedTab(owner, u"a"_s), scene_tab);
    EXPECT_EQ(scene_tab->name(), u"Renamed"_s);
    EXPECT_EQ(sceneDockOf(scene_tab)->layers().size(), 1U) << "same kind only retitles a scene tab";

    ASSERT_TRUE(api.view.createTabV2("a", "2d", "Image A"));
    EXPECT_EQ(sceneKindOf(pluginTabs(window).ownedTab(owner, u"a"_s)), u"2d"_s);
    EXPECT_TRUE(sceneDockOf(pluginTabs(window).ownedTab(owner, u"a"_s))->layers().empty());
    ASSERT_TRUE(api.view.create("a", "Plot again"));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    ASSERT_NE(pluginTabs(window).ownedTab(owner, u"a"_s), nullptr);
    EXPECT_TRUE(sceneKindOf(pluginTabs(window).ownedTab(owner, u"a"_s)).isEmpty());
    ids = api.view.list();
    ASSERT_TRUE(ids);
    EXPECT_EQ(*ids, (std::vector<std::string>{"a"}));

    // Operations of the other kind are errors that name the right slot.
    ASSERT_TRUE(api.view.createTabV2("s", "3d", "S"));
    const auto attach = api.view.attachTopic("a", "/t");
    ASSERT_FALSE(attach);
    EXPECT_NE(attach.error().find("tab 'a' is a plot tab: use add_curve"), std::string::npos) << attach.error();
    const auto detach = api.view.detachTopic("a", "/t");
    ASSERT_FALSE(detach);
    EXPECT_NE(detach.error().find("tab 'a' is a plot tab: use remove_curve"), std::string::npos) << detach.error();
    EXPECT_TRUE(api.view.focusTab("a"));
    EXPECT_TRUE(api.view.focusTab("s"));
    EXPECT_TRUE(api.view.close("a"));
    EXPECT_TRUE(api.view.close("s"));
  }

  static void checkAddCurveOnSceneTabIsRejected(MainWindow& window) {
    const QString owner = u"scene-addcurve-plugin"_s;
    AppSession& app = *window.session_;
    const auto dataset = pj_test::createDataset(app, "scene-addcurve", true);
    ASSERT_NE(pj_test::addScalarTopic(app, dataset, "/scalar"), 0U);
    TabsApi api = tabsApi(window, owner);
    ASSERT_TRUE(api.view.createTabV2("view", "2d", "V"));
    const auto add = api.view.addCurve("view", "/scalar", "value", "scene-addcurve");
    ASSERT_FALSE(add);
    EXPECT_NE(add.error().find("tab 'view' is a 2d scene tab: use attach_topic/detach_topic"), std::string::npos)
        << add.error();
    const auto remove = api.view.removeCurve("view", "/scalar", "value", "scene-addcurve");
    ASSERT_FALSE(remove);
    EXPECT_NE(remove.error().find("tab 'view' is a 2d scene tab: use attach_topic/detach_topic"), std::string::npos)
        << remove.error();
    ASSERT_TRUE(api.view.close("view"));
  }

  static void checkClearOnSceneTabDetachesEverything(MainWindow& window) {
    const QString owner = u"scene-clear-plugin"_s;
    AppSession& app = *window.session_;
    const auto dataset = pj_test::createDataset(app, "scene-clear", true);
    ASSERT_NE(dataset, 0u);
    registerObjectTopics(app, dataset, {"/cloud-1", "/cloud-2"});

    TabsApi api = tabsApi(window, owner);
    ASSERT_TRUE(api.view.createTabV2("view", "3d", "V"));
    ASSERT_TRUE(api.view.attachTopic("view", "/cloud-1", "scene-clear"));
    ASSERT_TRUE(api.view.attachTopic("view", "/cloud-2", "scene-clear"));
    PlotDocker* docker = pluginTabs(window).ownedTab(owner, u"view"_s);
    ASSERT_NE(sceneDockOf(docker), nullptr);
    EXPECT_EQ(sceneDockOf(docker)->layers().size(), 2U);

    ASSERT_TRUE(api.view.clear("view"));
    EXPECT_EQ(pluginTabs(window).ownedTab(owner, u"view"_s), docker) << "clear keeps the tab";
    EXPECT_EQ(sceneDockOf(docker)->layers().size(), 0U);
    const auto config = api.view.configOf("view");
    ASSERT_TRUE(config);
    EXPECT_EQ(config->find("/cloud-"), std::string::npos) << *config;
    ASSERT_TRUE(api.view.close("view"));
  }

  // A FrameTransforms topic is a scene-wide config topic of a 3D tab, not a layer:
  // tab_config still lists it and detach_topic removes it.
  static void checkTfTopicIsListedAndDetachable(MainWindow& window) {
    const QString owner = u"scene-tf-plugin"_s;
    AppSession& app = *window.session_;
    const auto dataset = pj_test::createDataset(app, "scene-tf", true);
    ASSERT_NE(dataset, 0u);
    registerObjectTopics(app, dataset, {"/tf-only"}, sdk::BuiltinObjectType::kFrameTransforms);

    TabsApi api = tabsApi(window, owner);
    ASSERT_TRUE(api.view.createTabV2("view", "3d", "TF"));
    ASSERT_TRUE(api.view.attachTopic("view", "/tf-only", "scene-tf"));
    auto config = api.view.configOf("view");
    ASSERT_TRUE(config);
    EXPECT_EQ(
        *config,
        R"({"kind":"3d","title":"TF","topics":[{"dataset":"scene-tf","topic":"/tf-only","type":"kFrameTransforms","visible":true}]})");

    ASSERT_TRUE(api.view.detachTopic("view", "/tf-only", "scene-tf"));
    config = api.view.configOf("view");
    ASSERT_TRUE(config);
    EXPECT_EQ(config->find("/tf-only"), std::string::npos) << *config;
    const auto absent = api.view.detachTopic("view", "/tf-only", "scene-tf");
    ASSERT_FALSE(absent);
    EXPECT_NE(absent.error().find("'/tf-only' is not attached"), std::string::npos) << absent.error();
    ASSERT_TRUE(api.view.close("view"));
  }

  // clear_tab releases a TF topic too, so the tab can then show another dataset.
  static void checkClearReleasesTfForAnotherDataset(MainWindow& window) {
    const QString owner = u"scene-tf-clear-plugin"_s;
    AppSession& app = *window.session_;
    const auto first = pj_test::createDataset(app, "tf-clear-a", true);
    const auto second = pj_test::createDataset(app, "tf-clear-b", true);
    registerObjectTopics(app, first, {"/tf-clear-a"}, sdk::BuiltinObjectType::kFrameTransforms);
    registerObjectTopics(app, second, {"/cloud-clear-b"});

    TabsApi api = tabsApi(window, owner);
    ASSERT_TRUE(api.view.createTabV2("view", "3d", "V"));
    ASSERT_TRUE(api.view.attachTopic("view", "/tf-clear-a"));
    ASSERT_TRUE(api.view.clear("view"));
    const auto config = api.view.configOf("view");
    ASSERT_TRUE(config);
    EXPECT_EQ(config->find("/tf-clear-a"), std::string::npos) << *config;
    const auto other = api.view.attachTopic("view", "/cloud-clear-b");
    EXPECT_TRUE(other) << other.error();
    ASSERT_TRUE(api.view.close("view"));
  }

  // A scene tab the plugin emptied (clear, or detaching its last topic) stays a scene
  // tab when a later load revalidates every widget.
  static void checkEmptiedSceneTabKeepsItsKind(MainWindow& window) {
    const QString owner = u"scene-emptied-plugin"_s;
    AppSession& app = *window.session_;
    const auto dataset = pj_test::createDataset(app, "scene-emptied", true);
    ASSERT_NE(dataset, 0u);
    registerObjectTopics(app, dataset, {"/emptied-cloud"});

    TabsApi api = tabsApi(window, owner);
    ASSERT_TRUE(api.view.createTabV2("cleared", "3d", "Cleared"));
    ASSERT_TRUE(api.view.attachTopic("cleared", "/emptied-cloud"));
    ASSERT_TRUE(api.view.clear("cleared"));
    ASSERT_TRUE(api.view.createTabV2("detached", "3d", "Detached"));
    ASSERT_TRUE(api.view.attachTopic("detached", "/emptied-cloud"));
    ASSERT_TRUE(api.view.detachTopic("detached", "/emptied-cloud"));

    window.syncWidgetsToCatalog();  // what a file load runs
    for (const char* id : {"cleared", "detached"}) {
      EXPECT_EQ(sceneKindOf(pluginTabs(window).ownedTab(owner, QString::fromUtf8(id))), u"3d"_s) << id;
      const auto attach = api.view.attachTopic(id, "/emptied-cloud");
      EXPECT_TRUE(attach) << id << ": " << attach.error();
      ASSERT_TRUE(api.view.close(id));
    }
  }

  /// What a dataset removal does: hide its catalog items, evict its objects, drop it.
  static void unloadDataset(AppSession& app, DatasetId dataset) {
    app.catalogModel().removeDataset(dataset, /*tombstone=*/false);
    app.sessionManager().evictDatasetObjects(dataset);
    app.sessionManager().removeDataset(dataset);
  }

  // Eviction empties a plugin's scene tab but, unlike a user's dock, does not turn it
  // into a plot tab: the plugin made it a scene and keeps attaching to it.
  static void checkEvictedPluginSceneTabKeepsItsKind(MainWindow& window) {
    const QString owner = u"scene-evict-plugin"_s;
    AppSession& app = *window.session_;
    const auto first = pj_test::createDataset(app, "scene-evict-a", true);
    registerObjectTopics(app, first, {"/evict-cloud-a"});

    TabsApi api = tabsApi(window, owner);
    ASSERT_TRUE(api.view.createTabV2("view", "3d", "V"));
    ASSERT_TRUE(api.view.attachTopic("view", "/evict-cloud-a"));

    unloadDataset(app, first);
    window.syncWidgetsToCatalog();
    PlotDocker* docker = pluginTabs(window).ownedTab(owner, u"view"_s);
    ASSERT_NE(docker, nullptr);
    ASSERT_EQ(sceneKindOf(docker), u"3d"_s) << "eviction must not reset a plugin's scene tab";
    EXPECT_TRUE(sceneDockOf(docker)->layers().empty());

    const auto second = pj_test::createDataset(app, "scene-evict-b", true);
    registerObjectTopics(app, second, {"/evict-cloud-b"});
    const auto attach = api.view.attachTopic("view", "/evict-cloud-b");
    EXPECT_TRUE(attach) << attach.error();
    ASSERT_TRUE(api.view.close("view"));
  }

  // clear_tab drops the restore intents a layout left pending, so the data loading
  // later does not bring the cleared topics back.
  static void checkClearDropsPendingLayoutRestores(MainWindow& window) {
    const QString owner = u"scene-pending-plugin"_s;
    AppSession& app = *window.session_;
    const auto dataset = pj_test::createDataset(app, "scene-pending", true);
    registerObjectTopics(app, dataset, {"/pending-cloud"});
    {
      TabsApi api = tabsApi(window, owner);
      ASSERT_TRUE(api.view.createTabV2("view", "3d", "V"));
      ASSERT_TRUE(api.view.attachTopic("view", "/pending-cloud"));
    }
    const QDomDocument saved = window.xmlSaveState(MainWindow::SnapshotScope::kFull);
    unloadDataset(app, dataset);
    window.xmlLoadState(saved, MainWindow::RestoreIntent::kReplace);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    SceneDockWidget* scene = sceneDockOf(pluginTabs(window).ownedTab(owner, u"view"_s));
    ASSERT_NE(scene, nullptr);
    ASSERT_FALSE(scene->unresolvedPendingRestores().isEmpty()) << "the layout must leave the topic pending";

    TabsApi api = tabsApi(window, owner);
    ASSERT_TRUE(api.view.clear("view"));
    EXPECT_TRUE(scene->unresolvedPendingRestores().isEmpty());

    const auto reloaded = pj_test::createDataset(app, "scene-pending", true);
    registerObjectTopics(app, reloaded, {"/pending-cloud"});
    scene->retryPendingRestores({});
    EXPECT_TRUE(scene->layers().empty()) << "a cleared tab must not get its topic back";
    ASSERT_TRUE(api.view.close("view"));
  }

#ifdef PJ_WITH_SCENE3D
  // A layer the user added by hand (a URDF file, TF trails) has no object topic: it is
  // not part of tab_config and clear_tab leaves it alone.
  static void checkLocalLayersAreNotThePluginsTopics(MainWindow& window) {
    const QString owner = u"scene-local-plugin"_s;
    AppSession& app = *window.session_;
    const auto dataset = pj_test::createDataset(app, "scene-local", true);
    registerObjectTopics(app, dataset, {"/local-cloud"});

    TabsApi api = tabsApi(window, owner);
    ASSERT_TRUE(api.view.createTabV2("view", "3d", "V"));
    ASSERT_TRUE(api.view.attachTopic("view", "/local-cloud"));
    auto* scene = qobject_cast<Scene3DDockWidget*>(sceneDockOf(pluginTabs(window).ownedTab(owner, u"view"_s)));
    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->addRobotModelLayer().id, 0U);
    ASSERT_EQ(scene->layers().size(), 2U);

    auto config = api.view.configOf("view");
    ASSERT_TRUE(config);
    EXPECT_EQ(
        *config,
        R"({"kind":"3d","title":"V","topics":[{"dataset":"scene-local","topic":"/local-cloud","type":"kPointCloud","visible":true}]})");

    ASSERT_TRUE(api.view.clear("view"));
    EXPECT_EQ(scene->layers().size(), 1U) << "clear_tab must leave the user's own layer";
    config = api.view.configOf("view");
    ASSERT_TRUE(config);
    EXPECT_EQ(config->find("/local-cloud"), std::string::npos) << *config;
    EXPECT_EQ(config->find(R"("topic":"")"), std::string::npos) << *config;
    ASSERT_TRUE(api.view.close("view"));
  }
#endif

  // The 3D attach runs the family's own gate: a colour image is not a depth cloud.
  static void checkColourImageIsRefusedBy3dTab(MainWindow& window) {
    const QString owner = u"scene-rgb-plugin"_s;
    AppSession& app = *window.session_;
    const auto dataset = pj_test::createDataset(app, "scene-rgb", true);
    ASSERT_NE(dataset, 0u);
    registerObjectTopics(app, dataset, {"/rgb"}, sdk::BuiltinObjectType::kImage);
    const std::vector<uint8_t> pixels(2 * 2 * 3, 128);
    sdk::Image image;
    image.width = 2;
    image.height = 2;
    image.encoding = "rgb8";
    image.data = Span<const uint8_t>(pixels.data(), pixels.size());
    const auto topic = app.sessionManager().objectStore().findTopic(dataset, "/rgb");
    ASSERT_TRUE(topic.has_value());
    ASSERT_TRUE(app.sessionManager().objectStore().pushOwned(*topic, 100, serializeImage(image)).has_value());
    // A bound parser: without one the depth-cloud layer refuses to attach, gate or no gate.
    auto parser = std::make_unique<MessageParserHandle>(MessageParserPluginBase::vtableWithCreate(
        []() noexcept -> void* { return new MessageParserPluginBase(); },
        R"({"id":"rgb-fixture","name":"RGB Fixture","version":"1.0.0","encoding":["fixture"]})"));
    ASSERT_TRUE(parser->valid());
    app.sessionManager().registerObjectTopicParser(*topic, std::move(parser));

    TabsApi api = tabsApi(window, owner);
    ASSERT_TRUE(api.view.createTabV2("view", "3d", "V"));
    const auto refused = api.view.attachTopic("view", "/rgb");
    ASSERT_FALSE(refused);
    EXPECT_NE(refused.error().find("could not attach topic '/rgb'"), std::string::npos) << refused.error();
    EXPECT_TRUE(sceneDockOf(pluginTabs(window).ownedTab(owner, u"view"_s))->layers().empty());
    ASSERT_TRUE(api.view.close("view"));
  }

  // The v1 JSON of a plot tab is a published contract: no "kind" key.
  static void checkTabConfigOfPlotTabIsUnchanged(MainWindow& window) {
    const QString owner = u"plot-config-plugin"_s;
    AppSession& app = *window.session_;
    const auto dataset = pj_test::createDataset(app, "config-run", true);
    ASSERT_NE(pj_test::addScalarTopic(app, dataset, "/speed"), 0U);
    TabsApi api = tabsApi(window, owner);
    ASSERT_TRUE(api.view.createTabV2("view", "plot", "Speed"));
    ASSERT_TRUE(api.view.addCurve("view", "/speed", "value", "config-run"));
    const auto config = api.view.configOf("view");
    ASSERT_TRUE(config);
    EXPECT_EQ(*config, R"({"curves":[{"dataset":"config-run","field":"value","topic":"/speed"}],"title":"Speed"})");
    ASSERT_TRUE(api.view.close("view"));
  }

  // GAP-44: a kind this build cannot show is refused naming it, and an existing tab
  // under that id stays as it was.
  static void checkKnownUnservableKind(MainWindow& window) {
    const QString owner = u"unservable-plugin"_s;
    auto controller = makeController(window, [](const QString&, QWidget*) -> IDataWidget* { return nullptr; });
    TabsApi api = tabsApi(*controller, owner);
    ASSERT_TRUE(api.view.createTabV2("t", "plot", "Kept"));
    PlotDocker* kept = controller->ownedTab(owner, u"t"_s);
    ASSERT_NE(kept, nullptr);
    for (const char* kind : {"3d", "2d"}) {
      const auto refused = api.view.createTabV2("t", kind, "Scene");
      ASSERT_FALSE(refused);
      EXPECT_NE(refused.error().find(std::string("'") + kind + "'"), std::string::npos) << refused.error();
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
      EXPECT_EQ(controller->ownedTab(owner, u"t"_s), kept);
      EXPECT_EQ(kept->name(), u"Kept"_s);
      EXPECT_TRUE(sceneKindOf(kept).isEmpty());
    }
    ASSERT_TRUE(api.view.close("t"));
  }

  // SCN-17 through the plugin: a scene tab showing dataset A refuses B's topic with the
  // dock's own reason; an unqualified name two datasets share is refused with both
  // qualified candidates.
  static void checkAttachOfAnotherDatasetsTopic(MainWindow& window) {
    const QString owner = u"scene-dataset-plugin"_s;
    AppSession& app = *window.session_;
    const auto first = pj_test::createDataset(app, "scene-run-a", true);
    const auto second = pj_test::createDataset(app, "scene-run-b", true);
    registerObjectTopics(app, first, {"/shared-cloud", "/a-cloud"});
    registerObjectTopics(app, second, {"/shared-cloud", "/b-cloud", "/b-cloud-2"});

    TabsApi api = tabsApi(window, owner);
    ASSERT_TRUE(api.view.createTabV2("view", "3d", "V"));
    const auto ambiguous = api.view.attachTopic("view", "/shared-cloud");
    ASSERT_FALSE(ambiguous);
    EXPECT_NE(ambiguous.error().find("scene-run-a:/shared-cloud"), std::string::npos) << ambiguous.error();
    EXPECT_NE(ambiguous.error().find("scene-run-b:/shared-cloud"), std::string::npos) << ambiguous.error();

    ASSERT_TRUE(api.view.attachTopic("view", "/a-cloud"));
    ASSERT_TRUE(api.view.attachTopic("view", "/shared-cloud", "scene-run-a"));
    const auto foreign = api.view.attachTopic("view", "/b-cloud");
    ASSERT_FALSE(foreign);
    EXPECT_NE(foreign.error().find("cannot be added"), std::string::npos) << foreign.error();
    EXPECT_NE(foreign.error().find("scene-run-b"), std::string::npos) << foreign.error();
    // A second refusal in the same event-loop turn still carries the dock's reason.
    const auto again = api.view.attachTopic("view", "/b-cloud-2");
    ASSERT_FALSE(again);
    EXPECT_NE(again.error().find("cannot be added"), std::string::npos) << again.error();
    EXPECT_EQ(sceneDockOf(pluginTabs(window).ownedTab(owner, u"view"_s))->layers().size(), 2U);
    ASSERT_TRUE(api.view.close("view"));
  }

  // The bench: attach_topic among 10k object topics of the catalog, a first attachment
  // (a new layer: its demand reference resolves the topic) apart from a re-attachment.
  static void benchAttachAmongTenThousandTopics(MainWindow& window) {
    AppSession& app = *window.session_;
    const auto dataset = pj_test::createDataset(app, "bench-run", true);
    std::vector<std::string> names;
    for (int index = 0; index < 10'000; ++index) {
      names.push_back("/bench/cloud_" + std::to_string(index));
    }
    registerObjectTopics(app, dataset, names);
    TabsApi api = tabsApi(window, u"bench-plugin"_s);
    ASSERT_TRUE(api.view.createTabV2("view", "3d", "Bench"));
    const auto median_of = [](std::vector<double> micros) {
      std::nth_element(micros.begin(), micros.begin() + micros.size() / 2, micros.end());
      return micros[micros.size() / 2];
    };
    std::vector<double> first;
    for (int call = 0; call < 51; ++call) {
      const std::string topic = "/bench/cloud_" + std::to_string(call * 100);
      const auto begin = std::chrono::steady_clock::now();
      EXPECT_TRUE(api.view.attachTopic("view", topic));
      first.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count());
    }
    std::vector<double> again;
    for (int call = 0; call < 51; ++call) {
      const auto begin = std::chrono::steady_clock::now();
      EXPECT_TRUE(api.view.attachTopic("view", "/bench/cloud_5000"));
      again.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count());
    }
    std::printf(
        "attach_topic among 10000 object topics: first attachment median %.1f us, re-attachment median %.1f us\n",
        median_of(first), median_of(again));
  }

  static void checkViewportIsolation(MainWindow& window) {
    auto& app = *window.session_;
    app.sessionManager().setUseTimeOffset(false);
    const auto dataset = pj_test::createDataset(app, "viewport-isolation", true);
    ASSERT_NE(pj_test::addScalarTopic(app, dataset, "/isolation", 0, 10'000'000'000), 0U);
    const auto key = app.catalogModel().resolveCurveKey(0, u"viewport-isolation"_s, {}, u"/isolation"_s, u"value"_s);
    ASSERT_TRUE(key);
    auto* tabs = window.findChild<TabbedPlotWidget*>();
    auto* user = tabs->addTab(u"User viewport"_s);
    ASSERT_TRUE(pluginTabs(window).createTab(u"zoom-a"_s, u"view"_s, u"A"_s));
    ASSERT_TRUE(pluginTabs(window).createTab(u"zoom-b"_s, u"view"_s, u"B"_s));
    auto* a = pluginTabs(window).ownedTab(u"zoom-a"_s, u"view"_s);
    auto* b = pluginTabs(window).ownedTab(u"zoom-b"_s, u"view"_s);
    const QRectF original(1.0, 8.0, 8.0, -10.0);
    for (auto* docker : {user, a, b}) {
      auto* dock = docker->plotAt(0);
      ASSERT_NE(dock, nullptr);
      auto* plot = dock->ensurePlotWidget();
      ASSERT_NE(plot, nullptr);
      ASSERT_NE(plot->addCurve(*key), nullptr);
      plot->setZoomRectangle(original, false);
    }
    auto* a_plot = a->plotAt(0)->plotWidget();
    const auto before = a_plot->currentBoundingRect();
    const auto user_before = user->plotAt(0)->plotWidget()->currentBoundingRect();
    const auto b_before = b->plotAt(0)->plotWidget()->currentBoundingRect();
    ASSERT_TRUE(pluginTabs(window).createTab(u"zoom-a"_s, u"empty"_s, u"Empty"_s));
    ASSERT_TRUE(pluginTabs(window).createTab(u"zoom-a"_s, u"xy"_s, u"XY"_s));
    auto* empty = pluginTabs(window).ownedTab(u"zoom-a"_s, u"empty"_s)->plotAt(0)->ensurePlotWidget();
    auto* xy = pluginTabs(window).ownedTab(u"zoom-a"_s, u"xy"_s)->plotAt(0)->ensurePlotWidget();
    ASSERT_NE(empty, nullptr);
    ASSERT_NE(xy, nullptr);
    xy->setModeXY(true);
    ASSERT_NE(xy->addCurveXY(*key, *key, u"XY"_s), nullptr);
    ASSERT_TRUE(xy->isXYPlot());
    const auto empty_before = empty->currentBoundingRect();
    const auto xy_before = xy->currentBoundingRect();

    ASSERT_TRUE(pluginTabs(window).zoomToTimeRange(u"zoom-a"_s, 2.0, 4.0));
    const auto zoomed = a_plot->currentBoundingRect();
    EXPECT_DOUBLE_EQ(zoomed.left(), 2.0);
    EXPECT_DOUBLE_EQ(zoomed.right(), 4.0);
    EXPECT_DOUBLE_EQ(zoomed.top(), before.top());
    EXPECT_DOUBLE_EQ(zoomed.bottom(), before.bottom());
    EXPECT_EQ(user->plotAt(0)->plotWidget()->currentBoundingRect(), user_before);
    EXPECT_EQ(b->plotAt(0)->plotWidget()->currentBoundingRect(), b_before);
    EXPECT_EQ(empty->currentBoundingRect(), empty_before);
    EXPECT_EQ(xy->currentBoundingRect(), xy_before);
    ASSERT_TRUE(pluginTabs(window).zoomOut(u"zoom-a"_s));
    EXPECT_NE(a_plot->currentBoundingRect(), zoomed);
    EXPECT_EQ(user->plotAt(0)->plotWidget()->currentBoundingRect(), user_before);
    EXPECT_EQ(b->plotAt(0)->plotWidget()->currentBoundingRect(), b_before);
  }

  static void checkCurveOwnershipAndResolution(MainWindow& window) {
    auto& app = *window.session_;
    const auto first = pj_test::createDataset(app, "curve-a", true);
    const auto second = pj_test::createDataset(app, "curve-b", true);
    ASSERT_NE(pj_test::addScalarTopic(app, first, "/shared"), 0U);
    ASSERT_NE(pj_test::addScalarTopic(app, second, "/shared"), 0U);
    ASSERT_TRUE(pluginTabs(window).createTab(u"curve-owner"_s, u"view"_s, u"Original"_s));
    auto* tab = pluginTabs(window).ownedTab(u"curve-owner"_s, u"view"_s);
    ASSERT_FALSE(pluginTabs(window).addCurve(u"curve-owner"_s, u"view"_s, u"/shared"_s, u"value"_s, {}));
    ASSERT_TRUE(pluginTabs(window).addCurve(u"curve-owner"_s, u"view"_s, u"/shared"_s, u"value"_s, u"curve-a"_s));
    auto* plot = tab->plotAt(0)->plotWidget();
    ASSERT_NE(plot, nullptr);
    ASSERT_EQ(plot->curveList().size(), 1U);
    ASSERT_TRUE(pluginTabs(window).addCurve(u"curve-owner"_s, u"view"_s, u"/shared"_s, u"value"_s, u"curve-a"_s));
    EXPECT_EQ(plot->curveList().size(), 1U);
    EXPECT_FALSE(pluginTabs(window).clearTab(u"intruder"_s, tab->stateId()));
    EXPECT_FALSE(pluginTabs(window).removeCurve(u"intruder"_s, tab->stateId(), u"/shared"_s, u"value"_s, u"curve-a"_s));
    EXPECT_FALSE(pluginTabs(window).addCurve(u"intruder"_s, tab->stateId(), u"/shared"_s, u"value"_s, u"curve-b"_s));
    EXPECT_FALSE(pluginTabs(window).tabConfig(u"intruder"_s, tab->stateId()));
    EXPECT_EQ(plot->curveList().size(), 1U);
    const auto config = pluginTabs(window).tabConfig(u"curve-owner"_s, u"view"_s);
    ASSERT_TRUE(config);
    EXPECT_NE(config->find("curve-a"), std::string::npos);
    EXPECT_EQ(config->find("curve-b"), std::string::npos);
    ASSERT_TRUE(pluginTabs(window).removeCurve(u"curve-owner"_s, u"view"_s, u"/shared"_s, u"value"_s, u"curve-a"_s));
    EXPECT_TRUE(plot->isEmpty());
    EXPECT_FALSE(pluginTabs(window).removeCurve(u"curve-owner"_s, u"view"_s, u"/shared"_s, u"value"_s, u"curve-a"_s));
    ASSERT_TRUE(pluginTabs(window).addCurve(u"curve-owner"_s, u"view"_s, u"/shared"_s, u"value"_s, u"curve-b"_s));
    ASSERT_TRUE(pluginTabs(window).createTab(u"curve-owner"_s, u"view"_s, u"Replacement"_s));
    EXPECT_EQ(pluginTabs(window).ownedTab(u"curve-owner"_s, u"view"_s), tab);
    EXPECT_EQ(tab->name(), u"Replacement"_s);
    EXPECT_TRUE(plot->isEmpty());
  }

  static void check(MainWindow& window) {
    const QString owner = u"plugin-a"_s;
    const QString other = u"plugin-b"_s;
    auto* tabs = window.findChild<TabbedPlotWidget*>();
    ASSERT_NE(tabs, nullptr);
    PlotDocker* user_tab = tabs->addTab(u"Temperature"_s);
    const QString user_id = user_tab->stateId();

    ASSERT_TRUE(pluginTabs(window).createTab(owner, u"run-a"_s, u"Temperature"_s));
    ASSERT_TRUE(pluginTabs(window).createTab(owner, u"run-b"_s, u"Temperature"_s));
    ASSERT_TRUE(pluginTabs(window).createTab(other, u"run-a"_s, u"Temperature"_s));
    PlotDocker* first = pluginTabs(window).ownedTab(owner, u"run-a"_s);
    PlotDocker* second = pluginTabs(window).ownedTab(owner, u"run-b"_s);
    PlotDocker* foreign = pluginTabs(window).ownedTab(other, u"run-a"_s);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(foreign, nullptr);
    EXPECT_NE(first, foreign);
    EXPECT_EQ(first->name(), second->name());
    const QString second_id = second->stateId();
    second->setName(u"Renamed"_s);
    EXPECT_EQ(pluginTabs(window).ownedTab(owner, u"run-b"_s), second);
    EXPECT_EQ(second->stateId(), second_id);

    EXPECT_FALSE(pluginTabs(window).closeTab(owner, user_id));
    EXPECT_FALSE(pluginTabs(window).closeTab(owner, foreign->stateId()));
    ASSERT_TRUE(pluginTabs(window).closeTab(owner, u"run-a"_s));
    EXPECT_EQ(pluginTabs(window).ownedTab(owner, u"run-b"_s), second);
    EXPECT_EQ(pluginTabs(window).ownedTab(other, u"run-a"_s), foreign);
    EXPECT_EQ(user_tab->stateId(), user_id);
    const auto ids = pluginTabs(window).listTabIds(owner);
    ASSERT_TRUE(ids);
    EXPECT_EQ(*ids, (std::vector<std::string>{"run-b"}));
    const auto config = pluginTabs(window).tabConfig(owner, u"run-b"_s);
    ASSERT_TRUE(config);
    EXPECT_NE(config->find("Renamed"), std::string::npos);

    DockWidget* second_dock = second->plotAt(0);
    ASSERT_NE(second_dock, nullptr);
    if (second_dock->plotWidget() == nullptr) {
      second_dock->setPlotWidget(new PlotWidget(nullptr, nullptr, second_dock));
    }
    second_dock->plotWidget()->setStateId(u"persisted-owned-content"_s);

    QPointer<PlotDocker> before_restore(second);
    const QDomDocument saved = window.xmlSaveState(MainWindow::SnapshotScope::kFull);
    const QString saved_xml = QString::fromUtf8(saved.toByteArray());
    EXPECT_TRUE(saved_xml.contains(u"owner_plugin=\"plugin-a\""_s));
    EXPECT_TRUE(saved_xml.contains(u"tab_id=\"run-b\""_s));
    ASSERT_TRUE(window.xmlLoadState(saved, MainWindow::RestoreIntent::kReplace));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_TRUE(before_restore.isNull());

    second = pluginTabs(window).ownedTab(owner, u"run-b"_s);
    foreign = pluginTabs(window).ownedTab(other, u"run-a"_s);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(foreign, nullptr);
    EXPECT_TRUE(second->isHistoryExempt());
    EXPECT_EQ(second->ownerPlugin(), owner);
    EXPECT_EQ(second->ownerTabId(), u"run-b"_s);
    EXPECT_FALSE(second->isOwnerPluginAvailable());
    EXPECT_TRUE(second->toolTip().contains(u"Plugin unavailable"_s));
    ASSERT_NE(second->plotAt(0), nullptr);
    ASSERT_NE(second->plotAt(0)->plotWidget(), nullptr);
    EXPECT_EQ(second->plotAt(0)->plotWidget()->stateId(), u"persisted-owned-content"_s);

    PlotDocker* restored = second;
    ASSERT_TRUE(pluginTabs(window).createTab(owner, u"run-b"_s, u"Updated"_s));
    EXPECT_EQ(pluginTabs(window).ownedTab(owner, u"run-b"_s), restored);
    EXPECT_EQ(restored->name(), u"Updated"_s);

    AppSession& app = *window.session_;
    SessionManager& session = app.sessionManager();
    session.setUseTimeOffset(false);
    const auto a = pj_test::createDataset(app, "same.mcap", /*own_time_domain=*/true);
    const auto b = pj_test::createDataset(app, "same.mcap", /*own_time_domain=*/true);
    ASSERT_NE(a, 0u);
    ASSERT_NE(b, 0u);
    ASSERT_NE(pj_test::addScalarTopic(app, a, "/speed"), 0u);
    ASSERT_NE(pj_test::addScalarTopic(app, b, "/speed"), 0u);
    session.setDisplayOffset(a, DisplayOffset{Duration{1'000'000'000}});
    session.setDisplayOffset(b, DisplayOffset{Duration{2'000'000'000}});
    const PJ_data_source_handle_t source_a{static_cast<uint32_t>(a)};
    const PJ_data_source_handle_t source_b{static_cast<uint32_t>(b)};
    const auto display_a = window.displayTimeForSource(source_a, 3'500'000'000);
    const auto display_b = window.displayTimeForSource(source_b, 3'500'000'000);
    ASSERT_TRUE(display_a);
    ASSERT_TRUE(display_b);
    EXPECT_DOUBLE_EQ(*display_a, 2.5);
    EXPECT_DOUBLE_EQ(*display_b, 1.5);
    session.setDisplayOffset(b, DisplayOffset{Duration{3'000'000'000}});
    const auto shifted = window.displayTimeForSource(source_b, 3'500'000'000);
    ASSERT_TRUE(shifted);
    EXPECT_DOUBLE_EQ(*shifted, 0.5);
    EXPECT_FALSE(window.displayTimeForSource({0}, 0));
    EXPECT_FALSE(window.displayTimeForSource({999999}, 0));
    app.catalogModel().removeDataset(b, /*tombstone=*/false);
    session.dataEngine().removeDataset(b);
    EXPECT_FALSE(window.displayTimeForSource(source_b, 3'500'000'000));
  }
};

}  // namespace PJ

class MainWindowPluginViewsTest : public ::testing::Test {
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

TEST_F(MainWindowPluginViewsTest, StableTabIdsAndExplicitSourceOffsets) {
  PJ::MainWindowPluginViewsTestPeer::check(*window_);
}

TEST_F(MainWindowPluginViewsTest, NewPlotInheritsLinkedTimeRangeWithoutChangingPeers) {
  PJ::MainWindowPluginViewsTestPeer::checkNewPlotLinkedZoom(*window_);
}

// Claim: time-range zoom preserves Y and both zoom operations leave user and other-plugin plots untouched.
TEST_F(MainWindowPluginViewsTest, ViewportCommandsOnlyChangeCallingPluginsPlots) {
  PJ::MainWindowPluginViewsTestPeer::checkViewportIsolation(*window_);
}

// Claim: curve operations resolve one dataset, enforce ownership, and replacing a tab clears its contents.
TEST_F(MainWindowPluginViewsTest, CurveCommandsEnforceOwnershipAndRejectAmbiguousDatasets) {
  PJ::MainWindowPluginViewsTestPeer::checkCurveOwnershipAndResolution(*window_);
}

TEST_F(MainWindowPluginViewsTest, PluginSceneViewsCreateAttachDetachClose) {
  PJ::MainWindowPluginViewsTestPeer::checkSceneViewsLifecycle(*window_);
}

TEST_F(MainWindowPluginViewsTest, PluginSceneViewsSurviveLayoutRoundTrip) {
  PJ::MainWindowPluginViewsTestPeer::checkSceneViewsLayoutRoundTrip(*window_);
}

TEST_F(MainWindowPluginViewsTest, PlotAndSceneTabsShareOneIdNamespacePerPlugin) {
  PJ::MainWindowPluginViewsTestPeer::checkSharedIdNamespace(*window_);
}

TEST_F(MainWindowPluginViewsTest, AddCurveOnSceneTabIsRejected) {
  PJ::MainWindowPluginViewsTestPeer::checkAddCurveOnSceneTabIsRejected(*window_);
}

TEST_F(MainWindowPluginViewsTest, ClearOnSceneTabDetachesEverything) {
  PJ::MainWindowPluginViewsTestPeer::checkClearOnSceneTabDetachesEverything(*window_);
}

TEST_F(MainWindowPluginViewsTest, TfTopicIsListedAndDetachable) {
  PJ::MainWindowPluginViewsTestPeer::checkTfTopicIsListedAndDetachable(*window_);
}

TEST_F(MainWindowPluginViewsTest, ClearReleasesTfForAnotherDataset) {
  PJ::MainWindowPluginViewsTestPeer::checkClearReleasesTfForAnotherDataset(*window_);
}

TEST_F(MainWindowPluginViewsTest, EmptiedSceneTabKeepsItsKind) {
  PJ::MainWindowPluginViewsTestPeer::checkEmptiedSceneTabKeepsItsKind(*window_);
}

TEST_F(MainWindowPluginViewsTest, EvictedPluginSceneTabKeepsItsKind) {
  PJ::MainWindowPluginViewsTestPeer::checkEvictedPluginSceneTabKeepsItsKind(*window_);
}

TEST_F(MainWindowPluginViewsTest, ClearDropsPendingLayoutRestores) {
  PJ::MainWindowPluginViewsTestPeer::checkClearDropsPendingLayoutRestores(*window_);
}

#ifdef PJ_WITH_SCENE3D
TEST_F(MainWindowPluginViewsTest, LocalLayersAreNotThePluginsTopics) {
  PJ::MainWindowPluginViewsTestPeer::checkLocalLayersAreNotThePluginsTopics(*window_);
}
#endif

TEST_F(MainWindowPluginViewsTest, ColourImageIsRefusedBy3dTab) {
  PJ::MainWindowPluginViewsTestPeer::checkColourImageIsRefusedBy3dTab(*window_);
}

TEST_F(MainWindowPluginViewsTest, TabConfigOfPlotTabIsUnchanged) {
  PJ::MainWindowPluginViewsTestPeer::checkTabConfigOfPlotTabIsUnchanged(*window_);
}

TEST_F(MainWindowPluginViewsTest, KnownUnservableKindIsRejectedByName) {
  PJ::MainWindowPluginViewsTestPeer::checkKnownUnservableKind(*window_);
}

TEST_F(MainWindowPluginViewsTest, AttachOfAnotherDatasetsTopicIsRefusedWithTheDockReason) {
  PJ::MainWindowPluginViewsTestPeer::checkAttachOfAnotherDatasetsTopic(*window_);
}

#ifdef PJ_HAS_FFMPEG
TEST_F(MainWindowPluginViewsTest, PluginSceneViewAcceptsVideo) {
  PJ::MainWindowPluginViewsTestPeer::checkVideoSceneView(*window_);
}
#endif

#ifdef PJ_WITH_SCENE3D
TEST_F(MainWindowPluginViewsTest, EmbeddedSceneViewAttachesTicksAndDiesWithItsFrame) {
  PJ::MainWindowPluginViewsTestPeer::checkEmbeddedSceneView(*window_);
}
#endif

#ifdef PJ_WITH_SCENE3D
TEST_F(MainWindowPluginViewsTest, EmbeddedSceneViewDropsAnEvictedTopic) {
  PJ::MainWindowPluginViewsTestPeer::checkEmbeddedSceneViewDropsAnEvictedTopic(*window_);
}

TEST_F(MainWindowPluginViewsTest, EmbeddedSceneViewDetachesTheTopicItAttached) {
  PJ::MainWindowPluginViewsTestPeer::checkEmbeddedSceneViewDetachesTheTopicItAttached(*window_);
}

TEST_F(MainWindowPluginViewsTest, EmbeddedSceneViewCatchesUpWhenRevealed) {
  PJ::MainWindowPluginViewsTestPeer::checkEmbeddedSceneViewCatchesUpWhenRevealed(*window_);
}

TEST_F(MainWindowPluginViewsTest, EmbeddedSceneViewReattachesAfterReload) {
  PJ::MainWindowPluginViewsTestPeer::checkEmbeddedSceneViewReattachesAfterReload(*window_);
}

TEST_F(MainWindowPluginViewsTest, EmbeddedSceneViewNeverDetachesARecycledId) {
  PJ::MainWindowPluginViewsTestPeer::checkEmbeddedSceneViewNeverDetachesARecycledId(*window_);
}

TEST_F(MainWindowPluginViewsTest, EmbeddedSceneViewKeepsATopicAnotherEntryHolds) {
  PJ::MainWindowPluginViewsTestPeer::checkEmbeddedSceneViewKeepsATopicAnotherEntryHolds(*window_);
}

TEST_F(MainWindowPluginViewsTest, EmbeddedSceneViewHasNoMainWindowSideEffects) {
  PJ::MainWindowPluginViewsTestPeer::checkEmbeddedSceneViewHasNoMainWindowSideEffects(*window_);
}
#endif

#ifdef PJ_WITH_SCENE2D
TEST_F(MainWindowPluginViewsTest, Embedded2dViewRefusesASecondBackground) {
  PJ::MainWindowPluginViewsTestPeer::checkEmbedded2dViewRefusesASecondBackground(*window_);
}
#endif

TEST_F(MainWindowPluginViewsTest, BenchAttachAmongTenThousandTopics) {
  if (!PJ::sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  PJ::MainWindowPluginViewsTestPeer::benchAttachAmongTenThousandTopics(*window_);
}
