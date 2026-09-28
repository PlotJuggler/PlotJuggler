// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene2d_widgets/Scene2DDockWidget.h"

#include <QBoxLayout>
#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QLabel>
#include <QPixmap>
#include <QSizePolicy>
#include <QStackedWidget>
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
#include "pj_scene2d_core/borrowed_media_source.h"
#include "pj_scene2d_core/composite_media_source.h"
#include "pj_scene2d_core/image_resolve.h"
#include "pj_scene2d_core/scene_decoder.h"
#include "pj_scene2d_widgets/layers/depth_image_layer.h"
#include "pj_scene2d_widgets/layers/image_layer.h"
#include "pj_scene2d_widgets/layers/scene2d_layer.h"
#include "pj_scene2d_widgets/layers/scene_decoder_layer.h"
#ifdef PJ_HAS_FFMPEG
#include "pj_scene2d_widgets/layers/video_layer.h"
#endif
#include "pj_scene2d_widgets/media_viewer_widget.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/SvgUtil.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

constexpr auto kViewTag = "view";
constexpr auto kViewZoom = "zoom";
constexpr auto kViewPanX = "pan_x";
constexpr auto kViewPanY = "pan_y";
constexpr auto kViewRotation = "rotation";
constexpr auto kLayerKind = "layer_kind";
constexpr auto kImageKind = "image";
constexpr auto kDepthKind = "depth";

[[nodiscard]] std::optional<MediaViewState> parseViewState(const QDomElement& root) {
  const QDomElement view = root.firstChildElement(QString::fromLatin1(kViewTag));
  if (view.isNull()) {
    return MediaViewState{};  // layout written before viewport persistence
  }

  MediaViewState state;
  const auto read = [&view](const char* name, float fallback, bool& valid) {
    const QString key = QString::fromLatin1(name);
    if (!view.hasAttribute(key)) {
      return fallback;
    }
    bool ok = false;
    const float value = view.attribute(key).toFloat(&ok);
    valid = valid && ok;
    return value;
  };
  bool valid = true;
  state.zoom = read(kViewZoom, state.zoom, valid);
  state.pan_x = read(kViewPanX, state.pan_x, valid);
  state.pan_y = read(kViewPanY, state.pan_y, valid);
  if (const QString rotation = view.attribute(QString::fromLatin1(kViewRotation)); !rotation.isEmpty()) {
    bool ok = false;
    state.rotation_deg = rotation.toInt(&ok);
    valid = valid && ok;
  }
  if (!valid || !MediaViewerWidget::isViewStateValid(state)) {
    return std::nullopt;
  }
  return state;
}

[[nodiscard]] bool validateLayerPayloads(const QDomElement& root) {
  for (QDomElement layer = root.firstChildElement(u"layer"_s); !layer.isNull();
       layer = layer.nextSiblingElement(u"layer"_s)) {
    const auto object_type = sdk::parseBuiltinObjectType(layer.attribute(u"object_type"_s).toStdString());
    if (!object_type.has_value()) {
      return false;
    }
    const QString kind = layer.attribute(QString::fromLatin1(kLayerKind));
    if (!kind.isEmpty() && kind != QString::fromLatin1(kImageKind) && kind != QString::fromLatin1(kDepthKind)) {
      return false;
    }

    const bool image_type = *object_type == sdk::BuiltinObjectType::kImage;
    const bool depth_type = *object_type == sdk::BuiltinObjectType::kDepthImage;
    if ((!kind.isEmpty() && !image_type && !depth_type) || (kind == QString::fromLatin1(kImageKind) && !image_type)) {
      return false;
    }

    const QDomElement payload = layer.firstChildElement();
    if (payload.isNull()) {
      continue;
    }
    if (!payload.nextSiblingElement().isNull() || payload.tagName() != u"scene2d_layer"_s) {
      return false;
    }

    // Legacy kImage XML has no layer_kind. Depth settings are unambiguous, so
    // validate with the same concrete class that first-sample dispatch selects.
    const bool has_depth_options = payload.hasAttribute(u"colormap"_s) || payload.hasAttribute(u"invert"_s) ||
                                   payload.hasAttribute(u"near_m"_s) || payload.hasAttribute(u"far_m"_s) ||
                                   payload.hasAttribute(u"opacity"_s);
    const bool validate_as_depth =
        depth_type || kind == QString::fromLatin1(kDepthKind) || (image_type && kind.isEmpty() && has_depth_options);
    if (validate_as_depth) {
      DepthImageLayer candidate(ObjectTopicId{}, *object_type, QString());
      if (!candidate.xmlLoadState(payload)) {
        return false;
      }
    } else if (image_type) {
      ImageLayer candidate(ObjectTopicId{}, *object_type, QString());
      if (!candidate.xmlLoadState(payload)) {
        return false;
      }
    }
  }
  return true;
}

// Resolves only genuinely unqualified (generic-layout) layer identities. A
// non-zero id or a source name remains authoritative and is left to the shared
// exact/source resolver used by undo and source-bound layouts.
[[nodiscard]] bool resolveGenericLayerIdentities(QDomElement root, SessionManager* session) {
  for (QDomElement layer = root.firstChildElement(u"layer"_s); !layer.isNull();
       layer = layer.nextSiblingElement(u"layer"_s)) {
    const QString dataset_id_key = u"dataset_id"_s;
    const bool has_dataset_id = layer.hasAttribute(dataset_id_key);
    bool dataset_ok = false;
    const qulonglong dataset_value = layer.attribute(dataset_id_key).toULongLong(&dataset_ok);
    if (has_dataset_id && (!dataset_ok || dataset_value > std::numeric_limits<uint32_t>::max())) {
      return false;
    }

    const QString dataset_source = layer.attribute(u"dataset_source"_s);
    const QString dataset_path = layer.attribute(u"dataset_path"_s);
    if ((has_dataset_id && dataset_value != 0) || !dataset_source.isEmpty() || !dataset_path.isEmpty()) {
      // A source-qualified layer with no numeric hint is still source-bound.
      // Give the base parser its neutral id so it can perform the existing
      // unique-source fallback without degrading to generic topic matching.
      if (!has_dataset_id) {
        layer.setAttribute(dataset_id_key, u"0"_s);
      }
      continue;
    }

    if (session == nullptr) {
      return false;
    }
    const QString topic_name = layer.attribute(u"topic_name"_s);
    const auto saved_type = sdk::parseBuiltinObjectType(layer.attribute(u"object_type"_s).toStdString());
    if (topic_name.isEmpty() || !saved_type.has_value() || !Scene2DDockWidget::handlesObjectType(*saved_type)) {
      return false;
    }

    const std::string topic_name_utf8 = topic_name.toStdString();
    std::optional<DatasetId> unique_dataset;
    for (const ObjectTopicId candidate : session->objectStore().listTopics()) {
      const ObjectTopicDescriptor descriptor = session->objectStore().descriptor(candidate);
      if (descriptor.topic_name != topic_name_utf8 || objectTypeFromMetadata(descriptor.metadata_json) != *saved_type) {
        continue;
      }
      if (unique_dataset.has_value()) {
        return false;  // same compatible topic in multiple loaded datasets
      }
      unique_dataset = descriptor.dataset_id;
    }
    if (!unique_dataset.has_value()) {
      return false;
    }

    const DatasetInfo* dataset = session->dataEngine().getDataset(*unique_dataset);
    if (dataset == nullptr) {
      return false;  // ObjectStore/DataEngine identity is not a loaded dataset
    }
    layer.setAttribute(dataset_id_key, QString::number(*unique_dataset));
    layer.setAttribute(u"dataset_source"_s, QString::fromStdString(dataset->source_name));
  }
  return true;
}

using LayerCreator = std::unique_ptr<ISceneLayer> (*)(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name);

struct LayerRegistration {
  sdk::BuiltinObjectType object_type;
  LayerCreator creator;
};

std::unique_ptr<ISceneLayer> createImageLayer(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name) {
  return std::make_unique<ImageLayer>(topic_id, object_type, display_name);
}

#ifdef PJ_HAS_FFMPEG
std::unique_ptr<ISceneLayer> createVideoLayer(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name) {
  return std::make_unique<VideoLayer>(topic_id, object_type, display_name);
}
#endif

std::unique_ptr<ISceneLayer> createDepthImageLayer(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name) {
  return std::make_unique<DepthImageLayer>(topic_id, object_type, display_name);
}

// Full-frame pixel sources: a dock shows at most one, beneath the overlays.
[[nodiscard]] bool isBackgroundType(sdk::BuiltinObjectType object_type) {
  return object_type == sdk::BuiltinObjectType::kImage || object_type == sdk::BuiltinObjectType::kDepthImage ||
         object_type == sdk::BuiltinObjectType::kVideoFrame;
}

// True for the depth `encoding` strings the DepthImageLayer colormaps. A scene2D-
// local copy: scene3D's identical predicate lives in a sibling widget family this
// module must not depend on.
[[nodiscard]] bool isDepthEncoding(const std::string& encoding) {
  return encoding == "16UC1" || encoding == "32FC1" || encoding == "compressedDepth";
}

// Peek a kImage topic's first sample and report whether it is depth-encoded.
// Depth and color share kImage, so the dock picks the layer from the payload's
// encoding (resolving one sample via the parser or canonical codec) — the consumer
// can classify depth-vs-color even though the parser can't at schema time.
[[nodiscard]] bool firstSampleIsDepthEncoded(SessionManager* session, ObjectTopicId topic_id) {
  if (session == nullptr) {
    return false;
  }
  auto first = session->objectStore().at(topic_id, static_cast<size_t>(0));
  if (!first.has_value() || first->payload.bytes.empty()) {
    return false;
  }
  const auto binding = session->parserBindingForObjectTopic(topic_id);
  const bool canonical = binding.parser == nullptr;
  auto resolved = resolveImage(binding.parser, binding.mutex, canonical, first->timestamp, first->payload);
  return resolved.has_value() && isDepthEncoding(resolved->image.encoding);
}

std::unique_ptr<ISceneLayer> createImageAnnotationsLayer(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name) {
  return std::make_unique<SceneDecoderLayer>(
      topic_id, object_type, display_name, u"Annotations"_s, kSchemaImageAnnotations);
}

std::unique_ptr<ISceneLayer> createSceneEntitiesLayer(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name) {
  return std::make_unique<SceneDecoderLayer>(topic_id, object_type, display_name, u"Markers"_s, kSchemaSceneEntities);
}

#ifdef PJ_HAS_FFMPEG
constexpr std::array<LayerRegistration, 5> kLayerRegistrations {
  {
#else
constexpr std::array<LayerRegistration, 4> kLayerRegistrations{{
#endif
    {sdk::BuiltinObjectType::kImage, &createImageLayer},
#ifdef PJ_HAS_FFMPEG
        {sdk::BuiltinObjectType::kVideoFrame, &createVideoLayer},
#endif
        {sdk::BuiltinObjectType::kDepthImage, &createDepthImageLayer},
        {sdk::BuiltinObjectType::kImageAnnotations, &createImageAnnotationsLayer},
        {sdk::BuiltinObjectType::kSceneEntities, &createSceneEntitiesLayer},
  }
};

}  // namespace

Scene2DDockWidget::Scene2DDockWidget(QWidget* parent) : SceneDockWidget(parent) {
  setWindowTitle(tr("2D View"));

  for (const LayerRegistration& registration : kLayerRegistrations) {
    if (registration.object_type == sdk::BuiltinObjectType::kImage) {
      continue;  // kImage is registered below with an encoding-aware dispatcher.
    }
    layerFactory().registerType(registration.object_type, registration.creator);
  }
  // kImage covers BOTH color and depth (they share the type). Route depth-encoded
  // images to the colormap DepthImageLayer and everything else to ImageLayer, by
  // peeking the topic's first sample's encoding.
  layerFactory().registerType(
      sdk::BuiltinObjectType::kImage,
      [this](ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name) {
        const auto restore_kind = restore_image_layer_kinds_.find(topicKey(topic_id));
        if (restore_kind != restore_image_layer_kinds_.end()) {
          return restore_kind->second == ImageLayerKind::kDepth
                     ? createDepthImageLayer(topic_id, object_type, display_name)
                     : createImageLayer(topic_id, object_type, display_name);
        }
        return firstSampleIsDepthEncoded(sessionManager(), topic_id)
                   ? createDepthImageLayer(topic_id, object_type, display_name)
                   : createImageLayer(topic_id, object_type, display_name);
      });
}

Scene2DDockWidget::~Scene2DDockWidget() {
  if (live_samples_conn_) {
    QObject::disconnect(live_samples_conn_);
  }
  if (viewer_ != nullptr) {
    viewer_->setMediaSource(nullptr);
  }
  composite_.reset();
}

QDomElement Scene2DDockWidget::xmlSaveState(QDomDocument& doc) const {
  QDomElement root = SceneDockWidget::xmlSaveState(doc);

  QDomElement layer_element = root.firstChildElement(u"layer"_s);
  for (const SceneLayerInfo& info : layers()) {
    if (layer_element.isNull()) {
      break;
    }
    if (dynamic_cast<DepthImageLayer*>(layerFor(info.topic_id)) != nullptr) {
      layer_element.setAttribute(QString::fromLatin1(kLayerKind), QString::fromLatin1(kDepthKind));
    } else if (dynamic_cast<ImageLayer*>(layerFor(info.topic_id)) != nullptr) {
      layer_element.setAttribute(QString::fromLatin1(kLayerKind), QString::fromLatin1(kImageKind));
    }
    layer_element = layer_element.nextSiblingElement(u"layer"_s);
  }

  const MediaViewState state = viewer_ != nullptr ? viewer_->viewState() : MediaViewState{};
  QDomElement view = doc.createElement(QString::fromLatin1(kViewTag));
  view.setAttribute(QString::fromLatin1(kViewZoom), QString::number(state.zoom, 'g', 9));
  view.setAttribute(QString::fromLatin1(kViewPanX), QString::number(state.pan_x, 'g', 9));
  view.setAttribute(QString::fromLatin1(kViewPanY), QString::number(state.pan_y, 'g', 9));
  view.setAttribute(QString::fromLatin1(kViewRotation), QString::number(state.rotation_deg));
  root.appendChild(view);
  return root;
}

bool Scene2DDockWidget::xmlLoadState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != xmlTag()) {
    return false;
  }
  if (!validateLayerPayloads(element)) {
    return false;
  }
  int view_elements = 0;
  for (QDomElement child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
    if (!acceptsStateChildTag(child.tagName()) ||
        (child.tagName() == QString::fromLatin1(kViewTag) && ++view_elements > 1)) {
      return false;
    }
  }
  const auto view_state = parseViewState(element);
  if (!view_state.has_value()) {
    return false;  // reject before the base destructively replaces the layer stack
  }

  // The base fills legacy order attributes while restoring, and generic layouts
  // need concrete dataset identities before that destructive pass. Work on a
  // private clone so neither normalization mutates the caller's XML document.
  QDomDocument restore_doc;
  QDomElement restore_element = restore_doc.importNode(element, /*deep=*/true).toElement();
  restore_doc.appendChild(restore_element);
  if (!resolveGenericLayerIdentities(restore_element, sessionManager())) {
    return false;
  }

  // The base restore clears the live layer stack before replay. Preserve a
  // private snapshot so a late duplicate/attach/payload failure can put this
  // same widget back exactly; direct clipboard paste has no MainWindow-level
  // transaction wrapper.
  QDomDocument previous_doc;
  QDomElement previous_element = xmlSaveState(previous_doc);
  previous_doc.appendChild(previous_element);
  const PendingRestoreSnapshot previous_pending = capturePendingRestoreState();
  const auto rollback = [this, &previous_element, &previous_pending]() {
    primeRestoreLayerKinds(previous_element);
    const bool layers_restored = SceneDockWidget::xmlLoadState(previous_element);
    restore_image_layer_kinds_.clear();
    const auto previous_view = parseViewState(previous_element);
    ensureSceneViewCreated();
    const bool restored =
        layers_restored && previous_view.has_value() && viewer_ != nullptr && viewer_->setViewState(*previous_view);
    if (restored) {
      restorePendingRestoreState(previous_pending);
    }
    return restored;
  };

  auto restore_guard = beginWorkspaceRestore();
  primeRestoreLayerKinds(restore_element);
  const bool loaded = SceneDockWidget::xmlLoadState(restore_element);
  restore_image_layer_kinds_.clear();
  if (!loaded) {
    if (!rollback()) {
      qWarning("Scene2DDockWidget: failed to roll back rejected XML state");
    }
    return false;
  }

  ensureSceneViewCreated();
  if (viewer_ == nullptr || !viewer_->setViewState(*view_state)) {
    if (!rollback()) {
      qWarning("Scene2DDockWidget: failed to roll back rejected view state");
    }
    return false;
  }
  return true;
}

void Scene2DDockWidget::setSessionManager(SessionManager* session) {
  SceneDockWidget::setSessionManager(session);
  reconnectLiveSamples(session);
}

bool Scene2DDockWidget::tryAcceptObjectTopic(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
  if (!isBackgroundType(object_type)) {
    return addTopic(topic_id, object_type, title);
  }
  if (!addTopic(topic_id, object_type, title)) {
    return false;
  }
  promoteBackground(topic_id);
  return true;
}

std::vector<ObjectTopicId> Scene2DDockWidget::backgroundTopics() const {
  std::vector<ObjectTopicId> topics;
  for (const SceneLayerInfo& info : layers()) {
    if (isBackgroundType(info.object_type)) {
      topics.push_back(info.topic_id);
    }
  }
  return topics;
}

void Scene2DDockWidget::promoteBackground(ObjectTopicId topic_id) {
  for (const ObjectTopicId background : backgroundTopics()) {
    if (background.id != topic_id.id) {
      removeTopic(background);
    }
  }
  reorderLayers({topic_id});
}

bool Scene2DDockWidget::setImageTopic(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
  const bool accepted = tryAcceptObjectTopic(topic_id, object_type, title);
  if (accepted) {
    setWindowTitle(title.isEmpty() ? tr("2D View") : tr("2D View - %1").arg(title));
  }
  return accepted;
}

void Scene2DDockWidget::setPointInspectorEnabled(bool enabled) {
  if (viewer_ != nullptr) {
    viewer_->setPointInspectorEnabled(enabled);
  }
}

bool Scene2DDockWidget::pointInspectorEnabled() const noexcept {
  return viewer_ != nullptr && viewer_->pointInspectorEnabled();
}

void Scene2DDockWidget::setViewRotation(int degrees) {
  ensureSceneViewCreated();
  if (viewer_ == nullptr || degrees == viewRotation()) {
    return;
  }
  MediaViewState state = viewer_->viewState();
  state.rotation_deg = degrees;
  if (viewer_->setViewState(state)) {
    notifyWorkspaceChanged();
  }
}

int Scene2DDockWidget::viewRotation() const {
  return viewer_ != nullptr ? viewer_->viewState().rotation_deg : 0;
}

size_t Scene2DDockWidget::compositeLayerCountForTesting() const noexcept {
  return composite_ != nullptr ? composite_->layerCount() : 0U;
}

std::vector<ObjectTopicId> Scene2DDockWidget::compositeTopicOrderForTesting() const {
  return composite_topic_order_;
}

QString Scene2DDockWidget::xmlTag() const {
  return u"scene2d"_s;
}

bool Scene2DDockWidget::acceptsStateChildTag(const QString& tag) const {
  return tag == u"layer"_s || tag == QString::fromLatin1(kViewTag);
}

QWidget* Scene2DDockWidget::createSceneView() {
  auto* container = new QWidget(this);
  container->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));

  auto* layout = new QVBoxLayout(container);
  layout->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
  layout->setSpacing(PJ::theme::space(PJ::theme::Space::None));

  // Forces native Qt 6.8 to create an RHI-backed window backing store before
  // first show(); dynamically added QRhiWidgets otherwise never get a QRhi.
  // The browser must instead let the first real, nonzero viewer trigger Qt's
  // dynamic raster-to-RHI switch. A zero-size WebGL surface can lose its
  // context across a cold browser picker. See TECHNICAL_NOTES.md.
#ifndef PJ_TARGET_WASM
  bootstrap_ = new MediaViewerWidget(container);
  bootstrap_->setObjectName(u"scene2dRhiBootstrap"_s);
  bootstrap_->setMaximumSize(0, 0);
  layout->addWidget(bootstrap_);
#endif

  // Page 0 = empty-state placeholder, page 1 = GPU viewer. A stack (rather than
  // an overlay) keeps the raster placeholder off the QRhiWidget's surface, where
  // it would not composite reliably.
  view_stack_ = new QStackedWidget(container);
  layout->addWidget(view_stack_, /*stretch=*/1);
  view_stack_->addWidget(makeEmptyPlaceholder(view_stack_));

#ifdef PJ_TARGET_WASM
  // Construct parentless so MediaViewerWidget::setApi() completes before
  // QStackedWidget inserts it into an already-visible top-level hierarchy.
  // QRhiWidget documents the API choice as immutable once that happens.
  viewer_ = new MediaViewerWidget();
#else
  viewer_ = new MediaViewerWidget(view_stack_);
#endif
  viewer_->setObjectName(u"scene2dMediaViewer"_s);
  viewer_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  connect(viewer_, &MediaViewerWidget::viewInteractionCommitted, this, [this]() { notifyWorkspaceChanged(); });
  connect(viewer_, &MediaViewerWidget::rotationChanged, this, &Scene2DDockWidget::viewRotationChanged);
  view_stack_->addWidget(viewer_);
  if (composite_ != nullptr) {
    viewer_->setMediaSource(composite_.get());
  }

  applyEmptyPlaceholderState();
  return container;
}

QWidget* Scene2DDockWidget::makeEmptyPlaceholder(QWidget* parent) {
  auto* page = new QWidget(parent);
  page->setObjectName(u"scene2dEmptyPlaceholder"_s);

  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));

  empty_placeholder_icon_ = new QLabel(page);
  empty_placeholder_icon_->setObjectName(u"scene2dEmptyPlaceholderIcon"_s);
  empty_placeholder_icon_->setAlignment(Qt::AlignCenter);
  // Dim the icon so it reads as a "drop a topic here" watermark, not chrome.
  auto* opacity = new QGraphicsOpacityEffect(empty_placeholder_icon_);
  opacity->setOpacity(0.35);
  empty_placeholder_icon_->setGraphicsEffect(opacity);

  layout->addStretch(1);
  layout->addWidget(empty_placeholder_icon_, 0, Qt::AlignCenter);
  layout->addStretch(1);

  retintEmptyPlaceholder();
  return page;
}

void Scene2DDockWidget::retintEmptyPlaceholder() {
  if (empty_placeholder_icon_ == nullptr) {
    return;
  }
  // Same 2D glyph the dock-level placeholder offers, tinted to the theme ink.
  constexpr int kIconPx = 96;
  const QPixmap pixmap = renderSvgPixmap(
      u":/resources/svg/image.svg"_s, currentTheme(), QSize(kIconPx, kIconPx),
      empty_placeholder_icon_->devicePixelRatioF());
  empty_placeholder_icon_->setPixmap(pixmap);
}

void Scene2DDockWidget::applyEmptyPlaceholderState() {
  if (view_stack_ != nullptr) {
    view_stack_->setCurrentIndex(empty_placeholder_active_ ? 0 : 1);
  }
}

void Scene2DDockWidget::changeEvent(QEvent* event) {
  SceneDockWidget::changeEvent(event);
  if (event != nullptr && (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)) {
    retintEmptyPlaceholder();
  }
}

std::unique_ptr<SceneLayerContext> Scene2DDockWidget::makeContext() {
  auto context = std::make_unique<SceneLayerContext>();
  context->session = sessionManager();
  return context;
}

bool Scene2DDockWidget::handlesObjectType(sdk::BuiltinObjectType object_type) {
  return std::any_of(kLayerRegistrations.begin(), kLayerRegistrations.end(), [object_type](const auto& registration) {
    return registration.object_type == object_type;
  });
}

bool Scene2DDockWidget::acceptsObjectType(sdk::BuiltinObjectType object_type) const {
  return handlesObjectType(object_type);
}

void Scene2DDockWidget::syncViewLayers(const std::vector<ISceneLayer*>& ordered_layers) {
  auto next = std::make_unique<CompositeMediaSource>();
  composite_topic_order_.clear();

  for (ISceneLayer* layer : ordered_layers) {
    if (layer == nullptr) {
      continue;
    }
    const auto info = layer->info();
    if (!info.visible) {
      continue;
    }
    auto* scene2d_layer = dynamic_cast<Scene2DLayer*>(layer);
    if (scene2d_layer == nullptr || scene2d_layer->mediaSource() == nullptr) {
      continue;
    }
    // Tag the layer with its underlying source pointer so the rebuilt composite
    // can inherit this layer's last frame from the outgoing one (carry-over
    // below), instead of starting blank and forcing a re-decode.
    MediaSource* underlying = scene2d_layer->mediaSource();
    next->addLayer(std::make_unique<BorrowedMediaSource>(underlying), 1.0f, underlying);
    composite_topic_order_.push_back(info.topic_id);
  }

  // Carry the persisting layers' current frame into the rebuilt composite so the
  // viewer keeps showing them across the swap instead of going black until each
  // source re-decodes (the "black until play" symptom on add/remove/hide).
  if (composite_ != nullptr) {
    next->adoptContributions(*composite_);
  }

  // Repoint the viewer at the new composite BEFORE the previous one is freed,
  // so the viewer never holds a dangling MediaSource — not even transiently
  // between the swap and the setMediaSource call.
  auto previous = std::move(composite_);
  composite_ = std::move(next);
  if (viewer_ != nullptr) {
    viewer_->setMediaSource(composite_.get());
  }
  // `previous` is destroyed here, after the viewer no longer references it.
  syncCompositeTimestamp(ordered_layers);
  // Show the placeholder iff no visible layer feeds the viewer.
  empty_placeholder_active_ = composite_topic_order_.empty();
  applyEmptyPlaceholderState();
  refreshView();
}

void Scene2DDockWidget::refreshView() {
  if (viewer_ != nullptr) {
    viewer_->update();
  }
}

void Scene2DDockWidget::primeRestoreLayerKinds(const QDomElement& root) {
  restore_image_layer_kinds_.clear();
  for (QDomElement layer = root.firstChildElement(u"layer"_s); !layer.isNull();
       layer = layer.nextSiblingElement(u"layer"_s)) {
    primeRestoreLayerKind(layer);
  }
}

void Scene2DDockWidget::primeRestoreLayerKind(const QDomElement& layer_element) {
  if (sessionManager() == nullptr) {
    return;
  }
  const QString saved_kind = layer_element.attribute(QString::fromLatin1(kLayerKind));
  if (saved_kind != QString::fromLatin1(kImageKind) && saved_kind != QString::fromLatin1(kDepthKind)) {
    return;  // legacy layout: retain first-sample encoding dispatch
  }

  bool dataset_ok = false;
  const qulonglong dataset_value = layer_element.attribute(u"dataset_id"_s).toULongLong(&dataset_ok);
  if (!dataset_ok || dataset_value > std::numeric_limits<uint32_t>::max()) {
    return;
  }
  const QString topic_name = layer_element.attribute(u"topic_name"_s);
  const auto dataset_id = sessionManager()
                              ->resolveObjectDatasetIdentity(
                                  static_cast<DatasetId>(dataset_value), layer_element.attribute(u"dataset_source"_s),
                                  layer_element.attribute(u"dataset_path"_s), topic_name)
                              .id;
  if (!dataset_id.has_value()) {
    return;
  }
  const auto topic_id = sessionManager()->objectStore().findTopic(*dataset_id, topic_name.toStdString());
  if (!topic_id.has_value()) {
    return;
  }
  restore_image_layer_kinds_.insert_or_assign(
      topicKey(*topic_id),
      saved_kind == QString::fromLatin1(kDepthKind) ? ImageLayerKind::kDepth : ImageLayerKind::kImage);
}

bool Scene2DDockWidget::restoreOnePending(const QDomElement& element) {
  // A deferred drop (advertised topic, no data yet) gets tryAcceptObjectTopic's
  // one-background policy; saved layout layers keep appending.
  const auto object_type = sdk::parseBuiltinObjectType(element.attribute(u"object_type"_s).toStdString());
  const bool dropped_background =
      element.attribute(u"pending_intent"_s) == u"true"_s && object_type.has_value() && isBackgroundType(*object_type);
  const std::vector<ObjectTopicId> previous_backgrounds =
      dropped_background ? backgroundTopics() : std::vector<ObjectTopicId>{};

  auto previous_kinds = std::move(restore_image_layer_kinds_);
  restore_image_layer_kinds_.clear();
  primeRestoreLayerKind(element);
  const bool restored = SceneDockWidget::restoreOnePending(element);
  restore_image_layer_kinds_ = std::move(previous_kinds);

  if (restored && dropped_background) {
    for (const ObjectTopicId candidate : backgroundTopics()) {
      const bool is_new = std::none_of(
          previous_backgrounds.begin(), previous_backgrounds.end(),
          [candidate](ObjectTopicId old_background) { return old_background.id == candidate.id; });
      if (is_new) {
        promoteBackground(candidate);
        break;
      }
    }
  }
  return restored;
}

void Scene2DDockWidget::reconnectLiveSamples(SessionManager* session) {
  if (live_samples_conn_) {
    QObject::disconnect(live_samples_conn_);
    live_samples_conn_ = {};
  }
  if (session == nullptr) {
    return;
  }
  live_samples_conn_ =
      connect(session, &SessionManager::samplesIngested, this, [this](const QVector<TopicId>&, bool live) {
        if (live) {
          driveVisibleLayersToLiveEdge();
        }
      });
}

void Scene2DDockWidget::driveVisibleLayersToLiveEdge() {
  if (sessionManager() == nullptr) {
    return;
  }

  ObjectStore& store = sessionManager()->objectStore();
  bool any = false;
  int64_t latest = std::numeric_limits<int64_t>::lowest();
  for (const SceneLayerInfo& info : layers()) {
    if (!info.visible || store.entryCount(info.topic_id) == 0) {
      continue;
    }
    latest = std::max(latest, store.timeRange(info.topic_id).second);
    any = true;
  }
  if (!any) {
    return;
  }

  noteTrackerTime(latest);
  for (const SceneLayerInfo& info : layers()) {
    ISceneLayer* layer = layerFor(info.topic_id);
    if (layer != nullptr && info.visible) {
      layer->setTrackerTime(PJ::fromRaw(latest));
    }
  }
  if (composite_ != nullptr) {
    composite_->setTimestamp(latest);
  }
  refreshView();
}

void Scene2DDockWidget::syncCompositeTimestamp(const std::vector<ISceneLayer*>& ordered_layers) {
  if (composite_ == nullptr) {
    return;
  }
  // A rebuild rewraps the same underlying sources, which still hold their
  // per-timestamp dedup; invalidate first so the re-seed below re-decodes at the
  // (usually unchanged) current time instead of leaving a stale/empty frame
  // until the tracker next moves (the "black until play" symptom).
  composite_->invalidate();
  const auto seed = seedTimestampNs(ordered_layers);
  if (seed.has_value()) {
    composite_->setTimestamp(*seed);
  }
}

std::optional<int64_t> Scene2DDockWidget::seedTimestampNs(const std::vector<ISceneLayer*>& ordered_layers) const {
  if (const auto ns = lastTrackerNs(); ns.has_value()) {
    return ns;
  }
  for (ISceneLayer* layer : ordered_layers) {
    auto* scene2d_layer = dynamic_cast<Scene2DLayer*>(layer);
    if (scene2d_layer == nullptr || !scene2d_layer->info().visible) {
      continue;
    }
    const auto last = scene2d_layer->lastTrackerTimeNs();
    if (last.has_value()) {
      return last;
    }
  }
  return std::nullopt;
}

}  // namespace PJ
