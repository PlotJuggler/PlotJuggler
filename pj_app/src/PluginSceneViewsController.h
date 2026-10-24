#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <functional>
#include <memory>
#include <string>

#include "pj_base/expected.hpp"
#include "pj_datastore/object_store.hpp"  // ObjectTopicId

class QWidget;

namespace PJ {

class CatalogModel;
class IDataWidget;
class PlotDocker;
class SceneDockWidget;
class SessionManager;

/// The object-widget kind ("scene3d" / "scene2d") MainWindow builds for a scene
/// kind ("3d" / "2d"), empty for any other.
[[nodiscard]] QString sceneWidgetKind(const QString& tab_kind);
/// The scene widget of `docker`'s tab, or null for a plot tab, an empty one or a null `docker`.
[[nodiscard]] SceneDockWidget* sceneDockOf(PlotDocker* docker);
/// "3d" or "2d" when `docker` holds a scene widget, empty for a plot tab or null.
/// Read from the live widget, so a tab a layout restored before any controller
/// existed still answers. `PluginPlotTabsController` routes each call by it.
[[nodiscard]] QString sceneKindOf(PlotDocker* docker);

/// The scene-tab half of `pj.plot_tabs.v1` (its tail slots): the 3D/2D scene widgets
/// of the tabs a plugin composes. An internal delegate of `PluginPlotTabsController`,
/// which resolves the plugin's tab, decides its kind and hands the docker in; this
/// class never looks a tab up by id, so it never decides ownership. Plain object;
/// every call is [main-thread].
class PluginSceneViewsController {
 public:
  /// Builds the seeded-empty object widget for a widget kind ("scene3d" / "scene2d"),
  /// or null when this build has none (`MainWindow::makeSeededEmptyObjectDock`).
  using MakeDock = std::function<IDataWidget*(const QString& kind, QWidget* parent)>;

  PluginSceneViewsController(CatalogModel& catalog, SessionManager& session, MakeDock make_dock);

  /// A new, unparented scene widget for the tab kind `kind` ("3d" or "2d"), or null
  /// when this build cannot show that kind.
  [[nodiscard]] std::unique_ptr<IDataWidget> makeSceneWidget(const QString& kind) const;
  /// Install `widget` (from makeSceneWidget) as the object widget of the fresh tab
  /// `docker`, which takes ownership. A `docker` without a dock deletes it.
  static void adoptSceneWidget(PlotDocker* docker, std::unique_ptr<IDataWidget> widget);
  /// What the scene tab holds, as the JSON the ABI specifies, read back from the
  /// live scene, never echoed from what the caller asked for.
  [[nodiscard]] Expected<std::string> viewConfig(PlotDocker* docker) const;
  /// Attaching an attached topic is success. A topic the dock refuses (another
  /// dataset's, SCN-17) fails with the dock's own reason.
  Status attachTopic(PlotDocker* docker, const QString& topic, const QString& dataset_source);
  /// Detaching a topic that is not attached is an error: a mistake worth seeing. A
  /// tab emptied this way (or by clearView) stays a scene tab when a load revalidates it.
  Status detachTopic(PlotDocker* docker, const QString& topic, const QString& dataset_source);
  /// The attach for a scene widget that is no tab's object widget (a view embedded
  /// in a plugin panel, MainWindow::makeEmbeddedViewHost). Returns the topic attached,
  /// for the caller to detach by identity: the name may turn ambiguous once another
  /// dataset loads. A 2D view refuses a second background rather than replacing the
  /// first, since the panel's binding tracks each topic it attached.
  Expected<ObjectTopicId> attachEmbeddedTopic(
      SceneDockWidget* scene_dock, const QString& topic, const QString& dataset_source);
  /// Detach every topic the scene tab holds, its layers and its config (TF) topics,
  /// and drop its pending layout-restore intents, keeping the tab itself and any
  /// layer the user added by hand (it has no object topic).
  Status clearView(PlotDocker* docker);

 private:
  /// The attach both paths share; `replace_background`: a 2D background replaces the
  /// current one (a tab) instead of being refused (an embedded view).
  Expected<ObjectTopicId> attach(
      SceneDockWidget* scene_dock, const QString& topic, const QString& dataset_source, bool replace_background);

  CatalogModel& catalog_;
  SessionManager& session_;
  MakeDock make_dock_;
};

}  // namespace PJ
