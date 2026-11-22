// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene2d_widgets/Scene2DDockWidget.h"

#include <QBoxLayout>
#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QLabel>
#include <QPixmap>
#include <QPointer>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStringList>
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pj_datastore/saturating_time.hpp"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DerivedTopics.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/XmlStateOptions.h"
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
#include "pj_scene_common/saved_layer_header.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/RenderingWarningLabel.h"
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

const QStringList& rotationTokens() {
  static const QStringList tokens = [] {
    QStringList list;
    for (const int degrees : kViewRotations) {
      list.append(QString::number(degrees));
    }
    return list;
  }();
  return tokens;
}

// The one reader of a <view>, for xmlLoadState and xmlValidateState. A state without one (a layout written before
// viewport persistence) takes the defaults; under a recorder an empty <view> is read instead, so the options are
// reported.
[[nodiscard]] std::optional<MediaViewState> parseViewState(const QDomElement& root) {
  QDomDocument absent;
  QDomElement view = root.firstChildElement(QString::fromLatin1(kViewTag));
  if (view.isNull()) {
    if (!XmlOptionsRecorder::active()) {
      return MediaViewState{};
    }
    view = absent.createElement(QString::fromLatin1(kViewTag));
  }

  constexpr float kLowest = std::numeric_limits<float>::lowest();
  constexpr float kMax = std::numeric_limits<float>::max();
  MediaViewState state;
  bool valid = readXmlFloat(view, QString::fromLatin1(kViewZoom), state.zoom, kMinViewZoom, kMaxViewZoom, state.zoom);
  valid = readXmlFloat(view, QString::fromLatin1(kViewPanX), state.pan_x, kLowest, kMax, state.pan_x) && valid;
  valid = readXmlFloat(view, QString::fromLatin1(kViewPanY), state.pan_y, kLowest, kMax, state.pan_y) && valid;

  // An empty rotation reads as absent, and any integer spelling of an accepted angle ("+90") is accepted.
  const XmlOption rotation{
      .attribute = QString::fromLatin1(kViewRotation),
      .type = u"enum"_s,
      .values = rotationTokens(),
      .fallback = QString::number(state.rotation_deg)};
  XmlOptionsRecorder::noteOption(view, rotation);
  if (const QString text = view.attribute(rotation.attribute); !text.isEmpty()) {
    bool ok = false;
    const int degrees = text.toInt(&ok);
    if (ok && std::ranges::find(kViewRotations, degrees) != kViewRotations.end()) {
      state.rotation_deg = degrees;
    } else {
      XmlOptionsRecorder::noteRejection(view, rotation.attribute, xmlValueProblem(rotation, text));
      valid = false;
    }
  }
  if (!valid) {
    return std::nullopt;
  }
  if (!MediaViewerWidget::isViewStateValid(state)) {
    XmlOptionsRecorder::noteRejection(view, {}, u"pan_x and pan_y must be 0 at zoom 1"_s);
    return std::nullopt;
  }
  return state;
}

using LayerCreator = std::unique_ptr<ISceneLayer> (*)(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name);

struct LayerRegistration {
  sdk::BuiltinObjectType object_type;
  LayerCreator creator;
  bool (*validator)(const QDomElement& payload);
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

[[nodiscard]] bool validateDepthImagePayload(const QDomElement& payload) {
  return DepthImageLayer::parseOptions(payload, DepthImageLayer::Options{}).has_value();
}

// The layer the kImage creator builds for a restore: its <layer>'s layer_kind (primeRestoreLayerKind) or, without
// one, depth settings in the payload identify a depth layer, as first-sample dispatch would.
[[nodiscard]] bool validateImagePayload(const QDomElement& payload) {
  const QString kind = payload.parentNode().toElement().attribute(QString::fromLatin1(kLayerKind));
  const bool has_depth_options = payload.hasAttribute(u"colormap"_s) || payload.hasAttribute(u"invert"_s) ||
                                 payload.hasAttribute(u"near_m"_s) || payload.hasAttribute(u"far_m"_s) ||
                                 payload.hasAttribute(u"opacity"_s);
  if (kind == QString::fromLatin1(kDepthKind) || (kind.isEmpty() && has_depth_options)) {
    return validateDepthImagePayload(payload);
  }
  return ImageLayer::parseOptions(payload, ImageLayer::kDefaultRectify).has_value();
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
  auto resolved = resolveImage(
      binding.parser, binding.mutex, canonical, first->timestamp, first->payload, first->payload_stamp_shift);
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
    {sdk::BuiltinObjectType::kImage, &createImageLayer, &validateImagePayload},
#ifdef PJ_HAS_FFMPEG
        {sdk::BuiltinObjectType::kVideoFrame, &createVideoLayer, &LayerFactory::acceptPayloadWithoutOptions},
#endif
        {sdk::BuiltinObjectType::kDepthImage, &createDepthImageLayer, &validateDepthImagePayload},
        {sdk::BuiltinObjectType::kImageAnnotations, &createImageAnnotationsLayer,
         &LayerFactory::acceptPayloadWithoutOptions},
        {sdk::BuiltinObjectType::kSceneEntities, &createSceneEntitiesLayer, &LayerFactory::acceptPayloadWithoutOptions},
  }
};

// The layer_kind a <layer> accepts.
const XmlOption& layerKindOption() {
  static const XmlOption option{
      .attribute = QString::fromLatin1(kLayerKind),
      .type = u"enum"_s,
      .values = {QString::fromLatin1(kImageKind), QString::fromLatin1(kDepthKind)}};
  return option;
}

// A non-zero id or a non-empty source or path; otherwise the layer is generic and binds by topic and type.
[[nodiscard]] bool namesDataset(const QDomElement& layer) {
  return layer.attribute(u"dataset_id"_s).toULongLong() != 0 || !layer.attribute(u"dataset_source"_s).isEmpty() ||
         !layer.attribute(u"dataset_path"_s).isEmpty();
}

// The header is the base dock's (it saves it and restores it from the same element); layer_kind and the payload are
// this family's.
[[nodiscard]] bool validateLayer(const QDomElement& layer, const LayerFactory& factory) {
  XmlOptionsRecorder::noteOption(layer, layerKindOption());
  const std::optional<SavedLayerHeader> header = parseSavedLayerHeader(layer, SavedTopicRef::kOptional, factory);
  if (!header.has_value()) {
    return false;
  }
  bool valid = true;
  const auto reject = [&layer, &valid](const QString& attribute, const QString& reason) {
    XmlOptionsRecorder::noteRejection(layer, attribute, reason);
    valid = false;
  };
  const QString image_kind = QString::fromLatin1(kImageKind);
  const QString depth_kind = QString::fromLatin1(kDepthKind);
  const bool names_dataset = namesDataset(layer);
  const std::optional<sdk::BuiltinObjectType>& object_type = header->object_type;
  const bool handled = object_type.has_value() && factory.supports(*object_type);

  // An unparsable type is refused; a parsable one the dock does not handle is refused only for a generic layer: one
  // that names its dataset is accepted, and refused when it is restored.
  if (!handled) {
    XmlOptionsRecorder::noteRejection(layer, u"object_type"_s, u"not a 2D layer type"_s);
    valid = object_type.has_value() && names_dataset;
  }
  const bool image_type = object_type == sdk::BuiltinObjectType::kImage;
  const bool depth_type = object_type == sdk::BuiltinObjectType::kDepthImage;
  const QString kind = layer.attribute(layerKindOption().attribute);
  if (!kind.isEmpty() && kind != image_kind && kind != depth_kind) {
    reject(layerKindOption().attribute, xmlValueProblem(layerKindOption(), kind));
  } else if ((!kind.isEmpty() && !image_type && !depth_type) || (kind == image_kind && !image_type)) {
    reject(layerKindOption().attribute, u"does not fit the object_type"_s);
  }

  if (const QDomElement payload = layer.firstChildElement(); !payload.isNull()) {
    if (!payload.nextSiblingElement().isNull() || payload.tagName() != u"scene2d_layer"_s) {
      reject({}, u"expected a single <scene2d_layer> child"_s);
    } else if (handled) {
      valid = factory.validatePayload(*object_type, payload) && valid;
    }
  }

  if (!names_dataset && header->topic_name.isEmpty()) {
    reject(u"topic_name"_s, u"expected a topic name: the layer names no dataset"_s);
  }
  return valid;
}

// Reports what a layer and each registered payload accept even when the state has none of them (a saved state holds
// only the layers it shows). Only under a recorder: it parses nothing a load uses.
void reportLayerSchema(const LayerFactory& factory) {
  if (!XmlOptionsRecorder::active()) {
    return;
  }
  QDomDocument schema;
  QDomElement layer = schema.createElement(u"layer"_s);
  XmlOptionsRecorder::noteOption(layer, layerKindOption());
  (void)parseSavedLayerHeader(layer, SavedTopicRef::kOptional, factory);
  const QDomElement payload = layer.appendChild(schema.createElement(u"scene2d_layer"_s)).toElement();
  for (const sdk::BuiltinObjectType type : factory.types()) {
    (void)factory.validatePayload(type, payload);
  }
}

// A zero id and empty qualifiers name no dataset: dropped from the (cloned) state, so the base treats the layer as
// unqualified.
void dropEmptyDatasetQualifiers(QDomElement root) {
  for (QDomElement layer = root.firstChildElement(u"layer"_s); !layer.isNull();
       layer = layer.nextSiblingElement(u"layer"_s)) {
    if (!namesDataset(layer)) {
      layer.removeAttribute(u"dataset_id"_s);
      layer.removeAttribute(u"dataset_source"_s);
      layer.removeAttribute(u"dataset_path"_s);
    }
  }
}

}  // namespace

Scene2DDockWidget::Scene2DDockWidget(QWidget* parent) : SceneDockWidget(parent) {
  setWindowTitle(tr("2D View"));

  for (const LayerRegistration& registration : kLayerRegistrations) {
    if (registration.object_type == sdk::BuiltinObjectType::kImage) {
      continue;  // kImage is registered below with an encoding-aware dispatcher.
    }
    layerFactory().registerType(registration.object_type, registration.creator, registration.validator);
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
      },
      &validateImagePayload);
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

bool Scene2DDockWidget::xmlValidateState(const QDomElement& element) const {
  return validatedViewState(element).has_value();
}

std::optional<MediaViewState> Scene2DDockWidget::validatedViewState(const QDomElement& element) const {
  if (element.isNull() || element.tagName() != xmlTag()) {
    XmlOptionsRecorder::noteRejection(element, {}, u"expected a <"_s + xmlTag() + u"> element"_s);
    return std::nullopt;
  }
  readXmlText(element, u"version"_s, u"1"_s);
  bool valid = true;
  int view_elements = 0;
  for (QDomElement child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
    if (!acceptsStateChildTag(child.tagName())) {
      XmlOptionsRecorder::noteRejection(child, {}, u"unexpected element"_s);
      valid = false;
    } else if (child.tagName() == QString::fromLatin1(kViewTag) && ++view_elements > 1) {
      XmlOptionsRecorder::noteRejection(child, {}, u"expected at most one <view>"_s);
      valid = false;
    } else if (child.tagName() == u"layer"_s) {
      valid = validateLayer(child, layerFactory()) && valid;
    }
  }
  reportLayerSchema(layerFactory());
  std::optional<MediaViewState> view_state = parseViewState(element);
  return valid ? view_state : std::nullopt;
}

bool Scene2DDockWidget::xmlLoadState(const QDomElement& element) {
  const std::optional<MediaViewState> view_state = validatedViewState(element);
  if (!view_state.has_value()) {
    return false;  // reject before the base destructively replaces the layer stack
  }

  // The generic-identity normalization and the base's order-attribute fill rewrite the XML: work on a clone, not the
  // caller's document.
  QDomDocument restore_doc;
  QDomElement restore_element = restore_doc.importNode(element, /*deep=*/true).toElement();
  restore_doc.appendChild(restore_element);
  dropEmptyDatasetQualifiers(restore_element);

  // The base restore clears the layer stack before replay: this snapshot puts the widget back on a late failure.
  // A clipboard paste has no MainWindow-level transaction around it.
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

std::optional<MediaFrame> Scene2DDockWidget::takeCompositeFrameForTesting() {
  return composite_ != nullptr ? composite_->takeFrame() : std::nullopt;
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

  // A stack, not an overlay: a raster widget over the QRhiWidget's surface does not composite reliably.
  view_stack_ = new QStackedWidget(container);
  layout->addWidget(view_stack_, /*stretch=*/1);
  view_stack_->addWidget(makeEmptyPlaceholder(view_stack_));

  // Parentless on purpose: Qt switches a shown raster window to QRhi composition
  // only when a QRhiWidget is reparented into it, and one constructed with its
  // parent is not a QRhiWidget yet when QWidget's constructor sets it ("QRhiWidget:
  // No QRhi"). It also lets setApi() finish before the widget is in a visible
  // hierarchy, after which the API is immutable.
  viewer_ = new MediaViewerWidget();
  viewer_->setObjectName(u"scene2dMediaViewer"_s);
  viewer_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  connect(viewer_, &MediaViewerWidget::viewInteractionCommitted, this, [this]() { notifyWorkspaceChanged(); });
  connect(viewer_, &MediaViewerWidget::rotationChanged, this, &Scene2DDockWidget::viewRotationChanged);
  view_stack_->addWidget(viewer_);

  // Below the viewer, not over it, for the same compositing reason. Shown while the viewer cannot render.
  QLabel* rendering_warning = PJ::createRenderingWarningLabel(container, u"scene2dRenderingWarning"_s);
  layout->addWidget(rendering_warning);
  connect(
      viewer_, &MediaViewerWidget::renderingWarningChanged, rendering_warning,
      [rendering_warning](const QString& warning) {
        rendering_warning->setText(warning);
        rendering_warning->setVisible(!warning.isEmpty());
      });
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

namespace {

/// The image interval around the entry at `entry_ts` on `topic`: the median positive gap among its last 16 entries at
/// or before it, 100 ms when fewer than two gaps are known.
int64_t imageCadence(const ObjectStore& store, ObjectTopicId topic, int64_t entry_ts) {
  constexpr int64_t kFallback = 100'000'000;
  const EntryTimestampsView timestamps = store.entryTimestamps(topic);
  const Timestamp* end = std::upper_bound(timestamps.begin(), timestamps.end(), entry_ts);
  const Timestamp* begin = end - std::min<std::ptrdiff_t>(end - timestamps.begin(), 16);
  std::vector<int64_t> gaps;
  for (const Timestamp* at = begin; at + 1 < end; ++at) {
    if (at[1] > at[0]) {
      gaps.push_back(saturatingSub(at[1], at[0]));
    }
  }
  if (gaps.empty()) {
    return kFallback;
  }
  const auto middle = gaps.begin() + static_cast<std::ptrdiff_t>(gaps.size() / 2);
  std::nth_element(gaps.begin(), middle, gaps.end());
  return *middle;
}

/// Sets `layer`'s annotation warning from the event loop: the composite is polled inside the viewer's render, and a
/// clear made later must queue behind a set made earlier.
void postAnnotationWarning(const QPointer<Scene2DLayer>& layer, const QString& text) {
  if (layer) {
    QMetaObject::invokeMethod(
        layer.data(),
        [layer, text] {
          if (layer) {
            layer->setAnnotationWarning(text);
          }
        },
        Qt::QueuedConnection);
  }
}

}  // namespace

// Only an on-demand derived topic's annotations name the image they were made from; a parser's `image_topic` is
// informational (a header frame id), so those are shown as they are. The warning reaches the layer row once per change.
void Scene2DDockWidget::setLayerPairing(
    CompositeMediaSource& composite, SessionManager& session, ObjectTopicId topic, const void* identity,
    Scene2DLayer* layer) {
  const ObjectStore* store = &session.objectStore();
  QPointer<Scene2DLayer> guarded(layer);
  auto last = std::make_shared<std::optional<std::string>>();  // empty: nothing delivered yet, so the first is
  const ObjectTopicDescriptor& descriptor = store->descriptor(topic);
  composite.setLayerPairing(
      identity, descriptor.topic_name, isDerivedObjectTopic(descriptor),
      [store, topic](int64_t entry_ts) { return imageCadence(*store, topic, entry_ts); },
      [guarded, last](const std::string& warning) {
        if (*last != warning) {
          *last = warning;
          postAnnotationWarning(guarded, QString::fromStdString(warning));
        }
      });
}

void Scene2DDockWidget::syncViewLayers(const std::vector<ISceneLayer*>& ordered_layers) {
  auto next = std::make_unique<CompositeMediaSource>();
  composite_topic_order_.clear();

  for (ISceneLayer* layer : ordered_layers) {
    if (layer == nullptr) {
      continue;
    }
    const auto info = layer->info();
    auto* scene2d_layer = dynamic_cast<Scene2DLayer*>(layer);
    if (scene2d_layer == nullptr) {
      continue;
    }
    // Tagged with the underlying source, so adoptContributions() below can carry its last frame over.
    MediaSource* underlying = scene2d_layer->mediaSource();
    if (!info.visible || underlying == nullptr) {
      postAnnotationWarning(QPointer<Scene2DLayer>(scene2d_layer), QString());  // left out, it hides nothing
      continue;
    }
    next->addLayer(std::make_unique<BorrowedMediaSource>(underlying), 1.0f, underlying);
    if (SessionManager* session = sessionManager()) {
      setLayerPairing(*next, *session, info.topic_id, underlying, scene2d_layer);
    }
    composite_topic_order_.push_back(info.topic_id);
  }

  // Without the carry-over the viewer shows black after an add, remove or hide until each source re-decodes.
  if (composite_ != nullptr) {
    next->adoptContributions(*composite_);
  }

  // Repoint the viewer before the previous composite is freed: it must never hold a dangling MediaSource.
  auto previous = std::move(composite_);
  composite_ = std::move(next);
  if (viewer_ != nullptr) {
    viewer_->setMediaSource(composite_.get());
  }
  syncCompositeTimestamp(ordered_layers);
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
    return;  // no saved kind: first-sample encoding dispatch decides
  }

  const ImageLayerKind kind =
      saved_kind == QString::fromLatin1(kDepthKind) ? ImageLayerKind::kDepth : ImageLayerKind::kImage;
  const std::optional<SavedLayerHeader> header =
      parseSavedLayerHeader(layer_element, SavedTopicRef::kOptional, layerFactory());
  if (!header.has_value()) {
    return;  // the base restore refuses it
  }
  const std::string topic_name = header->topic_name.toStdString();
  const ObjectStore& store = sessionManager()->objectStore();
  if (!header->qualified) {
    // Primed before the restore picks the dock's dataset, so every candidate the base restore could bind
    // (uniqueObjectTopic) gets the kind; only the bound one reads it.
    if (header->object_type.has_value()) {
      for (const ObjectTopicId candidate : objectTopicsMatching(store, topic_name, *header->object_type)) {
        restore_image_layer_kinds_.insert_or_assign(topicKey(candidate), kind);
      }
    }
    return;
  }
  const auto dataset_id = sessionManager()
                              ->resolveObjectDatasetIdentity(
                                  header->dataset_id, header->dataset_source, header->dataset_path, header->topic_name)
                              .id;
  if (!dataset_id.has_value()) {
    return;
  }
  const auto topic_id = store.findTopic(*dataset_id, topic_name);
  if (topic_id.has_value()) {
    restore_image_layer_kinds_.insert_or_assign(topicKey(*topic_id), kind);
  }
}

bool Scene2DDockWidget::restoreOnePending(const QDomElement& element) {
  // A deferred drop (advertised topic, no data yet) gets tryAcceptObjectTopic's
  // one-background policy; saved layout layers keep appending.
  const std::optional<SavedLayerHeader> header =
      parseSavedLayerHeader(element, SavedTopicRef::kOptional, layerFactory());
  const bool dropped_background = header.has_value() && header->pending_intent && header->object_type.has_value() &&
                                  isBackgroundType(*header->object_type);
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

  const std::optional<int64_t> edge = visibleLayersLiveEdge();
  if (!edge) {
    return;
  }
  const int64_t latest = *edge;

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
  // The rewrapped sources still dedup on the timestamp: without invalidate() the re-seed at an unchanged time
  // decodes nothing and the frame stays stale until the tracker moves.
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
