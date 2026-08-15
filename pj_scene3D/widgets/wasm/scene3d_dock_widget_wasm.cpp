// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QFontMetrics>
#include <QLabel>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QToolButton>
#include <algorithm>
#include <any>
#include <cmath>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>
#include <numbers>
#include <optional>
#include <string>
#include <utility>

#include "pj_base/builtin/image.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/object_topic_metadata.h"
#include "pj_scene3d_widgets/resolve_object.h"
#include "pj_scene3d_widgets/scene_state_xml.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_scene3d_widgets/transform_service.h"
#include "pj_scene3d_widgets/wasm/depth_cloud_layer_wasm.h"
#include "pj_scene3d_widgets/wasm/occupancy_grid_layer_wasm.h"
#include "pj_scene3d_widgets/wasm/point_cloud_layer_wasm.h"
#include "pj_scene3d_widgets/wasm/poses_in_frame_layer_wasm.h"
#include "pj_scene3d_widgets/wasm/robot_model_layer_wasm.h"
#include "pj_scene3d_widgets/wasm/scene_entities_layer_wasm.h"
#include "pj_scene3d_widgets/wasm/voxel_grid_layer_wasm.h"
#include "pj_widgets/ComboBox.h"
#include "pj_widgets/SvgUtil.h"

using namespace Qt::StringLiterals;

using pj::scene3d::readSceneControls;
using pj::scene3d::readXmlBool;
using pj::scene3d::readXmlFloat;
using pj::scene3d::validateCameraState;
using pj::scene3d::ValidatedCameraState;

namespace PJ {
namespace {

struct CameraModelName {
  pj::scene3d::SceneViewWidget::CameraModel model;
  const char* name;
};

constexpr CameraModelName kCameraModels[] = {
    {pj::scene3d::SceneViewWidget::CameraModel::kOrbit, "orbit"},
    {pj::scene3d::SceneViewWidget::CameraModel::kXyOrbit, "xy_orbit"},
    {pj::scene3d::SceneViewWidget::CameraModel::kFly, "fly"},
    {pj::scene3d::SceneViewWidget::CameraModel::kTopDownOrtho, "top_down_ortho"},
};

QString cameraModelName(pj::scene3d::SceneViewWidget::CameraModel model) {
  for (const CameraModelName& entry : kCameraModels) {
    if (entry.model == model) {
      return QString::fromLatin1(entry.name);
    }
  }
  return u"orbit"_s;
}

int cameraModelIndex(const QString& name) {
  for (const CameraModelName& entry : kCameraModels) {
    if (name == QLatin1String(entry.name)) {
      return static_cast<int>(entry.model);
    }
  }
  return -1;
}

bool containsFrame(const QList<pj::scene3d::FrameRow>& frames, const QString& frame) {
  return std::any_of(
      frames.cbegin(), frames.cend(), [&frame](const auto& row) { return QString::fromStdString(row.name) == frame; });
}

QString pickFixedFrame(const QList<pj::scene3d::FrameRow>& frames) {
  for (const char* preferred : {"map", "world", "odom"}) {
    const QString candidate = QString::fromLatin1(preferred);
    if (containsFrame(frames, candidate)) {
      return candidate;
    }
  }
  for (const auto& row : frames) {
    if (row.depth == 0) {
      return QString::fromStdString(row.name);
    }
  }
  return frames.isEmpty() ? QString{} : QString::fromStdString(frames.front().name);
}

// The shared backend-neutral controls plus the browser mesh/collision knobs
// (same extension shape as the native Scene3DDockWidget TU).
struct ValidatedSceneControls : pj::scene3d::ValidatedSceneControls {
  std::optional<bool> meshes_visible;
  std::optional<float> mesh_opacity;
  std::optional<bool> collisions_visible;
  std::optional<float> collision_opacity;
};

bool isKnownFutureScene3dLayer(sdk::BuiltinObjectType type) {
  switch (type) {
    case sdk::BuiltinObjectType::kPointCloud:
    case sdk::BuiltinObjectType::kCompressedPointCloud:
    case sdk::BuiltinObjectType::kOccupancyGrid:
    case sdk::BuiltinObjectType::kRobotDescription:
    case sdk::BuiltinObjectType::kSceneEntities:
    case sdk::BuiltinObjectType::kPosesInFrame:
    case sdk::BuiltinObjectType::kVoxelGrid:
    case sdk::BuiltinObjectType::kImage:
      return true;
    default:
      return false;
  }
}

bool isAvailablePointCloudLayer(sdk::BuiltinObjectType type) {
  if (type == sdk::BuiltinObjectType::kPointCloud) {
    return true;
  }
#if defined(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
  return type == sdk::BuiltinObjectType::kCompressedPointCloud;
#else
  return false;
#endif
}

bool isAvailableScene3dLayer(sdk::BuiltinObjectType type) {
  return isAvailablePointCloudLayer(type) || type == sdk::BuiltinObjectType::kPosesInFrame ||
         type == sdk::BuiltinObjectType::kOccupancyGrid || type == sdk::BuiltinObjectType::kVoxelGrid ||
         type == sdk::BuiltinObjectType::kImage || type == sdk::BuiltinObjectType::kSceneEntities ||
         type == sdk::BuiltinObjectType::kRobotDescription;
}

bool firstSampleIsDepthEncoded(PJ::SessionManager& session, ObjectTopicId topic_id) {
  const auto first = session.objectStore().at(topic_id, 0);
  if (!first.has_value() || first->payload.bytes.empty()) {
    return false;
  }
  auto object = pj::scene3d::resolveObject(
      session.parserBindingForObjectTopic(topic_id), sdk::BuiltinObjectType::kImage, first->timestamp, first->payload);
  const auto* image = object.has_value() ? std::any_cast<sdk::Image>(&object->object) : nullptr;
  return image != nullptr && pj::scene3d::WasmDepthCloudLayer::isDepthEncoding(image->encoding);
}

}  // namespace

Scene3DDockWidget::Scene3DDockWidget(QWidget* parent) : SceneDockWidget(parent) {
  setWindowTitle(tr("3D View"));

  const auto pointcloud_factory = [this](
                                      ObjectTopicId topic_id, sdk::BuiltinObjectType object_type,
                                      const QString& display_name) -> std::unique_ptr<ISceneLayer> {
    prepareTransformBuffer(topic_id);
    auto layer = std::make_unique<pj::scene3d::WasmPointCloudLayer>(topic_id, display_name, object_type, this);
    connect(layer.get(), &pj::scene3d::WasmPointCloudLayer::sourceFrameChanged, this, [this](const QString&) {
      // A sample may reveal/change its frame while QRhi is rendering.
      // Defer the fallback-frame rebuild so setPointRenderableLayers() never
      // replaces the view's pointer vector during its own iteration.
      QMetaObject::invokeMethod(this, [this]() { reconcileViewLayers(); }, Qt::QueuedConnection);
    });
    return layer;
  };
  layerFactory().registerType(sdk::BuiltinObjectType::kPointCloud, pointcloud_factory);
#if defined(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
  layerFactory().registerType(sdk::BuiltinObjectType::kCompressedPointCloud, pointcloud_factory);
#endif
  layerFactory().registerType(
      sdk::BuiltinObjectType::kImage,
      [this](ObjectTopicId topic_id, sdk::BuiltinObjectType /*object_type*/, const QString& display_name)
          -> std::unique_ptr<ISceneLayer> {
        prepareTransformBuffer(topic_id);
        auto layer = std::make_unique<pj::scene3d::WasmDepthCloudLayer>(topic_id, display_name, this);
        connect(layer.get(), &pj::scene3d::WasmDepthCloudLayer::sourceFrameChanged, this, [this](const QString&) {
          QMetaObject::invokeMethod(this, [this]() { reconcileViewLayers(); }, Qt::QueuedConnection);
        });
        return layer;
      });
  layerFactory().registerType(
      sdk::BuiltinObjectType::kPosesInFrame,
      [this](ObjectTopicId topic_id, sdk::BuiltinObjectType /*object_type*/, const QString& display_name)
          -> std::unique_ptr<ISceneLayer> {
        prepareTransformBuffer(topic_id);
        auto layer = std::make_unique<pj::scene3d::WasmPosesInFrameLayer>(topic_id, display_name, this);
        connect(layer.get(), &pj::scene3d::WasmPosesInFrameLayer::sourceFrameChanged, this, [this](const QString&) {
          QMetaObject::invokeMethod(this, [this]() { reconcileViewLayers(); }, Qt::QueuedConnection);
        });
        return layer;
      });
  layerFactory().registerType(
      sdk::BuiltinObjectType::kOccupancyGrid,
      [this](ObjectTopicId topic_id, sdk::BuiltinObjectType /*object_type*/, const QString& display_name)
          -> std::unique_ptr<ISceneLayer> {
        prepareTransformBuffer(topic_id);
        auto layer = std::make_unique<pj::scene3d::WasmOccupancyGridLayer>(topic_id, display_name, this);
        connect(layer.get(), &pj::scene3d::WasmOccupancyGridLayer::sourceFrameChanged, this, [this](const QString&) {
          QMetaObject::invokeMethod(this, [this]() { reconcileViewLayers(); }, Qt::QueuedConnection);
        });
        return layer;
      });
  layerFactory().registerType(
      sdk::BuiltinObjectType::kVoxelGrid,
      [this](ObjectTopicId topic_id, sdk::BuiltinObjectType /*object_type*/, const QString& display_name)
          -> std::unique_ptr<ISceneLayer> {
        prepareTransformBuffer(topic_id);
        auto layer = std::make_unique<pj::scene3d::WasmVoxelGridLayer>(topic_id, display_name, this);
        connect(layer.get(), &pj::scene3d::WasmVoxelGridLayer::sourceFrameChanged, this, [this](const QString&) {
          QMetaObject::invokeMethod(this, [this]() { reconcileViewLayers(); }, Qt::QueuedConnection);
        });
        return layer;
      });
  layerFactory().registerType(
      sdk::BuiltinObjectType::kSceneEntities,
      [this](ObjectTopicId topic_id, sdk::BuiltinObjectType /*object_type*/, const QString& display_name)
          -> std::unique_ptr<ISceneLayer> {
        prepareTransformBuffer(topic_id);
        auto layer = std::make_unique<pj::scene3d::WasmSceneEntitiesLayer>(topic_id, display_name, this);
        connect(layer.get(), &pj::scene3d::WasmSceneEntitiesLayer::sourceFrameChanged, this, [this](const QString&) {
          QMetaObject::invokeMethod(this, [this]() { reconcileViewLayers(); }, Qt::QueuedConnection);
        });
        return layer;
      });
  layerFactory().registerType(
      sdk::BuiltinObjectType::kRobotDescription,
      [this](ObjectTopicId topic_id, sdk::BuiltinObjectType /*object_type*/, const QString& display_name)
          -> std::unique_ptr<ISceneLayer> {
        const bool local = isLocalRobotLayerId(topic_id);
        if (local) {
          ensureLocalTransformBuffer();
        } else {
          prepareTransformBuffer(topic_id);
        }
        auto layer = std::make_unique<pj::scene3d::WasmRobotModelLayer>(topic_id, display_name, this);
        layer->setEmbeddedAssets(embedded_assets_);
        layer->setTransformBuffer(tf_buffer_);
        // A synthetic id has no parser binding. Put the layer in its browser-file
        // state before attach(); addRobotModelLayerFromContent/FromUrl supplies
        // the actual source immediately after registration.
        if (local) {
          layer->setSourceFileContent({}, {});
        }
        connect(layer.get(), &pj::scene3d::WasmRobotModelLayer::sourceFrameChanged, this, [this](const QString&) {
          QMetaObject::invokeMethod(this, [this]() { reconcileViewLayers(); }, Qt::QueuedConnection);
        });
        return layer;
      });

  frame_overlay_combo_ = new ComboBox(this);
  frame_overlay_combo_->setObjectName(u"scene3dFixedFrameCombo"_s);
  frame_overlay_combo_->setFocusPolicy(Qt::ClickFocus);
  frame_overlay_combo_->setToolTip(tr("Fixed frame"));
  connect(frame_overlay_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
    const QString frame = frame_overlay_combo_->currentData().toString();
    if (!frame.isEmpty()) {
      setFixedFrame(frame);
    }
  });

  camera_model_combo_ = new ComboBox(this);
  camera_model_combo_->setObjectName(u"scene3dCameraModelCombo"_s);
  camera_model_combo_->setFocusPolicy(Qt::ClickFocus);
  camera_model_combo_->addItems({tr("Orbit"), tr("XY Orbit"), tr("Fly"), tr("Top Down")});
  connect(camera_model_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
    if (view_ != nullptr && index >= 0 && index < static_cast<int>(std::size(kCameraModels))) {
      view_->setCameraModel(static_cast<pj::scene3d::SceneViewWidget::CameraModel>(index));
    }
  });

  home_button_ = new QToolButton(this);
  home_button_->setObjectName(u"cameraHomeButton"_s);
  home_button_->setToolTip(tr("Reset view to default"));
  home_button_->setIcon(PJ::loadSvg(u":/resources/svg/recenter.svg"_s));
  connect(home_button_, &QToolButton::clicked, this, [this]() {
    if (view_ != nullptr) {
      view_->resetCamera();
    }
  });

  rendering_warning_label_ = new QLabel(this);
  rendering_warning_label_->setObjectName(u"scene3dRenderingWarning"_s);
  rendering_warning_label_->setWordWrap(true);
  rendering_warning_label_->setAlignment(Qt::AlignCenter);
  rendering_warning_label_->setAttribute(Qt::WA_TransparentForMouseEvents);
  rendering_warning_label_->setStyleSheet(
      u"QLabel { color: #fff4d6; background: rgba(116, 70, 0, 220); border-radius: 4px; padding: 6px; }"_s);
  rendering_warning_label_->hide();
  connect(this, &SceneDockWidget::layerRemoved, this, [this](ObjectTopicId topic_id) {
    local_robot_layer_ids_.erase(topic_id.id);
    scene_topic_datasets_.erase(topic_id.id);
    resetTransformBindingIfDatasetGone();
  });
}

Scene3DDockWidget::~Scene3DDockWidget() {
  clearLayers();
}

void Scene3DDockWidget::setSessionManager(SessionManager* session) {
  SceneDockWidget::setSessionManager(session);
  reconnectLiveSamples(session);
}

void Scene3DDockWidget::setTransformService(pj::scene3d::TransformService* service) {
  if (transforms_ready_connection_) {
    disconnect(transforms_ready_connection_);
    transforms_ready_connection_ = {};
  }
  transform_service_ = service;
  if (transform_service_ != nullptr) {
    transforms_ready_connection_ = connect(
        transform_service_, &pj::scene3d::TransformService::datasetTransformsReady, this,
        &Scene3DDockWidget::onDatasetTransformsReady);
    // The application normally injects this service before a drop. Also make a
    // restored/focused dock robust to late injection by rebuilding the binding
    // from the TF config topic it already consumed.
    if ((tf_buffer_ == nullptr || tf_buffer_is_local_) && dataset_id_ != 0) {
      tf_buffer_ = transform_service_->transformBuffer(dataset_id_);
      tf_buffer_is_local_ = false;
    } else if (tf_buffer_ == nullptr && sessionManager() != nullptr) {
      const uint32_t raw_topic = !config_topics_.empty()  ? *config_topics_.cbegin()
                                 : !layer_topics_.empty() ? *layer_topics_.cbegin()
                                                          : 0;
      if (raw_topic != 0) {
        prepareTransformBuffer(ObjectTopicId{raw_topic});
      }
    }
  }
  if (view_ != nullptr) {
    view_->setTransformBuffer(tf_buffer_);
  }
  for (const SceneLayerInfo& info : layers()) {
    if (auto* robot = dynamic_cast<pj::scene3d::WasmRobotModelLayer*>(layerFor(info.topic_id))) {
      robot->setTransformBuffer(tf_buffer_);
    }
  }
}

bool Scene3DDockWidget::handlesObjectType(sdk::BuiltinObjectType object_type) {
  return object_type == sdk::BuiltinObjectType::kFrameTransforms || isAvailableScene3dLayer(object_type);
}

bool Scene3DDockWidget::addTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
  if (!handlesObjectType(object_type)) {
    return false;
  }
  const bool local_robot = object_type == sdk::BuiltinObjectType::kRobotDescription && isLocalRobotLayerId(topic_id);
  if (local_robot) {
    return SceneDockWidget::addTopic(topic_id, object_type, title);
  }
  if (sessionManager() == nullptr) {
    return false;
  }
  if (object_type == sdk::BuiltinObjectType::kFrameTransforms && config_topics_.contains(topic_id.id)) {
    return true;
  }
  const ObjectTopicDescriptor& descriptor = sessionManager()->objectStore().descriptor(topic_id);
  if (descriptor.topic_name.empty() ||
      (dataset_id_ != 0 && descriptor.dataset_id != 0 && descriptor.dataset_id != dataset_id_)) {
    return false;
  }
  // kImage also carries ordinary color images. Interactive drops must prove a
  // supported depth encoding; base restore calls addLayer() directly and thus
  // deliberately bypasses this first-sample gate for saved, not-yet-loaded data.
  if (object_type == sdk::BuiltinObjectType::kImage && !firstSampleIsDepthEncoded(*sessionManager(), topic_id)) {
    return false;
  }
  return SceneDockWidget::addTopic(topic_id, object_type, title);
}

ObjectTopicId Scene3DDockWidget::addRobotModelLayerFromContent(QString filename, QByteArray bytes) {
  const ObjectTopicId topic_id = allocateLocalRobotLayerId();
  if (topic_id.id == 0) {
    return {};
  }
  const QString title = filename.isEmpty() ? tr("Robot model") : filename;
  if (!addTopic(topic_id, sdk::BuiltinObjectType::kRobotDescription, title)) {
    local_robot_layer_ids_.erase(topic_id.id);
    return {};
  }
  auto* layer = dynamic_cast<pj::scene3d::WasmRobotModelLayer*>(layerFor(topic_id));
  if (layer == nullptr) {
    removeTopic(topic_id);
    return {};
  }
  layer->setSourceFileContent(std::move(filename), std::move(bytes));
  return topic_id;
}

ObjectTopicId Scene3DDockWidget::addRobotModelLayerFromUrl(const QString& url) {
  const ObjectTopicId topic_id = allocateLocalRobotLayerId();
  if (topic_id.id == 0) {
    return {};
  }
  const QString file_name = QUrl(url).fileName();
  const QString title = file_name.isEmpty() ? url : file_name;
  if (!addTopic(topic_id, sdk::BuiltinObjectType::kRobotDescription, title)) {
    local_robot_layer_ids_.erase(topic_id.id);
    return {};
  }
  auto* layer = dynamic_cast<pj::scene3d::WasmRobotModelLayer*>(layerFor(topic_id));
  if (layer == nullptr) {
    removeTopic(topic_id);
    return {};
  }
  layer->setSourceUrl(url);
  return topic_id;
}

QList<Scene3DDockWidget::RobotDescriptionTopic> Scene3DDockWidget::robotDescriptionTopics() const {
  QList<RobotDescriptionTopic> result;
  if (sessionManager() == nullptr) {
    return result;
  }
  ObjectStore& store = sessionManager()->objectStore();
  for (const ObjectTopicId topic_id : store.listTopics()) {
    const ObjectTopicDescriptor& descriptor = store.descriptor(topic_id);
    if (pj::scene3d::builtinObjectTypeFor(descriptor) == sdk::BuiltinObjectType::kRobotDescription) {
      result.append({topic_id, QString::fromStdString(descriptor.topic_name)});
    }
  }
  return result;
}

bool Scene3DDockWidget::tryAcceptObjectTopic(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
  return addTopic(topic_id, object_type, title);
}

void Scene3DDockWidget::onTrackerTime(double time) {
  if (std::isfinite(time)) {
    last_tracker_display_ = time;
  }
  SceneDockWidget::onTrackerTime(time);
}

QWidget* Scene3DDockWidget::createSceneView() {
  view_ = new pj::scene3d::SceneViewWidget();
  view_->setContentsMargins(0, 0, 0, 0);
  view_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  if (camera_model_combo_->currentIndex() >= 0) {
    view_->setCameraModel(static_cast<pj::scene3d::SceneViewWidget::CameraModel>(camera_model_combo_->currentIndex()));
  }
  connect(view_, &pj::scene3d::SceneViewWidget::framesChanged, this, &Scene3DDockWidget::onAvailableFrames);
  connect(view_, &pj::scene3d::SceneViewWidget::presentationChanged, this, &Scene3DDockWidget::notifyWorkspaceChanged);
  connect(view_, &pj::scene3d::SceneViewWidget::renderingWarningChanged, this, [this](const QString& warning) {
    if (rendering_warning_label_ == nullptr) {
      return;
    }
    rendering_warning_label_->setText(warning);
    rendering_warning_label_->setVisible(!warning.isEmpty());
    layoutOverlayControls();
  });
  view_->setTransformBuffer(tf_buffer_);
  refreshFrameOverlayCombo();
  emit sceneViewReady();
  return view_;
}

std::unique_ptr<SceneLayerContext> Scene3DDockWidget::makeContext() {
  auto context = std::make_unique<SceneLayerContext>();
  context->session = sessionManager();
  return context;
}

bool Scene3DDockWidget::acceptsObjectType(sdk::BuiltinObjectType object_type) const {
  return isAvailableScene3dLayer(object_type);
}

bool Scene3DDockWidget::acceptsDeferredObjectType(sdk::BuiltinObjectType object_type) const {
  return handlesObjectType(object_type) || isKnownFutureScene3dLayer(object_type);
}

SceneDockWidget::DeferredElementKind Scene3DDockWidget::deferredElementKind(sdk::BuiltinObjectType object_type) const {
  return object_type == sdk::BuiltinObjectType::kFrameTransforms ? DeferredElementKind::kConfigTopic
                                                                 : DeferredElementKind::kRenderLayer;
}

bool Scene3DDockWidget::handleSceneConfigTopic(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
  if (object_type != sdk::BuiltinObjectType::kFrameTransforms || sessionManager() == nullptr) {
    return false;
  }
  const ObjectTopicDescriptor& descriptor = sessionManager()->objectStore().descriptor(topic_id);
  if (descriptor.topic_name.empty() ||
      (dataset_id_ != 0 && descriptor.dataset_id != 0 && descriptor.dataset_id != dataset_id_)) {
    return false;
  }
  prepareTransformBuffer(topic_id);
  config_topics_.insert(topic_id.id);
  scene_topic_datasets_[topic_id.id] = descriptor.dataset_id;
  setWindowTitle(title.isEmpty() ? tr("3D View") : tr("3D View - %1").arg(title));
  return true;
}

void Scene3DDockWidget::syncViewLayers(const std::vector<ISceneLayer*>& ordered_layers) {
  for (const uint32_t topic : layer_topics_) {
    scene_topic_datasets_.erase(topic);
  }
  layer_topics_.clear();
  fallback_frames_.clear();

  for (ISceneLayer* layer : ordered_layers) {
    layer->setFixedFrame(currentFixedFrame());
    auto* point = dynamic_cast<pj::scene3d::WasmPointRenderable*>(layer);
    auto* pose = dynamic_cast<pj::scene3d::WasmPosesInFrameLayer*>(layer);
    auto* grid = dynamic_cast<pj::scene3d::WasmOccupancyGridLayer*>(layer);
    auto* voxel = dynamic_cast<pj::scene3d::WasmVoxelGridLayer*>(layer);
    auto* markers = dynamic_cast<pj::scene3d::WasmSceneEntitiesLayer*>(layer);
    auto* robot = dynamic_cast<pj::scene3d::WasmRobotModelLayer*>(layer);
    if (point == nullptr && pose == nullptr && grid == nullptr && voxel == nullptr && markers == nullptr &&
        robot == nullptr) {
      continue;
    }
    if (robot != nullptr) {
      robot->setEmbeddedAssets(embedded_assets_);
      robot->setTransformBuffer(tf_buffer_);
    }
    const std::string& source_frame = point != nullptr     ? point->sourceFrame()
                                      : pose != nullptr    ? pose->sourceFrame()
                                      : grid != nullptr    ? grid->sourceFrame()
                                      : voxel != nullptr   ? voxel->sourceFrame()
                                      : markers != nullptr ? markers->sourceFrame()
                                                           : robot->sourceFrame();
    const ObjectTopicId topic_id = layer->info().topic_id;
    if (!isLocalRobotLayerId(topic_id)) {
      layer_topics_.insert(topic_id.id);
    }
    if (!isLocalRobotLayerId(topic_id) && sessionManager() != nullptr) {
      const DatasetId dataset = sessionManager()->objectStore().descriptor(topic_id).dataset_id;
      scene_topic_datasets_[topic_id.id] = dataset;
      if (dataset_id_ == 0) {
        dataset_id_ = dataset;
      }
    }
    if (!source_frame.empty() &&
        std::find(fallback_frames_.cbegin(), fallback_frames_.cend(), source_frame) == fallback_frames_.cend()) {
      fallback_frames_.push_back(source_frame);
    }
  }
  if (view_ != nullptr) {
    view_->setLayers(ordered_layers);
  }
  resetTransformBindingIfDatasetGone();
  onAvailableFrames(transform_frames_);
}

void Scene3DDockWidget::refreshView() {
  if (view_ == nullptr) {
    return;
  }
  if (const auto time = lastTrackerNs(); time.has_value()) {
    view_->setTrackerTime(PJ::fromRaw(*time));
  }
  view_->update();
}

uint64_t Scene3DDockWidget::viewRenderKey(PJ::Timepoint time) const {
  return view_ == nullptr ? 0 : view_->tfRenderKey(time) ^ view_->followRenderKey(time);
}

QString Scene3DDockWidget::xmlTag() const {
  return u"scene3d"_s;
}

bool Scene3DDockWidget::acceptsStateChildTag(const QString& tag) const {
  return tag == "config_topic"_L1 || tag == "scene_controls"_L1 || tag == "layer"_L1;
}

DatasetId Scene3DDockWidget::representativeDatasetId() const {
  return dataset_id_ != 0 ? dataset_id_ : SceneDockWidget::representativeDatasetId();
}

void Scene3DDockWidget::prepareTransformBuffer(ObjectTopicId topic_id) {
  if (sessionManager() == nullptr) {
    return;
  }
  const DatasetId incoming = sessionManager()->objectStore().descriptor(topic_id).dataset_id;
  scene_topic_datasets_[topic_id.id] = incoming;
  if (dataset_id_ == 0) {
    dataset_id_ = incoming;
  }
  if (transform_service_ == nullptr) {
    return;
  }
  if (tf_buffer_ != nullptr && !tf_buffer_is_local_) {
    return;
  }
  tf_buffer_ = transform_service_->transformBuffer(dataset_id_);
  tf_buffer_is_local_ = false;
  if (view_ != nullptr) {
    view_->setTransformBuffer(tf_buffer_);
  }
}

void Scene3DDockWidget::resetTransformBindingIfDatasetGone() {
  // A local-only robot intentionally has no dataset id. Its synthetic buffer is
  // the capability-local frame context, not a stale dataset binding; keep it
  // stable across layer reconciliation and object-catalog revalidation.
  if (dataset_id_ == 0) {
    return;
  }
  const bool still_present = std::any_of(
      scene_topic_datasets_.cbegin(), scene_topic_datasets_.cend(),
      [this](const auto& item) { return item.second == dataset_id_; });
  if (!still_present) {
    resetTransformBinding();
  }
}

void Scene3DDockWidget::resetTransformBinding() {
  const bool had_frames = !available_frames_.isEmpty();
  tf_buffer_.reset();
  tf_buffer_is_local_ = false;
  dataset_id_ = 0;
  transform_frames_.clear();
  available_frames_.clear();
  if (view_ != nullptr) {
    view_->setTransformBuffer(nullptr);
    view_->setFixedFrame({});
  }
  if (had_frames) {
    emit availableFramesChanged({});
  }
  refreshFrameOverlayCombo();
  const bool has_local_robot = std::any_of(
      local_robot_layer_ids_.cbegin(), local_robot_layer_ids_.cend(),
      [this](uint32_t raw) { return layerFor(ObjectTopicId{raw}) != nullptr; });
  if (has_local_robot) {
    ensureLocalTransformBuffer();
  }
}

void Scene3DDockWidget::ensureLocalTransformBuffer() {
  if (tf_buffer_ != nullptr) {
    return;
  }
  tf_buffer_ = std::make_shared<pj::scene3d::TransformBuffer>();
  tf_buffer_is_local_ = true;
  if (view_ != nullptr) {
    view_->setTransformBuffer(tf_buffer_);
  }
  // resetTransformBinding() can replace an evicted dataset TF buffer while a
  // local robot remains. Keep the layer and view on the same new buffer so its
  // fixed-joint bridges are injected into the tree that actually renders it.
  for (const SceneLayerInfo& info : layers()) {
    if (isLocalRobotLayerId(info.topic_id)) {
      if (auto* robot = dynamic_cast<pj::scene3d::WasmRobotModelLayer*>(layerFor(info.topic_id))) {
        robot->setTransformBuffer(tf_buffer_);
      }
    }
  }
}

bool Scene3DDockWidget::isLocalRobotLayerId(ObjectTopicId topic_id) const {
  return local_robot_layer_ids_.contains(topic_id.id);
}

ObjectTopicId Scene3DDockWidget::allocateLocalRobotLayerId() {
  while (next_local_robot_topic_id_ > 0) {
    // Local ids descend from UINT32_MAX while real ObjectStore ids ascend from
    // low values; the ranges share one namespace, so every store access must gate
    // on isLocalRobotLayerId() first (all current call sites do).
    const ObjectTopicId topic_id{next_local_robot_topic_id_--};
    if (layerFor(topic_id) == nullptr && local_robot_layer_ids_.insert(topic_id.id).second) {
      return topic_id;
    }
  }
  return {};
}

bool Scene3DDockWidget::pruneEvictedObjects() {
  if (sessionManager() == nullptr) {
    return !layers().empty() || !config_topics_.empty();
  }
  ObjectStore& store = sessionManager()->objectStore();
  std::vector<ObjectTopicId> dead;
  for (const SceneLayerInfo& info : layers()) {
    if (!isLocalRobotLayerId(info.topic_id) && store.descriptor(info.topic_id).topic_name.empty()) {
      dead.push_back(info.topic_id);
    }
  }
  for (const ObjectTopicId topic_id : dead) {
    removeTopic(topic_id);
  }
  for (auto iterator = config_topics_.begin(); iterator != config_topics_.end();) {
    if (store.descriptor(ObjectTopicId{*iterator}).topic_name.empty()) {
      scene_topic_datasets_.erase(*iterator);
      iterator = config_topics_.erase(iterator);
    } else {
      ++iterator;
    }
  }
  resetTransformBindingIfDatasetGone();
  return !layers().empty() || !config_topics_.empty();
}

void Scene3DDockWidget::reconnectLiveSamples(SessionManager* session) {
  if (live_samples_connection_) {
    disconnect(live_samples_connection_);
    live_samples_connection_ = {};
  }
  if (session == nullptr) {
    return;
  }
  live_samples_connection_ =
      connect(session, &SessionManager::samplesIngested, this, [this](const QVector<TopicId>&, bool live) {
        if (!live || transform_service_ == nullptr || tf_buffer_ == nullptr || tf_buffer_is_local_ ||
            dataset_id_ == 0 || sessionManager() == nullptr || view_ == nullptr) {
          return;
        }
        transform_service_->ingestNewTransforms(dataset_id_);
        view_->refreshAvailableFrames();
        int64_t newest = std::numeric_limits<int64_t>::lowest();
        for (const uint32_t raw_topic : config_topics_) {
          const ObjectTopicId topic{raw_topic};
          if (sessionManager()->objectStore().entryCount(topic) != 0) {
            newest = std::max(newest, sessionManager()->objectStore().timeRange(topic).second);
          }
        }
        if (newest != std::numeric_limits<int64_t>::lowest()) {
          noteTrackerTime(newest);
          view_->setTrackerTime(PJ::fromRaw(newest));
          view_->update();
          invalidateTrackerRenderKey();
        }
      });
}

void Scene3DDockWidget::onDatasetTransformsReady(DatasetId dataset_id) {
  if (transform_service_ == nullptr || tf_buffer_ == nullptr || dataset_id != dataset_id_ || view_ == nullptr) {
    return;
  }
  const std::shared_ptr<pj::scene3d::TransformBuffer> ready_buffer = transform_service_->transformBuffer(dataset_id);
  if (tf_buffer_ != ready_buffer) {
    tf_buffer_ = ready_buffer;
    tf_buffer_is_local_ = false;
    view_->setTransformBuffer(tf_buffer_);
    for (const SceneLayerInfo& info : layers()) {
      if (auto* robot = dynamic_cast<pj::scene3d::WasmRobotModelLayer*>(layerFor(info.topic_id))) {
        robot->setTransformBuffer(tf_buffer_);
      }
    }
  }
  view_->refreshAvailableFrames();
  onTrackerTime(last_tracker_display_);
  view_->update();
}

void Scene3DDockWidget::onAvailableFrames(const QList<pj::scene3d::FrameRow>& frames) {
  transform_frames_ = frames;
  QList<pj::scene3d::FrameRow> effective = frames;
  for (const std::string& fallback : fallback_frames_) {
    if (!containsFrame(effective, QString::fromStdString(fallback))) {
      effective.append(pj::scene3d::FrameRow{fallback, 0});
    }
  }
  if (effective == available_frames_) {
    return;
  }
  available_frames_ = effective;
  emit availableFramesChanged(effective);
  refreshFrameOverlayCombo();
  if (!effective.isEmpty() && (fixed_frame_mode_ == FixedFrameMode::kAutoRoot || currentFixedFrame().isEmpty())) {
    applyResolvedFixedFrame(resolveAutoFixedFrame(effective));
  }
}

bool Scene3DDockWidget::layerVisible(ObjectTopicId topic_id) const {
  const ISceneLayer* layer = layerFor(topic_id);
  return layer != nullptr && layer->info().visible;
}

QString Scene3DDockWidget::resolveAutoFixedFrame(const QList<pj::scene3d::FrameRow>& frames) const {
  if (transform_service_ != nullptr && dataset_id_ != 0) {
    const QString remembered = transform_service_->rememberedFixedFrame(dataset_id_);
    if (!remembered.isEmpty() && containsFrame(frames, remembered)) {
      return remembered;
    }
  }
  return pickFixedFrame(frames);
}

QString Scene3DDockWidget::currentFixedFrame() const {
  return view_ == nullptr ? QString{} : QString::fromStdString(view_->fixedFrame());
}

QString Scene3DDockWidget::currentFollowFrame() const {
  return view_ == nullptr ? QString{} : QString::fromStdString(view_->followFrame());
}

void Scene3DDockWidget::setFixedFrame(const QString& frame) {
  if (view_ == nullptr || frame.isEmpty()) {
    return;
  }
  const bool changed = currentFixedFrame() != frame || fixed_frame_mode_ != FixedFrameMode::kExplicit;
  fixed_frame_mode_ = FixedFrameMode::kExplicit;
  applyResolvedFixedFrame(frame);
  if (transform_service_ != nullptr && dataset_id_ != 0) {
    transform_service_->rememberFixedFrame(dataset_id_, frame);
  }
  if (changed) {
    emit fixedFrameModeChanged(false);
    notifyWorkspaceChanged();
  }
}

void Scene3DDockWidget::setFixedFrameAutoRoot() {
  const bool changed = fixed_frame_mode_ != FixedFrameMode::kAutoRoot;
  fixed_frame_mode_ = FixedFrameMode::kAutoRoot;
  applyResolvedFixedFrame(resolveAutoFixedFrame(available_frames_));
  if (changed) {
    emit fixedFrameModeChanged(true);
    notifyWorkspaceChanged();
  }
}

void Scene3DDockWidget::applyResolvedFixedFrame(const QString& frame) {
  if (view_ == nullptr || frame.isEmpty() || currentFixedFrame() == frame) {
    return;
  }
  view_->setFixedFrame(frame.toStdString());
  // DepthCloud uses the current fixed frame as its render frame when the image
  // carries no frame_id. Reconcile after the view owns the new value; other
  // browser layers inherit ISceneLayer's no-op setter.
  reconcileViewLayers();
  emit currentFixedFrameChanged(frame);
  refreshFrameOverlayCombo();
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

void Scene3DDockWidget::refreshFrameOverlayCombo() {
  if (frame_overlay_combo_ == nullptr) {
    return;
  }
  const QSignalBlocker blocker(frame_overlay_combo_);
  frame_overlay_combo_->clear();
  for (const auto& row : available_frames_) {
    const QString name = QString::fromStdString(row.name);
    frame_overlay_combo_->addItem(QString(row.depth * 2, QLatin1Char(' ')) + name, name);
  }
  const int selected = frame_overlay_combo_->findData(currentFixedFrame());
  if (selected >= 0) {
    frame_overlay_combo_->setCurrentIndex(selected);
  }
  layoutOverlayControls();
}

void Scene3DDockWidget::layoutOverlayControls() {
  if (view_ == nullptr || frame_overlay_combo_ == nullptr) {
    return;
  }
  constexpr int margin = 8;
  constexpr int gap = 6;
  const QPoint origin = view_->pos();
  const int height = frame_overlay_combo_->sizeHint().height();
  const int frame_width = std::clamp(frame_overlay_combo_->sizeHint().width(), 80, std::max(80, view_->width() / 3));
  frame_overlay_combo_->setGeometry(origin.x() + margin, origin.y() + margin, frame_width, height);
  home_button_->setGeometry(origin.x() + margin + frame_width + gap, origin.y() + margin, height, height);
  home_button_->setIconSize(QSize(std::max(1, height - 2), std::max(1, height - 2)));
  const int camera_width = std::clamp(camera_model_combo_->sizeHint().width(), 90, std::max(90, view_->width() / 3));
  camera_model_combo_->setGeometry(
      origin.x() + view_->width() - margin - camera_width, origin.y() + margin, camera_width, height);
  if (rendering_warning_label_ != nullptr && rendering_warning_label_->isVisible()) {
    const int warning_width = std::max(120, view_->width() - 2 * margin);
    rendering_warning_label_->setFixedWidth(warning_width);
    const int warning_height = rendering_warning_label_->heightForWidth(warning_width);
    rendering_warning_label_->setGeometry(
        origin.x() + margin, origin.y() + view_->height() - margin - warning_height, warning_width, warning_height);
    rendering_warning_label_->raise();
  }
  frame_overlay_combo_->raise();
  home_button_->raise();
  camera_model_combo_->raise();
}

void Scene3DDockWidget::resizeEvent(QResizeEvent* event) {
  SceneDockWidget::resizeEvent(event);
  layoutOverlayControls();
}

Scene3DDockWidget::RestoreResult Scene3DDockWidget::restoreConfigTopic(const QDomElement& element) {
  if (element.tagName() != "config_topic"_L1 || sessionManager() == nullptr) {
    return sessionManager() == nullptr ? RestoreResult::kDeferred : RestoreResult::kInvalid;
  }
  const QString topic_name = element.attribute(u"topic_name"_s);
  const auto object_type = sdk::parseBuiltinObjectType(element.attribute(u"object_type"_s).toStdString());
  if (topic_name.isEmpty() || object_type != sdk::BuiltinObjectType::kFrameTransforms) {
    return RestoreResult::kInvalid;
  }

  bool dataset_ok = false;
  const qulonglong raw_dataset = element.attribute(u"dataset_id"_s).toULongLong(&dataset_ok);
  if (element.hasAttribute(u"dataset_id"_s) &&
      (!dataset_ok || raw_dataset == 0 || raw_dataset > std::numeric_limits<uint32_t>::max())) {
    return RestoreResult::kInvalid;
  }
  const DatasetId saved_dataset = element.hasAttribute(u"dataset_id"_s) ? static_cast<DatasetId>(raw_dataset) : 0;
  std::optional<DatasetId> dataset;
  if (saved_dataset == 0 && !element.hasAttribute(u"dataset_source"_s) && !element.hasAttribute(u"dataset_path"_s)) {
    const auto unique =
        pj::scene3d::resolveUniqueObjectTopic(sessionManager()->objectStore(), topic_name.toStdString(), *object_type);
    if (unique.ambiguous) {
      return RestoreResult::kInvalid;
    }
    if (unique.topic_id.has_value()) {
      dataset = sessionManager()->objectStore().descriptor(*unique.topic_id).dataset_id;
    }
  } else {
    dataset = resolveObjectDataset(
        saved_dataset, element.attribute(u"dataset_source"_s), element.attribute(u"dataset_path"_s), topic_name);
  }
  if (!dataset.has_value()) {
    return RestoreResult::kDeferred;
  }
  const auto topic = sessionManager()->objectStore().findTopic(*dataset, topic_name.toStdString());
  if (!topic.has_value()) {
    return RestoreResult::kDeferred;
  }
  const sdk::BuiltinObjectType live_type =
      pj::scene3d::builtinObjectTypeFor(sessionManager()->objectStore().descriptor(*topic));
  if (live_type != sdk::BuiltinObjectType::kNone && live_type != *object_type) {
    return RestoreResult::kInvalid;
  }
  return addTopic(*topic, *object_type, topic_name) ? RestoreResult::kRestored : RestoreResult::kInvalid;
}

Scene3DDockWidget::RestoreResult Scene3DDockWidget::restoreLocalRobotLayer(const QDomElement& element) {
  if (element.tagName() != "layer"_L1 || element.attribute(u"local"_s) != "true"_L1) {
    return RestoreResult::kInvalid;
  }
  const auto type = sdk::parseBuiltinObjectType(element.attribute(u"object_type"_s).toStdString());
  const QDomElement payload = element.firstChildElement();
  if (type != sdk::BuiltinObjectType::kRobotDescription || payload.isNull() ||
      !pj::scene3d::WasmRobotModelLayer::validateXml(payload) || !payload.nextSiblingElement().isNull() ||
      payload.attribute(u"source_type"_s, u"topic"_s) == "topic"_L1) {
    return RestoreResult::kInvalid;
  }
  const ObjectTopicId topic_id = allocateLocalRobotLayerId();
  if (topic_id.id == 0) {
    return RestoreResult::kInvalid;
  }
  if (!addTopic(topic_id, *type, element.attribute(u"display_name"_s))) {
    local_robot_layer_ids_.erase(topic_id.id);
    return RestoreResult::kInvalid;
  }
  ISceneLayer* layer = layerFor(topic_id);
  if (layer == nullptr || !layer->xmlLoadState(payload)) {
    removeTopic(topic_id);
    return RestoreResult::kInvalid;
  }
  bool order_ok = false;
  const int saved_order = element.attribute(u"order"_s).toInt(&order_ok);
  if (order_ok && saved_order >= 0) {
    std::vector<SceneLayerInfo> ordered_layers = layers();
    const auto restored = std::find_if(ordered_layers.begin(), ordered_layers.end(), [topic_id](const auto& info) {
      return info.topic_id == topic_id;
    });
    if (restored != ordered_layers.end()) {
      const SceneLayerInfo restored_info = *restored;
      ordered_layers.erase(restored);
      const auto insertion =
          ordered_layers.begin() + std::min<std::size_t>(static_cast<std::size_t>(saved_order), ordered_layers.size());
      ordered_layers.insert(insertion, restored_info);
      std::vector<ObjectTopicId> ordered_ids;
      ordered_ids.reserve(ordered_layers.size());
      for (const SceneLayerInfo& info : ordered_layers) {
        ordered_ids.push_back(info.topic_id);
      }
      reorderLayers(ordered_ids);
    }
  }
  if (element.attribute(u"visible"_s, u"true"_s) == "false"_L1) {
    setLayerVisible(topic_id, false);
  }
  return RestoreResult::kRestored;
}

bool Scene3DDockWidget::restoreOnePending(const QDomElement& element) {
  if (element.tagName() == "layer"_L1) {
    if (element.attribute(u"local"_s) == "true"_L1) {
      const RestoreResult result = restoreLocalRobotLayer(element);
      if (result == RestoreResult::kInvalid) {
        markWorkspaceRestoreFailed();
      }
      return true;
    }
    const auto type = sdk::parseBuiltinObjectType(element.attribute(u"object_type"_s).toStdString());
    if (type.has_value() && isAvailableScene3dLayer(*type)) {
      const bool qualified = element.hasAttribute(u"dataset_id"_s) || element.hasAttribute(u"dataset_source"_s) ||
                             element.hasAttribute(u"dataset_path"_s);
      if (!qualified && sessionManager() != nullptr) {
        const auto generic = pj::scene3d::resolveUniqueObjectTopic(
            sessionManager()->objectStore(), element.attribute(u"topic_name"_s).toStdString(), *type);
        if (generic.ambiguous) {
          markWorkspaceRestoreFailed();
          return true;
        }
        if (!generic.topic_id.has_value()) {
          return false;
        }
        QDomDocument resolved_document;
        QDomElement resolved = resolved_document.importNode(element, /*deep=*/true).toElement();
        resolved_document.appendChild(resolved);
        const ObjectTopicDescriptor& descriptor = sessionManager()->objectStore().descriptor(*generic.topic_id);
        resolved.setAttribute(u"dataset_id"_s, QString::number(descriptor.dataset_id));
        return SceneDockWidget::restoreOnePending(resolved);
      }
      return SceneDockWidget::restoreOnePending(element);
    }
    if (!type.has_value() || !isKnownFutureScene3dLayer(*type)) {
      markWorkspaceRestoreFailed();
      return true;
    }
    return false;
  }
  const RestoreResult result = restoreConfigTopic(element);
  if (result == RestoreResult::kInvalid) {
    markWorkspaceRestoreFailed();
    return true;
  }
  return result == RestoreResult::kRestored;
}

QDomElement Scene3DDockWidget::xmlSaveState(QDomDocument& document) const {
  QDomElement root = SceneDockWidget::xmlSaveState(document);
  const std::vector<SceneLayerInfo> live_layers = layers();
  std::size_t live_index = 0;
  for (QDomElement child = root.firstChildElement(u"layer"_s); !child.isNull() && live_index < live_layers.size();
       child = child.nextSiblingElement(u"layer"_s), ++live_index) {
    if (!isLocalRobotLayerId(live_layers[live_index].topic_id)) {
      continue;
    }
    child.setAttribute(u"local"_s, u"true"_s);
    child.removeAttribute(u"dataset_id"_s);
    child.removeAttribute(u"dataset_source"_s);
    child.removeAttribute(u"dataset_path"_s);
    child.setAttribute(u"topic_name"_s, QString{});
  }
  if (sessionManager() != nullptr) {
    ObjectStore& store = sessionManager()->objectStore();
    for (const uint32_t raw_topic : config_topics_) {
      const ObjectTopicDescriptor& descriptor = store.descriptor(ObjectTopicId{raw_topic});
      if (descriptor.topic_name.empty()) {
        continue;
      }
      QDomElement config = document.createElement(u"config_topic"_s);
      config.setAttribute(u"dataset_id"_s, QString::number(descriptor.dataset_id));
      config.setAttribute(u"dataset_source"_s, datasetSourceName(sessionManager(), descriptor.dataset_id));
      const QString path = sessionManager()->datasetSourcePath(descriptor.dataset_id);
      if (!path.isEmpty()) {
        config.setAttribute(u"dataset_path"_s, path);
      }
      config.setAttribute(u"topic_name"_s, QString::fromStdString(descriptor.topic_name));
      const auto type_name = sdk::name(sdk::BuiltinObjectType::kFrameTransforms);
      config.setAttribute(
          u"object_type"_s, QString::fromLatin1(type_name.data(), static_cast<qsizetype>(type_name.size())));
      root.appendChild(config);
    }
  }

  root.setAttribute(
      u"fixed_frame_mode"_s, fixed_frame_mode_ == FixedFrameMode::kAutoRoot ? u"auto_root"_s : u"explicit"_s);
  root.setAttribute(u"fixed_frame"_s, currentFixedFrame());
  root.setAttribute(u"follow_frame"_s, currentFollowFrame());
  if (view_ != nullptr) {
    root.setAttribute(u"camera_model"_s, cameraModelName(view_->cameraModel()));
    root.setAttribute(
        u"camera_state"_s, QString::fromStdString(pj::scene3d::cameraStateToJson(view_->camera().state())));
    QDomElement controls = document.createElement(u"scene_controls"_s);
    const auto boolean = [](bool value) { return value ? u"true"_s : u"false"_s; };
    controls.setAttribute(u"grid_visible"_s, boolean(view_->gridVisible()));
    controls.setAttribute(
        u"grid_style"_s, view_->gridStyle() == pj::scene3d::SceneViewWidget::GridStyle::kFilledCells ? 1 : 0);
    controls.setAttribute(u"grid_extent_m"_s, view_->gridExtentMetres());
    controls.setAttribute(u"grid_divisions"_s, view_->gridDivisions());
    controls.setAttribute(u"axes_visible"_s, boolean(view_->axesVisible()));
    controls.setAttribute(u"gizmo_size_m"_s, view_->gizmoSize());
    controls.setAttribute(u"gizmo_opacity"_s, view_->gizmoOpacity());
    controls.setAttribute(u"tf_parent_lines"_s, boolean(view_->tfConnectionsVisible()));
    const auto& shading = view_->meshShadingParams();
    controls.setAttribute(u"meshes_visible"_s, boolean(shading.meshes_visible));
    controls.setAttribute(u"mesh_opacity"_s, shading.mesh_opacity);
    controls.setAttribute(u"collisions_visible"_s, boolean(shading.collisions_visible));
    controls.setAttribute(u"collision_opacity"_s, shading.collision_opacity);
    root.appendChild(controls);
  }
  return root;
}

bool Scene3DDockWidget::xmlLoadState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "scene3d"_L1 ||
      (element.hasAttribute(u"version"_s) && element.attribute(u"version"_s) != "1"_L1)) {
    markWorkspaceRestoreFailed();
    return false;
  }
  const QString mode = element.attribute(u"fixed_frame_mode"_s, u"auto_root"_s);
  if (mode != "auto_root"_L1 && mode != "explicit"_L1) {
    markWorkspaceRestoreFailed();
    return false;
  }
  int scene_controls_count = 0;
  for (QDomElement child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
    if (!acceptsStateChildTag(child.tagName()) ||
        (child.tagName() == "scene_controls"_L1 && ++scene_controls_count > 1)) {
      markWorkspaceRestoreFailed();
      return false;
    }
    if (child.tagName() == "layer"_L1) {
      const auto type = sdk::parseBuiltinObjectType(child.attribute(u"object_type"_s).toStdString());
      const QString local = child.attribute(u"local"_s, u"false"_s);
      const bool is_local = local == "true"_L1;
      if ((local != "true"_L1 && local != "false"_L1) || !type.has_value() || !isKnownFutureScene3dLayer(*type) ||
          (is_local ? *type != sdk::BuiltinObjectType::kRobotDescription
                    : child.attribute(u"topic_name"_s).isEmpty())) {
        markWorkspaceRestoreFailed();
        return false;
      }
      const QString visible = child.attribute(u"visible"_s, u"true"_s);
      if (visible != "true"_L1 && visible != "false"_L1) {
        markWorkspaceRestoreFailed();
        return false;
      }
      if (child.hasAttribute(u"order"_s)) {
        bool order_ok = false;
        const int order = child.attribute(u"order"_s).toInt(&order_ok);
        if (!order_ok || order < 0) {
          markWorkspaceRestoreFailed();
          return false;
        }
      }
      if (isAvailablePointCloudLayer(*type)) {
        const QDomElement payload = child.firstChildElement();
        if ((!payload.isNull() && !pj::scene3d::WasmPointCloudLayer::validateXml(payload)) ||
            !payload.nextSiblingElement().isNull()) {
          markWorkspaceRestoreFailed();
          return false;
        }
      } else if (*type == sdk::BuiltinObjectType::kImage) {
        const QDomElement payload = child.firstChildElement();
        if ((!payload.isNull() && !pj::scene3d::WasmDepthCloudLayer::validateXml(payload)) ||
            !payload.nextSiblingElement().isNull()) {
          markWorkspaceRestoreFailed();
          return false;
        }
      } else if (*type == sdk::BuiltinObjectType::kPosesInFrame) {
        const QDomElement payload = child.firstChildElement();
        if ((!payload.isNull() && !pj::scene3d::WasmPosesInFrameLayer::validateXml(payload)) ||
            !payload.nextSiblingElement().isNull()) {
          markWorkspaceRestoreFailed();
          return false;
        }
      } else if (*type == sdk::BuiltinObjectType::kOccupancyGrid) {
        const QDomElement payload = child.firstChildElement();
        if ((!payload.isNull() && !pj::scene3d::WasmOccupancyGridLayer::validateXml(payload)) ||
            !payload.nextSiblingElement().isNull()) {
          markWorkspaceRestoreFailed();
          return false;
        }
      } else if (*type == sdk::BuiltinObjectType::kVoxelGrid) {
        const QDomElement payload = child.firstChildElement();
        if ((!payload.isNull() && !pj::scene3d::WasmVoxelGridLayer::validateXml(payload)) ||
            !payload.nextSiblingElement().isNull()) {
          markWorkspaceRestoreFailed();
          return false;
        }
      } else if (*type == sdk::BuiltinObjectType::kSceneEntities) {
        const QDomElement payload = child.firstChildElement();
        if ((!payload.isNull() && !pj::scene3d::WasmSceneEntitiesLayer::validateXml(payload)) ||
            !payload.nextSiblingElement().isNull()) {
          markWorkspaceRestoreFailed();
          return false;
        }
      } else if (*type == sdk::BuiltinObjectType::kRobotDescription) {
        const QDomElement payload = child.firstChildElement();
        if ((!payload.isNull() && !pj::scene3d::WasmRobotModelLayer::validateXml(payload)) ||
            !payload.nextSiblingElement().isNull() ||
            (is_local && (payload.isNull() || payload.attribute(u"source_type"_s, u"topic"_s) == "topic"_L1))) {
          markWorkspaceRestoreFailed();
          return false;
        }
      }
    } else if (child.tagName() == "config_topic"_L1) {
      const auto type = sdk::parseBuiltinObjectType(child.attribute(u"object_type"_s).toStdString());
      if (type != sdk::BuiltinObjectType::kFrameTransforms || child.attribute(u"topic_name"_s).isEmpty() ||
          !child.firstChildElement().isNull()) {
        markWorkspaceRestoreFailed();
        return false;
      }
    } else if (!child.firstChildElement().isNull()) {
      markWorkspaceRestoreFailed();
      return false;
    }
  }

  const int model_index = cameraModelIndex(element.attribute(u"camera_model"_s));
  if (element.hasAttribute(u"camera_model"_s) && model_index < 0) {
    markWorkspaceRestoreFailed();
    return false;
  }
  std::optional<ValidatedCameraState> camera_state;
  if (element.hasAttribute(u"camera_state"_s)) {
    camera_state = validateCameraState(element.attribute(u"camera_state"_s));
    if (!camera_state.has_value()) {
      markWorkspaceRestoreFailed();
      return false;
    }
  }

  std::optional<ValidatedSceneControls> scene_controls;
  const QDomElement controls = element.firstChildElement(u"scene_controls"_s);
  if (!controls.isNull()) {
    scene_controls.emplace();
    ValidatedSceneControls& parsed = *scene_controls;
    if (!readSceneControls(controls, parsed) || !readXmlBool(controls, u"meshes_visible"_s, parsed.meshes_visible) ||
        !readXmlFloat(controls, u"mesh_opacity"_s, 0.0F, 1.0F, parsed.mesh_opacity) ||
        !readXmlBool(controls, u"collisions_visible"_s, parsed.collisions_visible) ||
        !readXmlFloat(controls, u"collision_opacity"_s, 0.0F, 1.0F, parsed.collision_opacity)) {
      markWorkspaceRestoreFailed();
      return false;
    }
  }

  ensureSceneViewCreated();
  auto restore_guard = beginWorkspaceRestore();
  QDomDocument previous_document;
  QDomElement previous_state;
  const PendingRestoreSnapshot previous_pending = capturePendingRestoreState();
  if (!xml_rollback_in_progress_) {
    previous_state = xmlSaveState(previous_document);
    previous_document.appendChild(previous_state);
  }
  resetWorkspaceRestoreStatus();
  clearPendingRestores();
  clearLayers();
  local_robot_layer_ids_.clear();
  config_topics_.clear();
  layer_topics_.clear();
  scene_topic_datasets_.clear();
  resetTransformBinding();

  const auto reject_and_rollback = [this, &previous_state, &previous_pending]() {
    clearPendingRestores();
    clearLayers();
    local_robot_layer_ids_.clear();
    config_topics_.clear();
    layer_topics_.clear();
    scene_topic_datasets_.clear();
    resetTransformBinding();
    setWindowTitle(tr("3D View"));
    if (!previous_state.isNull()) {
      QScopedValueRollback rollback_guard(xml_rollback_in_progress_, true);
      if (!xmlLoadState(previous_state)) {
        qCritical("Failed to roll back rejected WASM Scene3D XML state");
      } else {
        restorePendingRestoreState(previous_pending);
      }
    }
    return false;
  };

  for (QDomElement child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
    if (child.tagName() == "layer"_L1) {
      const auto type = sdk::parseBuiltinObjectType(child.attribute(u"object_type"_s).toStdString());
      if (child.attribute(u"local"_s) == "true"_L1) {
        if (restoreLocalRobotLayer(child) != RestoreResult::kRestored) {
          markWorkspaceRestoreFailed();
          return reject_and_rollback();
        }
      } else if (type.has_value() && isAvailableScene3dLayer(*type)) {
        if (!restoreOnePending(child)) {
          rememberPendingRestore(child, child.attribute(u"topic_name"_s));
        }
        if (workspaceRestoreFailed()) {
          return reject_and_rollback();
        }
      } else {
        rememberPendingRestore(child, child.attribute(u"topic_name"_s));
      }
      continue;
    }
    if (child.tagName() != "config_topic"_L1) {
      continue;
    }
    const RestoreResult result = restoreConfigTopic(child);
    if (result == RestoreResult::kDeferred) {
      rememberPendingRestore(child, child.attribute(u"topic_name"_s));
    } else if (result == RestoreResult::kInvalid) {
      markWorkspaceRestoreFailed();
      return reject_and_rollback();
    }
  }

  fixed_frame_mode_ = mode == "explicit"_L1 ? FixedFrameMode::kExplicit : FixedFrameMode::kAutoRoot;
  const QString saved_fixed = element.attribute(u"fixed_frame"_s);
  if (!saved_fixed.isEmpty()) {
    applyResolvedFixedFrame(saved_fixed);
  } else if (fixed_frame_mode_ == FixedFrameMode::kAutoRoot) {
    applyResolvedFixedFrame(resolveAutoFixedFrame(available_frames_));
  }

  if (model_index >= 0) {
    camera_model_combo_->setCurrentIndex(model_index);
  }
  if (camera_state.has_value()) {
    pj::scene3d::CameraState adopted = view_->camera().state();
    if (camera_state->focal.has_value()) {
      adopted.focal = *camera_state->focal;
    }
    if (camera_state->radius.has_value()) {
      adopted.radius = *camera_state->radius;
    }
    if (camera_state->azimuth.has_value()) {
      adopted.azimuth = *camera_state->azimuth;
    }
    if (camera_state->elevation.has_value()) {
      adopted.elevation = *camera_state->elevation;
    }
    if (camera_state->fov_y.has_value()) {
      adopted.fov_y = *camera_state->fov_y;
    }
    if (camera_state->ortho_scale.has_value()) {
      adopted.ortho_scale = *camera_state->ortho_scale;
    }
    if (camera_state->perspective.has_value()) {
      adopted.perspective = *camera_state->perspective;
    }
    view_->camera().adoptState(adopted);
  }
  setFollowFrame(element.attribute(u"follow_frame"_s));

  if (scene_controls.has_value()) {
    const ValidatedSceneControls& parsed = *scene_controls;
    if (parsed.grid_visible.has_value()) {
      view_->setGridVisible(*parsed.grid_visible);
    }
    if (parsed.grid_style.has_value()) {
      view_->setGridStyle(
          *parsed.grid_style == 1 ? pj::scene3d::SceneViewWidget::GridStyle::kFilledCells
                                  : pj::scene3d::SceneViewWidget::GridStyle::kLines);
    }
    if (parsed.grid_extent_m.has_value()) {
      view_->setGridExtentMetres(*parsed.grid_extent_m);
    }
    if (parsed.grid_divisions.has_value()) {
      view_->setGridDivisions(*parsed.grid_divisions);
    }
    if (parsed.axes_visible.has_value()) {
      view_->setAxesVisible(*parsed.axes_visible);
    }
    if (parsed.gizmo_size_m.has_value()) {
      view_->setGizmoSize(*parsed.gizmo_size_m);
    }
    if (parsed.gizmo_opacity.has_value()) {
      view_->setGizmoOpacity(*parsed.gizmo_opacity);
    }
    if (parsed.tf_parent_lines.has_value()) {
      view_->setTfConnectionsVisible(*parsed.tf_parent_lines);
    }
    auto shading = view_->meshShadingParams();
    if (parsed.meshes_visible.has_value()) {
      shading.meshes_visible = *parsed.meshes_visible;
    }
    if (parsed.mesh_opacity.has_value()) {
      shading.mesh_opacity = *parsed.mesh_opacity;
    }
    if (parsed.collisions_visible.has_value()) {
      shading.collisions_visible = *parsed.collisions_visible;
    }
    if (parsed.collision_opacity.has_value()) {
      shading.collision_opacity = *parsed.collision_opacity;
    }
    view_->setMeshShadingParams(shading);
  }
  emit fixedFrameModeChanged(fixed_frame_mode_ == FixedFrameMode::kAutoRoot);
  refreshFrameOverlayCombo();
  view_->update();
  return true;
}

}  // namespace PJ
