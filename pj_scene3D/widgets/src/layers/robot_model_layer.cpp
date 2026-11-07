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
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariant>
#include <algorithm>
#include <any>
#include <array>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>

#include "layer_xml_validation.h"
#include "mesh_import.h"
#include "mesh_load_queue.h"
#include "pj_base/builtin/robot_description.hpp"
#include "pj_base/time.hpp"
#include "pj_runtime/HttpUrl.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/parse_locked.h"
#include "pj_scene3d_core/model_budget.h"
#include "pj_scene3d_core/robot_model_bridges.h"
#include "pj_scene3d_widgets/layout_resources.h"
#include "pj_scene3d_widgets/object_topic_metadata.h"
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
const glm::vec4 kPlaceholderColor{1.0f, 0.0f, 1.0f, 1.0f};

constexpr UrdfParseLimits kUrdfLimits{
    .max_links = kSceneLimits.max_urdf_links,
    .max_joints = kSceneLimits.max_urdf_joints,
    .max_geometries = kSceneLimits.max_urdf_geometries,
    .max_materials = kSceneLimits.max_urdf_materials,
    .max_string_bytes = kSceneLimits.max_urdf_string_bytes,
};

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
  if (!doc.setContent(text.toUtf8())) {
    return u"unknown"_s;
  }
  const QString root = doc.documentElement().tagName();
  return root == "robot"_L1 ? u"urdf"_s : root;
}

std::optional<QByteArray> readFile(const QString& path, QString* error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    if (error != nullptr) {
      *error = file.errorString();
    }
    return std::nullopt;
  }
  return file.readAll();
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

QString formatHint(const QString& source) {
  const QUrl url(source);
  return QFileInfo(url.isValid() && !url.scheme().isEmpty() ? url.path() : source).suffix().toLower();
}

// URDF primitives are drawn as unit meshes the layer retains: box ±0.5, cylinder
// radius 1 / height 1 along Z, sphere radius 1.
constexpr std::string_view kBoxKey = "robot:primitive:box";
constexpr std::string_view kCylinderKey = "robot:primitive:cylinder";
constexpr std::string_view kSphereKey = "robot:primitive:sphere";
constexpr std::string_view kPlaceholderKey = "robot:placeholder";

std::shared_ptr<const Material> solidMaterial(const glm::vec4& color) {
  Material material;
  material.base_color_factor = color;
  return std::make_shared<const Material>(std::move(material));
}

MeshData makeCube(const glm::vec4& color) {
  constexpr glm::vec3 positions[] = {
      {0.5F, -0.5F, -0.5F},  {0.5F, -0.5F, 0.5F},  {0.5F, 0.5F, 0.5F},   {0.5F, 0.5F, -0.5F},   {-0.5F, -0.5F, 0.5F},
      {-0.5F, -0.5F, -0.5F}, {-0.5F, 0.5F, -0.5F}, {-0.5F, 0.5F, 0.5F},  {-0.5F, 0.5F, -0.5F},  {0.5F, 0.5F, -0.5F},
      {0.5F, 0.5F, 0.5F},    {-0.5F, 0.5F, 0.5F},  {-0.5F, -0.5F, 0.5F}, {0.5F, -0.5F, 0.5F},   {0.5F, -0.5F, -0.5F},
      {-0.5F, -0.5F, -0.5F}, {-0.5F, -0.5F, 0.5F}, {-0.5F, 0.5F, 0.5F},  {0.5F, 0.5F, 0.5F},    {0.5F, -0.5F, 0.5F},
      {0.5F, -0.5F, -0.5F},  {0.5F, 0.5F, -0.5F},  {-0.5F, 0.5F, -0.5F}, {-0.5F, -0.5F, -0.5F},
  };
  constexpr glm::vec3 normals[] = {
      {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}, {-1, 0, 0}, {-1, 0, 0}, {-1, 0, 0},
      {0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, -1, 0}, {0, -1, 0}, {0, -1, 0}, {0, -1, 0},
      {0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1},
  };
  constexpr std::uint32_t indices[] = {
      0,  1,  2,  0,  2,  3,  4,  5,  6,  4,  6,  7,  8,  9,  10, 8,  10, 11,
      12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23,
  };
  MeshData mesh;
  mesh.ok = true;
  mesh.vertices.reserve(std::size(positions));
  for (std::size_t index = 0; index < std::size(positions); ++index) {
    mesh.vertices.push_back(Vertex{positions[index], normals[index], color});
  }
  mesh.indices.assign(std::begin(indices), std::end(indices));
  mesh.submeshes.push_back(SubMesh{0, mesh.indices.size(), solidMaterial(color)});
  return mesh;
}

MeshData makeCylinder() {
  constexpr int segments = 40;
  const glm::vec4 color{0.7F, 0.7F, 0.7F, 1.0F};
  MeshData mesh;
  mesh.ok = true;
  for (int index = 0; index < segments; ++index) {
    const float angle = static_cast<float>(index) * 2.0F * std::numbers::pi_v<float> / static_cast<float>(segments);
    const float x = std::cos(angle);
    const float y = std::sin(angle);
    mesh.vertices.push_back(Vertex{{x, y, -0.5F}, glm::normalize(glm::vec3{x, y, 0.0F}), color});
    mesh.vertices.push_back(Vertex{{x, y, 0.5F}, glm::normalize(glm::vec3{x, y, 0.0F}), color});
  }
  const std::uint32_t top = static_cast<std::uint32_t>(mesh.vertices.size());
  mesh.vertices.push_back(Vertex{{0, 0, 0.5F}, {0, 0, 1}, color});
  const std::uint32_t bottom = static_cast<std::uint32_t>(mesh.vertices.size());
  mesh.vertices.push_back(Vertex{{0, 0, -0.5F}, {0, 0, -1}, color});
  for (int index = 0; index < segments; ++index) {
    const auto a = static_cast<std::uint32_t>(2 * index);
    const auto b = static_cast<std::uint32_t>(2 * ((index + 1) % segments));
    mesh.indices.insert(mesh.indices.end(), {a, b, a + 1, b, b + 1, a + 1, top, a + 1, b + 1, bottom, b, a});
  }
  mesh.submeshes.push_back(SubMesh{0, mesh.indices.size(), solidMaterial(color)});
  return mesh;
}

MeshData makeSphere() {
  constexpr int latitudes = 16;
  constexpr int longitudes = 32;
  const glm::vec4 color{0.7F, 0.7F, 0.7F, 1.0F};
  MeshData mesh;
  mesh.ok = true;
  for (int latitude = 0; latitude <= latitudes; ++latitude) {
    const float theta = static_cast<float>(latitude) * std::numbers::pi_v<float> / static_cast<float>(latitudes);
    const float z = std::cos(theta);
    const float radius = std::sin(theta);
    for (int longitude = 0; longitude <= longitudes; ++longitude) {
      const float phi =
          static_cast<float>(longitude) * 2.0F * std::numbers::pi_v<float> / static_cast<float>(longitudes);
      const glm::vec3 point{radius * std::cos(phi), radius * std::sin(phi), z};
      mesh.vertices.push_back(Vertex{point, glm::normalize(point), color});
    }
  }
  for (int latitude = 0; latitude < latitudes; ++latitude) {
    for (int longitude = 0; longitude < longitudes; ++longitude) {
      const auto a = static_cast<std::uint32_t>(latitude * (longitudes + 1) + longitude);
      const auto b = static_cast<std::uint32_t>((latitude + 1) * (longitudes + 1) + longitude);
      mesh.indices.insert(mesh.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
    }
  }
  mesh.submeshes.push_back(SubMesh{0, mesh.indices.size(), solidMaterial(color)});
  return mesh;
}

}  // namespace

RobotModelLayer::RobotModelLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent)
    : Scene3DLayer(parent),
      topic_id_(topic_id),
      source_topic_id_(topic_id),
      display_name_(std::move(display_name)),
      source_value_(display_name_),
      owned_resolver_(std::make_unique<UrdfPackageResolver>()),
      mesh_queue_(std::make_unique<MeshLoadQueue>(this)) {
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
  return QString::fromStdString(source_frame_);
}

QDomElement RobotModelLayer::xmlSaveState(QDomDocument& doc) const {
  QDomElement el = doc.createElement(u"robot_model"_s);
  el.setAttribute(u"source_type"_s, sourceTypeToString(source_type_));
  el.setAttribute(u"source_value"_s, source_value_);
  // For a topic source, source_value_ is only a display string. Persist the
  // resolvable identity (dataset_id + topic_name) so restore re-binds the SAME
  // topic even when the user switched the config combo to a different one.
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
  if (!package_url_roots_.isEmpty()) {
    el.setAttribute(u"package_url_roots"_s, serializePackageUrlRoots(package_url_roots_));
  }
  if (remote_resources_only_) {
    el.setAttribute(u"remote_resources_only"_s, u"true"_s);
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

std::optional<RobotModelLayer::ParsedSettings> RobotModelLayer::parseSettings(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "robot_model"_L1 || !detail::isLeafPayload(element)) {
    return std::nullopt;
  }
  const QString source_type_text = element.attribute(u"source_type"_s, u"topic"_s);
  if (source_type_text != "topic"_L1 && source_type_text != "file"_L1 && source_type_text != "url"_L1) {
    return std::nullopt;
  }
  const QString display_mode_text = element.attribute(u"display_mode"_s, u"auto"_s);
  if (display_mode_text != "auto"_L1 && display_mode_text != "visual"_L1 && display_mode_text != "collision"_L1) {
    return std::nullopt;
  }
  ParsedSettings settings;
  settings.source_type = sourceTypeFromString(source_type_text);
  settings.display_mode = displayModeFromString(display_mode_text);
  if (!detail::parseTrueFalse(element, "visible", true, settings.visible) ||
      !detail::parseTrueFalse(element, "ignore_collada_up_axis", false, settings.ignore_collada_up_axis) ||
      !detail::parseTrueFalse(element, "remote_resources_only", false, settings.remote_resources_only)) {
    return std::nullopt;
  }
  if (element.hasAttribute(u"color"_s)) {
    settings.color = QColor(element.attribute(u"color"_s));
    if (!settings.color->isValid()) {
      return std::nullopt;
    }
  }
  const bool has_topic_name = element.hasAttribute(u"source_topic_name"_s);
  const bool has_dataset_id = element.hasAttribute(u"source_dataset_id"_s);
  const bool has_dataset_source = element.hasAttribute(u"source_dataset_source"_s);
  const bool has_dataset_path = element.hasAttribute(u"source_dataset_path"_s);
  const bool has_topic_identity = has_topic_name || has_dataset_id || has_dataset_source || has_dataset_path;
  if ((settings.source_type != SourceType::kTopic && has_topic_identity) ||
      (!has_topic_name && (has_dataset_id || has_dataset_source || has_dataset_path))) {
    return std::nullopt;
  }
  if (has_dataset_id) {
    bool id_ok = false;
    const qulonglong value = element.attribute(u"source_dataset_id"_s).toULongLong(&id_ok);
    if (!id_ok || value == 0 || value > std::numeric_limits<uint32_t>::max()) {
      return std::nullopt;
    }
    settings.dataset_id = static_cast<PJ::DatasetId>(value);
  }
  settings.topic_name = element.attribute(u"source_topic_name"_s);
  if (has_topic_name && settings.topic_name.isEmpty()) {
    return std::nullopt;
  }
  settings.dataset_source = element.attribute(u"source_dataset_source"_s);
  settings.dataset_path = element.attribute(u"source_dataset_path"_s);
  std::optional<QMap<QString, QUrl>> roots = parsePackageUrlRoots(element.attribute(u"package_url_roots"_s));
  if (!roots.has_value()) {
    return std::nullopt;
  }
  settings.package_url_roots = std::move(*roots);
  return settings;
}

bool RobotModelLayer::validateXml(const QDomElement& element) {
  return parseSettings(element).has_value();
}

RobotModelLayer::XmlLoadResult RobotModelLayer::xmlLoadStateResult(const QDomElement& element) {
  const std::optional<ParsedSettings> settings = parseSettings(element);
  if (!settings.has_value()) {
    return XmlLoadResult::kInvalid;
  }
  const QString& topic_name = settings->topic_name;
  std::optional<PJ::ObjectTopicId> restored_topic;
  if (settings->source_type == SourceType::kTopic && ctx_.session != nullptr && !topic_name.isEmpty()) {
    const QString& source = settings->dataset_source;
    const QString& path = settings->dataset_path;
    if (settings->dataset_id == 0 && source.isEmpty() && path.isEmpty()) {
      const UniqueObjectTopicResolution generic = resolveUniqueObjectTopic(
          ctx_.session->objectStore(), topic_name.toStdString(), PJ::sdk::BuiltinObjectType::kRobotDescription);
      if (generic.ambiguous) {
        return XmlLoadResult::kInvalid;
      }
      restored_topic = generic.topic_id;
    } else {
      const PJ::DatasetIdentityResolution dataset =
          ctx_.session->resolveDatasetIdentity(settings->dataset_id, source, path);
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
  const QMap<QString, QUrl> old_roots = package_url_roots_;
  const bool old_remote_resources_only = remote_resources_only_;
  source_type_ = settings->source_type;
  source_value_ = element.attribute(u"source_value"_s, source_value_);
  frame_prefix_ = element.attribute(u"frame_prefix"_s);
  display_mode_ = settings->display_mode;
  visible_ = settings->visible;
  fallback_color_ = settings->color.value_or(fallback_color_);
  ignore_collada_up_axis_ = settings->ignore_collada_up_axis;
  package_url_roots_ = settings->package_url_roots;
  remote_resources_only_ = settings->remote_resources_only;
  // Layout bytes never carry file content: a restored kFile reads its path
  // (desktop) or asks for the file again (browser).
  local_file_bytes_.clear();
  bool source_setter_emitted = false;
  if (restored_topic.has_value()) {
    source_setter_emitted = source_topic_id_.id != restored_topic->id;
    setSourceTopic(*restored_topic);
  } else if (ctx_.session != nullptr) {
    loadFromCurrentSource();
  } else {
    rebuildDrawCalls();
  }
  const bool changed = old_source_type != source_type_ || old_source_value != source_value_ ||
                       old_topic_id != source_topic_id_.id || old_prefix != frame_prefix_ ||
                       old_mode != display_mode_ || old_visible != visible_ || old_color != fallback_color_.rgb() ||
                       old_ignore_up_axis != ignore_collada_up_axis_ || old_roots != package_url_roots_ ||
                       old_remote_resources_only != remote_resources_only_;
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
  tf_buffer_ = ctx_.tf_buffer;  // the dock re-pushes it via setTransformBuffer on rebind
  // disconnect first: the reload path re-attaches without an intervening detach.
  disconnect(reload_connection_);
  reload_connection_ = {};
  if (ctx_.session != nullptr) {
    reload_connection_ = connect(
        ctx_.session, &PJ::SessionManager::datasetAboutToBeReplaced, this,
        &RobotModelLayer::onDatasetAboutToBeReplaced);
  }
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
  disconnect(reload_connection_);
  reload_connection_ = {};
  resetModelState();
  tf_buffer_.reset();
  ctx_ = {};
}

void RobotModelLayer::setFixedFrame(const QString& frame) {
  fixed_frame_ = frame;
  emit repaintRequested();
}

void RobotModelLayer::setTrackerTime(PJ::Timepoint time) {
  tracker_time_ = time;
  ensureStaticBridges();
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
  emit visibilityChanged(visible);
  emit infoChanged();
  emit repaintRequested();
}

void RobotModelLayer::rebuildDrawCalls() {
  model_draw_calls_.clear();
  if (model_.has_value()) {
    const std::string prefix = frame_prefix_.toStdString();
    const auto append = [&](const RobotLink& link, const LinkGeom& geom, bool collision) {
      ModelDrawCall call;
      call.frame_id = prefix + link.name;
      call.group = collision ? ModelDrawGroup::kCollision : ModelDrawGroup::kVisual;
      // Primitives and the placeholder carry the call's colour alone (their unit
      // meshes' material and vertex colours would apply it a second time); only a
      // mesh without an explicit URDF colour shows its own materials.
      call.use_material = false;
      call.mesh_key = std::string(kPlaceholderKey);
      bool explicit_color = true;
      if (geom.has_color) {
        call.override_color = geom.color;
      } else if (collision) {
        // Collision geometry with no <material>: orange tint so hulls read as
        // distinct from the visual meshes (RViz convention).
        call.override_color = look::kCollisionDefaultColor;
      } else {
        explicit_color = false;
      }
      glm::dvec3 scale{1.0};
      if (const auto* box = std::get_if<GeomBox>(&geom.shape); box != nullptr) {
        call.mesh_key = std::string(kBoxKey);
        scale = box->size;
      } else if (const auto* cylinder = std::get_if<GeomCylinder>(&geom.shape); cylinder != nullptr) {
        call.mesh_key = std::string(kCylinderKey);
        scale = {cylinder->radius, cylinder->radius, cylinder->length};
      } else if (const auto* sphere = std::get_if<GeomSphere>(&geom.shape); sphere != nullptr) {
        call.mesh_key = std::string(kSphereKey);
        scale = glm::dvec3(sphere->radius);
      } else if (const auto* mesh = std::get_if<GeomMesh>(&geom.shape); mesh != nullptr) {
        scale = mesh->scale;
        if (mesh->resolved && meshReady(mesh->resolved_path)) {
          call.mesh_key = mesh->resolved_path;
          call.use_material = !explicit_color;
        } else {
          call.override_color = kPlaceholderColor;
        }
      }
      call.model = glm::scale(originToMat4(geom.origin_xyz, geom.origin_rpy), scale);
      model_draw_calls_.push_back(std::move(call));
    };
    for (const RobotLink& link : model_->links) {
      if (display_mode_ == DisplayMode::kCollision) {
        for (const LinkGeom& geom : link.collisions) {
          append(link, geom, true);
        }
      } else if (display_mode_ == DisplayMode::kVisual || !link.visuals.empty()) {
        for (const LinkGeom& geom : link.visuals) {
          append(link, geom, false);
        }
      } else {
        // kAuto, collision-only link: promoted to the visuals group only when the
        // whole model has no visuals (see DisplayMode).
        for (const LinkGeom& geom : link.collisions) {
          append(link, geom, /*collision=*/model_has_visuals_);
        }
      }
    }
  }
  ++model_revision_;
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
  // Themed icon buttons, matching the app's chrome.
  const QString icon_theme = QGuiApplication::palette().color(QPalette::Window).valueF() < 0.5 ? u"dark"_s : u"light"_s;
  auto* browse_button = new QToolButton(file_row);
  browse_button->setAutoRaise(true);
  browse_button->setFocusPolicy(Qt::NoFocus);
  browse_button->setToolTip(tr("Browse for a URDF file"));
  browse_button->setIcon(PJ::loadSvg(u":/resources/svg/folder_open.svg"_s, icon_theme));
  file_layout->addWidget(file_edit, 1);
  file_layout->addWidget(browse_button);
  form->addRow(QString(), file_row);
#ifdef PJ_TARGET_WASM
  // Browser policy: a URDF file is granted through the panel's file chooser as
  // bytes; there is no host path to browse to or to reopen.
  browse_button->hide();
  if (source_type_ == SourceType::kFile && local_file_bytes_.isEmpty()) {
    auto* note = new QLabel(
        tr("A browser layout cannot reopen a host file automatically. Remove this row and select the URDF again."),
        container);
    note->setWordWrap(true);
    outer->addWidget(note);
  }
#endif

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
#ifdef PJ_TARGET_WASM
  // Browser policy: package roots are host directories the sandbox cannot read.
  group->hide();
#endif

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
    status_label->setText(status_text_);
#ifdef PJ_TARGET_WASM
    locate_button->setVisible(false);
#else
    locate_button->setVisible(!unresolvedPackagesList().isEmpty());
#endif
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
    retry();
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

  return container;
}

void RobotModelLayer::setPackageResolver(UrdfPackageResolver* resolver) {
  resolver_ = resolver != nullptr ? resolver : owned_resolver_.get();
}

void RobotModelLayer::setTransformBuffer(std::shared_ptr<TransformBuffer> buffer) {
  tf_buffer_ = std::move(buffer);
  ensureStaticBridges();
  emit repaintRequested();
}

void RobotModelLayer::setEmbeddedAssets(QMap<QString, QByteArray> assets) {
  embedded_assets_ = std::move(assets);
}

void RobotModelLayer::setSourceTopic(PJ::ObjectTopicId topic_id, QString display_name) {
  const bool changed = source_type_ != SourceType::kTopic || source_topic_id_.id != topic_id.id ||
                       (!display_name.isEmpty() && display_name_ != display_name);
  source_type_ = SourceType::kTopic;
  source_topic_id_ = topic_id;
  local_file_bytes_.clear();
  if (!display_name.isEmpty()) {
    display_name_ = std::move(display_name);
    source_value_ = display_name_;
  }
  if (ctx_.session != nullptr) {
    // Existence check only; the actual decode re-resolves the binding per use.
    if (!ctx_.session->parserBindingForObjectTopic(source_topic_id_)) {
      fail(tr("No parser for %1").arg(source_value_));
      return;
    }
    loadFromCurrentSource();
  }
  if (changed) {
    emit configurationChanged();
  }
}

void RobotModelLayer::setSourceFile(QString path) {
  if (source_type_ == SourceType::kFile && source_value_ == path && local_file_bytes_.isEmpty()) {
    return;
  }
  source_type_ = SourceType::kFile;
  source_value_ = std::move(path);
  local_file_bytes_.clear();
  loadFromCurrentSource();
  emit configurationChanged();
}

void RobotModelLayer::setSourceFileContent(QString filename, QByteArray bytes) {
  source_type_ = SourceType::kFile;
  source_value_ = std::move(filename);
  local_file_bytes_ = std::move(bytes);
  loadFromCurrentSource();
  emit configurationChanged();
}

void RobotModelLayer::setSourceUrl(QString url) {
  if (source_type_ == SourceType::kUrl && source_value_ == url) {
    return;
  }
  source_type_ = SourceType::kUrl;
  source_value_ = std::move(url);
  local_file_bytes_.clear();
  loadFromCurrentSource();
  emit configurationChanged();
}

void RobotModelLayer::setPackageUrlRoots(const QMap<QString, QUrl>& roots) {
  if (package_url_roots_ == roots) {
    return;
  }
  package_url_roots_ = roots;
  loadFromCurrentSource();
  emit configurationChanged();
}

void RobotModelLayer::setFramePrefix(QString prefix) {
  if (frame_prefix_ == prefix) {
    return;
  }
  frame_prefix_ = std::move(prefix);
  source_frame_ = model_.has_value() ? linkFrameName(model_->root_link).toStdString() : std::string{};
  rebuildStaticBridges();  // bridge frames carry the prefix too
  rebuildDrawCalls();      // changes which TF frames each link resolves against
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
  if (!frame_prefix_.isEmpty()) {
    const std::string prefix = frame_prefix_.toStdString();
    for (StampedTransform& bridge : static_bridges_) {
      bridge.parent_frame = prefix + bridge.parent_frame;
      bridge.child_frame = prefix + bridge.child_frame;
    }
  }
  ensureStaticBridges();
}

void RobotModelLayer::ensureStaticBridges(TransformBuffer& buf) {
  if (!static_bridges_.empty()) {
    injectMissingStaticTransforms(buf, static_bridges_);
  }
}

void RobotModelLayer::ensureStaticBridges() {
  if (tf_buffer_ != nullptr) {
    ensureStaticBridges(*tf_buffer_);
  }
}

void RobotModelLayer::setDisplayMode(DisplayMode mode) {
  if (display_mode_ == mode) {
    return;
  }
  display_mode_ = mode;
  rebuildDrawCalls();
  emit configurationChanged();
  emit repaintRequested();
}

void RobotModelLayer::setFallbackColor(QColor color) {
  if (!color.isValid() || fallback_color_.rgb() == color.rgb()) {
    return;
  }
  fallback_color_ = std::move(color);
  rebuildDrawCalls();
  emit configurationChanged();
  emit repaintRequested();
}

void RobotModelLayer::setIgnoreColladaUpAxis(bool ignore) {
  if (ignore_collada_up_axis_ == ignore) {
    return;
  }
  ignore_collada_up_axis_ = ignore;
  // The flip is baked into the loaded MeshData, so the toggle must reload
  // (loadFromCurrentSource clears the loader cache).
  loadFromCurrentSource();
  emit configurationChanged();
}

void RobotModelLayer::retry() {
  loadFromCurrentSource();
}

QString RobotModelLayer::linkFrameName(const std::string& link_name) const {
  return frame_prefix_ + QString::fromStdString(link_name);
}

void RobotModelLayer::resetModelState() {
  // Every in-flight URDF fetch, mesh fetch and import of the previous source is
  // now stale: its completion must not land on the next source.
  ++load_generation_;
  // Aborts the URDF and mesh fetches. Running imports keep their concurrency
  // slots until their workers finish; their results are dropped.
  mesh_queue_->cancelAll();
  mesh_errors_.clear();
  // Release the previous model's cached MeshData and make Retry/Locate
  // genuinely re-import (failed loads included).
  mesh_queue_->clearLoaderCache();
  model_.reset();
  static_bridges_.clear();
  model_has_visuals_ = false;
  source_frame_.clear();
  total_mesh_count_ = 0;
  unresolved_mesh_count_ = 0;
  loaded_mesh_count_ = 0;
  mesh_bytes_.clear();
  mesh_retained_bytes_ = 0;
  latch_pending_ = false;
  base_status_.clear();
  noteRenderSuccess();
  model_meshes_.clear();
  rebuildDrawCalls();
}

void RobotModelLayer::onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id) {
  if (source_type_ != SourceType::kTopic || ctx_.session == nullptr ||
      ctx_.session->objectStore().descriptor(source_topic_id_).dataset_id != dataset_id) {
    return;
  }
  const bool had_model = model_.has_value();
  resetModelState();  // new generation: outstanding mesh fetches/imports are dropped
  if (had_model) {
    emit sourceFrameChanged({});
  }
  // The signal precedes the swap and replaceDataset runs no event loop, so a
  // queued reload reads the replacement. A newer load (source change, retry)
  // bumps the generation and supersedes it.
  const std::uint64_t generation = load_generation_;
  QMetaObject::invokeMethod(
      this,
      [this, generation]() {
        if (generation == load_generation_ && source_type_ == SourceType::kTopic && ctx_.session != nullptr) {
          loadFromCurrentSource();
        }
      },
      Qt::QueuedConnection);
  emit repaintRequested();
}

bool RobotModelLayer::loadFromCurrentSource() {
  const bool had_model = model_.has_value();
  resetModelState();
  if (had_model) {
    emit sourceFrameChanged({});
  }
  setDataWarning({});

  if (source_type_ == SourceType::kTopic) {
    return tryLoadTopicDescription();
  }

  if (source_type_ == SourceType::kFile) {
    if (!local_file_bytes_.isEmpty()) {
      const QString text = QString::fromUtf8(local_file_bytes_);
      return applyRobotDescription(text, formatFromXml(text), source_value_, QString(), /*source_is_url=*/false);
    }
#ifdef PJ_TARGET_WASM
    fail(tr("Select the local URDF again; browser layouts cannot retain host-file access"));
    return false;
#else
    QString error;
    const std::optional<QByteArray> bytes = readFile(source_value_, &error);
    if (!bytes.has_value()) {
      fail(tr("Failed to read URDF: %1").arg(error));
      return false;
    }
    const QString text = QString::fromUtf8(*bytes);
    return applyRobotDescription(
        text, formatFromXml(text), source_value_, QFileInfo(source_value_).absolutePath(), /*source_is_url=*/false);
#endif
  }

  const QUrl url(source_value_);
#ifdef PJ_TARGET_WASM
  if (!PJ::isHttpUrl(url)) {
    fail(tr("Only http(s) URDF URLs are supported in the browser"));
    return false;
  }
#endif
  setStatus(tr("Fetching %1").arg(source_value_));
  const QString url_text = source_value_;
  const std::uint64_t generation = load_generation_;
  // Fire-and-forget: attach()/xmlLoadState never block on the network. Through
  // the mesh queue, so the model's meshes reuse this fetch's connections; the
  // layer-owned queue's destruction is the primary lifetime guard.
  QPointer<RobotModelLayer> guard(this);
  mesh_queue_->fetch(url, [this, guard, generation, url_text](const FetchResult& fetched) {
    if (guard.isNull() || generation != load_generation_) {
      return;  // layer destroyed, or a newer load superseded this fetch
    }
    if (!fetched.ok) {
      fail(tr("Fetch failed (%1)").arg(fetched.error));
      return;
    }
    // A redirected URDF anchors its relative meshes and roots where it was served.
    // A local source keeps its text: the mesh loader reads plain paths, not file:// URLs.
    const QUrl served = fetched.final_url.isEmpty() ? QUrl(url_text) : fetched.final_url;
    const QString text = QString::fromUtf8(fetched.bytes);
    applyRobotDescription(
        text, formatFromXml(text), url_text, urlDirectory(served.toString()), /*source_is_url=*/true, served);
  });
  return true;
}

bool RobotModelLayer::tryLoadTopicDescription() {
  if (ctx_.session == nullptr) {
    return false;
  }
  // Resolve a fresh binding per use: a file reload re-registers the topic's
  // parser slot, so a pointer cached across calls would dangle.
  const auto binding = ctx_.session->parserBindingForObjectTopic(source_topic_id_);
  PJ::ObjectStore& store = ctx_.session->objectStore();
  const QString topic =
      source_value_.isEmpty() ? QString::fromStdString(store.descriptor(source_topic_id_).topic_name) : source_value_;
  if (!binding) {
    fail(tr("No parser for %1").arg(topic));
    return false;
  }
  const auto entry = store.latestAt(source_topic_id_, std::numeric_limits<PJ::Timestamp>::max());
  if (!entry.has_value() || entry->payload.bytes.empty()) {
    latch_pending_ = true;
    last_latch_retry_ = std::chrono::steady_clock::now();
    setStatus(tr("Waiting for %1...").arg(topic));
    return true;
  }

  auto obj = parseLocked(binding, entry->timestamp, entry->payload);
  if (!obj.has_value()) {
    fail(tr("Parse error: %1").arg(QString::fromStdString(obj.error())));
    return false;
  }
  const auto* desc = obj->object.get<PJ::sdk::RobotDescription>();
  if (desc == nullptr) {
    fail(tr("Parse error: parser did not return RobotDescription"));
    return false;
  }
  const QString label = source_value_.isEmpty() ? QString::fromStdString(desc->topic) : source_value_;
  return applyRobotDescription(
      QString::fromStdString(desc->text), QString::fromStdString(desc->format), label, QString(), false);
}

bool RobotModelLayer::applyRobotDescription(
    const QString& text, const QString& format, const QString& label, const QString& urdf_dir, bool source_is_url,
    const QUrl& roots_base) {
  latch_pending_ = false;
  const std::string xml = text.toStdString();
  if (static_cast<std::uint64_t>(xml.size()) > kSceneLimits.max_model_source_bytes) {
    fail(tr("robot_description exceeds the %1 MiB limit")
             .arg(kSceneLimits.max_model_source_bytes / (1024ULL * 1024ULL)));
    return false;
  }
  if (format.compare("urdf"_L1, Qt::CaseInsensitive) != 0) {
    fail(tr("Format '%1' is not supported — only URDF").arg(format));
    return false;
  }

  if (resolver_ != nullptr) {
    if (!embedded_assets_.isEmpty()) {
      resolver_->setEmbeddedAssets(embedded_assets_);
    }
    resolver_->autoSeedSearchRoots(urdf_dir);
  }
  QMap<QString, QUrl> roots = package_url_roots_;
  for (QUrl& root : roots) {
    root = roots_base.resolved(root);  // absolute roots come back unchanged
  }
  auto parsed = parseUrdf(
      xml, resolver_, urdf_dir.toStdString(), source_is_url, label.toStdString(), &kUrdfLimits, roots,
      remote_resources_only_);
  if (!parsed.first.has_value()) {
    fail(QString::fromStdString(parsed.second));
    return false;
  }

  model_ = std::move(parsed.first);
  source_frame_ = linkFrameName(model_->root_link).toStdString();
  model_has_visuals_ = std::any_of(
      model_->links.cbegin(), model_->links.cend(), [](const RobotLink& link) { return !link.visuals.empty(); });
  rebuildStaticBridges();  // cache (and inject) the fixed-joint TF bridges
  const auto retain = [this](std::string_view key, MeshData mesh) {
    mesh_retained_bytes_ = saturatedAdd(mesh_retained_bytes_, estimateRetainedMeshBytes(mesh));
    model_meshes_.emplace(std::string(key), std::make_shared<const MeshData>(std::move(mesh)));
  };
  retain(kBoxKey, makeCube({0.7F, 0.7F, 0.7F, 1.0F}));
  retain(kCylinderKey, makeCylinder());
  retain(kSphereKey, makeSphere());
  retain(kPlaceholderKey, makeCube(kPlaceholderColor));
  base_status_ = tr("URDF: %1").arg(label);
  queueMeshLoads();
  rebuildDrawCalls();
  updateMeshCounters();
  updateStatus();
  const QStringList unresolved = unresolvedPackagesList();
  emit sourceFrameChanged(sourceFrame());
  emit fallbackFramesChanged(fallbackFrames());
  emit unresolvedPackages(unresolved);
  emit meshLoadStatusChanged(loaded_mesh_count_, total_mesh_count_, unresolved);
  emit repaintRequested();
  return true;
}

void RobotModelLayer::queueMeshLoads() {
  if (!model_.has_value()) {
    return;
  }
  // Links sharing a mesh file queue one load.
  std::unordered_set<std::string> queued;
  const auto inspect = [this, &queued](const LinkGeom& geometry) {
    const auto* mesh = std::get_if<GeomMesh>(&geometry.shape);
    if (mesh == nullptr || !mesh->resolved || mesh->resolved_path.empty() ||
        !queued.insert(mesh->resolved_path).second) {
      return;
    }
    MeshLoadQueue::Source source;
    source.location = QString::fromStdString(mesh->resolved_path);
    const QUrl url(source.location);
    source.kind = PJ::isHttpUrl(url) ? MeshLoadQueue::Source::Kind::kUrl : MeshLoadQueue::Source::Kind::kPath;
    source.format_hint = formatHint(source.location);
    // COLLADA flip override is scoped to .dae/.collada (the checkbox's intent).
    const bool collada = source.format_hint == "dae"_L1 || source.format_hint == "collada"_L1;
    if (ignore_collada_up_axis_ && collada) {
      source.flip_override = false;
    }
    mesh_queue_->enqueue(
        [source]() { return std::optional(source); },
        [this, key = mesh->resolved_path](MeshLoadQueue::Outcome outcome) { admitMesh(key, std::move(outcome.mesh)); });
  };
  for (const RobotLink& link : model_->links) {
    for (const LinkGeom& geometry : link.visuals) {
      inspect(geometry);
    }
    for (const LinkGeom& geometry : link.collisions) {
      inspect(geometry);
    }
  }
  mesh_queue_->pump();
}

void RobotModelLayer::admitMesh(const std::string& key, MeshData mesh) {
  if (!mesh.ok) {
    mesh_errors_[key] = std::move(mesh.error);
  } else {
    const std::uint64_t retained = estimateRetainedMeshBytes(mesh);
    if (!retainedMeshFits(retained, mesh_retained_bytes_)) {
      mesh_errors_[key] = tr("mesh would exceed the layer memory limit");
    } else {
      // The settle requests its own repaint, so a landed mesh replaces its
      // placeholder without waiting for an unrelated paint.
      model_meshes_[key] = std::make_shared<const MeshData>(std::move(mesh));
      mesh_bytes_[key] = retained;
      mesh_retained_bytes_ += retained;
      mesh_errors_.erase(key);
    }
  }
  onMeshLoadSettled();
}

void RobotModelLayer::onMeshLoadSettled() {
  // A placeholder cube must swap to the freshly-loaded mesh.
  rebuildDrawCalls();
  updateMeshCounters();
  updateStatus();
  emit meshLoadStatusChanged(loaded_mesh_count_, total_mesh_count_, unresolvedPackagesList());
  emit repaintRequested();
}

std::size_t RobotModelLayer::meshImportsOutstandingForTest() const {
  return mesh_queue_->importsRunning();
}

bool RobotModelLayer::meshReady(const std::string& key) const {
  return mesh_bytes_.contains(key);
}

void RobotModelLayer::updateMeshCounters() {
  total_mesh_count_ = 0;
  unresolved_mesh_count_ = 0;
  loaded_mesh_count_ = 0;
  if (!model_.has_value()) {
    return;
  }
  // Per-geometry mesh references (links sharing one file each count once).
  const auto count = [&](const LinkGeom& geom) {
    const auto* mesh = std::get_if<GeomMesh>(&geom.shape);
    if (mesh == nullptr) {
      return;
    }
    ++total_mesh_count_;
    if (!mesh->resolved || mesh->resolved_path.empty()) {
      ++unresolved_mesh_count_;
    } else if (meshReady(mesh->resolved_path)) {
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

QString RobotModelLayer::loadFailureClause() const {
  if (mesh_errors_.empty()) {
    return {};
  }
  return tr(
      "  •  %n mesh load(s) failed (shown as magenta placeholders)", nullptr, static_cast<int>(mesh_errors_.size()));
}

void RobotModelLayer::updateStatus() {
  if (!model_.has_value()) {
    return;
  }
  QString status = base_status_;
  if (unresolved_mesh_count_ > 0) {
    status += tr("  •  %1/%2 meshes  •  %3 packages unresolved")
                  .arg(total_mesh_count_ - unresolved_mesh_count_)
                  .arg(total_mesh_count_)
                  .arg(unresolvedPackagesList().size());
  } else {
    status += tr("  •  %1/%1 meshes").arg(total_mesh_count_);
  }
  status += unresolvedIssueClause();
  QString warning;
#ifdef PJ_TARGET_WASM
  // Browser policy: a local URDF grants no sibling files, and URL assets need CORS.
  if (unresolved_mesh_count_ > 0) {
    warning = source_type_ == SourceType::kUrl
                  ? tr("  •  %n mesh reference(s) unresolved; URL/package paths must be CORS-accessible", nullptr,
                       unresolved_mesh_count_)
                  : tr("  •  %n mesh reference(s) unavailable in the browser sandbox (shown as magenta placeholders)",
                       nullptr, unresolved_mesh_count_);
  }
#endif
  warning += loadFailureClause();
  setStatus(status + warning + (renderWarning().isEmpty() ? QString() : u"  •  "_s + renderWarning()));
  setDataWarning(warning.mid(warning.startsWith(u"  •  "_s) ? 5 : 0));
}

void RobotModelLayer::fail(const QString& message) {
  setStatus(message);
  setDataWarning(message);
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
  // layers. Distinct package names, in first-seen order.
  QStringList packages;
  if (!model_.has_value()) {
    return packages;
  }
  const auto collect = [&](const LinkGeom& geom) {
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
  // kBlockedHttp and kMissingFile are the kinds Locate cannot fix, so name them
  // explicitly.
  int blocked_http = 0;
  int missing_file = 0;
  const auto tally = [&](const LinkGeom& geom) {
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
