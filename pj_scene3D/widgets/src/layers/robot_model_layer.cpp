// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/layers/robot_model_layer.h"

#include <QComboBox>
#include <QDir>
#include <QDomElement>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLoggingCategory>
#include <QPalette>
#include <QPointer>
#include <QSettings>
#include <QSignalBlocker>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariant>
#include <any>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "layer_xml_validation.h"
#include "mesh_load_set.h"
#include "mesh_loader.h"
#include "pj_base/builtin/robot_description.hpp"
#include "pj_base/time.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/robot_model_bridges.h"
#include "pj_scene3d_widgets/object_topic_metadata.h"
#include "pj_scene3d_widgets/parse_locked.h"
#include "pj_scene3d_widgets/passes/mesh_render_pass.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"
#include "pj_widgets/CheckButton.h"
#include "pj_widgets/ColorPickerWidget.h"
#include "pj_widgets/ComboBox.h"
#include "pj_widgets/FileDialog.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/SvgUtil.h"
#include "urdf_package_resolver.h"
#include "urdf_parser.h"
#include "url_fetcher.h"
using namespace Qt::StringLiterals;

namespace pj::scene3d {
namespace {
Q_LOGGING_CATEGORY(lcRobotModelLayer, "pj.scene3d.layer.robot_model")

constexpr auto kLatchRetryInterval = std::chrono::milliseconds(500);

QString sourceTypeToString(RobotModelLayer::SourceType type) {
  switch (type) {
    case RobotModelLayer::SourceType::kFile:
      return u"file"_s;
    case RobotModelLayer::SourceType::kUrl:
      return u"url"_s;
    case RobotModelLayer::SourceType::kTopic:
    default:
      return u"topic"_s;
  }
}

RobotModelLayer::SourceType sourceTypeFromString(const QString& s) {
  if (s == "file"_L1) {
    return RobotModelLayer::SourceType::kFile;
  }
  if (s == "url"_L1) {
    return RobotModelLayer::SourceType::kUrl;
  }
  return RobotModelLayer::SourceType::kTopic;
}

QString displayModeToString(RobotModelLayer::DisplayMode mode) {
  switch (mode) {
    case RobotModelLayer::DisplayMode::kVisual:
      return u"visual"_s;
    case RobotModelLayer::DisplayMode::kCollision:
      return u"collision"_s;
    case RobotModelLayer::DisplayMode::kAuto:
    default:
      return u"auto"_s;
  }
}

RobotModelLayer::DisplayMode displayModeFromString(const QString& s) {
  if (s == "visual"_L1) {
    return RobotModelLayer::DisplayMode::kVisual;
  }
  if (s == "collision"_L1) {
    return RobotModelLayer::DisplayMode::kCollision;
  }
  return RobotModelLayer::DisplayMode::kAuto;
}

QString datasetSource(PJ::SessionManager& session, PJ::DatasetId dataset_id) {
  const PJ::DatasetInfo* info = session.dataEngine().getDataset(dataset_id);
  return info != nullptr ? QString::fromStdString(info->source_name) : QString();
}

void addObjectTopicToCombo(QComboBox* combo, PJ::ObjectTopicId topic_id, const PJ::ObjectTopicDescriptor& desc) {
  combo->addItem(QString::fromStdString(desc.topic_name), QVariant::fromValue(static_cast<uint>(topic_id.id)));
}

std::optional<PJ::ObjectTopicId> currentObjectTopicId(const QComboBox* combo) {
  if (combo == nullptr || !combo->isEnabled() || combo->currentIndex() < 0) {
    return std::nullopt;
  }
  bool ok = false;
  const uint id = combo->currentData().toUInt(&ok);
  if (!ok || id == 0) {
    return std::nullopt;
  }
  return PJ::ObjectTopicId{.id = static_cast<uint32_t>(id)};
}

QString formatFromXml(const QString& text) {
  QDomDocument doc;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  const auto result = doc.setContent(text.toUtf8());
  if (!result) {
    return u"unknown"_s;
  }
#else
  if (!doc.setContent(text)) {
    return u"unknown"_s;
  }
#endif
  const QString root = doc.documentElement().tagName();
  return root == "robot"_L1 ? u"urdf"_s : root;
}

std::optional<QString> readTextFile(const QString& path, QString* error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    if (error != nullptr) {
      *error = file.errorString();
    }
    return std::nullopt;
  }
  return QString::fromUtf8(file.readAll());
}

QString urlDirectory(const QString& url_text) {
  QUrl url(url_text);
  QString path = url.path();
  const qsizetype slash = path.lastIndexOf('/');
  if (slash >= 0) {
    path = path.left(slash + 1);
  }
  url.setPath(path);
  url.setQuery(QString());
  url.setFragment(QString());
  return url.toString(QUrl::RemoveQuery | QUrl::RemoveFragment);
}

glm::vec3 toVec3(const glm::dvec3& v) {
  return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

}  // namespace

RobotModelLayer::RobotModelLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent)
    : Scene3DLayer(parent),
      topic_id_(topic_id),
      source_topic_id_(topic_id),
      display_name_(std::move(display_name)),
      source_value_(display_name_),
      owned_resolver_(std::make_unique<UrdfPackageResolver>()),
      mesh_loader_(std::make_unique<MeshLoader>()),
      mesh_pass_(std::make_unique<MeshRenderPass>()),
      mesh_loads_(std::make_unique<MeshLoadSet>()) {
  resolver_ = owned_resolver_.get();
}

RobotModelLayer::~RobotModelLayer() = default;

PJ::SceneLayerInfo RobotModelLayer::info() const {
  return PJ::SceneLayerInfo{
      .topic_id = topic_id_,
      .object_type = PJ::sdk::BuiltinObjectType::kRobotDescription,
      .display_name = display_name_.isEmpty() ? tr("Robot model") : display_name_,
      .family_name = u"RobotModel"_s,
      .visible = visible_,
  };
}

PJ::Range<PJ::Timepoint> RobotModelLayer::timeRange() const {
  return {PJ::Timepoint::max(), PJ::Timepoint::min()};
}

QStringList RobotModelLayer::fallbackFrames() const {
  QStringList out;
  if (!model_.has_value()) {
    return out;
  }
  for (const RobotLink& link : model_->links) {
    out.push_back(linkFrameName(link.name));
  }
  return out;
}

QString RobotModelLayer::sourceFrame() const {
  if (!model_.has_value() || model_->root_link.empty()) {
    return {};
  }
  return linkFrameName(model_->root_link);
}

QDomElement RobotModelLayer::xmlSaveState(QDomDocument& doc) const {
  QDomElement el = doc.createElement(u"robot_model"_s);
  el.setAttribute(u"source_type"_s, sourceTypeToString(source_type_));
  el.setAttribute(u"source_value"_s, source_value_);
  // For a topic source, source_value_ is only a display string. Persist the
  // resolvable identity (dataset_id + topic_name) so restore re-binds the SAME
  // topic even when the user switched the config combo to a different one
  // (source_topic_id_ != the constructor's topic_id_).
  if (source_type_ == SourceType::kTopic && ctx_.session != nullptr) {
    const auto desc = ctx_.session->objectStore().descriptor(source_topic_id_);
    el.setAttribute(u"source_topic_name"_s, QString::fromStdString(desc.topic_name));
    el.setAttribute(u"source_dataset_id"_s, static_cast<uint>(desc.dataset_id));
    el.setAttribute(u"source_dataset_source"_s, datasetSource(*ctx_.session, desc.dataset_id));
    const QString path = ctx_.session->datasetSourcePath(desc.dataset_id);
    if (!path.isEmpty()) {
      el.setAttribute(u"source_dataset_path"_s, path);
    }
  }
  el.setAttribute(u"frame_prefix"_s, frame_prefix_);
  el.setAttribute(u"display_mode"_s, displayModeToString(display_mode_));
  el.setAttribute(u"visible"_s, visible_ ? u"true"_s : u"false"_s);
  el.setAttribute(u"color"_s, fallback_color_.name(QColor::HexRgb));
  el.setAttribute(u"ignore_collada_up_axis"_s, ignore_collada_up_axis_ ? u"true"_s : u"false"_s);
  return el;
}

bool RobotModelLayer::xmlLoadState(const QDomElement& element) {
  return xmlLoadStateResult(element) == XmlLoadResult::kRestored;
}

RobotModelLayer::XmlLoadResult RobotModelLayer::xmlLoadStateResult(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "robot_model"_L1 || !detail::isLeafPayload(element)) {
    return XmlLoadResult::kInvalid;
  }
  const QString source_type_text = element.attribute(u"source_type"_s, u"topic"_s);
  if (source_type_text != "topic"_L1 && source_type_text != "file"_L1 && source_type_text != "url"_L1) {
    return XmlLoadResult::kInvalid;
  }
  const SourceType restored_source_type = sourceTypeFromString(source_type_text);
  const QString display_mode_text = element.attribute(u"display_mode"_s, u"auto"_s);
  if (display_mode_text != "auto"_L1 && display_mode_text != "visual"_L1 && display_mode_text != "collision"_L1) {
    return XmlLoadResult::kInvalid;
  }
  bool restored_visible = true;
  bool restored_ignore_up_axis = false;
  if (!detail::parseTrueFalse(element, "visible", true, restored_visible) ||
      !detail::parseTrueFalse(element, "ignore_collada_up_axis", false, restored_ignore_up_axis)) {
    return XmlLoadResult::kInvalid;
  }
  QColor restored_color = fallback_color_;
  if (element.hasAttribute(u"color"_s)) {
    restored_color = QColor(element.attribute(u"color"_s));
    if (!restored_color.isValid()) {
      return XmlLoadResult::kInvalid;
    }
  }
  const bool has_topic_name = element.hasAttribute(u"source_topic_name"_s);
  const bool has_dataset_id = element.hasAttribute(u"source_dataset_id"_s);
  const bool has_dataset_source = element.hasAttribute(u"source_dataset_source"_s);
  const bool has_dataset_path = element.hasAttribute(u"source_dataset_path"_s);
  const bool has_topic_identity = has_topic_name || has_dataset_id || has_dataset_source || has_dataset_path;
  if ((restored_source_type != SourceType::kTopic && has_topic_identity) ||
      (!has_topic_name && (has_dataset_id || has_dataset_source || has_dataset_path))) {
    return XmlLoadResult::kInvalid;
  }
  PJ::DatasetId saved_dataset_id = 0;
  if (has_dataset_id) {
    bool id_ok = false;
    const qulonglong value = element.attribute(u"source_dataset_id"_s).toULongLong(&id_ok);
    if (!id_ok || value == 0 || value > std::numeric_limits<uint32_t>::max()) {
      return XmlLoadResult::kInvalid;
    }
    saved_dataset_id = static_cast<PJ::DatasetId>(value);
  }
  const QString topic_name = element.attribute(u"source_topic_name"_s);
  if (has_topic_name && topic_name.isEmpty()) {
    return XmlLoadResult::kInvalid;
  }
  std::optional<PJ::ObjectTopicId> restored_topic;
  if (restored_source_type == SourceType::kTopic && ctx_.session != nullptr && has_topic_name) {
    const QString source = element.attribute(u"source_dataset_source"_s);
    const QString path = element.attribute(u"source_dataset_path"_s);
    if (!has_dataset_id && source.isEmpty() && path.isEmpty()) {
      const UniqueObjectTopicResolution generic = resolveUniqueObjectTopic(
          ctx_.session->objectStore(), topic_name.toStdString(), PJ::sdk::BuiltinObjectType::kRobotDescription);
      if (generic.ambiguous) {
        return XmlLoadResult::kInvalid;
      }
      restored_topic = generic.topic_id;
    } else {
      const PJ::DatasetIdentityResolution dataset =
          ctx_.session->resolveDatasetIdentity(saved_dataset_id, source, path);
      if (dataset.ambiguous) {
        return XmlLoadResult::kInvalid;
      }
      if (dataset.id.has_value()) {
        restored_topic = ctx_.session->objectStore().findTopic(*dataset.id, topic_name.toStdString());
      }
    }
    if (!restored_topic.has_value()) {
      return XmlLoadResult::kDeferred;
    }
    const PJ::ObjectTopicDescriptor& descriptor = ctx_.session->objectStore().descriptor(*restored_topic);
    const PJ::sdk::BuiltinObjectType live_type = builtinObjectTypeFor(descriptor);
    if (live_type != PJ::sdk::BuiltinObjectType::kNone && live_type != PJ::sdk::BuiltinObjectType::kRobotDescription) {
      return XmlLoadResult::kInvalid;
    }
  }

  const SourceType old_source_type = source_type_;
  const QString old_source_value = source_value_;
  const uint32_t old_topic_id = source_topic_id_.id;
  const QString old_prefix = frame_prefix_;
  const DisplayMode old_mode = display_mode_;
  const bool old_visible = visible_;
  const QRgb old_color = fallback_color_.rgb();
  const bool old_ignore_up_axis = ignore_collada_up_axis_;
  source_type_ = restored_source_type;
  source_value_ = element.attribute(u"source_value"_s, source_value_);
  frame_prefix_ = element.attribute(u"frame_prefix"_s);
  display_mode_ = displayModeFromString(display_mode_text);
  visible_ = restored_visible;
  fallback_color_ = restored_color;
  ignore_collada_up_axis_ = restored_ignore_up_axis;
  draws_dirty_ = true;
  bool source_setter_emitted = false;
  if (restored_topic.has_value()) {
    source_setter_emitted = source_topic_id_.id != restored_topic->id;
    setSourceTopic(*restored_topic);
  } else if (ctx_.session != nullptr) {
    loadFromCurrentSource();
  }
  const bool changed = old_source_type != source_type_ || old_source_value != source_value_ ||
                       old_topic_id != source_topic_id_.id || old_prefix != frame_prefix_ ||
                       old_mode != display_mode_ || old_visible != visible_ || old_color != fallback_color_.rgb() ||
                       old_ignore_up_axis != ignore_collada_up_axis_;
  if (changed && !source_setter_emitted) {
    emit configurationChanged();
  }
  emit infoChanged();
  emit visibilityChanged(visible_);
  emit repaintRequested();
  return XmlLoadResult::kRestored;
}

bool RobotModelLayer::attach(const PJ::SceneLayerContext& ctx) {
  const auto& scene3d_ctx = static_cast<const Scene3DLayerContext&>(ctx);
  ctx_ = scene3d_ctx;
  if (source_type_ == SourceType::kTopic) {
    if (ctx_.session == nullptr) {
      qCWarning(lcRobotModelLayer) << "attach: session is null";
      return false;
    }
    // Existence check only — the binding is re-resolved per decode (see
    // tryLoadTopicDescription) so a reload that swaps the parser slot rebinds us.
    if (!ctx_.session->parserBindingForObjectTopic(source_topic_id_)) {
      qCWarning(lcRobotModelLayer) << "attach: no parser for robot-description topic" << source_topic_id_.id;
      return false;
    }
    const auto desc = ctx_.session->objectStore().descriptor(source_topic_id_);
    if (source_value_.isEmpty()) {
      source_value_ = QString::fromStdString(desc.topic_name);
    }
  }
  loadFromCurrentSource();
  return true;
}

void RobotModelLayer::detach() {
  // Abort any in-flight URL fetch: a detached layer must not apply late results.
  ++url_fetch_generation_;
  url_fetcher_.reset();
  model_.reset();
  static_bridges_.clear();
  model_has_visuals_ = false;
  mesh_loads_->clear();
  cached_visual_draws_.clear();
  cached_collision_draws_.clear();
  draws_dirty_ = true;
  cached_render_origin_.reset();
  if (mesh_pass_) {
    mesh_pass_->clearMeshes();
  }
  ctx_ = {};
}

void RobotModelLayer::setFixedFrame(const QString& frame) {
  fixed_frame_ = frame;
  draws_dirty_ = true;
  emit repaintRequested();
}

void RobotModelLayer::setTrackerTime(PJ::Timepoint time) {
  tracker_time_ = time;
  // TF reaches this layer ONLY via tracker ticks (the dock drives setTrackerTime
  // on every live ingest tick and every scrub), so this is where TF-driven pose
  // changes invalidate the draw cache.
  draws_dirty_ = true;
  if (source_type_ == SourceType::kTopic && latch_pending_) {
    const auto now = std::chrono::steady_clock::now();
    if (last_latch_retry_ == std::chrono::steady_clock::time_point{} ||
        now - last_latch_retry_ >= kLatchRetryInterval) {
      last_latch_retry_ = now;
      tryLoadTopicDescription();
    }
  }
  emit repaintRequested();
}

void RobotModelLayer::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  if (visible) {
    // The cache may predate the hide (TF advanced while we skipped render).
    draws_dirty_ = true;
  }
  emit visibilityChanged(visible);
  emit infoChanged();
  emit repaintRequested();
}

void RobotModelLayer::initializeGL() {
  if (mesh_pass_) {
    mesh_pass_->initializeGL();
  }
}

void RobotModelLayer::rebuildDrawCache(const FrameContext& frame_ctx) {
  // Reuse the member vectors (clear, never realloc fresh locals) so a steady
  // robot does not churn the heap every time the cache invalidates.
  cached_visual_draws_.clear();
  cached_collision_draws_.clear();

  // Bridge the URDF's fixed joints into the buffer we actually pose links against:
  // frame_ctx.tf is the dock's LIVE buffer, whereas ctx_.tf_buffer is a snapshot
  // taken at attach that can be null/stale if the layer attached before the
  // dataset bound (or was rebound on reload). Guarded + idempotent, so re-running
  // every rebuild is cheap and self-heals after a buffer clear. const_cast is safe
  // here: TransformBuffer is internally synchronized and rebuildDrawCache runs on
  // the GUI thread; FrameContext only hands the buffer to passes read-only.
  ensureStaticBridges(const_cast<TransformBuffer&>(frame_ctx.tf));

  const glm::vec4 placeholder_color{1.0f, 0.0f, 1.0f, 1.0f};

  auto append_geom = [&](const LinkGeom& geom, const glm::mat4& link_model, bool collision) {
    MeshRenderPass::DrawCall draw;
    draw.model = link_model * glm::mat4(originToMat4(geom.origin_xyz, geom.origin_rpy));
    if (geom.has_color) {
      draw.use_vertex_color = false;
      draw.color = geom.color;
    } else if (collision) {
      // Collision geometry with no <material>: orange tint so hulls read as
      // distinct from the visual meshes (RViz convention).
      draw.use_vertex_color = false;
      draw.color = look::kCollisionDefaultColor;
    } else {
      draw.use_vertex_color = true;
      draw.color = glm::vec4(1.0f);
    }

    if (const auto* box = std::get_if<GeomBox>(&geom.shape); box != nullptr) {
      draw.kind = MeshRenderPass::GeometryKind::kBox;
      draw.model = glm::scale(draw.model, toVec3(box->size));
      draw.use_vertex_color = false;
    } else if (const auto* cylinder = std::get_if<GeomCylinder>(&geom.shape); cylinder != nullptr) {
      draw.kind = MeshRenderPass::GeometryKind::kCylinder;
      draw.model = glm::scale(
          draw.model, glm::vec3(
                          static_cast<float>(cylinder->radius), static_cast<float>(cylinder->radius),
                          static_cast<float>(cylinder->length)));
      draw.use_vertex_color = false;
    } else if (const auto* sphere = std::get_if<GeomSphere>(&geom.shape); sphere != nullptr) {
      draw.kind = MeshRenderPass::GeometryKind::kSphere;
      draw.model = glm::scale(draw.model, glm::vec3(static_cast<float>(sphere->radius)));
      draw.use_vertex_color = false;
    } else if (const auto* mesh = std::get_if<GeomMesh>(&geom.shape); mesh != nullptr) {
      draw.model = glm::scale(draw.model, toVec3(mesh->scale));
      if (mesh->resolved && meshReady(mesh->resolved_path)) {
        draw.kind = MeshRenderPass::GeometryKind::kMesh;
        draw.mesh_key = mesh->resolved_path;
      } else {
        draw.kind = MeshRenderPass::GeometryKind::kPlaceholderCube;
        draw.color = placeholder_color;
        draw.use_vertex_color = false;
      }
    }

    if (collision) {
      cached_collision_draws_.push_back(std::move(draw));
    } else {
      cached_visual_draws_.push_back(std::move(draw));
    }
  };

  // frame_prefix_ is constant across this rebuild; convert it once instead of
  // a QString concat + toStdString round-trip per link. rebuildDrawCache runs
  // for every link on every draws_dirty_ rebuild (i.e. every tracker tick during
  // playback), so the empty-prefix common case looks up link.name with no alloc.
  const std::string frame_prefix = frame_prefix_.toStdString();
  for (const RobotLink& link : model_->links) {
    const auto tf = frame_prefix.empty() ? frame_ctx.lookup(link.name) : frame_ctx.lookup(frame_prefix + link.name);
    if (!tf.has_value()) {
      continue;
    }
    const glm::mat4 link_model = glm::mat4(tf->matrix());

    if (display_mode_ == DisplayMode::kCollision) {
      for (const LinkGeom& geom : link.collisions) {
        append_geom(geom, link_model, true);
      }
    } else if (display_mode_ == DisplayMode::kVisual) {
      for (const LinkGeom& geom : link.visuals) {
        append_geom(geom, link_model, false);
      }
    } else {
      // kAuto: render a link's visuals when it has them. A collision-only link is
      // promoted to the visuals group (rendered solid) ONLY when the whole model
      // has no visuals (review L.21 — an all-collision URDF must not ghost at the
      // collision opacity). In a MIXED model (this one has visual links), a
      // collision-only link is auxiliary geometry — e.g. the self-collision
      // capsules of panda_link*_sc — so it renders in the COLLISION group and
      // obeys the Collision opacity/visibility toggle instead of overlaying the
      // visuals as an unhideable solid.
      if (!link.visuals.empty()) {
        for (const LinkGeom& geom : link.visuals) {
          append_geom(geom, link_model, false);
        }
      } else {
        for (const LinkGeom& geom : link.collisions) {
          append_geom(geom, link_model, /*collision=*/model_has_visuals_);
        }
      }
    }
  }
  cached_render_origin_ = frame_ctx.render_origin;
}

bool RobotModelLayer::drawCacheNeedsRebuild(const FrameContext& frame_ctx) const {
  if (draws_dirty_ || !cached_render_origin_.has_value()) {
    return true;
  }
  const glm::dvec3 delta = *cached_render_origin_ - frame_ctx.render_origin;
  return delta.x != 0.0 || delta.y != 0.0 || delta.z != 0.0;
}

void RobotModelLayer::render(const ViewParams& view_params, const FrameContext& frame_ctx) {
  if (!visible_ || !mesh_pass_ || !model_.has_value()) {
    return;
  }

  // DrawCall matrices are in camera-relative render space, so the cache is
  // rebuilt when draws_dirty_ OR the render origin moved (pan / zoom-to-cursor /
  // follow); a pure view/projection repaint reuses it. Shared with the shadow
  // hooks via ensureDrawCache (which also drains finished mesh loads) so a frame's
  // pre-pass and color pass use one list built from the same geometry.
  ensureDrawCache(frame_ctx);

  // Per-view opacities (Part C "Meshes"/"Collision" sliders); 0 hides the group
  // entirely. These gates stay per-frame — only the draw list is cached. The
  // per-layer DisplayMode stays the structural override.
  const MeshShadingParams& shading = view_params.shading;
  if (shading.meshes_visible && shading.mesh_opacity > 0.0f) {
    mesh_pass_->renderVisuals(view_params, cached_visual_draws_, shading.mesh_opacity);
  }
  if (shading.collisions_visible && shading.collision_opacity > 0.0f) {
    mesh_pass_->renderCollisions(view_params, cached_collision_draws_, shading.collision_opacity);
  }
}

void RobotModelLayer::ensureDrawCache(const FrameContext& frame_ctx) {
  // Drain any finished async mesh loads FIRST, so the shadow pre-pass and the color
  // pass in the same frame agree on the geometry: without this a future completing
  // between the shadow hooks and render() would cast placeholder cubes but draw the
  // real mesh. drain() is idempotent (changed == false on a second call this frame),
  // so the redundant poll across the three callers is cheap.
  pollMeshLoads();
  if (drawCacheNeedsRebuild(frame_ctx)) {
    rebuildDrawCache(frame_ctx);
    draws_dirty_ = false;
  }
}

std::optional<AABB> RobotModelLayer::meshShadowBounds(const FrameContext& frame_ctx) {
  if (!visible_ || !mesh_pass_ || !model_.has_value()) {
    return std::nullopt;
  }
  ensureDrawCache(frame_ctx);
  const AABB bounds = mesh_pass_->worldBoundsOfDraws(cached_visual_draws_);
  return bounds.valid ? std::optional<AABB>(bounds) : std::nullopt;
}

void RobotModelLayer::renderShadowCasters(const glm::mat4& light_view_proj, const FrameContext& frame_ctx) {
  if (!visible_ || !mesh_pass_ || !model_.has_value()) {
    return;
  }
  // Visual links only: collision hulls coincide with the visuals and would
  // double-darken the silhouette. The "meshes hidden" per-view toggle is not
  // consulted here (the hook has no ViewParams); a hidden mesh still casting is an
  // accepted v1 edge case.
  ensureDrawCache(frame_ctx);
  mesh_pass_->renderDepthOnly(light_view_proj, cached_visual_draws_);
}

void RobotModelLayer::releaseGL() {
  if (mesh_pass_) {
    mesh_pass_->releaseGL();
  }
}

QWidget* RobotModelLayer::createConfigWidget(QWidget* parent) {
  auto* container = new QWidget(parent);
  auto* outer = new QVBoxLayout(container);
  outer->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
  outer->setSpacing(PJ::theme::space(PJ::theme::Space::Snug));

  auto* form = new QFormLayout();
  form->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
  // Match the Grid / Transforms section grids in Scene3DConfigPanel: comfortable
  // label↔field gap, snug row pitch — so the panel keeps one consistent rhythm.
  form->setHorizontalSpacing(PJ::theme::space(PJ::theme::Space::Comfortable));
  form->setVerticalSpacing(PJ::theme::space(PJ::theme::Space::Snug));
  outer->addLayout(form);

  auto* source_combo = new PJ::ComboBox(container);
  source_combo->addItem(tr("Topic"), static_cast<int>(SourceType::kTopic));
  source_combo->addItem(tr("File"), static_cast<int>(SourceType::kFile));
  source_combo->addItem(tr("URL"), static_cast<int>(SourceType::kUrl));
  if (const int idx = source_combo->findData(static_cast<int>(source_type_)); idx >= 0) {
    source_combo->setCurrentIndex(idx);
  }
  form->addRow(tr("Source"), source_combo);

  auto* topic_combo = new PJ::ComboBox(container);
  if (ctx_.session != nullptr) {
    PJ::ObjectStore& store = ctx_.session->objectStore();
    for (const PJ::ObjectTopicId topic_id : store.listTopics()) {
      const PJ::ObjectTopicDescriptor& desc = store.descriptor(topic_id);
      if (builtinObjectTypeFor(desc) == PJ::sdk::BuiltinObjectType::kRobotDescription) {
        addObjectTopicToCombo(topic_combo, topic_id, desc);
      }
    }
  }
  if (topic_combo->count() == 0) {
    topic_combo->addItem(tr("No robot description topic in this dataset"), QVariant::fromValue(0U));
    QFont italic = topic_combo->font();
    italic.setItalic(true);
    topic_combo->setItemData(0, italic, Qt::FontRole);
    topic_combo->setEnabled(false);
  } else {
    int idx = topic_combo->findData(QVariant::fromValue(static_cast<uint>(source_topic_id_.id)));
    if (idx < 0) {
      idx = topic_combo->findText(u"/robot_description"_s);
    }
    topic_combo->setCurrentIndex(idx >= 0 ? idx : 0);
  }
  form->addRow(QString(), topic_combo);

  auto* file_row = new QWidget(container);
  auto* file_layout = new QHBoxLayout(file_row);
  file_layout->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
  // Shows only the file name (full path lives in source_value_ / the tooltip); the
  // field is read-only — Browse is the way to change it.
  auto* file_edit =
      new QLineEdit(source_type_ == SourceType::kFile ? QFileInfo(source_value_).fileName() : QString(), file_row);
  file_edit->setReadOnly(true);
  file_edit->setToolTip(source_type_ == SourceType::kFile ? source_value_ : QString());
  // Themed icon buttons, matching the app's chrome (resources are registered
  // process-wide by pj_app; the LayerListView eye/trash rows are the pattern).
  const QString icon_theme = QGuiApplication::palette().color(QPalette::Window).valueF() < 0.5 ? u"dark"_s : u"light"_s;
  auto* browse_button = new QToolButton(file_row);
  browse_button->setAutoRaise(true);
  browse_button->setFocusPolicy(Qt::NoFocus);
  browse_button->setToolTip(tr("Browse for a URDF file"));
  browse_button->setIcon(PJ::loadSvg(u":/resources/svg/folder_open.svg"_s, icon_theme));
  file_layout->addWidget(file_edit, 1);
  file_layout->addWidget(browse_button);
  form->addRow(QString(), file_row);

  auto* url_row = new QWidget(container);
  auto* url_layout = new QHBoxLayout(url_row);
  url_layout->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
  auto* url_edit = new QLineEdit(source_type_ == SourceType::kUrl ? source_value_ : QString(), url_row);
  auto* load_button = new QToolButton(url_row);
  load_button->setAutoRaise(true);
  load_button->setFocusPolicy(Qt::NoFocus);
  load_button->setToolTip(tr("Fetch the URDF from this URL"));
  load_button->setIcon(PJ::loadSvg(u":/resources/svg/import.svg"_s, icon_theme));
  url_layout->addWidget(url_edit, 1);
  url_layout->addWidget(load_button);
  form->addRow(QString(), url_row);

  auto* status_row = new QWidget(container);
  auto* status_layout = new QHBoxLayout(status_row);
  status_layout->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
  status_layout->setSpacing(PJ::theme::space(PJ::theme::Space::Snug));
  auto* status_label = new QLabel(status_text_, status_row);
  status_label->setWordWrap(true);
  status_label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  auto* locate_button = new QToolButton(status_row);
  locate_button->setText(tr("Locate..."));
  locate_button->setAutoRaise(true);
  auto* retry_button = new QToolButton(status_row);
  retry_button->setText(tr("Retry"));
  retry_button->setAutoRaise(true);
  status_layout->addWidget(status_label, 1);
  status_layout->addWidget(locate_button);
  status_layout->addWidget(retry_button);
  form->addRow(tr("Status"), status_row);

  auto* prefix_edit = new QLineEdit(frame_prefix_, container);
  prefix_edit->setPlaceholderText(tr("e.g. robot1/"));
  form->addRow(tr("Frame prefix"), prefix_edit);

  auto* mode_combo = new PJ::ComboBox(container);
  mode_combo->addItem(tr("Auto"), static_cast<int>(DisplayMode::kAuto));
  mode_combo->addItem(tr("Visual"), static_cast<int>(DisplayMode::kVisual));
  mode_combo->addItem(tr("Collision"), static_cast<int>(DisplayMode::kCollision));
  mode_combo->setCurrentIndex(static_cast<int>(display_mode_));
  form->addRow(tr("Display mode"), mode_combo);

  auto* color_button = new PJ::ColorPickerWidget(container);
  color_button->setColor(fallback_color_);
  form->addRow(tr("Color"), color_button);

  auto* collada_box = new PJ::CheckButton(tr("Ignore COLLADA up_axis"), container);
  collada_box->setChecked(ignore_collada_up_axis_);
  form->addRow(collada_box);

  auto* group = new QGroupBox(tr("Mesh resolution"), container);
  group->setCheckable(true);
  group->setChecked(false);
  auto* group_layout = new QVBoxLayout(group);
  auto* roots_list = new QListWidget(group);
  if (resolver_ != nullptr) {
    roots_list->addItems(resolver_->searchRoots());
  }
  group_layout->addWidget(roots_list);
  outer->addWidget(group);

  const auto refresh_roots_list = [this, roots_list]() {
    roots_list->clear();
    if (resolver_ != nullptr) {
      roots_list->addItems(resolver_->searchRoots());
    }
  };
  const auto apply_source_visibility = [this, topic_combo, file_row, url_row]() {
    topic_combo->setVisible(source_type_ == SourceType::kTopic);
    file_row->setVisible(source_type_ == SourceType::kFile);
    url_row->setVisible(source_type_ == SourceType::kUrl);
  };
  const auto refresh_status = [this, status_label, locate_button, retry_button]() {
    QString status = status_text_;
    if (total_mesh_count_ > 0) {
      const int resolved = total_mesh_count_ - unresolved_mesh_count_;
      const qsizetype split = status.indexOf(u"  •  "_s);
      const QString prefix = split > 0 ? status.left(split) : tr("URDF: %1").arg(source_value_);
      if (unresolved_mesh_count_ > 0) {
        status = tr("%1  •  %2/%3 meshes  •  %4 packages unresolved")
                     .arg(prefix)
                     .arg(resolved)
                     .arg(total_mesh_count_)
                     .arg(unresolvedPackagesList().size());
      } else {
        status = tr("%1  •  %2/%2 meshes").arg(prefix).arg(total_mesh_count_);
      }
      status += unresolvedIssueClause();
    }
    status_label->setText(status);
    locate_button->setVisible(!unresolvedPackagesList().isEmpty());
    retry_button->setVisible(!status_text_.isEmpty());
  };
  apply_source_visibility();
  refresh_status();

  connect(
      source_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
      [this, source_combo, topic_combo, file_edit, url_edit, apply_source_visibility, refresh_status](int) {
        const auto selected = static_cast<SourceType>(source_combo->currentData().toInt());
        source_type_ = selected;
        apply_source_visibility();
        if (selected == SourceType::kTopic) {
          if (const auto topic_id = currentObjectTopicId(topic_combo); topic_id.has_value()) {
            setSourceTopic(*topic_id, topic_combo->currentText());
          } else {
            setStatus(tr("No robot description topic in this dataset"));
          }
        } else if (selected == SourceType::kFile) {
          source_value_ = file_edit->text();
        } else {
          source_value_ = url_edit->text();
        }
        refresh_status();
      });
  connect(
      topic_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, topic_combo, refresh_status](int) {
        if (source_type_ != SourceType::kTopic) {
          return;
        }
        if (const auto topic_id = currentObjectTopicId(topic_combo); topic_id.has_value()) {
          setSourceTopic(*topic_id, topic_combo->currentText());
        }
        refresh_status();
      });
  connect(browse_button, &QToolButton::clicked, this, [this, container, file_edit, source_combo, refresh_status]() {
    // Remember the last-browsed folder in the app QSettings so reopening the
    // dialog lands where the user last picked a URDF (the current file wins if set).
    QSettings settings;
    const QString remembered = settings.value(u"pj_scene3d/urdf_browse_dir"_s).toString();
    const QString start_dir = !source_value_.isEmpty() ? source_value_ : remembered;
    const QString path = PJ::FileDialog::getOpenFileName(
        container, tr("Open URDF"), start_dir, tr("URDF files (*.urdf *.xml);;All files (*)"));
    if (path.isEmpty()) {
      return;
    }
    settings.setValue(u"pj_scene3d/urdf_browse_dir"_s, QFileInfo(path).absolutePath());
    file_edit->setText(QFileInfo(path).fileName());
    file_edit->setToolTip(path);
    if (const int idx = source_combo->findData(static_cast<int>(SourceType::kFile)); idx >= 0) {
      source_combo->setCurrentIndex(idx);
    }
    setSourceFile(path);
    refresh_status();
  });
  connect(load_button, &QToolButton::clicked, this, [this, url_edit, source_combo, refresh_status]() {
    if (const int idx = source_combo->findData(static_cast<int>(SourceType::kUrl)); idx >= 0) {
      source_combo->setCurrentIndex(idx);
    }
    setSourceUrl(url_edit->text());
    refresh_status();
  });
  connect(locate_button, &QToolButton::clicked, this, [this, container, refresh_roots_list, refresh_status]() {
    const QStringList packages = unresolvedPackagesList();
    if (packages.isEmpty() || resolver_ == nullptr) {
      return;
    }
    const QString root = PJ::FileDialog::getExistingDirectory(
        container, tr("Select the folder that contains your robot packages"), QString());
    if (root.isEmpty()) {
      return;
    }
    QStringList missing;
    for (const QString& package : packages) {
      const QString package_dir = QDir(root).filePath(package);
      if (QFileInfo(package_dir).isDir()) {
        resolver_->rememberPackageRoot(package.toStdString(), package_dir);
      } else {
        missing.push_back(package);
      }
    }
    refresh_roots_list();
    if (missing.size() == packages.size()) {
      setStatus(
          tr("Package '%1' not found under '%2' - expected a subdirectory named '%1'").arg(missing.first(), root));
      refresh_status();
      return;
    }
    loadFromCurrentSource();
    refresh_status();
  });
  connect(retry_button, &QToolButton::clicked, this, [this, refresh_status]() {
    loadFromCurrentSource();
    refresh_status();
  });
  connect(
      prefix_edit, &QLineEdit::editingFinished, this, [this, prefix_edit]() { setFramePrefix(prefix_edit->text()); });
  connect(mode_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, mode_combo](int) {
    setDisplayMode(static_cast<DisplayMode>(mode_combo->currentData().toInt()));
  });
  connect(color_button, &PJ::ColorPickerWidget::colorChanged, this, &RobotModelLayer::setFallbackColor);
  connect(collada_box, &PJ::CheckButton::toggled, this, &RobotModelLayer::setIgnoreColladaUpAxis);
  connect(this, &RobotModelLayer::statusTextChanged, container, [refresh_status](const QString&) { refresh_status(); });
  connect(this, &RobotModelLayer::meshLoadStatusChanged, container, [refresh_status](int, int, const QStringList&) {
    refresh_status();
  });

  return container;
}

void RobotModelLayer::setPackageResolver(UrdfPackageResolver* resolver) {
  resolver_ = resolver != nullptr ? resolver : owned_resolver_.get();
}

void RobotModelLayer::setSourceTopic(PJ::ObjectTopicId topic_id, QString display_name) {
  const bool changed = source_type_ != SourceType::kTopic || source_topic_id_.id != topic_id.id ||
                       (!display_name.isEmpty() && display_name_ != display_name);
  source_type_ = SourceType::kTopic;
  source_topic_id_ = topic_id;
  if (!display_name.isEmpty()) {
    display_name_ = std::move(display_name);
    source_value_ = display_name_;
  }
  if (ctx_.session != nullptr) {
    // Existence check only; the actual decode re-resolves the binding per use.
    if (!ctx_.session->parserBindingForObjectTopic(source_topic_id_)) {
      setStatus(tr("No parser for %1").arg(source_value_));
      return;
    }
    loadFromCurrentSource();
  }
  if (changed) {
    emit configurationChanged();
  }
}

void RobotModelLayer::setSourceFile(QString path) {
  if (source_type_ == SourceType::kFile && source_value_ == path) {
    return;
  }
  source_type_ = SourceType::kFile;
  source_value_ = std::move(path);
  loadFromCurrentSource();
  emit configurationChanged();
}

void RobotModelLayer::setSourceUrl(QString url) {
  if (source_type_ == SourceType::kUrl && source_value_ == url) {
    return;
  }
  source_type_ = SourceType::kUrl;
  source_value_ = std::move(url);
  loadFromCurrentSource();
  emit configurationChanged();
}

void RobotModelLayer::setFramePrefix(QString prefix) {
  if (frame_prefix_ == prefix) {
    return;
  }
  frame_prefix_ = std::move(prefix);
  draws_dirty_ = true;     // changes which TF frames each link resolves against
  rebuildStaticBridges();  // bridge frames carry the prefix too
  emit sourceFrameChanged(sourceFrame());
  emit fallbackFramesChanged(fallbackFrames());
  emit configurationChanged();
  emit repaintRequested();
}

void RobotModelLayer::rebuildStaticBridges() {
  static_bridges_.clear();
  if (!model_.has_value()) {
    return;
  }
  static_bridges_ = fixedJointStaticTransforms(*model_);
  if (frame_prefix_.isEmpty()) {
    return;
  }
  const std::string prefix = frame_prefix_.toStdString();
  for (StampedTransform& bridge : static_bridges_) {
    bridge.parent_frame = prefix + bridge.parent_frame;
    bridge.child_frame = prefix + bridge.child_frame;
  }
}

void RobotModelLayer::ensureStaticBridges(TransformBuffer& buf) {
  if (static_bridges_.empty()) {
    return;
  }
  injectMissingStaticTransforms(buf, static_bridges_);
}

void RobotModelLayer::setDisplayMode(DisplayMode mode) {
  if (display_mode_ == mode) {
    return;
  }
  display_mode_ = mode;
  draws_dirty_ = true;  // visuals/collisions selection changes the draw list
  emit configurationChanged();
  emit repaintRequested();
}

void RobotModelLayer::setFallbackColor(QColor color) {
  if (!color.isValid() || fallback_color_.rgb() == color.rgb()) {
    return;
  }
  fallback_color_ = std::move(color);
  draws_dirty_ = true;
  emit configurationChanged();
  emit repaintRequested();
}

void RobotModelLayer::setIgnoreColladaUpAxis(bool ignore) {
  if (ignore_collada_up_axis_ == ignore) {
    return;
  }
  ignore_collada_up_axis_ = ignore;
  // The flip is baked into the loaded MeshData, so the toggle must reload from
  // the current source. loadFromCurrentSource() clears the loader cache, so the
  // .dae meshes re-import with the new effective flip (it also re-resolves the
  // override per path in startMeshLoads).
  loadFromCurrentSource();
  emit configurationChanged();
}

QString RobotModelLayer::linkFrameName(const std::string& link_name) const {
  return frame_prefix_ + QString::fromStdString(link_name);
}

bool RobotModelLayer::loadFromCurrentSource() {
  // Any in-flight URL fetch is now stale: its result must not clobber the
  // newly-selected source when it lands.
  ++url_fetch_generation_;
  model_.reset();
  mesh_loads_->clear();
  // Stale draws must not survive a source switch; render() also guards on
  // model_, but rebuild on the next render once a new model lands. Covers
  // setIgnoreColladaUpAxis, attach, and the kUrl fetch-pending window.
  draws_dirty_ = true;
  total_mesh_count_ = 0;
  unresolved_mesh_count_ = 0;
  loaded_mesh_count_ = 0;
  latch_pending_ = false;
  if (mesh_loader_) {
    // Release the previous model's cached MeshData and make Retry/Locate
    // genuinely re-import (failed loads included) instead of replaying the
    // cached future.
    mesh_loader_->clearCache();
  }
  if (mesh_pass_) {
    mesh_pass_->clearMeshes();
  }

  if (source_type_ == SourceType::kTopic) {
    return tryLoadTopicDescription();
  }

  if (source_type_ == SourceType::kUrl) {
    if (!url_fetcher_) {
      url_fetcher_ = std::make_unique<UrlFetcher>();
    }
    setStatus(tr("Fetching %1").arg(source_value_));
    const QString url_text = source_value_;
    const uint64_t generation = url_fetch_generation_;
    // Fire-and-forget kickoff: attach()/xmlLoadState (and undo/redo restores)
    // no longer block on the network. The layer-owned fetcher's destruction is
    // the primary lifetime guard; the QPointer is insurance.
    QPointer<RobotModelLayer> guard(this);
    url_fetcher_->fetch(QUrl(url_text), [this, guard, generation, url_text](const FetchResult& fetched) {
      if (guard.isNull() || generation != url_fetch_generation_) {
        return;  // layer destroyed, or a newer load superseded this fetch
      }
      if (!fetched.ok) {
        setStatus(tr("Fetch failed (%1)").arg(fetched.error));
        return;
      }
      const QString text = QString::fromUtf8(fetched.bytes);
      applyRobotDescription(text, formatFromXml(text), url_text, urlDirectory(url_text), /*source_is_url=*/true);
    });
    return true;
  }

  QString error;
  const std::optional<QString> text = readTextFile(source_value_, &error);
  if (!text.has_value()) {
    setStatus(tr("Failed to read URDF: %1").arg(error));
    return false;
  }
  return applyRobotDescription(
      *text, formatFromXml(*text), source_value_, QFileInfo(source_value_).absolutePath(), /*source_is_url=*/false);
}

bool RobotModelLayer::tryLoadTopicDescription() {
  if (ctx_.session == nullptr) {
    return false;
  }
  // Resolve a fresh binding per use: a file reload re-registers the topic's
  // parser slot, so a pointer cached across calls would dangle.
  const auto binding = ctx_.session->parserBindingForObjectTopic(source_topic_id_);
  PJ::ObjectStore& store = ctx_.session->objectStore();
  if (!binding) {
    const QString topic =
        source_value_.isEmpty() ? QString::fromStdString(store.descriptor(source_topic_id_).topic_name) : source_value_;
    setStatus(tr("No parser for %1").arg(topic));
    return false;
  }
  const auto entry = store.latestAt(source_topic_id_, std::numeric_limits<PJ::Timestamp>::max());
  if (!entry.has_value() || entry->payload.bytes.empty()) {
    latch_pending_ = true;
    last_latch_retry_ = std::chrono::steady_clock::now();
    const QString topic =
        source_value_.isEmpty() ? QString::fromStdString(store.descriptor(source_topic_id_).topic_name) : source_value_;
    setStatus(tr("Waiting for %1...").arg(topic));
    return true;
  }

  auto obj = parseLocked(binding, entry->timestamp, entry->payload);
  if (!obj.has_value()) {
    setStatus(tr("Parse error: %1").arg(QString::fromStdString(obj.error())));
    return false;
  }
  const auto* desc = obj->object.get<PJ::sdk::RobotDescription>();
  if (desc == nullptr) {
    setStatus(tr("Parse error: parser did not return RobotDescription"));
    return false;
  }
  const QString label = source_value_.isEmpty() ? QString::fromStdString(desc->topic) : source_value_;
  return applyRobotDescription(
      QString::fromStdString(desc->text), QString::fromStdString(desc->format), label, QString(), false);
}

bool RobotModelLayer::applyRobotDescription(
    const QString& text, const QString& format, const QString& label, const QString& urdf_dir, bool source_is_url) {
  latch_pending_ = false;
  if (format.compare("urdf"_L1, Qt::CaseInsensitive) != 0) {
    setStatus(tr("Format '%1' is not supported — only URDF").arg(format));
    return false;
  }

  if (resolver_ != nullptr) {
    // No clearUnresolved(): the resolver no longer keeps a global tally (the
    // unresolved list is derived per-model from model_ — see M.29).
    resolver_->autoSeedSearchRoots(urdf_dir);
  }
  auto parsed = parseUrdf(text.toStdString(), resolver_, urdf_dir.toStdString(), source_is_url, label.toStdString());
  if (!parsed.first.has_value()) {
    setStatus(QString::fromStdString(parsed.second));
    return false;
  }

  model_ = std::move(parsed.first);
  draws_dirty_ = true;     // a new model: render() must rebuild the draw lists
  rebuildStaticBridges();  // cache the fixed-joint TF bridges for this model
  // Whether ANY link has visual geometry — gates kAuto's collision-only promotion
  // (see rebuildDrawCache). Depends only on the latched model, so cache it once
  // here instead of rescanning every rebuild.
  model_has_visuals_ = false;
  for (const RobotLink& link : model_->links) {
    if (!link.visuals.empty()) {
      model_has_visuals_ = true;
      break;
    }
  }
  startMeshLoads();
  updateMeshCounters();
  const QStringList unresolved = unresolvedPackagesList();
  QString mesh_status = unresolved_mesh_count_ > 0 ? tr("URDF: %1  •  %2/%3 meshes  •  %4 packages unresolved")
                                                         .arg(label)
                                                         .arg(total_mesh_count_ - unresolved_mesh_count_)
                                                         .arg(total_mesh_count_)
                                                         .arg(unresolved.size())
                                                   : tr("URDF: %1  •  %2/%2 meshes").arg(label).arg(total_mesh_count_);
  mesh_status += unresolvedIssueClause();
  setStatus(mesh_status);
  emit sourceFrameChanged(sourceFrame());
  emit fallbackFramesChanged(fallbackFrames());
  emit unresolvedPackages(unresolved);
  emit meshLoadStatusChanged(loaded_mesh_count_, total_mesh_count_, unresolved);
  emit repaintRequested();
  return true;
}

void RobotModelLayer::startMeshLoads() {
  if (!model_.has_value() || mesh_loader_ == nullptr) {
    return;
  }
  // Dispatch only: the counters are owned entirely by updateMeshCounters(). The
  // map dedups by resolved path, so links sharing a mesh file kick one load.
  // COLLADA flip override is scoped to .dae/.collada (the checkbox's intent):
  // force the Y->Z flip OFF only for those, otherwise let the loader decide.
  auto start = [&](const LinkGeom& geom) {
    const auto* mesh = std::get_if<GeomMesh>(&geom.shape);
    if (mesh == nullptr || !mesh->resolved || mesh->resolved_path.empty()) {
      return;
    }
    if (mesh_loads_->find(mesh->resolved_path) != nullptr) {
      return;  // already dispatched this path
    }
    const QString path = QString::fromStdString(mesh->resolved_path);
    const QString suffix = QFileInfo(path).suffix().toLower();
    const bool is_collada = suffix == QLatin1String("dae") || suffix == QLatin1String("collada");
    const std::optional<bool> flip_override =
        (ignore_collada_up_axis_ && is_collada) ? std::optional<bool>(false) : std::nullopt;
    MeshLoadEntry& entry = mesh_loads_->insertOrReplace(mesh->resolved_path, mesh->resolved_path);
    entry.future = mesh_loader_->load(path, flip_override);
    mesh_loads_->arm(entry, this, [this]() { pollMeshLoads(); });
  };
  for (const RobotLink& link : model_->links) {
    for (const LinkGeom& geom : link.visuals) {
      start(geom);
    }
    for (const LinkGeom& geom : link.collisions) {
      start(geom);
    }
  }
}

void RobotModelLayer::pollMeshLoads() {
  if (!mesh_pass_) {
    return;
  }
  // Evict failed paths from the loader so a Retry re-imports instead of being
  // handed the cached failure forever (M.30).
  const MeshLoadSet::DrainResult drained =
      mesh_loads_->drain(*mesh_pass_, [this](const std::string& key, const MeshLoadEntry&) {
        if (mesh_loader_ != nullptr) {
          mesh_loader_->evict(QString::fromStdString(key));
        }
      });
  if (drained.changed) {
    // A placeholder cube must swap to the freshly-loaded mesh, so the cached
    // draw lists are now stale (meshReady() flips for the drained keys).
    draws_dirty_ = true;
    updateMeshCounters();  // recompute loaded_mesh_count_ from ready() entries
    emit meshLoadStatusChanged(loaded_mesh_count_, total_mesh_count_, unresolvedPackagesList());
    // Swap the placeholder for the loaded mesh now — meshLoadStatusChanged only
    // feeds the config-widget label and schedules no paint.
    emit repaintRequested();
  }
}

bool RobotModelLayer::meshReady(const std::string& key) const {
  return mesh_loads_->ready(key);
}

void RobotModelLayer::updateMeshCounters() {
  total_mesh_count_ = 0;
  unresolved_mesh_count_ = 0;
  loaded_mesh_count_ = 0;
  if (!model_.has_value()) {
    return;
  }
  // Single traversal, all counts per-geometry mesh reference (links sharing one
  // file each count once). A reference is unresolved when it has no resolved
  // path; loaded when its resolved path's async load is ready().
  auto count = [&](const LinkGeom& geom) {
    const auto* mesh = std::get_if<GeomMesh>(&geom.shape);
    if (mesh == nullptr) {
      return;
    }
    ++total_mesh_count_;
    if (!mesh->resolved || mesh->resolved_path.empty()) {
      ++unresolved_mesh_count_;
    } else if (mesh_loads_->ready(mesh->resolved_path)) {
      ++loaded_mesh_count_;
    }
  };
  for (const RobotLink& link : model_->links) {
    for (const LinkGeom& geom : link.visuals) {
      count(geom);
    }
    for (const LinkGeom& geom : link.collisions) {
      count(geom);
    }
  }
}

void RobotModelLayer::setStatus(QString status) {
  if (status_text_ == status) {
    return;
  }
  status_text_ = std::move(status);
  emit statusTextChanged(status_text_);
}

QStringList RobotModelLayer::unresolvedPackagesList() const {
  // Per-MODEL, not resolver-global: the resolver is dock-shared across robot
  // layers, so deriving the list from this layer's own model_ keeps one robot's
  // unresolved packages from polluting a sibling's status/Locate (review M.29).
  // Distinct package names, in first-seen order.
  QStringList packages;
  if (!model_.has_value()) {
    return packages;
  }
  auto collect = [&](const LinkGeom& geom) {
    const auto* mesh = std::get_if<GeomMesh>(&geom.shape);
    if (mesh == nullptr || mesh->unresolved_package.empty()) {
      return;
    }
    const QString package = QString::fromStdString(mesh->unresolved_package);
    if (!packages.contains(package)) {
      packages.append(package);
    }
  };
  for (const RobotLink& link : model_->links) {
    for (const LinkGeom& geom : link.visuals) {
      collect(geom);
    }
    for (const LinkGeom& geom : link.collisions) {
      collect(geom);
    }
  }
  return packages;
}

QString RobotModelLayer::unresolvedIssueClause() const {
  if (!model_.has_value()) {
    return {};
  }
  // Count unresolved meshes by reason kind so the status line stays actionable
  // beyond the generic "N packages unresolved" (which only covers package://
  // misses). kBlockedHttp and kMissingFile are the kinds the user can do nothing
  // about via Locate, so we name them explicitly (review L.31).
  int blocked_http = 0;
  int missing_file = 0;
  auto tally = [&](const LinkGeom& geom) {
    const auto* mesh = std::get_if<GeomMesh>(&geom.shape);
    if (mesh == nullptr) {
      return;
    }
    if (mesh->issue == MeshResolveIssue::kBlockedHttp) {
      ++blocked_http;
    } else if (mesh->issue == MeshResolveIssue::kMissingFile) {
      ++missing_file;
    }
  };
  for (const RobotLink& link : model_->links) {
    for (const LinkGeom& geom : link.visuals) {
      tally(geom);
    }
    for (const LinkGeom& geom : link.collisions) {
      tally(geom);
    }
  }
  QString clause;
  if (blocked_http > 0) {
    clause += tr("  •  %n http ref(s) blocked (file-source URDF)", nullptr, blocked_http);
  }
  if (missing_file > 0) {
    clause += tr("  •  %n absolute path(s) missing", nullptr, missing_file);
  }
  return clause;
}

}  // namespace pj::scene3d
