// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "PluginSceneViewsController.h"

#include <QStringList>
#include <QWidget>
#include <nlohmann/json.hpp>
#include <utility>
#include <vector>

#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/IDataWidget.h"
#include "pj_runtime/SessionManager.h"
#ifdef PJ_WITH_SCENE2D
#include "pj_scene2d_widgets/Scene2DDockWidget.h"
#endif
#ifdef PJ_WITH_SCENE3D
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#endif
#include "pj_scene_common/scene_dock_widget.h"
#include "scene_object_classification.h"

using namespace Qt::StringLiterals;

namespace PJ {

namespace {

QString sceneKindOfWidget([[maybe_unused]] SceneDockWidget* scene) {
#ifdef PJ_WITH_SCENE3D
  if (qobject_cast<Scene3DDockWidget*>(scene) != nullptr) {
    return u"3d"_s;
  }
#endif
#ifdef PJ_WITH_SCENE2D
  if (qobject_cast<Scene2DDockWidget*>(scene) != nullptr) {
    return u"2d"_s;
  }
#endif
  return {};
}

QString typeName(sdk::BuiltinObjectType type) {
  return QString::fromUtf8(sdk::name(type));
}

/// Resolves `topic`, qualified by `dataset_source` (the source NAME as
/// `PJ_data_source_info_t` reports it), to the catalog's object topic. An empty
/// `dataset_source` requires the topic to be unique across loaded datasets; an
/// ambiguous one is refused with the qualified candidates.
Expected<ObjectTopicPayload> resolveObjectTopic(
    const CatalogModel& catalog, const QString& topic, const QString& dataset_source) {
  const std::vector<CatalogItem> matches = catalog.objectTopicsNamed(topic, dataset_source);
  if (matches.empty()) {
    return unexpected(("no object topic '" + topic + "' is loaded").toStdString());
  }
  if (matches.size() > 1) {
    QStringList qualified;
    for (const CatalogItem& item : matches) {
      qualified << (catalog.datasetSourceName(item.dataset_id).value_or(QString()) + u":"_s + topic);
    }
    return unexpected(
        ("topic '" + topic + "' is ambiguous across datasets; name one with dataset_source: " + qualified.join(u", "_s))
            .toStdString());
  }
  return *asObjectTopic(matches.front());
}

/// Adds the resolved topic the way its dock family takes one, through its own gates:
/// a 2D full-frame image replaces the background, a 3D image must be a depth image.
bool addToSceneDock(SceneDockWidget* scene_dock, const ObjectTopicPayload& object, const QString& title) {
#ifdef PJ_WITH_SCENE2D
  if (auto* scene2d = qobject_cast<Scene2DDockWidget*>(scene_dock);
      scene2d != nullptr && object.object_type != sdk::BuiltinObjectType::kImageAnnotations) {
    return scene2d->setImageTopic(object.object_topic_id, object.object_type, title);
  }
#endif
  return scene_dock->tryAcceptObjectTopic(object.object_topic_id, object.object_type, title);
}

}  // namespace

QString sceneWidgetKind(const QString& tab_kind) {
  return tab_kind == u"3d"_s ? u"scene3d"_s : tab_kind == u"2d"_s ? u"scene2d"_s : QString();
}

SceneDockWidget* sceneDockOf(PlotDocker* docker) {
  DockWidget* dock = docker == nullptr ? nullptr : docker->plotAt(0);
  IDataWidget* object_widget = dock == nullptr ? nullptr : dock->objectWidget();
  return object_widget == nullptr ? nullptr : qobject_cast<SceneDockWidget*>(object_widget->widget());
}

QString sceneKindOf(PlotDocker* docker) {
  return sceneKindOfWidget(sceneDockOf(docker));
}

PluginSceneViewsController::PluginSceneViewsController(
    CatalogModel& catalog, SessionManager& session, MakeDock make_dock)
    : catalog_(catalog), session_(session), make_dock_(std::move(make_dock)) {}

std::unique_ptr<IDataWidget> PluginSceneViewsController::makeSceneWidget(const QString& kind) const {
  return std::unique_ptr<IDataWidget>(make_dock_ ? make_dock_(sceneWidgetKind(kind), nullptr) : nullptr);
}

void PluginSceneViewsController::adoptSceneWidget(PlotDocker* docker, std::unique_ptr<IDataWidget> widget) {
  DockWidget* dock = docker->plotAt(0);
  if (dock == nullptr) {
    return;
  }
  IDataWidget* adopted = widget.release();
  dock->adoptObjectWidget(adopted);
#ifdef PJ_WITH_SCENE3D
  if (auto* scene3d = qobject_cast<Scene3DDockWidget*>(adopted->widget())) {
    scene3d->frameInitialContent();
  }
#endif
}

Expected<std::string> PluginSceneViewsController::viewConfig(PlotDocker* docker) const {
  nlohmann::json root;
  root["kind"] = sceneKindOf(docker).toStdString();
  root["title"] = docker->name().toStdString();
  nlohmann::json topics = nlohmann::json::array();
  if (SceneDockWidget* scene_dock = sceneDockOf(docker); scene_dock != nullptr) {
    for (const auto& shown : {scene_dock->layers(), scene_dock->configTopics()}) {
      for (const SceneLayerInfo& info : shown) {
        const ObjectTopicDescriptor descriptor = session_.objectStore().descriptor(info.topic_id);
        if (descriptor.topic_name.empty()) {
          continue;  // a local layer the user added (URDF file, TF trail), not an object topic
        }
        nlohmann::json entry;
        entry["topic"] = descriptor.topic_name;
        entry["dataset"] = catalog_.datasetSourceName(descriptor.dataset_id).value_or(QString()).toStdString();
        entry["type"] = std::string(sdk::name(info.object_type));
        entry["visible"] = info.visible;
        topics.push_back(std::move(entry));
      }
    }
  }
  root["topics"] = std::move(topics);
  return root.dump();
}

Status PluginSceneViewsController::attachTopic(
    PlotDocker* docker, const QString& topic, const QString& dataset_source) {
  const Expected<ObjectTopicId> attached =
      attach(sceneDockOf(docker), topic, dataset_source, /*replace_background=*/true);
  if (!attached) {
    return unexpected(attached.error());
  }
  return okStatus();
}

Expected<ObjectTopicId> PluginSceneViewsController::attachEmbeddedTopic(
    SceneDockWidget* scene_dock, const QString& topic, const QString& dataset_source) {
  return attach(scene_dock, topic, dataset_source, /*replace_background=*/false);
}

Expected<ObjectTopicId> PluginSceneViewsController::attach(
    SceneDockWidget* scene_dock, const QString& topic, const QString& dataset_source, bool replace_background) {
  const Expected<ObjectTopicPayload> resolved = resolveObjectTopic(catalog_, topic, dataset_source);
  if (!resolved) {
    return unexpected(resolved.error());
  }
  if (scene_dock == nullptr) {
    return unexpected("the tab has no scene widget to attach a topic to");
  }
  if (scene_dock->layerFor(resolved->object_topic_id) != nullptr) {
    return resolved->object_topic_id;  // already attached: success, per the ABI contract
  }
#ifdef PJ_WITH_SCENE2D
  if (auto* scene2d = qobject_cast<Scene2DDockWidget*>(scene_dock); scene2d != nullptr) {
    if (!isImageFamilyObjectType(resolved->object_type)) {
      return unexpected(
          ("2D scene tabs do not accept topics of type '" + typeName(resolved->object_type) + "'").toStdString());
    }
    if (!replace_background && resolved->object_type != sdk::BuiltinObjectType::kImageAnnotations &&
        !scene2d->backgroundTopics().empty()) {
      return unexpected(("a 2D view shows one background; detach it before attaching '" + topic + "'").toStdString());
    }
  }
#else
  (void)replace_background;
#endif
  // The dock names why it refuses another dataset's topic (SCN-17); the generic
  // message covers the rest.
  (void)scene_dock->takeRefusalReason();
  if (!addToSceneDock(scene_dock, *resolved, topic)) {
    const QString refusal = scene_dock->takeRefusalReason();
    return unexpected((refusal.isEmpty()
                           ? "could not attach topic '" + topic + "' of type '" + typeName(resolved->object_type) + "'"
                           : refusal)
                          .toStdString());
  }
  return resolved->object_topic_id;
}

Status PluginSceneViewsController::detachTopic(
    PlotDocker* docker, const QString& topic, const QString& dataset_source) {
  const Expected<ObjectTopicPayload> resolved = resolveObjectTopic(catalog_, topic, dataset_source);
  if (!resolved) {
    return unexpected(resolved.error());
  }
  SceneDockWidget* scene_dock = sceneDockOf(docker);
  if (scene_dock == nullptr || !scene_dock->detachTopic(resolved->object_topic_id)) {
    return unexpected(("'" + topic + "' is not attached to that tab").toStdString());
  }
  return okStatus();
}

Status PluginSceneViewsController::clearView(PlotDocker* docker) {
  if (SceneDockWidget* scene_dock = sceneDockOf(docker); scene_dock != nullptr) {
    scene_dock->clearPendingRestores();
    for (const auto& shown : {scene_dock->layers(), scene_dock->configTopics()}) {  // snapshots
      for (const SceneLayerInfo& info : shown) {
        if (!session_.objectStore().descriptor(info.topic_id).topic_name.empty()) {  // not a local layer
          scene_dock->detachTopic(info.topic_id);
        }
      }
    }
  }
  return okStatus();
}

}  // namespace PJ
