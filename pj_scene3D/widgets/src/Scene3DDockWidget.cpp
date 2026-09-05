// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/Scene3DDockWidget.h"

#include <QAbstractItemView>
#include <QDomDocument>
#include <QDomElement>
#include <QFileInfo>
#include <QFontMetrics>
#include <QIcon>
#include <QLoggingCategory>
#include <QMenu>
#include <QPoint>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QToolButton>
#include <QUrl>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <numbers>
#include <optional>
#include <unordered_set>
#include <utility>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/layers/depth_cloud_layer.h"
#include "pj_scene3d_widgets/layers/grid_map_layer.h"
#include "pj_scene3d_widgets/layers/occupancy_grid_layer.h"
#include "pj_scene3d_widgets/layers/pointcloud_layer.h"
#include "pj_scene3d_widgets/layers/poses_in_frame_layer.h"
#include "pj_scene3d_widgets/layers/robot_model_layer.h"
#include "pj_scene3d_widgets/layers/scene_entities_layer.h"
#include "pj_scene3d_widgets/layers/trail_layer.h"
#include "pj_scene3d_widgets/layers/voxel_grid_layer.h"
#include "pj_scene3d_widgets/object_topic_metadata.h"
#include "pj_scene3d_widgets/parse_locked.h"
#include "pj_scene3d_widgets/render_pass.h"
#include "pj_scene3d_widgets/scene_state_xml.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_scene3d_widgets/transform_service.h"
#include "pj_widgets/ComboBox.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/SvgUtil.h"
#include "urdf_package_resolver.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {
Q_LOGGING_CATEGORY(lcScene3DDock, "pj.scene3d.dock")

using pj::scene3d::DepthCloudLayer;
using pj::scene3d::FrameRow;
using pj::scene3d::GridMapLayer;
using pj::scene3d::OccupancyGridLayer;
using pj::scene3d::PointCloudLayer;
using pj::scene3d::PosesInFrameLayer;
using pj::scene3d::readXmlBool;
using pj::scene3d::readXmlFloat;
using pj::scene3d::readXmlInt;
using pj::scene3d::RobotModelLayer;
using pj::scene3d::Scene3DLayer;
using pj::scene3d::Scene3DLayerContext;
using pj::scene3d::SceneEntitiesLayer;
using pj::scene3d::SceneViewWidget;
using pj::scene3d::validateCameraState;
using pj::scene3d::ValidatedCameraState;
using pj::scene3d::VoxelGridLayer;

// The TrailLayer a scene layer IS (a standalone TF-frame trail) or OWNS (a pose
// layer's embedded trail), else nullptr. One lookup so every trail-wide fan-out
// — notably the TF-buffer re-bind — reaches both flavors.
pj::scene3d::TrailLayer* trailOf(ISceneLayer* layer) {
  if (auto* trail = dynamic_cast<pj::scene3d::TrailLayer*>(layer); trail != nullptr) {
    return trail;
  }
  if (const auto* poses = dynamic_cast<const PosesInFrameLayer*>(layer); poses != nullptr) {
    return poses->trail();
  }
  return nullptr;
}

// Stable enum <-> on-disk-name table for the camera model, persisted in the
// layout. The enum value == combo index, so the on-disk name stays independent of
// the combo's display order; one table feeds both directions so they can't drift.
struct CameraModelName {
  SceneViewWidget::CameraModel model;
  const char* id;
};
constexpr CameraModelName kCameraModelNames[] = {
    {SceneViewWidget::CameraModel::kOrbit, "orbit"},
    {SceneViewWidget::CameraModel::kXyOrbit, "xy_orbit"},
    {SceneViewWidget::CameraModel::kFly, "fly"},
    {SceneViewWidget::CameraModel::kTopDownOrtho, "top_down_ortho"},
};

QString cameraModelToString(int combo_index) {
  const auto model = static_cast<SceneViewWidget::CameraModel>(combo_index);
  for (const auto& entry : kCameraModelNames) {
    if (entry.model == model) {
      return QString::fromLatin1(entry.id);
    }
  }
  return u"orbit"_s;
}

// Combo index for a persisted model name, or -1 when missing / unknown (→ keep
// the default).
int cameraModelFromString(const QString& name) {
  for (const auto& entry : kCameraModelNames) {
    if (name == QLatin1String(entry.id)) {
      return static_cast<int>(entry.model);
    }
  }
  return -1;
}

// The shared backend-neutral controls plus the native-only mesh/collision knobs.
struct ValidatedSceneControls : pj::scene3d::ValidatedSceneControls {
  std::optional<bool> meshes_visible;
  std::optional<float> mesh_opacity;
  std::optional<bool> collisions_visible;
  std::optional<float> collision_opacity;
};

struct ValidatedSceneState {
  bool explicit_fixed_frame = false;
  int camera_model_index = -1;
  std::optional<ValidatedCameraState> camera_state;
  std::optional<ValidatedSceneControls> controls;
};

std::optional<ValidatedSceneState> validateSceneState(const QDomElement& element) {
  ValidatedSceneState state;
  const QString mode = element.attribute(u"fixed_frame_mode"_s, u"auto_root"_s);
  if (mode != "auto_root"_L1 && mode != "explicit"_L1) {
    return std::nullopt;
  }
  state.explicit_fixed_frame = mode == "explicit"_L1;
  if (element.hasAttribute(u"camera_model"_s)) {
    state.camera_model_index = cameraModelFromString(element.attribute(u"camera_model"_s));
    if (state.camera_model_index < 0) {
      return std::nullopt;
    }
  }
  if (element.hasAttribute(u"camera_state"_s)) {
    state.camera_state = validateCameraState(element.attribute(u"camera_state"_s));
    if (!state.camera_state.has_value()) {
      return std::nullopt;
    }
  }
  QDomElement scene_controls;
  for (QDomNode child = element.firstChild(); !child.isNull(); child = child.nextSibling()) {
    if (!child.isElement()) {
      continue;
    }
    const QString tag = child.toElement().tagName();
    if (tag == "layer"_L1 || tag == "config_topic"_L1) {
      continue;
    }
    if (tag == "scene_controls"_L1 && scene_controls.isNull()) {
      scene_controls = child.toElement();
      continue;
    }
    return std::nullopt;
  }
  if (!scene_controls.isNull()) {
    if (!scene_controls.firstChildElement().isNull()) {
      return std::nullopt;
    }
    ValidatedSceneControls controls;
    if (!pj::scene3d::readSceneControls(scene_controls, controls) ||
        !readXmlBool(scene_controls, u"meshes_visible"_s, controls.meshes_visible) ||
        !readXmlFloat(scene_controls, u"mesh_opacity"_s, 0.0f, 1.0f, controls.mesh_opacity) ||
        !readXmlBool(scene_controls, u"collisions_visible"_s, controls.collisions_visible) ||
        !readXmlFloat(scene_controls, u"collision_opacity"_s, 0.0f, 1.0f, controls.collision_opacity)) {
      return std::nullopt;
    }
    state.controls = controls;
  }
  return state;
}

[[nodiscard]] bool framesContain(const QList<FrameRow>& frames, const QString& name) {
  const auto needle = name.toStdString();
  return std::any_of(frames.begin(), frames.end(), [&](const FrameRow& r) { return r.name == needle; });
}

[[nodiscard]] QString pickFixedFrame(const QList<FrameRow>& frames) {
  for (const auto* name : {"map", "world", "odom", "base_link", "base_footprint"}) {
    if (framesContain(frames, QString::fromLatin1(name))) {
      return QString::fromLatin1(name);
    }
  }
  return frames.isEmpty() ? QString() : QString::fromStdString(frames.first().name);
}

// True when topic_id's first stored sample is an sdk::Image with a DEPTH encoding.
// The kImage type alone can't distinguish depth from color, so the dock peeks the
// first sample (the parser can't — classify_schema sees no payload) to offer only
// depth images as DepthClouds. False when there is no sample yet or it can't decode.
[[nodiscard]] bool firstSampleIsDepthEncoded(PJ::SessionManager& session, ObjectTopicId topic_id) {
  PJ::ObjectStore& store = session.objectStore();
  auto first = store.at(topic_id, static_cast<size_t>(0));
  if (!first.has_value() || first->payload.bytes.empty()) {
    return false;
  }
  const auto binding = session.parserBindingForObjectTopic(topic_id);
  if (!binding) {
    return false;
  }
  auto obj = pj::scene3d::parseLocked(binding, first->timestamp, first->payload);
  if (!obj.has_value()) {
    return false;
  }
  const auto* image = obj->object.get<PJ::sdk::Image>();
  return image != nullptr && pj::scene3d::isDepthEncoding(image->encoding);
}

}  // namespace

Scene3DDockWidget::Scene3DDockWidget(QWidget* parent) : SceneDockWidget(parent) {
  setWindowTitle(tr("3D View"));
  package_resolver_ = std::make_unique<pj::scene3d::UrdfPackageResolver>();

  // One dual-mode layer renders both raw and compressed clouds: PointCloudLayer
  // detects a CompressedPointCloud per-sample and decodes it (off the UI thread)
  // into the same render path, so both object types share this factory.
  auto pointcloud_factory = [this](
                                ObjectTopicId topic_id, sdk::BuiltinObjectType object_type,
                                const QString& display_name) -> std::unique_ptr<ISceneLayer> {
    prepareTransformBufferForTopic(topic_id);
    auto layer = std::make_unique<PointCloudLayer>(topic_id, display_name, object_type, this);
    wireScene3DLayer(layer.get());
    return layer;
  };
  layerFactory().registerType(sdk::BuiltinObjectType::kPointCloud, pointcloud_factory);
  layerFactory().registerType(sdk::BuiltinObjectType::kCompressedPointCloud, pointcloud_factory);
  // Depth image -> back-projected point cloud ("DepthCloud"). Depth arrives as
  // sdk::Image with a depth encoding (no kDepthImage producer exists); addTopic()
  // gates kImage topics so only depth-encoded ones become DepthClouds. Intrinsics
  // come from a CameraInfo topic matched by frame_id; see DepthCloudLayer.
  layerFactory().registerType(
      sdk::BuiltinObjectType::kImage,
      [this](ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name)
          -> std::unique_ptr<ISceneLayer> {
        prepareTransformBufferForTopic(topic_id);
        auto layer = std::make_unique<DepthCloudLayer>(topic_id, display_name, object_type, this);
        wireScene3DLayer(layer.get());
        return layer;
      });
  layerFactory().registerType(
      sdk::BuiltinObjectType::kRobotDescription,
      [this](ObjectTopicId topic_id, sdk::BuiltinObjectType /*object_type*/, const QString& display_name)
          -> std::unique_ptr<ISceneLayer> {
        const bool local_layer = isLocalLayerId(topic_id);
        if (!local_layer) {
          prepareTransformBufferForTopic(topic_id);
        }
        auto layer = std::make_unique<RobotModelLayer>(topic_id, display_name, this);
        layer->setPackageResolver(package_resolver_.get());
        if (local_layer) {
          layer->setSourceFile(QString());
        }
        wireScene3DLayer(layer.get());
        return layer;
      });
  layerFactory().registerType(
      sdk::BuiltinObjectType::kOccupancyGrid,
      [this](ObjectTopicId topic_id, sdk::BuiltinObjectType /*object_type*/, const QString& display_name)
          -> std::unique_ptr<ISceneLayer> {
        prepareTransformBufferForTopic(topic_id);
        auto layer = std::make_unique<OccupancyGridLayer>(topic_id, display_name, this);
        wireScene3DLayer(layer.get());
        return layer;
      });
  layerFactory().registerType(
      sdk::BuiltinObjectType::kSceneEntities,
      [this](ObjectTopicId topic_id, sdk::BuiltinObjectType /*object_type*/, const QString& display_name)
          -> std::unique_ptr<ISceneLayer> {
        prepareTransformBufferForTopic(topic_id);
        auto layer = std::make_unique<SceneEntitiesLayer>(topic_id, display_name, this);
        wireScene3DLayer(layer.get());
        return layer;
      });
  layerFactory().registerType(
      sdk::BuiltinObjectType::kPosesInFrame,
      [this](ObjectTopicId topic_id, sdk::BuiltinObjectType /*object_type*/, const QString& display_name)
          -> std::unique_ptr<ISceneLayer> {
        prepareTransformBufferForTopic(topic_id);
        auto layer = std::make_unique<PosesInFrameLayer>(topic_id, display_name, this);
        wireScene3DLayer(layer.get());
        return layer;
      });
  layerFactory().registerType(
      sdk::BuiltinObjectType::kVoxelGrid,
      [this](ObjectTopicId topic_id, sdk::BuiltinObjectType /*object_type*/, const QString& display_name)
          -> std::unique_ptr<ISceneLayer> {
        prepareTransformBufferForTopic(topic_id);
        auto layer = std::make_unique<VoxelGridLayer>(topic_id, display_name, this);
        wireScene3DLayer(layer.get());
        return layer;
      });
  layerFactory().registerType(
      sdk::BuiltinObjectType::kGridMap,
      [this](ObjectTopicId topic_id, sdk::BuiltinObjectType /*object_type*/, const QString& display_name)
          -> std::unique_ptr<ISceneLayer> {
        prepareTransformBufferForTopic(topic_id);
        auto layer = std::make_unique<GridMapLayer>(topic_id, display_name, this);
        wireScene3DLayer(layer.get());
        return layer;
      });

  frame_overlay_combo_ = new ComboBox(this);
  frame_overlay_combo_->setFocusPolicy(Qt::ClickFocus);
  frame_overlay_combo_->raise();
  refreshFrameOverlayCombo();
  connect(frame_overlay_combo_, &QComboBox::currentIndexChanged, this, &Scene3DDockWidget::onOverlayFramePicked);

  // Camera-model selector + Home button — same styled overlay control as the
  // fixed-frame combo (a pj_widgets ComboBox), anchored top-right just left of
  // the orientation gizmo. Children of `this` (NOT view_) so they layer above
  // the QOpenGLWidget without fighting ADS's native-window flags. addItems()
  // before connect() avoids a spurious callback into a not-yet-constructed view
  // during the ctor.
  camera_model_combo_ = new ComboBox(this);
  camera_model_combo_->setObjectName(u"cameraModelCombo"_s);
  camera_model_combo_->setFocusPolicy(Qt::ClickFocus);
  camera_model_combo_->addItems({tr("Orbit"), tr("XYOrbit"), tr("Fly"), tr("Top-down ortho")});
  camera_model_combo_->raise();
  connect(camera_model_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
    if (view_ != nullptr && index >= 0) {
      view_->setCameraModel(static_cast<pj::scene3d::SceneViewWidget::CameraModel>(index));
      view_->update();
    }
  });

  home_button_ = new QToolButton(this);
  home_button_->setObjectName(u"cameraHomeButton"_s);
  home_button_->setFocusPolicy(Qt::ClickFocus);
  home_button_->setToolTip(tr("Reset view to default"));
  // Bundled "recenter" glyph (resources.qrc). Pinned to the light-theme
  // ink so it stays dark on this always-light overlay button, even when the
  // app is in dark mode (theme-following ink would render near-invisible here).
  home_button_->setIcon(PJ::loadSvg(u":/resources/svg/recenter.svg"_s));
  // No padding: let the glyph fill the button (icon sized below).
  home_button_->setStyleSheet(
      QStringLiteral(
          "QToolButton { background-color: rgba(255, 255, 255, 200); border: 1px solid rgba(60, 60, 60, 180); "
          "padding: %1px; border-radius: 3px; }")
          .arg(PJ::theme::space(PJ::theme::Space::None)));
  home_button_->raise();
  connect(home_button_, &QToolButton::clicked, this, [this]() {
    if (view_ != nullptr) {
      view_->resetCamera();
    }
  });

  // Forget a removed topic's cached orphan/warning state. pj_app drives its UI
  // off the base SceneDockWidget layer* signals directly, so no relay is needed.
  connect(this, &SceneDockWidget::layerRemoved, this, [this](ObjectTopicId topic_id) {
    orphan_states_.erase(topicKey(topic_id));
    restored_layer_orders_.erase(topicKey(topic_id));
    local_layer_ids_.erase(topic_id.id);
    scene_topic_datasets_.erase(topic_id.id);
    // A local robot layer carries no store dataset, so its removal never affects
    // the TF binding; a store-backed layer's removal might be the last topic of
    // the bound dataset, which frees the buffer for a rebind (M.17).
    resetTransformBindingIfDatasetGone();
  });
}

Scene3DDockWidget::~Scene3DDockWidget() {
  // Release the view's layer references — and their GL resources — while this
  // concrete class is still alive: clearLayers() reconciles the view through
  // syncViewLayers(), which the base destructor can no longer dispatch to us.
  // Without this, SceneViewWidget::layers_ would dangle over the destroyed
  // layers and releaseGlResources()/setLayers() would dereference freed
  // memory (and the layers' GL objects would die without a current context).
  clearLayers();
}

void Scene3DDockWidget::setTransformService(pj::scene3d::TransformService* service) {
  if (tf_ready_conn_) {
    QObject::disconnect(tf_ready_conn_);
    tf_ready_conn_ = {};
  }
  transform_service_ = service;
  if (view_ != nullptr && tf_buffer_ != nullptr) {
    view_->setTransformBuffer(tf_buffer_);
  }
  // Progressive publication can fill the current pointer, while replacement
  // commit publishes a new generation pointer. The ready signal handles both:
  // refresh cached frame/orphan state or rebind before refreshing.
  if (transform_service_ != nullptr) {
    tf_ready_conn_ = connect(
        transform_service_, &pj::scene3d::TransformService::datasetTransformsReady, this,
        &Scene3DDockWidget::onDatasetTransformsReady, Qt::UniqueConnection);
  }
}

void Scene3DDockWidget::onDatasetTransformsReady(DatasetId dataset_id) {
  // Only the dataset bound by prepareTransformBufferForTopic may replace this
  // dock's buffer. Before any layer binds there is nothing to refresh.
  if (view_ == nullptr || transform_service_ == nullptr || tf_buffer_ == nullptr || dataset_id != dataset_id_) {
    return;
  }
  const std::shared_ptr<pj::scene3d::TransformBuffer> ready_buffer = transform_service_->transformBuffer(dataset_id);
  if (tf_buffer_ != ready_buffer) {
    tf_buffer_ = ready_buffer;
    view_->setTransformBuffer(tf_buffer_);
    pushTransformBufferToTrailLayers();
  }
  // Re-enumerate the whole hierarchy, including a progressive in-place update
  // where setTransformBuffer correctly no-ops on the unchanged pointer.
  view_->refreshAvailableFrames();
  recomputeOrphanStates();
  // A progressive file load folds TF in while the tracker is paused, so the
  // time-gated refresh inside setTrackerTime won't fire; re-resolve the layers at
  // the current playhead so the scene fills in as the file loads, not only at the
  // end.
  onTrackerTime(last_tracker_display_);
  view_->update();
}

void Scene3DDockWidget::setSessionManager(SessionManager* session) {
  SceneDockWidget::setSessionManager(session);
  reconnectLiveSamples(session);
}

void Scene3DDockWidget::reconnectLiveSamples(SessionManager* session) {
  if (live_samples_conn_) {
    QObject::disconnect(live_samples_conn_);
    live_samples_conn_ = {};
  }
  if (session == nullptr) {
    return;
  }
  // Streamed FrameTransform messages must be folded into the TF buffer as they
  // arrive: a file load is driven by FileLoader (ingestFrameTransformsForDataset
  // per flush -> datasetTransformsReady, handled in setTransformService), but a
  // live stream has no such driver, so without this slot the buffer stays empty
  // and every sensor frame is orphan (red). samplesIngested fires on the UI thread
  // after the retention trim, with live=true only while following a live stream —
  // file load emits live=false and is served by the FileLoader path instead.
  live_samples_conn_ =
      connect(session, &SessionManager::samplesIngested, this, [this](const QVector<TopicId>&, bool live) {
        if (!live || transform_service_ == nullptr || tf_buffer_ == nullptr) {
          return;
        }
        transform_service_->ingestNewTransforms(dataset_id_);
        // Recompute orphan states only when the TF buffer actually changed
        // (force=false gates on revision()+fixed-frame): a sibling 3D dock sharing
        // this dataset may have advanced the shared ingest cursor, so we cannot
        // gate on our own ingest call's return value — but the buffer revision
        // catches any writer. driveVisibleLayersToLiveEdge() then advances the
        // object layers to the newest store data and repaints (itself a no-op when
        // the live edge hasn't moved).
        recomputeOrphanStates(/*force=*/false);
        driveVisibleLayersToLiveEdge();
      });
}

void Scene3DDockWidget::driveVisibleLayersToLiveEdge() {
  if (sessionManager() == nullptr) {
    return;
  }
  bool any = false;
  int64_t latest = std::numeric_limits<int64_t>::lowest();
  for (const SceneLayerInfo& info : layers()) {
    ISceneLayer* layer = layerFor(info.topic_id);
    if (!info.visible || layer == nullptr) {
      continue;
    }
    // Consult the layer's own timeRange() rather than store.timeRange(topic_id):
    // a multi-topic layer (OccupancyGrid + its _updates sibling) reports a live
    // edge that the base topic alone would miss, freezing it at the last
    // keyframe. Empty layers report an inverted range and are skipped.
    const PJ::Range<PJ::Timepoint> range = layer->timeRange();
    if (range.max < range.min) {
      continue;
    }
    latest = std::max(latest, PJ::toRaw(range.max));
    any = true;
  }
  // Fold config (TF) topics into the live edge so a layer-less TF-only dock still
  // advances + repaints every live tick (H.3). Without this, layers() being empty
  // bailed before any view update and the TF axes stayed frozen on the seed time.
  ObjectStore& store = sessionManager()->objectStore();
  for (const uint32_t topic_raw : config_topics_) {
    const ObjectTopicId topic_id{topic_raw};
    if (store.entryCount(topic_id) == 0) {
      continue;
    }
    latest = std::max(latest, store.timeRange(topic_id).second);
    any = true;
  }
  if (!any) {
    return;
  }
  // Skip when the edge hasn't moved: MainWindow's playhead path repaints per tick
  // regardless, so re-pushing the same time here is wasted work (L.45/L.46).
  if (const auto last_ns = lastTrackerNs(); last_ns.has_value() && *last_ns == latest) {
    return;
  }
  noteTrackerTime(latest);
  pushTrackerTimeToView(latest);
}

void Scene3DDockWidget::pushTrackerTimeToView(int64_t time_ns) {
  for (const SceneLayerInfo& info : layers()) {
    if (ISceneLayer* layer = layerFor(info.topic_id); layer != nullptr && info.visible) {
      layer->setTrackerTime(PJ::fromRaw(time_ns));
    }
  }
  if (view_ != nullptr) {
    view_->setTrackerTime(PJ::fromRaw(time_ns));
  }
  refreshView();  // refreshView() already unions scene bounds before repainting
  // This live-edge push painted OUTSIDE the per-tick gate, so the gate's
  // last-painted key now lags the pixels on screen. Force the next onTrackerTime()
  // to re-evaluate rather than trust a stale match (e.g. a tracker lagging the live
  // edge could otherwise coalesce away a needed correction).
  invalidateTrackerRenderKey();
}

void Scene3DDockWidget::setSettings(QSettings* settings) {
  settings_ = settings;
  if (package_resolver_ != nullptr) {
    package_resolver_->setSettings(settings_);
  }
}

void Scene3DDockWidget::setEmbeddedAssets(QMap<QString, QByteArray> assets) {
  embedded_assets_ = std::move(assets);
  if (package_resolver_ != nullptr) {
    package_resolver_->setEmbeddedAssets(embedded_assets_);
  }
}

void Scene3DDockWidget::setSourcePath(const QString& path) {
  source_path_ = path;
  if (package_resolver_ != nullptr) {
    package_resolver_->setSourcePath(source_path_);
  }
}

bool Scene3DDockWidget::handlesObjectType(sdk::BuiltinObjectType object_type) {
  return object_type == sdk::BuiltinObjectType::kPointCloud ||
         object_type == sdk::BuiltinObjectType::kCompressedPointCloud ||
         object_type == sdk::BuiltinObjectType::kFrameTransforms ||
         object_type == sdk::BuiltinObjectType::kOccupancyGrid ||
         object_type == sdk::BuiltinObjectType::kRobotDescription ||
         object_type == sdk::BuiltinObjectType::kSceneEntities ||
         object_type == sdk::BuiltinObjectType::kPosesInFrame || object_type == sdk::BuiltinObjectType::kVoxelGrid ||
         object_type == sdk::BuiltinObjectType::kGridMap ||
         // kImage is accepted only for DEPTH-encoded images; addTopic() peeks the
         // first sample's encoding and rejects color images (which share kImage).
         object_type == sdk::BuiltinObjectType::kImage;
}

bool Scene3DDockWidget::addTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
  // Interactive add (drop / family switch): enforce the kImage depth-encoding gate.
  return addTopicImpl(topic_id, object_type, title, /*enforce_image_gate=*/true);
}

bool Scene3DDockWidget::addTopicImpl(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title, bool enforce_image_gate) {
  if (sessionManager() == nullptr) {
    qCWarning(lcScene3DDock) << "addTopic: session is null";
    return false;
  }
  if (layerFor(topic_id) != nullptr) {
    return true;
  }
  if (object_type == sdk::BuiltinObjectType::kFrameTransforms && config_topics_.contains(topic_id.id)) {
    return true;
  }
  if (!isLocalLayerId(topic_id) && dataset_id_ != 0) {
    const DatasetId incoming_dataset = sessionManager()->objectStore().descriptor(topic_id).dataset_id;
    if (incoming_dataset != 0 && incoming_dataset != dataset_id_) {
      return false;
    }
  }
  if (!handlesObjectType(object_type)) {
    qCWarning(lcScene3DDock) << "addTopic: unsupported object_type" << static_cast<int>(object_type);
    return false;
  }
  // Encoding gate: a kImage topic is a DepthCloud only when its samples are depth
  // pixels. Color images share kImage and must not become 3D layers. Skipped on the
  // restore path (enforce_image_gate=false): a saved DepthCloud layer was already
  // validated as depth when created, and its first sample may not be loaded yet at
  // restore time — gating on an absent sample would silently drop the layer (C1).
  if (enforce_image_gate && object_type == sdk::BuiltinObjectType::kImage &&
      !firstSampleIsDepthEncoded(*sessionManager(), topic_id)) {
    return false;
  }

  const bool accepted = SceneDockWidget::addTopic(topic_id, object_type, title);
  if (accepted) {
    setWindowTitle(title.isEmpty() ? tr("3D View") : tr("3D View - %1").arg(title));
    recomputeOrphanStates();
  }
  return accepted;
}

ObjectTopicId Scene3DDockWidget::addRobotModelLayer(const QString& urdf_path) {
  const ObjectTopicId topic_id = allocateLocalLayerId();
  if (topic_id.id == 0) {
    qCWarning(lcScene3DDock) << "addRobotModelLayer: exhausted local topic ids";
    return ObjectTopicId{0};
  }
  const QString title = urdf_path.isEmpty() ? tr("Robot model") : QFileInfo(urdf_path).fileName();
  if (!addTopic(topic_id, sdk::BuiltinObjectType::kRobotDescription, title)) {
    local_layer_ids_.erase(topic_id.id);
    return ObjectTopicId{0};
  }
  // One-click flow: point the freshly created File-source layer at the chosen
  // URDF immediately (the creator already forced kFile per the PUNCH-4 rule).
  if (!urdf_path.isEmpty()) {
    if (auto* layer = dynamic_cast<RobotModelLayer*>(layerFor(topic_id)); layer != nullptr) {
      layer->setSourceFile(urdf_path);
    }
  }
  return topic_id;
}

ObjectTopicId Scene3DDockWidget::addRobotModelLayerFromUrl(const QString& url) {
  const ObjectTopicId topic_id = allocateLocalLayerId();
  if (topic_id.id == 0) {
    qCWarning(lcScene3DDock) << "addRobotModelLayerFromUrl: exhausted local topic ids";
    return ObjectTopicId{0};
  }
  const QString file_name = QUrl(url).fileName();
  const QString title = file_name.isEmpty() ? url : file_name;
  if (!addTopic(topic_id, sdk::BuiltinObjectType::kRobotDescription, title)) {
    local_layer_ids_.erase(topic_id.id);
    return ObjectTopicId{0};
  }
  if (auto* layer = dynamic_cast<RobotModelLayer*>(layerFor(topic_id)); layer != nullptr) {
    layer->setSourceUrl(url);
  }
  return topic_id;
}

ObjectTopicId Scene3DDockWidget::addTrailLayer(const pj::scene3d::TrailSource& source) {
  const ObjectTopicId topic_id = allocateLocalLayerId();
  if (topic_id.id == 0) {
    qCWarning(lcScene3DDock) << "addTrailLayer: exhausted local topic ids";
    return ObjectTopicId{0};
  }
  return addTrailLayerImpl(topic_id, std::make_unique<pj::scene3d::TrailLayer>(topic_id, source, this));
}

ObjectTopicId Scene3DDockWidget::addTrailLayerImpl(
    ObjectTopicId topic_id, std::unique_ptr<pj::scene3d::TrailLayer> layer) {
  wireScene3DLayer(layer.get());
  if (!insertLayer(topic_id, std::move(layer))) {
    // The layer is already destroyed, so there is no row to carry a warning —
    // the log is the only trace of a dead "Create trail" click.
    qCWarning(lcScene3DDock) << "addTrailLayer: trail layer rejected (attach failed or id in use)";
    local_layer_ids_.erase(topic_id.id);
    return ObjectTopicId{0};
  }
  recomputeOrphanStates();
  return topic_id;
}

QList<Scene3DDockWidget::RobotDescriptionTopic> Scene3DDockWidget::robotDescriptionTopics() const {
  QList<RobotDescriptionTopic> result;
  if (sessionManager() == nullptr) {
    return result;
  }
  ObjectStore& store = sessionManager()->objectStore();
  for (const ObjectTopicId topic_id : store.listTopics()) {
    const ObjectTopicDescriptor& desc = store.descriptor(topic_id);
    if (pj::scene3d::builtinObjectTypeFor(desc) == sdk::BuiltinObjectType::kRobotDescription) {
      result.append({topic_id, QString::fromStdString(desc.topic_name)});
    }
  }
  return result;
}

bool Scene3DDockWidget::pruneEvictedObjects() {
  // Family-specific prune only; the base revalidateObjects() adds the keep-if-
  // never-populated rule. Reports whether any live render layer or config topic
  // remains (a TF-only dock stays alive via config_topics_).
  if (sessionManager() == nullptr) {
    return !layers().empty() || !config_topics_.empty();
  }
  ObjectStore& store = sessionManager()->objectStore();
  std::vector<ObjectTopicId> dead;
  for (const SceneLayerInfo& info : layers()) {
    if (const auto* trail = dynamic_cast<const pj::scene3d::TrailLayer*>(layerFor(info.topic_id)); trail != nullptr) {
      // A trail's own id is synthetic (never in the store), but its POSE source
      // topic can be evicted — drop the trail with its dataset. TF trails
      // orphan instead (follow-frame philosophy: they revive if the frame returns).
      if (trail->source().kind == pj::scene3d::TrailSource::Kind::kPoseTopic &&
          store.descriptor(trail->source().topic).topic_name.empty()) {
        dead.push_back(info.topic_id);
      }
      continue;
    }
    if (isLocalLayerId(info.topic_id)) {
      continue;
    }
    if (store.descriptor(info.topic_id).topic_name.empty()) {
      dead.push_back(info.topic_id);
    }
  }
  for (const ObjectTopicId topic_id : dead) {
    removeTopic(topic_id);
  }
  // Prune evicted config (TF) topics. Unlike render-layer topics they have no
  // layer to remove; an empty descriptor means the dataset was unloaded. Keeping
  // them tracked is what lets a layer-less TF dock survive catalog churn (H.4).
  bool config_changed = false;
  for (auto it = config_topics_.begin(); it != config_topics_.end();) {
    if (store.descriptor(ObjectTopicId{*it}).topic_name.empty()) {
      scene_topic_datasets_.erase(*it);
      it = config_topics_.erase(it);
      config_changed = true;
    } else {
      ++it;
    }
  }
  if (config_changed) {
    resetTransformBindingIfDatasetGone();
  }
  return !layers().empty() || !config_topics_.empty();
}

bool Scene3DDockWidget::tryAcceptObjectTopic(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
  // addTopic (this class's shadow, with title/orphan bookkeeping) already
  // guards on handlesObjectType() and consumes config topics via the base.
  return addTopic(topic_id, object_type, title);
}

void Scene3DDockWidget::onTrackerTime(double time) {
  // Remember the instant we are showing so a mid-load TF update (datasetTransformsReady)
  // can re-render here without a new playback push. Guard finiteness so a NaN tick
  // never poisons the replayed value.
  if (std::isfinite(time)) {
    last_tracker_display_ = time;
  }
  // The base converts (NaN/inf-safe), clamps with the latched-layer rule, drives
  // the layers, and — only when the visible scene would actually differ at this
  // time — calls refreshView() (below), which pushes the render time into the view.
  // When nothing moved the base returns without painting, so a 60 Hz playhead over
  // <10 Hz data (or a static pose) no longer repaints the GL view every tick. The
  // view's setTrackerTime/bounds therefore live in refreshView(), not here, so they
  // are gated together with the repaint.
  SceneDockWidget::onTrackerTime(time);
}

DatasetId Scene3DDockWidget::representativeDatasetId() const {
  // A TF-only dock has no render layer for the base to find, so fall back to the
  // bound TF dataset; a dock with render layers keeps the base's layer-derived id.
  const DatasetId from_layers = SceneDockWidget::representativeDatasetId();
  return (from_layers != 0) ? from_layers : dataset_id_;
}

QWidget* Scene3DDockWidget::createSceneView() {
  auto* view = new pj::scene3d::SceneViewWidget();
  view_ = view;
  view_->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
  view_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  if (camera_model_combo_ != nullptr && camera_model_combo_->currentIndex() >= 0) {
    view_->setCameraModel(static_cast<SceneViewWidget::CameraModel>(camera_model_combo_->currentIndex()));
  }
  connect(view_, &pj::scene3d::SceneViewWidget::framesChanged, this, &Scene3DDockWidget::onAvailableFrames);
  // Right-click on a frame gizmo: the view picks the frame (hover machinery)
  // and the dock owns the menu — trail creation plus two one-click conveniences
  // over existing dock APIs. Empty-space right-clicks never reach here (the
  // view ignore()s them, so the standard dock menu appears instead).
  connect(
      view_, &pj::scene3d::SceneViewWidget::frameContextMenuRequested, this,
      [this](const QString& frame, const QPoint& global_pos) {
        QMenu menu(this);
        menu.addAction(tr("Create trail for '%1'").arg(frame), this, [this, frame]() {
          addTrailLayer(pj::scene3d::TrailSource::tfFrame(frame));
        });
        menu.addAction(tr("Set as fixed frame"), this, [this, frame]() { setFixedFrame(frame); });
        menu.addAction(tr("Follow this frame"), this, [this, frame]() { setFollowFrame(frame); });
        menu.exec(global_pos);
      });
  if (tf_buffer_ != nullptr) {
    view_->setTransformBuffer(tf_buffer_);
  }
  // refreshFrameOverlayCombo() already ends with layoutFrameOverlayCombo(),
  // which itself calls raise() — no second pass needed (L.82).
  refreshFrameOverlayCombo();
  // view_ is non-null from here: let the host apply view-only state (scene
  // controls) that could not be pushed while the lazy view did not yet exist.
  emit sceneViewReady();
  connect(view_, &SceneViewWidget::presentationChanged, this, &Scene3DDockWidget::notifyWorkspaceChanged);
  return view_;
}

std::unique_ptr<SceneLayerContext> Scene3DDockWidget::makeContext() {
  auto ctx = std::make_unique<Scene3DLayerContext>();
  ctx->session = sessionManager();
  ctx->tf_buffer = tf_buffer_;
  return ctx;
}

bool Scene3DDockWidget::acceptsObjectType(sdk::BuiltinObjectType object_type) const {
  // The factory registrations in the constructor are the single source of truth
  // for which render-layer types this dock can host.
  return layerFactory().supports(object_type);
}

bool Scene3DDockWidget::acceptsDeferredObjectType(sdk::BuiltinObjectType object_type) const {
  return handlesObjectType(object_type);
}

SceneDockWidget::DeferredElementKind Scene3DDockWidget::deferredElementKind(sdk::BuiltinObjectType object_type) const {
  return object_type == sdk::BuiltinObjectType::kFrameTransforms ? DeferredElementKind::kConfigTopic
                                                                 : DeferredElementKind::kRenderLayer;
}

bool Scene3DDockWidget::handleSceneConfigTopic(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
  if (object_type != sdk::BuiltinObjectType::kFrameTransforms) {
    return false;
  }
  if (sessionManager() == nullptr) {
    qCWarning(lcScene3DDock) << "addTopic: session is null";
    return false;
  }
  prepareTransformBufferForTopic(topic_id);
  // Remember this as a config (TF) topic so a layer-less dock survives catalog
  // churn (revalidateObjects) and folds into the live-edge drive (H.3 / H.4).
  const DatasetId dataset = sessionManager()->objectStore().descriptor(topic_id).dataset_id;
  scene_topic_datasets_[topic_id.id] = dataset;
  config_topics_.insert(topic_id.id);
  setWindowTitle(title.isEmpty() ? tr("3D View") : tr("3D View - %1").arg(title));
  return true;
}

void Scene3DDockWidget::syncViewLayers(const std::vector<ISceneLayer*>& ordered_layers) {
  std::vector<Scene3DLayer*> ordered;
  ordered.reserve(ordered_layers.size());
  const QString fixed = currentFixedFrame();
  for (ISceneLayer* layer : ordered_layers) {
    auto* scene3d_layer = dynamic_cast<Scene3DLayer*>(layer);
    if (scene3d_layer == nullptr) {
      continue;
    }
    absorbFallbackFrames(scene3d_layer);
    if (!fixed.isEmpty()) {
      scene3d_layer->setFixedFrame(fixed);
    }
    ordered.push_back(scene3d_layer);
  }
  if (view_ != nullptr) {
    view_->setLayers(ordered);
    updateSceneBounds();
  }
  recomputeOrphanStates();
}

void Scene3DDockWidget::updateSceneBounds() {
  if (view_ == nullptr) {
    return;
  }
  pj::scene3d::AABB scene;
  static const pj::scene3d::TransformBuffer k_empty_buffer;
  const pj::scene3d::TransformBuffer& tf = tf_buffer_ != nullptr ? *tf_buffer_ : k_empty_buffer;
  const pj::scene3d::FrameContext frame_ctx{tf, view_->fixedFrame(), PJ::fromRaw(lastTrackerNs().value_or(0))};
  for (Scene3DLayer* layer : view_->layers()) {
    if (layer == nullptr) {
      continue;
    }
    if (const auto bounds = layer->worldBounds(); bounds.has_value()) {
      std::string source_frame = layer->sourceFrame().toStdString();
      if (source_frame.empty()) {
        source_frame = view_->fixedFrame();  // matches DepthCloud's unframed-image fallback
      }
      if (const auto fixed_from_source = frame_ctx.lookup(source_frame); fixed_from_source.has_value()) {
        scene = pj::scene3d::unionAABB(
            scene, pj::scene3d::transformedAABB(glm::mat4(fixed_from_source->matrix()), *bounds));
      }
    }
  }
  view_->setSceneBounds(scene);
}

void Scene3DDockWidget::refreshView() {
  if (view_ == nullptr) {
    return;
  }
  // Push the current render time into the view HERE (not in onTrackerTime) so the
  // base's per-tick repaint gate also gates this work: refreshView() runs only on a
  // tracker tick that actually changed the scene, or on an async layer push
  // (compressed-cloud decode / settings) arriving via repaintRequested — both must
  // re-render at the current playhead. setTrackerTime() self-guards on render_time_
  // equality, so a settings-only refresh skips the frame-list rebuild.
  if (const auto ns = lastTrackerNs(); ns.has_value()) {
    view_->setTrackerTime(PJ::fromRaw(*ns));
  }
  // Async pushes can arrive with no tracker tick (paused), so the camera's scene
  // bounds must refresh too. O(layers), cheap.
  updateSceneBounds();
  view_->update();
}

uint64_t Scene3DDockWidget::viewRenderKey(PJ::Timepoint time) const {
  // The TF axis triads + parent-connection lines are drawn by the view from the
  // whole frame forest, owned by no layer; without folding them into the gate a
  // moving TF tree freezes whenever every visible layer's renderKey is unchanged
  // (a static/fixed-frame cloud, or a TF-only dock). Returns 0 when no TF overlay
  // is drawn (the view decides), so coalescing is full when it should be.
  if (view_ == nullptr) {
    return 0;
  }
  // Fold the followed frame's pose in too: Position-follow moves the camera with the
  // target, which no layer's renderKey reflects, so without this a moving follow
  // target would freeze whenever the TF overlay is hidden. followRenderKey is 0 when
  // not following, so it doesn't reduce coalescing then.
  return view_->tfRenderKey(time) ^ view_->followRenderKey(time);
}

QString Scene3DDockWidget::xmlTag() const {
  return u"scene3d"_s;
}

void Scene3DDockWidget::prepareTransformBufferForTopic(ObjectTopicId topic_id) {
  if (transform_service_ == nullptr || sessionManager() == nullptr) {
    return;
  }
  ObjectStore& store = sessionManager()->objectStore();
  const auto dataset_id = store.descriptor(topic_id).dataset_id;
  // Record the topic→dataset mapping for both layer and config topics so the
  // removal path can detect when the TF-bound dataset has no remaining topics
  // and rebind (M.17). handleSceneConfigTopic also records config topics in
  // config_topics_; this covers render-layer topics.
  scene_topic_datasets_[topic_id.id] = dataset_id;

  if (tf_buffer_ != nullptr) {
    // Already bound. Mixing a second dataset's topics into one 3D dock would
    // resolve B's frames against A's TF tree — silently wrong. Warn loudly and
    // keep the existing binding rather than rebinding underneath live layers.
    // The reset-on-removal path lets a genuine rebind happen once dataset A's
    // topics are gone (tf_buffer_ goes back to null first).
    if (dataset_id != dataset_id_) {
      qCWarning(lcScene3DDock) << "prepareTransformBufferForTopic: topic from dataset" << dataset_id
                               << "added to a 3D dock already bound to dataset" << dataset_id_
                               << "- its frames will resolve against the wrong TF tree";
    }
    return;
  }
  dataset_id_ = dataset_id;  // cached for the live-samples TF ingest slot
  tf_buffer_ = transform_service_->transformBuffer(dataset_id);
  if (view_ != nullptr) {
    view_->setTransformBuffer(tf_buffer_);
  }
  pushTransformBufferToTrailLayers();
}

void Scene3DDockWidget::resetTransformBindingIfDatasetGone() {
  if (tf_buffer_ == nullptr) {
    return;
  }
  const bool dataset_still_present = std::any_of(
      scene_topic_datasets_.begin(), scene_topic_datasets_.end(),
      [this](const auto& entry) { return entry.second == dataset_id_; });
  if (dataset_still_present) {
    return;
  }
  // No tracked topic belongs to the bound dataset anymore: drop the binding so a
  // later topic from a different dataset can rebind via prepareTransformBufferForTopic.
  tf_buffer_.reset();
  dataset_id_ = 0;
  if (view_ != nullptr) {
    view_->setTransformBuffer(nullptr);  // setTransformBuffer tolerates a null buffer
  }
  // TF-source trails belonged to the unloaded dataset's frame tree: remove them
  // with it (pose-source trails prune with their own source topic instead). A
  // missing FRAME merely orphans a trail; a gone DATASET deletes it. Restored
  // trails on a never-bound dock are unaffected — this runs only when a live
  // binding resets.
  std::vector<ObjectTopicId> dead_trails;
  for (const SceneLayerInfo& info : layers()) {
    if (const auto* trail = dynamic_cast<const pj::scene3d::TrailLayer*>(layerFor(info.topic_id));
        trail != nullptr && trail->source().kind == pj::scene3d::TrailSource::Kind::kTfFrame) {
      dead_trails.push_back(info.topic_id);
    }
  }
  for (const ObjectTopicId topic_id : dead_trails) {
    removeTopic(topic_id);
  }
  pushTransformBufferToTrailLayers();
}

void Scene3DDockWidget::pushTransformBufferToTrailLayers() {
  for (const SceneLayerInfo& info : layers()) {
    if (auto* trail = trailOf(layerFor(info.topic_id)); trail != nullptr) {
      trail->setTransformBuffer(tf_buffer_);
    }
  }
}

void Scene3DDockWidget::wireScene3DLayer(Scene3DLayer* layer) {
  if (layer == nullptr) {
    return;
  }
  connect(layer, &Scene3DLayer::fallbackFramesChanged, this, [this, layer](const QStringList&) {
    absorbFallbackFrames(layer);
  });
  connect(layer, &Scene3DLayer::sourceFrameChanged, this, [this](const QString&) { recomputeOrphanStates(); });
  // A layer-self warning (e.g. a model that failed to fetch/import) shares the
  // per-row warning slot with the orphan state; force a recompute so the combine
  // runs even when TF/fixed-frame are unchanged (the fast-path guard would skip).
  connect(layer, &Scene3DLayer::statusWarningChanged, this, [this]() { recomputeOrphanStates(/*force=*/true); });
}

void Scene3DDockWidget::absorbFallbackFrames(Scene3DLayer* layer) {
  if (layer == nullptr) {
    return;
  }
  bool changed = false;
  for (const QString& frame : layer->fallbackFrames()) {
    const std::string frame_std = frame.toStdString();
    if (std::find(fallback_frames_.begin(), fallback_frames_.end(), frame_std) == fallback_frames_.end()) {
      fallback_frames_.push_back(frame_std);
      changed = true;
    }
  }
  if (changed) {
    onAvailableFrames(available_frames_);
  }
}

void Scene3DDockWidget::onAvailableFrames(const QList<FrameRow>& frames) {
  std::vector<QList<FrameRow>> clusters;
  for (const auto& row : frames) {
    if (row.depth == 0 || clusters.empty()) {
      clusters.emplace_back();
    }
    clusters.back().append(row);
  }
  for (const auto& fallback : fallback_frames_) {
    if (!framesContain(frames, QString::fromStdString(fallback))) {
      clusters.push_back({FrameRow{fallback, 0}});
    }
  }
  std::sort(clusters.begin(), clusters.end(), [](const QList<FrameRow>& a, const QList<FrameRow>& b) {
    return pj::scene3d::frameNameLess(a.first().name, b.first().name);
  });

  QList<FrameRow> effective;
  for (auto& cluster : clusters) {
    for (auto& row : cluster) {
      effective.append(std::move(row));
    }
  }

  if (effective.isEmpty()) {
    return;
  }
  available_frames_ = effective;
  emit availableFramesChanged(effective);
  refreshFrameOverlayCombo();

  if (fixed_frame_mode_ == FixedFrameMode::kAutoRoot || currentFixedFrame().isEmpty()) {
    applyResolvedFixedFrame(resolveAutoFixedFrame(effective));
  }
  recomputeOrphanStates();
}

QString Scene3DDockWidget::resolveAutoFixedFrame(const QList<FrameRow>& frames) const {
  // Prefer the frame the user last picked by hand for this dataset's TransformBuffer
  // (shared across docks, persisted across restarts), but only when it still exists
  // in this dock's tree — a different recording of the same source may lack it.
  if (transform_service_ != nullptr && dataset_id_ != 0) {
    const QString remembered = transform_service_->rememberedFixedFrame(dataset_id_);
    if (!remembered.isEmpty() && framesContain(frames, remembered)) {
      return remembered;
    }
  }
  return pickFixedFrame(frames);
}

QString Scene3DDockWidget::currentFixedFrame() const {
  if (view_ == nullptr) {
    return {};
  }
  return QString::fromStdString(view_->fixedFrame());
}

QString Scene3DDockWidget::currentFollowFrame() const {
  if (view_ == nullptr) {
    return {};
  }
  return QString::fromStdString(view_->followFrame());
}

void Scene3DDockWidget::setFollowFrame(const QString& frame) {
  if (view_ == nullptr || currentFollowFrame() == frame) {
    return;
  }
  view_->setFollowFrame(frame.toStdString());
  emit followFrameChanged(frame);
  notifyWorkspaceChanged();
}

void Scene3DDockWidget::recenterOnFollowFrame() {
  if (view_ != nullptr) {
    view_->recenterOnFollowFrame();
  }
}

bool Scene3DDockWidget::layerVisible(ObjectTopicId topic_id) const {
  const ISceneLayer* layer = layerFor(topic_id);
  return layer != nullptr && layer->info().visible;
}

void Scene3DDockWidget::setFixedFrame(const QString& frame) {
  if (frame.isEmpty() || view_ == nullptr) {
    return;
  }
  const bool mode_flipped = (fixed_frame_mode_ == FixedFrameMode::kAutoRoot);
  const bool frame_changed = currentFixedFrame() != frame;
  fixed_frame_mode_ = FixedFrameMode::kExplicit;
  if (mode_flipped) {
    emit fixedFrameModeChanged(false);
    refreshFrameOverlayCombo();
  }
  applyResolvedFixedFrame(frame);
  // Remember this explicit pick so a NEWLY-created dock on the same dataset's
  // TransformBuffer defaults to it (resolveAutoFixedFrame). dataset_id_ is 0 for a
  // not-yet-bound or local-only dock — nothing to key the memory by, so skip.
  if (transform_service_ != nullptr && dataset_id_ != 0) {
    transform_service_->rememberFixedFrame(dataset_id_, frame);
  }
  if (mode_flipped || frame_changed) {
    notifyWorkspaceChanged();
  }
}

void Scene3DDockWidget::setFixedFrameAutoRoot() {
  if (view_ == nullptr) {
    return;
  }
  const bool mode_flipped = (fixed_frame_mode_ == FixedFrameMode::kExplicit);
  const QString resolved = resolveAutoFixedFrame(available_frames_);
  const bool frame_changed = currentFixedFrame() != resolved;
  fixed_frame_mode_ = FixedFrameMode::kAutoRoot;
  if (mode_flipped) {
    emit fixedFrameModeChanged(true);
    refreshFrameOverlayCombo();
  }
  applyResolvedFixedFrame(resolved);
  if (mode_flipped || frame_changed) {
    notifyWorkspaceChanged();
  }
}

void Scene3DDockWidget::applyResolvedFixedFrame(const QString& frame) {
  if (frame.isEmpty() || view_ == nullptr) {
    return;
  }
  if (currentFixedFrame() == frame) {
    return;
  }
  view_->setFixedFrame(frame.toStdString());
  // Single fan-out: reconcileViewLayers() re-runs syncViewLayers(), whose 3D
  // override pushes the (now-changed) currentFixedFrame() to every layer, updates
  // bounds, AND recomputes orphans — so we don't re-iterate layers or recompute
  // here (L.83). currentFixedFrame() must already read the new frame before this:
  // view_->setFixedFrame above made it so.
  reconcileViewLayers();
  view_->update();
  emit currentFixedFrameChanged(frame);
  refreshFrameOverlayCombo();
}

void Scene3DDockWidget::applyRestoredFixedFrame(FixedFrameMode mode, const QString& frame) {
  const bool mode_changed = fixed_frame_mode_ != mode;
  fixed_frame_mode_ = mode;
  if (!frame.isEmpty()) {
    applyResolvedFixedFrame(frame);
  } else if (mode == FixedFrameMode::kAutoRoot) {
    applyResolvedFixedFrame(resolveAutoFixedFrame(available_frames_));
  }
  if (mode_changed) {
    emit fixedFrameModeChanged(mode == FixedFrameMode::kAutoRoot);
  }
  refreshFrameOverlayCombo();
}

void Scene3DDockWidget::resetDatasetBindingForRestore() {
  const bool had_frames = !available_frames_.isEmpty();
  tf_buffer_.reset();
  dataset_id_ = 0;
  scene_topic_datasets_.clear();
  config_topics_.clear();
  available_frames_.clear();
  fallback_frames_.clear();
  orphan_states_.clear();
  last_orphan_revision_ = 0;
  last_orphan_fixed_frame_.clear();
  if (view_ != nullptr) {
    view_->setTransformBuffer(nullptr);
    view_->setFixedFrame({});
  }
  if (had_frames) {
    emit availableFramesChanged({});
  }
  refreshFrameOverlayCombo();
}

void Scene3DDockWidget::refreshFrameOverlayCombo() {
  if (frame_overlay_combo_ == nullptr) {
    return;
  }
  QSignalBlocker block(frame_overlay_combo_);
  frame_overlay_combo_->clear();
  for (const auto& row : available_frames_) {
    const QString name = QString::fromStdString(row.name);
    const QString display = QString(row.depth * 2, QChar(' ')) + name;
    frame_overlay_combo_->addItem(display, name);
  }
  const QString current = currentFixedFrame();
  const int idx = frame_overlay_combo_->findData(current);
  if (idx >= 0) {
    frame_overlay_combo_->setCurrentIndex(idx);
  }
  layoutFrameOverlayCombo();
}

void Scene3DDockWidget::onOverlayFramePicked(int /*index*/) {
  if (frame_overlay_combo_ == nullptr) {
    return;
  }
  const QString name = frame_overlay_combo_->currentData().toString();
  if (!name.isEmpty()) {
    setFixedFrame(name);
  }
}

void Scene3DDockWidget::layoutFrameOverlayCombo() {
  if (frame_overlay_combo_ == nullptr || view_ == nullptr) {
    return;
  }
  constexpr int kMargin = 8;
  // Combo width tracks the selected item only (so the overlay stays compact even
  // when one frame name is very long). The chrome is added by the active combo
  // style rather than a hardcoded slack, which avoids clipping themed combos.
  const QFontMetrics fm(frame_overlay_combo_->font());
  const QString current_text = frame_overlay_combo_->currentText();
  const auto combo_width_for = [&](const QString& text) {
    QStyleOptionComboBox opt;
    opt.initFrom(frame_overlay_combo_);
    const QSize content(fm.horizontalAdvance(text), fm.height());
    return frame_overlay_combo_->style()
        ->sizeFromContents(QStyle::CT_ComboBox, &opt, content, frame_overlay_combo_)
        .width();
  };
  const int natural_w = combo_width_for(current_text);
  const int avail = width() - 2 * kMargin;
  const int w = (avail > 0) ? std::min(natural_w, avail) : natural_w;
  const int h = frame_overlay_combo_->sizeHint().height();
  const QPoint view_origin = view_->pos();
  frame_overlay_combo_->setGeometry(view_origin.x() + kMargin, view_origin.y() + kMargin, w, h);

  if (auto* popup_view = frame_overlay_combo_->view()) {
    int popup_w = natural_w;
    for (int i = 0; i < frame_overlay_combo_->count(); ++i) {
      popup_w = std::max(popup_w, combo_width_for(frame_overlay_combo_->itemText(i)));
    }
    popup_view->setMinimumWidth(popup_w);
  }
  frame_overlay_combo_->raise();

  // Recenter button sits immediately to the right of the fixed-frame combo.
  constexpr int kGap = 6;
  if (home_button_ != nullptr) {
    const int hb = h;  // square, matching the fixed-frame combo height
    home_button_->setGeometry(view_origin.x() + kMargin + w + kGap, view_origin.y() + kMargin, hb, hb);
    const int home_icon_dim = std::max(hb - 2, 1);  // fill minus the 1px border, matching config-panel icons
    home_button_->setIconSize(QSize(home_icon_dim, home_icon_dim));
    home_button_->raise();
  }

  // Camera-model combo, anchored flush to the top-right edge. The orientation
  // gizmo now lives in the bottom-right corner (set in the SceneViewWidget ctor),
  // so nothing is reserved up here.
  if (camera_model_combo_ != nullptr) {
    // Size via the combo's own style chrome (chevron + padding), matching the
    // fixed-frame combo's style-driven sizing above — a hardcoded slack would
    // clip a themed ComboBox.
    const QFontMetrics cm_fm(camera_model_combo_->font());
    QStyleOptionComboBox cam_opt;
    cam_opt.initFrom(camera_model_combo_);
    int cam_w = 0;
    for (int i = 0; i < camera_model_combo_->count(); ++i) {
      const QSize content(cm_fm.horizontalAdvance(camera_model_combo_->itemText(i)), cm_fm.height());
      cam_w = std::max(
          cam_w, camera_model_combo_->style()
                     ->sizeFromContents(QStyle::CT_ComboBox, &cam_opt, content, camera_model_combo_)
                     .width());
    }
    const int cam_h = camera_model_combo_->sizeHint().height();
    const int top_y = view_origin.y() + kMargin;
    // Right edge sits a margin in from the view's right edge, mirroring the
    // fixed-frame combo's left margin.
    const int left_x = view_origin.x() + view_->width() - kMargin - cam_w;
    camera_model_combo_->setGeometry(left_x, top_y, cam_w, cam_h);
    camera_model_combo_->raise();
  }
}

void Scene3DDockWidget::resizeEvent(QResizeEvent* event) {
  SceneDockWidget::resizeEvent(event);
  layoutFrameOverlayCombo();
}

Scene3DDockWidget::OrphanSnapshot Scene3DDockWidget::orphanState(ObjectTopicId topic_id) const {
  auto it = orphan_states_.find(topicKey(topic_id));
  if (it == orphan_states_.end()) {
    return {};
  }
  return it->second;
}

void Scene3DDockWidget::recomputeOrphanStates(bool force) {
  if (tf_buffer_ == nullptr) {
    return;
  }
  const QString fixed = currentFixedFrame();
  // Hot live-ingest path: skip the per-layer frame walk when neither the buffer
  // nor the fixed frame moved since the last full recompute (L.45/L.47).
  if (!force && tf_buffer_->revision() == last_orphan_revision_ && fixed == last_orphan_fixed_frame_) {
    return;
  }
  const std::string fixed_std = fixed.toStdString();

  std::unordered_set<std::string> known_frames;
  for (auto&& frame : tf_buffer_->getAllFrames()) {
    known_frames.insert(std::move(frame));
  }

  for (const SceneLayerInfo& info : layers()) {
    auto* layer = dynamic_cast<Scene3DLayer*>(layerFor(info.topic_id));
    if (layer == nullptr) {
      continue;
    }
    const QString src = layer->sourceFrame();
    bool warn = false;
    QString reason;
    if (src.isEmpty() || fixed.isEmpty()) {
      // Pending data.
    } else if (src == fixed) {
      // Identity transform.
    } else {
      const std::string src_std = src.toStdString();
      if (known_frames.count(src_std) == 0) {
        warn = true;
        reason = tr("Frame '%1' can't be resolved").arg(src);
      } else if (!tf_buffer_->areConnected(fixed_std, src_std)) {
        warn = true;
        reason = tr("Frame '%1' is not connected to fixed frame '%2'").arg(src, fixed);
      }
    }
    // A frame issue blocks rendering entirely, so it wins the row's message when
    // both apply; otherwise surface the layer's own warning (e.g. a model that
    // failed to fetch/import). Combining here keeps a 169 Hz TF stream from
    // clobbering a model-load warning that shares this per-row slot.
    if (const QString self_warn = layer->statusWarning(); !warn && !self_warn.isEmpty()) {
      warn = true;
      reason = self_warn;
    }

    auto& state = orphan_states_[topicKey(info.topic_id)];
    if (state.is_orphan != warn || state.reason != reason) {
      state.is_orphan = warn;
      state.reason = reason;
      emit layerWarningChanged(info.topic_id, warn, reason);
    }
  }

  // Record what this full recompute observed so the force=false fast path can
  // detect "nothing changed" on the next live tick.
  last_orphan_revision_ = tf_buffer_->revision();
  last_orphan_fixed_frame_ = fixed;
}

bool Scene3DDockWidget::isLocalLayerId(ObjectTopicId topic_id) const {
  return local_layer_ids_.find(topic_id.id) != local_layer_ids_.end();
}

ObjectTopicId Scene3DDockWidget::allocateLocalLayerId() {
  while (next_local_layer_topic_id_ > 0) {
    ObjectTopicId topic_id;
    topic_id.id = next_local_layer_topic_id_--;
    if (layerFor(topic_id) == nullptr && local_layer_ids_.insert(topic_id.id).second) {
      return topic_id;
    }
  }
  return {};
}

Scene3DDockWidget::ElementRestoreResult Scene3DDockWidget::restoreLayerElement(
    const QDomElement& layer_el, QString* deferred_topic_name) {
  if (deferred_topic_name != nullptr) {
    deferred_topic_name->clear();
  }
  if (layer_el.isNull() || layer_el.tagName() != "layer"_L1) {
    return ElementRestoreResult::kInvalid;
  }
  if (sessionManager() == nullptr) {
    return ElementRestoreResult::kDeferred;
  }
  // Trails are not object-type layers (object_type is kNone): branch BEFORE the
  // acceptsObjectType gate, which would reject them.
  if (layer_el.attribute(u"role"_s) == "trail"_L1) {
    return restoreTrailElement(layer_el, deferred_topic_name);
  }
  const QString object_type_str = layer_el.attribute(u"object_type"_s);
  const auto object_type_opt = sdk::parseBuiltinObjectType(object_type_str.toStdString());
  if (!object_type_opt.has_value() || !acceptsObjectType(*object_type_opt)) {
    return ElementRestoreResult::kInvalid;
  }
  const QString display_name = layer_el.attribute(u"display_name"_s);
  const QString visible_text = layer_el.attribute(u"visible"_s, u"true"_s);
  if (visible_text != "true"_L1 && visible_text != "false"_L1) {
    return ElementRestoreResult::kInvalid;
  }
  const bool visible = visible_text == "true"_L1;
  bool order_ok = false;
  const int saved_order = layer_el.attribute(u"order"_s).toInt(&order_ok);
  if (!order_ok || saved_order < 0) {
    return ElementRestoreResult::kInvalid;
  }

  ObjectTopicId topic_id;
  const bool local_layer = layer_el.attribute(u"local"_s) == "true"_L1;
  if (local_layer) {
    if (*object_type_opt != sdk::BuiltinObjectType::kRobotDescription) {
      return ElementRestoreResult::kInvalid;
    }
    topic_id = allocateLocalLayerId();
    if (topic_id.id == 0) {
      return ElementRestoreResult::kInvalid;
    }
  } else {
    std::optional<ObjectTopicId> topic_id_opt;
    const ElementRestoreResult resolved =
        resolveSavedTopicRef(layer_el, deferred_topic_name, *object_type_opt, topic_id_opt);
    if (resolved != ElementRestoreResult::kRestored) {
      return resolved;
    }
    topic_id = *topic_id_opt;
  }
  if (layerFor(topic_id) != nullptr) {
    return ElementRestoreResult::kInvalid;
  }
  const QString previous_title = windowTitle();
  if (!addTopicImpl(topic_id, *object_type_opt, display_name, /*enforce_image_gate=*/false)) {
    if (local_layer) {
      local_layer_ids_.erase(topic_id.id);
    }
    return ElementRestoreResult::kInvalid;
  }
  ISceneLayer* layer = layerFor(topic_id);
  if (layer == nullptr) {
    return ElementRestoreResult::kInvalid;
  }
  const QDomElement payload = layer_el.firstChildElement();
  bool payload_invalid = !payload.isNull() && !payload.nextSiblingElement().isNull();
  if (!payload_invalid && !payload.isNull()) {
    if (auto* robot = dynamic_cast<RobotModelLayer*>(layer); robot != nullptr) {
      const RobotModelLayer::XmlLoadResult result = robot->xmlLoadStateResult(payload);
      if (result == RobotModelLayer::XmlLoadResult::kDeferred) {
        if (deferred_topic_name != nullptr) {
          *deferred_topic_name = payload.attribute(u"source_topic_name"_s);
        }
        removeTopic(topic_id);
        setWindowTitle(previous_title);
        return ElementRestoreResult::kDeferred;
      }
      payload_invalid = result == RobotModelLayer::XmlLoadResult::kInvalid;
    } else {
      payload_invalid = !layer->xmlLoadState(payload);
    }
  }
  if (payload_invalid) {
    removeTopic(topic_id);
    setWindowTitle(previous_title);
    return ElementRestoreResult::kInvalid;
  }
  if (!visible) {
    setLayerVisible(topic_id, false);
  }
  restored_layer_orders_[topicKey(topic_id)] = saved_order;
  applyRestoredLayerOrder();
  return ElementRestoreResult::kRestored;
}

Scene3DDockWidget::ElementRestoreResult Scene3DDockWidget::resolveSavedTopicRef(
    const QDomElement& el, QString* deferred_topic_name, sdk::BuiltinObjectType expected_type,
    std::optional<ObjectTopicId>& out_topic) const {
  out_topic.reset();
  const QString topic_name = el.attribute(u"topic_name"_s);
  if (topic_name.isEmpty()) {
    return ElementRestoreResult::kInvalid;
  }
  std::optional<ObjectTopicId> topic_id_opt;
  const bool qualified =
      el.hasAttribute(u"dataset_id"_s) || el.hasAttribute(u"dataset_source"_s) || el.hasAttribute(u"dataset_path"_s);
  if (!qualified) {
    const auto generic =
        pj::scene3d::resolveUniqueObjectTopic(sessionManager()->objectStore(), topic_name.toStdString(), expected_type);
    if (generic.ambiguous) {
      return ElementRestoreResult::kInvalid;
    }
    topic_id_opt = generic.topic_id;
  } else {
    bool dataset_ok = false;
    const qulonglong dataset_value = el.attribute(u"dataset_id"_s).toULongLong(&dataset_ok);
    if (el.hasAttribute(u"dataset_id"_s) &&
        (!dataset_ok || dataset_value == 0 || dataset_value > std::numeric_limits<uint32_t>::max())) {
      return ElementRestoreResult::kInvalid;
    }
    const DatasetId saved_id = el.hasAttribute(u"dataset_id"_s) ? static_cast<DatasetId>(dataset_value) : 0;
    const auto dataset_id_opt =
        resolveObjectDataset(saved_id, el.attribute(u"dataset_source"_s), el.attribute(u"dataset_path"_s), topic_name);
    if (dataset_id_opt.has_value()) {
      topic_id_opt = sessionManager()->objectStore().findTopic(*dataset_id_opt, topic_name.toStdString());
    }
  }
  if (!topic_id_opt.has_value()) {
    if (deferred_topic_name != nullptr) {
      *deferred_topic_name = topic_name;
    }
    return ElementRestoreResult::kDeferred;
  }
  const ObjectTopicDescriptor& descriptor = sessionManager()->objectStore().descriptor(*topic_id_opt);
  const sdk::BuiltinObjectType live_type = pj::scene3d::builtinObjectTypeFor(descriptor);
  if (live_type != sdk::BuiltinObjectType::kNone && live_type != expected_type) {
    return ElementRestoreResult::kInvalid;
  }
  out_topic = topic_id_opt;
  return ElementRestoreResult::kRestored;
}

Scene3DDockWidget::ElementRestoreResult Scene3DDockWidget::restoreTrailElement(
    const QDomElement& layer_el, QString* deferred_topic_name) {
  // A corrupted layout drops the element with a reason in the log — kInvalid
  // itself only surfaces as the aggregate workspaceRestoreFailed() flag.
  const auto invalid = [](const char* reason) {
    qCWarning(lcScene3DDock) << "restoreTrailElement:" << reason;
    return ElementRestoreResult::kInvalid;
  };
  const QString visible_text = layer_el.attribute(u"visible"_s, u"true"_s);
  if (visible_text != "true"_L1 && visible_text != "false"_L1) {
    return invalid("malformed 'visible' attribute");
  }
  bool order_ok = false;
  const int saved_order = layer_el.attribute(u"order"_s).toInt(&order_ok);
  if (!order_ok || saved_order < 0) {
    return invalid("malformed 'order' attribute");
  }
  const QDomElement payload = layer_el.firstChildElement();
  if (payload.isNull() || payload.tagName() != "trail"_L1 || !payload.nextSiblingElement().isNull()) {
    return invalid("expected a single <trail> payload");
  }

  // The layer owns its restore (source resolution included) — the dock only
  // hosts it and honors the tri-state result, exactly like RobotModelLayer.
  const ObjectTopicId layer_id = allocateLocalLayerId();
  if (layer_id.id == 0) {
    return invalid("exhausted local topic ids");
  }
  if (addTrailLayerImpl(layer_id, std::make_unique<pj::scene3d::TrailLayer>(layer_id, this)).id == 0) {
    return invalid("trail layer could not be created");
  }
  auto* trail = dynamic_cast<pj::scene3d::TrailLayer*>(layerFor(layer_id));
  if (trail == nullptr) {
    return invalid("trail layer could not be created");
  }
  const pj::scene3d::TrailLayer::XmlLoadResult result = trail->xmlLoadStateResult(payload);
  if (result == pj::scene3d::TrailLayer::XmlLoadResult::kDeferred) {
    if (deferred_topic_name != nullptr) {
      *deferred_topic_name = payload.attribute(u"source_topic_name"_s);
    }
    removeTopic(layer_id);
    return ElementRestoreResult::kDeferred;
  }
  if (result == pj::scene3d::TrailLayer::XmlLoadResult::kInvalid) {
    removeTopic(layer_id);
    return invalid("malformed <trail> payload");
  }
  if (visible_text == "false"_L1) {
    setLayerVisible(layer_id, false);
  }
  restored_layer_orders_[topicKey(layer_id)] = saved_order;
  applyRestoredLayerOrder();
  return ElementRestoreResult::kRestored;
}

Scene3DDockWidget::ElementRestoreResult Scene3DDockWidget::restoreConfigTopicElement(const QDomElement& config_el) {
  if (config_el.isNull() || config_el.tagName() != "config_topic"_L1 || !config_el.firstChildElement().isNull()) {
    return ElementRestoreResult::kInvalid;
  }
  if (sessionManager() == nullptr) {
    return ElementRestoreResult::kDeferred;
  }
  const QString topic_name = config_el.attribute(u"topic_name"_s);
  if (topic_name.isEmpty()) {
    return ElementRestoreResult::kInvalid;
  }
  const QString object_type_str = config_el.attribute(u"object_type"_s);
  const auto object_type_opt = sdk::parseBuiltinObjectType(object_type_str.toStdString());
  if (!object_type_opt.has_value() || *object_type_opt != sdk::BuiltinObjectType::kFrameTransforms) {
    return ElementRestoreResult::kInvalid;
  }
  std::optional<ObjectTopicId> topic_id_opt;
  const bool qualified = config_el.hasAttribute(u"dataset_id"_s) || config_el.hasAttribute(u"dataset_source"_s) ||
                         config_el.hasAttribute(u"dataset_path"_s);
  if (!qualified) {
    const auto generic = pj::scene3d::resolveUniqueObjectTopic(
        sessionManager()->objectStore(), topic_name.toStdString(), *object_type_opt);
    if (generic.ambiguous) {
      return ElementRestoreResult::kInvalid;
    }
    topic_id_opt = generic.topic_id;
  } else {
    bool dataset_ok = false;
    const qulonglong dataset_value = config_el.attribute(u"dataset_id"_s).toULongLong(&dataset_ok);
    if (config_el.hasAttribute(u"dataset_id"_s) &&
        (!dataset_ok || dataset_value == 0 || dataset_value > std::numeric_limits<uint32_t>::max())) {
      return ElementRestoreResult::kInvalid;
    }
    const DatasetId saved_id = config_el.hasAttribute(u"dataset_id"_s) ? static_cast<DatasetId>(dataset_value) : 0;
    const auto dataset_id_opt = resolveObjectDataset(
        saved_id, config_el.attribute(u"dataset_source"_s), config_el.attribute(u"dataset_path"_s), topic_name);
    if (dataset_id_opt.has_value()) {
      topic_id_opt = sessionManager()->objectStore().findTopic(*dataset_id_opt, topic_name.toStdString());
    }
  }
  if (!topic_id_opt.has_value()) {
    return ElementRestoreResult::kDeferred;
  }
  const sdk::BuiltinObjectType live_type =
      pj::scene3d::builtinObjectTypeFor(sessionManager()->objectStore().descriptor(*topic_id_opt));
  if (live_type != sdk::BuiltinObjectType::kNone && live_type != *object_type_opt) {
    return ElementRestoreResult::kInvalid;
  }
  if (!addTopic(*topic_id_opt, *object_type_opt, topic_name) || !config_topics_.contains(topic_id_opt->id)) {
    return ElementRestoreResult::kInvalid;
  }
  return ElementRestoreResult::kRestored;
}

void Scene3DDockWidget::applyRestoredLayerOrder() {
  std::vector<SceneLayerInfo> infos = layers();
  std::stable_sort(infos.begin(), infos.end(), [this](const SceneLayerInfo& left, const SceneLayerInfo& right) {
    const auto left_rank = restored_layer_orders_.find(topicKey(left.topic_id));
    const auto right_rank = restored_layer_orders_.find(topicKey(right.topic_id));
    const bool left_ranked = left_rank != restored_layer_orders_.end();
    const bool right_ranked = right_rank != restored_layer_orders_.end();
    if (left_ranked != right_ranked) {
      return left_ranked;
    }
    return left_ranked && left_rank->second < right_rank->second;
  });
  std::vector<ObjectTopicId> ordered;
  ordered.reserve(infos.size());
  for (const SceneLayerInfo& info : infos) {
    ordered.push_back(info.topic_id);
  }
  reorderLayers(ordered);
}

bool Scene3DDockWidget::restoreOnePending(const QDomElement& element) {
  // 3D defers two element kinds into the base's shared pending queue: scene-config
  // topics (e.g. TF) and render layers. Dispatch on the tag; the base SceneDockWidget
  // owns the queue + the retry/unresolved/clear bookkeeping.
  const ElementRestoreResult result =
      element.tagName() == "config_topic"_L1 ? restoreConfigTopicElement(element) : restoreLayerElement(element);
  if (result == ElementRestoreResult::kInvalid) {
    markWorkspaceRestoreFailed();
    return true;
  }
  return result == ElementRestoreResult::kRestored || result == ElementRestoreResult::kConsumed;
}

QDomElement Scene3DDockWidget::xmlSaveState(QDomDocument& doc) const {
  QDomElement root = doc.createElement(xmlTag());
  root.setAttribute(u"version"_s, u"1"_s);

  ObjectStore* store = sessionManager() != nullptr ? &sessionManager()->objectStore() : nullptr;
  int saved_order = 0;
  for (const SceneLayerInfo& info : layers()) {
    const bool local_layer = isLocalLayerId(info.topic_id);
    if (!local_layer && store == nullptr) {
      continue;
    }

    QDomElement layer_el = doc.createElement(u"layer"_s);
    if (local_layer) {
      layer_el.setAttribute(u"local"_s, u"true"_s);
      layer_el.setAttribute(u"dataset_id"_s, u"0"_s);
      layer_el.setAttribute(u"topic_name"_s, QString());
    } else {
      const auto& desc = store->descriptor(info.topic_id);
      layer_el.setAttribute(u"dataset_id"_s, QString::number(desc.dataset_id));
      // Source name is stable across sessions; the raw id is a load-order counter
      // (see the injected resolver). Persist both so restore can re-resolve (M.55).
      layer_el.setAttribute(u"dataset_source"_s, datasetSourceName(sessionManager(), desc.dataset_id));
      const QString path = sessionManager()->datasetSourcePath(desc.dataset_id);
      if (!path.isEmpty()) {
        layer_el.setAttribute(u"dataset_path"_s, path);
      }
      layer_el.setAttribute(u"topic_name"_s, QString::fromStdString(desc.topic_name));
    }
    if (dynamic_cast<const pj::scene3d::TrailLayer*>(layerFor(info.topic_id)) != nullptr) {
      // Trails restore through restoreTrailElement, keyed on this role marker
      // (their object_type is kNone, which the normal path would reject). The
      // SOURCE identity lives inside the layer's own <trail> payload — the
      // RobotModelLayer topic-source idiom.
      layer_el.setAttribute(u"role"_s, u"trail"_s);
    }
    const auto object_type_name = sdk::name(info.object_type);
    layer_el.setAttribute(
        u"object_type"_s,
        QString::fromLatin1(object_type_name.data(), static_cast<qsizetype>(object_type_name.size())));
    layer_el.setAttribute(u"display_name"_s, info.display_name);
    layer_el.setAttribute(u"visible"_s, info.visible ? u"true"_s : u"false"_s);
    layer_el.setAttribute(u"order"_s, saved_order++);

    if (ISceneLayer* layer = layerFor(info.topic_id); layer != nullptr) {
      QDomElement payload = layer->xmlSaveState(doc);
      if (!payload.isNull()) {
        layer_el.appendChild(payload);
      }
    }
    root.appendChild(layer_el);
  }

  // FrameTransforms config topics create no layer (handleSceneConfigTopic just
  // binds tf_buffer_/dataset_id_), so the layer loop above leaves no trace of a
  // TF-only / URDF+TF dock's TF source. Persist them separately so restore can
  // re-bind the TF buffer (M.18).
  if (store != nullptr) {
    for (const uint32_t topic_raw : config_topics_) {
      const ObjectTopicId topic_id{topic_raw};
      const ObjectTopicDescriptor& desc = store->descriptor(topic_id);
      if (desc.topic_name.empty()) {
        continue;  // evicted; nothing to restore
      }
      QDomElement config_el = doc.createElement(u"config_topic"_s);
      config_el.setAttribute(u"dataset_id"_s, QString::number(desc.dataset_id));
      config_el.setAttribute(u"dataset_source"_s, datasetSourceName(sessionManager(), desc.dataset_id));
      const QString path = sessionManager()->datasetSourcePath(desc.dataset_id);
      if (!path.isEmpty()) {
        config_el.setAttribute(u"dataset_path"_s, path);
      }
      config_el.setAttribute(u"topic_name"_s, QString::fromStdString(desc.topic_name));
      const auto frame_transforms_name = sdk::name(sdk::BuiltinObjectType::kFrameTransforms);
      config_el.setAttribute(
          u"object_type"_s,
          QString::fromLatin1(frame_transforms_name.data(), static_cast<qsizetype>(frame_transforms_name.size())));
      root.appendChild(config_el);
    }
  }

  root.setAttribute(
      u"fixed_frame_mode"_s, fixed_frame_mode_ == FixedFrameMode::kAutoRoot ? u"auto_root"_s : u"explicit"_s);
  root.setAttribute(u"fixed_frame"_s, currentFixedFrame());
  root.setAttribute(u"follow_frame"_s, currentFollowFrame());
  if (view_ != nullptr && camera_model_combo_ != nullptr) {
    root.setAttribute(u"camera_model"_s, cameraModelToString(static_cast<int>(view_->cameraModel())));
    root.setAttribute(
        u"camera_state"_s, QString::fromStdString(pj::scene3d::cameraStateToJson(view_->camera().state())));
  }
  if (view_ != nullptr) {
    // Per-dock scene-look controls travel WITH the layout (export/import), so a
    // restored layout gives each view its own grid/frame/mesh look instead of a
    // shared global one. The QSettings group is only the seed for brand-new docks.
    // Field set mirrors Scene3DConfigPanel::applySceneControlsTo (keep in sync; 4 sites).
    const auto bool_attr = [](bool value) { return value ? u"true"_s : u"false"_s; };
    QDomElement sc = doc.createElement(u"scene_controls"_s);
    sc.setAttribute(u"grid_visible"_s, bool_attr(view_->gridVisible()));
    sc.setAttribute(u"grid_style"_s, view_->gridStyle() == pj::scene3d::GridRenderPass::Style::kFilledCells ? 1 : 0);
    sc.setAttribute(u"grid_extent_m"_s, view_->gridExtentMetres());
    sc.setAttribute(u"grid_divisions"_s, view_->gridDivisions());
    sc.setAttribute(u"axes_visible"_s, bool_attr(view_->axesVisible()));
    sc.setAttribute(u"gizmo_size_m"_s, view_->gizmoSize());
    sc.setAttribute(u"gizmo_opacity"_s, view_->gizmoOpacity());
    sc.setAttribute(u"tf_parent_lines"_s, bool_attr(view_->tfConnectionsVisible()));
    const auto& shading = view_->meshShadingParams();
    sc.setAttribute(u"meshes_visible"_s, bool_attr(shading.meshes_visible));
    sc.setAttribute(u"mesh_opacity"_s, shading.mesh_opacity);
    sc.setAttribute(u"collisions_visible"_s, bool_attr(shading.collisions_visible));
    sc.setAttribute(u"collision_opacity"_s, shading.collision_opacity);
    // shadows_enabled is intentionally NOT persisted: shadows are always on (see MeshShadingParams).
    root.appendChild(sc);
  }
  appendPendingRestoreElements(doc, root);
  return root;
}

bool Scene3DDockWidget::xmlLoadState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "scene3d"_L1) {
    return false;
  }
  const std::optional<ValidatedSceneState> validated = validateSceneState(element);
  if (!validated.has_value()) {
    markWorkspaceRestoreFailed();
    return false;
  }
  auto restoring_guard = beginWorkspaceRestore();

  // Force the lazily-created view now. PlotDocker calls xmlLoadState
  // synchronously during layout restore, before the deferred singleShot fires;
  // without this view_ is null and the explicit fixed frame, fixed-frame mode,
  // and camera state below are silently dropped when zero layers restore (M.19).
  ensureSceneViewCreated();
  QDomDocument previous_doc;
  QDomElement previous_state;
  const PendingRestoreSnapshot previous_pending = capturePendingRestoreState();
  if (!xml_rollback_in_progress_) {
    previous_state = xmlSaveState(previous_doc);
    previous_doc.appendChild(previous_state);
  }
  resetWorkspaceRestoreStatus();
  clearPendingRestores();
  const QString saved_frame = element.attribute(u"fixed_frame"_s);
  clearLayers();
  local_layer_ids_.clear();
  restored_layer_orders_.clear();
  resetDatasetBindingForRestore();
  int unresolved_topics = 0;
  bool invalid_state = false;
  if (sessionManager() != nullptr) {
    int default_order = 0;
    for (QDomElement layer_el = element.firstChildElement(u"layer"_s); !layer_el.isNull();
         layer_el = layer_el.nextSiblingElement(u"layer"_s)) {
      if (!layer_el.hasAttribute(u"order"_s)) {
        layer_el.setAttribute(u"order"_s, default_order);
      }
      ++default_order;
      QString deferred_topic;
      const ElementRestoreResult result = restoreLayerElement(layer_el, &deferred_topic);
      if (result == ElementRestoreResult::kDeferred) {
        ++unresolved_topics;
        rememberPendingRestore(layer_el, std::move(deferred_topic));
      } else if (result == ElementRestoreResult::kInvalid) {
        invalid_state = true;
        break;
      }
    }

    // Re-add persisted FrameTransforms config topics so handleSceneConfigTopic
    // re-binds tf_buffer_/dataset_id_ (M.18). These create no layer; a TF-only
    // dock has nothing in the layer loop above and depends entirely on this.
    if (!invalid_state) {
      for (QDomElement config_el = element.firstChildElement(u"config_topic"_s); !config_el.isNull();
           config_el = config_el.nextSiblingElement(u"config_topic"_s)) {
        const ElementRestoreResult result = restoreConfigTopicElement(config_el);
        if (result == ElementRestoreResult::kDeferred) {
          ++unresolved_topics;
          rememberPendingRestore(config_el);
        } else if (result == ElementRestoreResult::kInvalid) {
          invalid_state = true;
          break;
        }
      }
    }
  }
  if (invalid_state) {
    markWorkspaceRestoreFailed();
    clearPendingRestores();
    clearLayers();
    local_layer_ids_.clear();
    restored_layer_orders_.clear();
    resetDatasetBindingForRestore();
    setWindowTitle(tr("3D View"));
    if (!previous_state.isNull()) {
      QScopedValueRollback rollback_guard(xml_rollback_in_progress_, true);
      if (!xmlLoadState(previous_state)) {
        qCCritical(lcScene3DDock) << "Failed to roll back rejected Scene3D XML state";
      } else {
        // The snapshot's status epoch comes back with the pending state: after
        // a successful rollback the dock is NOT in a failed-restore state — the
        // caller learns of the rejection from the false return.
        restorePendingRestoreState(previous_pending);
      }
    }
    return false;
  }
  if (unresolved_topics > 0) {
    qCWarning(lcScene3DDock) << unresolved_topics << "saved layer(s) could not be restored (dataset not loaded)";
  }

  applyRestoredFixedFrame(
      validated->explicit_fixed_frame ? FixedFrameMode::kExplicit : FixedFrameMode::kAutoRoot, saved_frame);

  // Restore the camera model + pose (tolerant of older layouts without them).
  // Setting the combo index switches the active controller via its signal; the
  // adoptState then applies the saved pose on top.
  if (view_ != nullptr) {
    if (camera_model_combo_ != nullptr && validated->camera_model_index >= 0) {
      camera_model_combo_->setCurrentIndex(validated->camera_model_index);
    }
    if (validated->camera_state.has_value()) {
      const ValidatedCameraState& saved = *validated->camera_state;
      pj::scene3d::CameraState camera = view_->camera().state();
      if (saved.focal.has_value()) {
        camera.focal = *saved.focal;
      }
      if (saved.radius.has_value()) {
        camera.radius = *saved.radius;
      }
      if (saved.azimuth.has_value()) {
        camera.azimuth = *saved.azimuth;
      }
      if (saved.elevation.has_value()) {
        camera.elevation = *saved.elevation;
      }
      if (saved.fov_y.has_value()) {
        camera.fov_y = *saved.fov_y;
      }
      if (saved.ortho_scale.has_value()) {
        camera.ortho_scale = *saved.ortho_scale;
      }
      if (saved.perspective.has_value()) {
        camera.perspective = *saved.perspective;
      }
      view_->camera().adoptState(camera);
    }
    // Restore the camera follow target (empty = off). Tolerant of older layouts
    // (missing attribute → empty → follow off). A target absent from the current
    // TF tree stays inert until it appears (applyFollow holds on lookup failure).
    setFollowFrame(element.attribute(u"follow_frame"_s));
    // Per-dock scene controls: override the global seed applied at view creation
    // (sceneViewReady -> applySceneControlsTo) with this dock's saved look. Older
    // layouts have no <scene_controls> child -> keep the seed. Each attribute
    // falls back to the current value so a partial element never zeroes a control.
    // Field set mirrors Scene3DConfigPanel::applySceneControlsTo (keep in sync; 4 sites).
    if (validated->controls.has_value()) {
      const ValidatedSceneControls& controls = *validated->controls;
      if (controls.grid_visible.has_value()) {
        view_->setGridVisible(*controls.grid_visible);
      }
      if (controls.grid_style.has_value()) {
        view_->setGridStyle(
            *controls.grid_style == 1 ? pj::scene3d::GridRenderPass::Style::kFilledCells
                                      : pj::scene3d::GridRenderPass::Style::kLines);
      }
      if (controls.grid_extent_m.has_value()) {
        view_->setGridExtentMetres(*controls.grid_extent_m);
      }
      if (controls.grid_divisions.has_value()) {
        view_->setGridDivisions(*controls.grid_divisions);
      }
      if (controls.axes_visible.has_value()) {
        view_->setAxesVisible(*controls.axes_visible);
      }
      if (controls.gizmo_size_m.has_value()) {
        view_->setGizmoSize(*controls.gizmo_size_m);
      }
      if (controls.gizmo_opacity.has_value()) {
        view_->setGizmoOpacity(*controls.gizmo_opacity);
      }
      if (controls.tf_parent_lines.has_value()) {
        view_->setTfConnectionsVisible(*controls.tf_parent_lines);
      }
      auto shading = view_->meshShadingParams();
      if (controls.meshes_visible.has_value()) {
        shading.meshes_visible = *controls.meshes_visible;
      }
      if (controls.mesh_opacity.has_value()) {
        shading.mesh_opacity = *controls.mesh_opacity;
      }
      if (controls.collisions_visible.has_value()) {
        shading.collisions_visible = *controls.collisions_visible;
      }
      if (controls.collision_opacity.has_value()) {
        shading.collision_opacity = *controls.collision_opacity;
      }
      view_->setMeshShadingParams(shading);
    }
    view_->update();
  }

  const auto infos = layers();
  if (infos.empty()) {
    setWindowTitle(tr("3D View"));
  } else {
    const QString& title = infos.back().display_name;
    setWindowTitle(title.isEmpty() ? tr("3D View") : tr("3D View - %1").arg(title));
  }
  recomputeOrphanStates();
  return true;
}

}  // namespace PJ
