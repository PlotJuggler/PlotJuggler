// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/wasm/robot_model_layer_wasm.h"

#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QPointer>
#include <QPushButton>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <algorithm>
#include <any>
#include <array>
#include <cmath>
#include <exception>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <numbers>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>

#include "layers/layer_xml_validation.h"
#include "mesh_loader.h"
#include "pj_base/builtin/robot_description.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/model_budget.h"
#include "pj_scene3d_core/robot_model_bridges.h"
#include "pj_scene3d_widgets/parse_locked.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"
#include "pj_widgets/CheckButton.h"
#include "pj_widgets/ColorPickerWidget.h"
#include "pj_widgets/ComboBox.h"
#include "urdf_package_resolver.h"
#include "urdf_parser.h"
#include "url_fetcher.h"

using namespace Qt::StringLiterals;

namespace pj::scene3d {
namespace {

Q_LOGGING_CATEGORY(lcWasmRobotModel, "pj.scene3d.wasm.robot_model")

constexpr auto kLatchRetryInterval = std::chrono::milliseconds(500);
constexpr std::string_view kBoxKey = "robot:primitive:box";
constexpr std::string_view kCylinderKey = "robot:primitive:cylinder";
constexpr std::string_view kSphereKey = "robot:primitive:sphere";
constexpr std::string_view kPlaceholderKey = "robot:placeholder";
constexpr std::uint64_t kSaturated = std::numeric_limits<std::uint64_t>::max();

constexpr UrdfParseLimits kBrowserUrdfLimits{
    .max_links = 4'096,
    .max_joints = 8'192,
    .max_geometries = 16'384,
    .max_materials = 4'096,
    .max_string_bytes = 8'192,
};

QString sourceTypeName(WasmRobotModelLayer::SourceType type) {
  switch (type) {
    case WasmRobotModelLayer::SourceType::kFile:
      return u"file"_s;
    case WasmRobotModelLayer::SourceType::kUrl:
      return u"url"_s;
    case WasmRobotModelLayer::SourceType::kTopic:
      return u"topic"_s;
  }
  return u"topic"_s;
}

QString displayModeName(WasmRobotModelLayer::DisplayMode mode) {
  switch (mode) {
    case WasmRobotModelLayer::DisplayMode::kVisual:
      return u"visual"_s;
    case WasmRobotModelLayer::DisplayMode::kCollision:
      return u"collision"_s;
    case WasmRobotModelLayer::DisplayMode::kAuto:
      return u"auto"_s;
  }
  return u"auto"_s;
}

QString urlDirectory(const QString& text) {
  QUrl url(text);
  QString path = url.path();
  const qsizetype slash = path.lastIndexOf('/');
  path = slash >= 0 ? path.left(slash + 1) : QString{};
  url.setPath(path);
  url.setQuery(QString{});
  url.setFragment(QString{});
  return url.toString(QUrl::RemoveQuery | QUrl::RemoveFragment);
}

QString formatHint(const QString& source) {
  const QUrl url(source);
  return QFileInfo(url.isValid() && !url.scheme().isEmpty() ? url.path() : source).suffix().toLower();
}

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

std::uint64_t saturatedAdd(std::uint64_t left, std::uint64_t right) {
  return right > kSaturated - left ? kSaturated : left + right;
}

std::uint64_t estimateMeshBytes(const MeshData& mesh) {
  std::uint64_t texture_bytes = 0;
  std::unordered_set<const Material*> counted;
  for (const SubMesh& submesh : mesh.submeshes) {
    if (submesh.material == nullptr || !counted.insert(submesh.material.get()).second) {
      continue;
    }
    const std::array<const TextureSource*, 5> textures{
        &submesh.material->base_color, &submesh.material->metallic_roughness, &submesh.material->normal,
        &submesh.material->occlusion, &submesh.material->emissive};
    for (const TextureSource* texture : textures) {
      texture_bytes = saturatedAdd(texture_bytes, texture->bytes.size());
    }
  }
  const auto bytes = browserModelRetainedBytes(
      mesh.vertices.size(), sizeof(Vertex), mesh.indices.size(), mesh.submeshes.size(), sizeof(SubMesh), texture_bytes);
  return bytes.value_or(kSaturated);
}

}  // namespace

struct WasmRobotModelLayer::ActiveImport {
  MeshTask task;
  QFuture<MeshData> future;
  std::unique_ptr<QFutureWatcher<MeshData>> watcher;
};

WasmRobotModelLayer::WasmRobotModelLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent)
    : PJ::ISceneLayer(parent),
      topic_id_(topic_id),
      source_topic_id_(topic_id),
      display_name_(std::move(display_name)),
      source_value_(display_name_),
      mesh_loader_(std::make_unique<MeshLoader>()),
      parse_watcher_(new QFutureWatcher<ParseResult>(this)) {
  connect(parse_watcher_, &QFutureWatcher<ParseResult>::finished, this, &WasmRobotModelLayer::onParseFinished);
}

WasmRobotModelLayer::~WasmRobotModelLayer() = default;

PJ::SceneLayerInfo WasmRobotModelLayer::info() const {
  return PJ::SceneLayerInfo{
      .topic_id = topic_id_,
      .object_type = PJ::sdk::BuiltinObjectType::kRobotDescription,
      .display_name = display_name_.isEmpty() ? tr("Robot model") : display_name_,
      .family_name = u"RobotModel"_s,
      .visible = visible_,
  };
}

PJ::Range<PJ::Timepoint> WasmRobotModelLayer::timeRange() const {
  return {PJ::Timepoint::max(), PJ::Timepoint::min()};
}

bool WasmRobotModelLayer::attach(const PJ::SceneLayerContext& context) {
  session_ = context.session;
  if (source_type_ == SourceType::kTopic) {
    if (session_ == nullptr || !session_->parserBindingForObjectTopic(source_topic_id_)) {
      qCWarning(lcWasmRobotModel) << "attach: no parser for robot-description topic" << source_topic_id_.id;
      session_ = nullptr;
      return false;
    }
  }
  disconnect(reload_connection_);
  if (session_ != nullptr && source_type_ == SourceType::kTopic) {
    reload_connection_ =
        connect(session_, &PJ::SessionManager::datasetAboutToBeReplaced, this, [this](PJ::DatasetId dataset) {
          if (session_ != nullptr && session_->objectStore().descriptor(source_topic_id_).dataset_id == dataset) {
            resetModelState();
            session_ = nullptr;
          }
        });
  }
  loadFromCurrentSource();
  return true;
}

void WasmRobotModelLayer::detach() {
  disconnect(reload_connection_);
  reload_connection_ = {};
  session_ = nullptr;
  tf_buffer_.reset();
  resetModelState();
}

void WasmRobotModelLayer::setTrackerTime(PJ::Timepoint time) {
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
  if (visible_) {
    emit repaintRequested();
  }
}

std::uint64_t WasmRobotModelLayer::renderKey(PJ::Timepoint time) const {
  return static_cast<std::uint64_t>(PJ::toRaw(time)) ^ (model_revision_ * 0x9E3779B185EBCA87ULL);
}

void WasmRobotModelLayer::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  emit visibilityChanged(visible_);
  emit infoChanged();
  emit repaintRequested();
}

void WasmRobotModelLayer::setTransformBuffer(std::shared_ptr<TransformBuffer> buffer) {
  tf_buffer_ = std::move(buffer);
  ensureStaticBridges();
  emit repaintRequested();
}

void WasmRobotModelLayer::setEmbeddedAssets(QMap<QString, QByteArray> assets) {
  embedded_assets_ = std::move(assets);
}

void WasmRobotModelLayer::setSourceTopic(PJ::ObjectTopicId topic_id, QString display_name) {
  source_type_ = SourceType::kTopic;
  source_topic_id_ = topic_id;
  source_value_ = display_name.isEmpty() ? source_value_ : std::move(display_name);
  local_file_bytes_.clear();
  loadFromCurrentSource();
  emit configurationChanged();
}

void WasmRobotModelLayer::setSourceFileContent(QString filename, QByteArray bytes) {
  source_type_ = SourceType::kFile;
  source_value_ = std::move(filename);
  local_file_bytes_ = std::move(bytes);
  loadFromCurrentSource();
  emit configurationChanged();
}

void WasmRobotModelLayer::setSourceUrl(QString url) {
  source_type_ = SourceType::kUrl;
  source_value_ = std::move(url);
  local_file_bytes_.clear();
  loadFromCurrentSource();
  emit configurationChanged();
}

void WasmRobotModelLayer::setFramePrefix(QString prefix) {
  if (frame_prefix_ == prefix) {
    return;
  }
  frame_prefix_ = std::move(prefix);
  source_frame_ =
      model_.has_value() ? (frame_prefix_ + QString::fromStdString(model_->root_link)).toStdString() : std::string{};
  rebuildStaticBridges();
  rebuildDrawCalls();
  emit sourceFrameChanged(QString::fromStdString(source_frame_));
  emit configurationChanged();
  emit repaintRequested();
}

void WasmRobotModelLayer::setDisplayMode(DisplayMode mode) {
  if (display_mode_ == mode) {
    return;
  }
  display_mode_ = mode;
  rebuildDrawCalls();
  emit configurationChanged();
  emit repaintRequested();
}

void WasmRobotModelLayer::setFallbackColor(QColor color) {
  if (!color.isValid() || fallback_color_.rgb() == color.rgb()) {
    return;
  }
  fallback_color_ = std::move(color);
  rebuildDrawCalls();
  emit configurationChanged();
  emit repaintRequested();
}

void WasmRobotModelLayer::setIgnoreColladaUpAxis(bool ignore) {
  if (ignore_collada_up_axis_ == ignore) {
    return;
  }
  ignore_collada_up_axis_ = ignore;
  loadFromCurrentSource();
  emit configurationChanged();
}

void WasmRobotModelLayer::retry() {
  loadFromCurrentSource();
}

void WasmRobotModelLayer::loadFromCurrentSource() {
  resetModelState();
  latch_pending_ = false;
  if (source_type_ == SourceType::kTopic) {
    tryLoadTopicDescription();
    return;
  }
  if (source_type_ == SourceType::kFile) {
    if (local_file_bytes_.isEmpty()) {
      setStatus(tr("Select the local URDF again; browser layouts cannot retain host-file access"));
      setWarning(status_text_);
      return;
    }
    queueParse(local_file_bytes_, u"urdf"_s, source_value_, QString{}, false);
    return;
  }

  const QUrl url(source_value_);
  if (!url.isValid() || (url.scheme() != "http"_L1 && url.scheme() != "https"_L1)) {
    setStatus(tr("Only http(s) URDF URLs are supported in the browser"));
    setWarning(status_text_);
    return;
  }
  url_fetcher_ = std::make_unique<UrlFetcher>();
  const std::uint64_t generation = load_generation_;
  const QString url_text = source_value_;
  setStatus(tr("Fetching %1").arg(url_text));
  QPointer<WasmRobotModelLayer> guard(this);
  url_fetcher_->fetch(QUrl(url_text), [this, guard, generation, url_text](FetchResult result) {
    if (guard.isNull() || generation != load_generation_) {
      return;
    }
    if (!result.ok) {
      setStatus(tr("Fetch failed (%1)").arg(result.error));
      setWarning(status_text_);
      return;
    }
    queueParse(std::move(result.bytes), u"urdf"_s, url_text, urlDirectory(url_text), true);
  });
}

bool WasmRobotModelLayer::tryLoadTopicDescription() {
  if (session_ == nullptr) {
    setStatus(tr("Robot description topic is unavailable"));
    setWarning(status_text_);
    return false;
  }
  const auto binding = session_->parserBindingForObjectTopic(source_topic_id_);
  if (!binding) {
    setStatus(tr("No parser for %1").arg(source_value_));
    setWarning(status_text_);
    return false;
  }
  const auto entry = session_->objectStore().latestAt(source_topic_id_, std::numeric_limits<PJ::Timestamp>::max());
  if (!entry.has_value() || entry->payload.bytes.empty()) {
    latch_pending_ = true;
    last_latch_retry_ = std::chrono::steady_clock::now();
    setStatus(tr("Waiting for %1...").arg(source_value_));
    return true;
  }
  auto object = parseLocked(binding, entry->timestamp, entry->payload);
  if (!object.has_value()) {
    setStatus(tr("Parse error: %1").arg(QString::fromStdString(object.error())));
    setWarning(status_text_);
    return false;
  }
  const auto* description = object->object.get<PJ::sdk::RobotDescription>();
  if (description == nullptr) {
    setStatus(tr("Parse error: parser did not return RobotDescription"));
    setWarning(status_text_);
    return false;
  }
  latch_pending_ = false;
  const QString label = source_value_.isEmpty() ? QString::fromStdString(description->topic) : source_value_;
  queueParse(
      QByteArray::fromStdString(description->text), QString::fromStdString(description->format), label, QString{},
      false);
  return true;
}

void WasmRobotModelLayer::queueParse(
    QByteArray bytes, QString format, QString label, QString base, bool source_is_url) {
  if (!browserModelSourceFits(static_cast<std::uint64_t>(bytes.size()))) {
    setStatus(tr("robot_description exceeds the %1 MiB browser limit")
                  .arg(kBrowserMaxModelSourceBytes / (1024ULL * 1024ULL)));
    setWarning(status_text_);
    return;
  }
  if (format.compare("urdf"_L1, Qt::CaseInsensitive) != 0) {
    setStatus(tr("Format '%1' is not supported — only URDF").arg(format));
    setWarning(status_text_);
    return;
  }
  auto resolver = std::make_shared<UrdfPackageResolver>();
  resolver->setEmbeddedAssets(embedded_assets_);
  ParseRequest request{
      .bytes = std::move(bytes),
      .format = std::move(format),
      .label = std::move(label),
      .base = std::move(base),
      .source_is_url = source_is_url,
      .generation = next_parse_generation_++,
      .resolver = std::move(resolver),
  };
  wanted_parse_generation_ = request.generation;
  setStatus(tr("Parsing %1...").arg(request.label));
  if (parse_watcher_->isRunning()) {
    pending_parse_ = std::move(request);
  } else {
    startParse(std::move(request));
  }
}

void WasmRobotModelLayer::startParse(ParseRequest request) {
  running_parse_generation_ = request.generation;
  parse_watcher_->setFuture(QtConcurrent::run([request = std::move(request)]() mutable {
    return WasmRobotModelLayer::parse(std::move(request));
  }));
}

WasmRobotModelLayer::ParseResult WasmRobotModelLayer::parse(ParseRequest request) {
  ParseResult result;
  result.generation = request.generation;
  result.label = request.label;
  result.resolver = request.resolver;
  auto parsed = parseUrdf(
      request.bytes.toStdString(), request.resolver.get(), request.base.toStdString(), request.source_is_url,
      request.label.toStdString(), &kBrowserUrdfLimits);
  result.model = std::move(parsed.first);
  result.error = QString::fromStdString(parsed.second);
  return result;
}

void WasmRobotModelLayer::onParseFinished() {
  QFuture<ParseResult> completed = parse_watcher_->future();
  ParseResult result;
  try {
    result = completed.takeResult();
  } catch (const std::exception& error) {
    result.generation = running_parse_generation_;
    result.error = tr("URDF parse threw: %1").arg(QString::fromUtf8(error.what()));
  } catch (...) {
    result.generation = running_parse_generation_;
    result.error = tr("URDF parse threw an unknown exception");
  }
  if (result.generation == wanted_parse_generation_) {
    applyParsed(std::move(result));
  }
  if (pending_parse_.has_value()) {
    ParseRequest next = std::move(*pending_parse_);
    pending_parse_.reset();
    startParse(std::move(next));
  }
}

void WasmRobotModelLayer::applyParsed(ParseResult result) {
  if (!result.model.has_value()) {
    setStatus(result.error.isEmpty() ? tr("Could not parse URDF") : std::move(result.error));
    setWarning(status_text_);
    return;
  }
  model_ = std::move(result.model);
  resolver_ = std::move(result.resolver);
  source_frame_ = (frame_prefix_ + QString::fromStdString(model_->root_link)).toStdString();
  model_has_visuals_ = std::any_of(
      model_->links.cbegin(), model_->links.cend(), [](const RobotLink& link) { return !link.visuals.empty(); });
  rebuildStaticBridges();

  auto retain = [this](std::string key, MeshData mesh) {
    const std::uint64_t bytes = estimateMeshBytes(mesh);
    mesh_retained_bytes_ = saturatedAdd(mesh_retained_bytes_, bytes);
    model_meshes_.emplace(std::move(key), std::make_shared<MeshData>(std::move(mesh)));
  };
  retain(std::string(kBoxKey), makeCube({0.7F, 0.7F, 0.7F, 1.0F}));
  retain(std::string(kCylinderKey), makeCylinder());
  retain(std::string(kSphereKey), makeSphere());
  retain(std::string(kPlaceholderKey), makeCube({1.0F, 0.0F, 1.0F, 1.0F}));

  base_status_ = tr("URDF: %1").arg(result.label);
  queueMeshLoads();
  rebuildDrawCalls();
  ensureStaticBridges();
  updateStatus();
  emit sourceFrameChanged(QString::fromStdString(source_frame_));
  emit repaintRequested();
}

void WasmRobotModelLayer::resetModelState() {
  wanted_parse_generation_ = next_parse_generation_++;
  pending_parse_.reset();
  load_generation_ = next_load_generation_++;
  url_fetcher_.reset();
  active_fetch_.reset();
  active_import_.reset();
  mesh_queue_.clear();
  mesh_errors_.clear();
  if (mesh_loader_ != nullptr) {
    mesh_loader_->clearCache();
  }
  model_.reset();
  resolver_.reset();
  static_bridges_.clear();
  model_has_visuals_ = false;
  source_frame_.clear();
  total_mesh_count_ = 0;
  loaded_mesh_count_ = 0;
  unresolved_mesh_count_ = 0;
  mesh_retained_bytes_ = 0;
  model_meshes_.clear();
  model_draw_calls_.clear();
  base_status_.clear();
  render_warning_.clear();
  ++model_revision_;
  emit sourceFrameChanged({});
  emit repaintRequested();
}

void WasmRobotModelLayer::rebuildStaticBridges() {
  static_bridges_.clear();
  if (!model_.has_value()) {
    return;
  }
  static_bridges_ = fixedJointStaticTransforms(*model_);
  if (!frame_prefix_.isEmpty()) {
    const std::string prefix = frame_prefix_.toStdString();
    for (StampedTransform& transform : static_bridges_) {
      transform.parent_frame = prefix + transform.parent_frame;
      transform.child_frame = prefix + transform.child_frame;
    }
  }
  ensureStaticBridges();
}

void WasmRobotModelLayer::ensureStaticBridges() {
  if (tf_buffer_ != nullptr && !static_bridges_.empty()) {
    injectMissingStaticTransforms(*tf_buffer_, static_bridges_);
  }
}

void WasmRobotModelLayer::rebuildDrawCalls() {
  model_draw_calls_.clear();
  loaded_mesh_count_ = 0;
  if (!model_.has_value()) {
    ++model_revision_;
    return;
  }
  const std::string prefix = frame_prefix_.toStdString();
  const glm::vec4 placeholder{1.0F, 0.0F, 1.0F, 1.0F};
  const auto count_loaded = [this](const LinkGeom& geometry) {
    const auto* mesh = std::get_if<GeomMesh>(&geometry.shape);
    if (mesh != nullptr && mesh->resolved && model_meshes_.contains(mesh->resolved_path)) {
      ++loaded_mesh_count_;
    }
  };
  for (const RobotLink& link : model_->links) {
    for (const LinkGeom& geometry : link.visuals) {
      count_loaded(geometry);
    }
    for (const LinkGeom& geometry : link.collisions) {
      count_loaded(geometry);
    }
  }
  const auto append = [this, &prefix, &placeholder](const RobotLink& link, const LinkGeom& geometry, bool collision) {
    WasmModelDrawCall draw;
    draw.frame_id = prefix + link.name;
    draw.model = originToMat4(geometry.origin_xyz, geometry.origin_rpy);
    draw.group = collision ? WasmModelDrawGroup::kCollision : WasmModelDrawGroup::kVisual;
    draw.use_material = !geometry.has_color;
    draw.override_color = geometry.has_color ? geometry.color : glm::vec4(1.0F);
    if (collision && !geometry.has_color) {
      draw.use_material = false;
      draw.override_color = look::kCollisionDefaultColor;
    }

    if (const auto* box = std::get_if<GeomBox>(&geometry.shape); box != nullptr) {
      draw.mesh_key = std::string(kBoxKey);
      draw.model = glm::scale(draw.model, glm::dvec3(box->size));
    } else if (const auto* cylinder = std::get_if<GeomCylinder>(&geometry.shape); cylinder != nullptr) {
      draw.mesh_key = std::string(kCylinderKey);
      draw.model = glm::scale(draw.model, glm::dvec3(cylinder->radius, cylinder->radius, cylinder->length));
    } else if (const auto* sphere = std::get_if<GeomSphere>(&geometry.shape); sphere != nullptr) {
      draw.mesh_key = std::string(kSphereKey);
      draw.model = glm::scale(draw.model, glm::dvec3(sphere->radius));
    } else if (const auto* mesh = std::get_if<GeomMesh>(&geometry.shape); mesh != nullptr) {
      draw.model = glm::scale(draw.model, mesh->scale);
      const auto loaded = model_meshes_.find(mesh->resolved_path);
      if (mesh->resolved && loaded != model_meshes_.end()) {
        draw.mesh_key = mesh->resolved_path;
      } else {
        draw.mesh_key = std::string(kPlaceholderKey);
        draw.use_material = false;
        draw.override_color = placeholder;
      }
    }
    model_draw_calls_.push_back(std::move(draw));
  };

  for (const RobotLink& link : model_->links) {
    if (display_mode_ == DisplayMode::kCollision) {
      for (const LinkGeom& geometry : link.collisions) {
        append(link, geometry, true);
      }
    } else if (display_mode_ == DisplayMode::kVisual) {
      for (const LinkGeom& geometry : link.visuals) {
        append(link, geometry, false);
      }
    } else if (!link.visuals.empty()) {
      for (const LinkGeom& geometry : link.visuals) {
        append(link, geometry, false);
      }
    } else {
      for (const LinkGeom& geometry : link.collisions) {
        append(link, geometry, model_has_visuals_);
      }
    }
  }
  ++model_revision_;
}

void WasmRobotModelLayer::queueMeshLoads() {
  total_mesh_count_ = 0;
  unresolved_mesh_count_ = 0;
  if (!model_.has_value()) {
    return;
  }
  std::unordered_set<std::string> queued;
  const auto inspect = [this, &queued](const LinkGeom& geometry) {
    const auto* mesh = std::get_if<GeomMesh>(&geometry.shape);
    if (mesh == nullptr) {
      return;
    }
    ++total_mesh_count_;
    if (!mesh->resolved || mesh->resolved_path.empty()) {
      ++unresolved_mesh_count_;
      return;
    }
    if (!queued.insert(mesh->resolved_path).second) {
      return;
    }
    const QString source = QString::fromStdString(mesh->resolved_path);
    const QUrl url(source);
    mesh_queue_.push_back(
        MeshTask{
            .key = mesh->resolved_path,
            .source = source,
            .format_hint = formatHint(source),
            .is_url = url.scheme() == "http"_L1 || url.scheme() == "https"_L1,
            .generation = load_generation_,
        });
  };
  for (const RobotLink& link : model_->links) {
    for (const LinkGeom& geometry : link.visuals) {
      inspect(geometry);
    }
    for (const LinkGeom& geometry : link.collisions) {
      inspect(geometry);
    }
  }
  pumpMeshLoads();
}

void WasmRobotModelLayer::pumpMeshLoads() {
  if (active_fetch_.has_value() || active_import_ != nullptr || mesh_queue_.empty()) {
    return;
  }
  MeshTask task = std::move(mesh_queue_.front());
  mesh_queue_.pop_front();
  if (task.generation != load_generation_) {
    pumpMeshLoads();
    return;
  }
  if (!task.is_url) {
    startMeshImport(std::move(task));
    return;
  }
  if (url_fetcher_ == nullptr) {
    url_fetcher_ = std::make_unique<UrlFetcher>();
  }
  active_fetch_ = task;
  QPointer<WasmRobotModelLayer> guard(this);
  url_fetcher_->fetch(QUrl(task.source), [this, guard, task](FetchResult result) mutable {
    if (guard.isNull() || !active_fetch_.has_value() || active_fetch_->generation != task.generation ||
        active_fetch_->key != task.key) {
      return;
    }
    active_fetch_.reset();
    if (task.generation != load_generation_) {
      pumpMeshLoads();
      return;
    }
    if (!result.ok) {
      mesh_errors_[task.key] = std::move(result.error);
      rebuildDrawCalls();
      updateStatus();
      emit repaintRequested();
      pumpMeshLoads();
      return;
    }
    startMeshImport(std::move(task), std::move(result.bytes));
  });
}

void WasmRobotModelLayer::startMeshImport(MeshTask task, QByteArray bytes) {
  if (task.generation != load_generation_ || active_import_ != nullptr) {
    return;
  }
  try {
    auto active = std::make_unique<ActiveImport>();
    active->task = std::move(task);
    const bool collada = active->task.format_hint == "dae"_L1 || active->task.format_hint == "collada"_L1;
    const std::optional<bool> flip = ignore_collada_up_axis_ && collada ? std::optional<bool>(false) : std::nullopt;
    active->future = bytes.isEmpty() ? mesh_loader_->load(active->task.source, flip)
                                     : mesh_loader_->loadFromMemory(bytes, active->task.format_hint, flip);
    active->watcher = std::make_unique<QFutureWatcher<MeshData>>();
    connect(active->watcher.get(), &QFutureWatcher<MeshData>::finished, this, &WasmRobotModelLayer::finishMeshImport);
    QFutureWatcher<MeshData>* watcher = active->watcher.get();
    active_import_ = std::move(active);
    watcher->setFuture(active_import_->future);
  } catch (const std::exception& error) {
    mesh_errors_[task.key] = tr("could not start import: %1").arg(QString::fromUtf8(error.what()));
    updateStatus();
    pumpMeshLoads();
  } catch (...) {
    mesh_errors_[task.key] = tr("could not start import");
    updateStatus();
    pumpMeshLoads();
  }
}

void WasmRobotModelLayer::finishMeshImport() {
  if (active_import_ == nullptr || !active_import_->future.isFinished()) {
    return;
  }
  std::unique_ptr<ActiveImport> completed_job = std::move(active_import_);
  const MeshTask task = completed_job->task;
  QFuture<MeshData> completed = std::move(completed_job->future);
  if (QFutureWatcher<MeshData>* watcher = completed_job->watcher.release(); watcher != nullptr) {
    watcher->deleteLater();
  }
  MeshData mesh;
  try {
    mesh = completed.takeResult();
  } catch (const std::exception& error) {
    mesh.error = tr("mesh import threw: %1").arg(QString::fromUtf8(error.what()));
  } catch (...) {
    mesh.error = tr("mesh import threw an unknown exception");
  }
  if (task.generation != load_generation_) {
    pumpMeshLoads();
    return;
  }
  if (!mesh.ok) {
    mesh_errors_[task.key] = mesh.error.isEmpty() ? tr("mesh contains no renderable geometry") : std::move(mesh.error);
  } else {
    const std::uint64_t retained = estimateMeshBytes(mesh);
    if (retained == kSaturated || retained > kBrowserMaxModelRetainedBytesPerMesh ||
        mesh_retained_bytes_ > kBrowserMaxModelRetainedBytesPerLayer ||
        retained > kBrowserMaxModelRetainedBytesPerLayer - mesh_retained_bytes_) {
      mesh_errors_[task.key] = tr("mesh would exceed the browser layer memory limit");
    } else {
      try {
        model_meshes_[task.key] = std::make_shared<MeshData>(std::move(mesh));
        mesh_retained_bytes_ += retained;
        mesh_errors_.erase(task.key);
      } catch (const std::exception& error) {
        mesh_errors_[task.key] = tr("could not retain mesh: %1").arg(QString::fromUtf8(error.what()));
      } catch (...) {
        mesh_errors_[task.key] = tr("could not retain mesh");
      }
    }
  }
  rebuildDrawCalls();
  updateStatus();
  emit repaintRequested();
  pumpMeshLoads();
}

QString WasmRobotModelLayer::unresolvedClause() const {
  if (unresolved_mesh_count_ == 0) {
    return {};
  }
  if (source_type_ == SourceType::kUrl) {
    return tr(
        " • %n mesh reference(s) unresolved; URL/package paths must be CORS-accessible", nullptr,
        unresolved_mesh_count_);
  }
  return tr(
      " • %n mesh reference(s) unavailable in the browser sandbox (shown as magenta placeholders)", nullptr,
      unresolved_mesh_count_);
}

QString WasmRobotModelLayer::loadFailureClause() const {
  if (mesh_errors_.empty()) {
    return {};
  }
  return tr(
      " • %n mesh load(s) failed (shown as magenta placeholders)", nullptr, static_cast<int>(mesh_errors_.size()));
}

void WasmRobotModelLayer::updateStatus() {
  QString status = base_status_;
  if (model_.has_value()) {
    status += tr(" • %1/%2 meshes").arg(loaded_mesh_count_).arg(total_mesh_count_);
  }
  status += unresolvedClause();
  status += loadFailureClause();
  if (!render_warning_.isEmpty()) {
    status += tr(" • %1").arg(render_warning_);
  }
  setStatus(status);
  QString warning;
  if (unresolved_mesh_count_ != 0 || !mesh_errors_.empty()) {
    warning = unresolvedClause() + loadFailureClause();
  }
  if (!render_warning_.isEmpty()) {
    warning += (warning.isEmpty() ? QString{} : u" • "_s) + render_warning_;
  }
  setWarning(warning.trimmed());
}

void WasmRobotModelLayer::setStatus(QString status) {
  if (status_text_ == status) {
    return;
  }
  status_text_ = std::move(status);
  emit statusTextChanged(status_text_);
}

void WasmRobotModelLayer::setWarning(QString warning) {
  if (warning_reason_ == warning) {
    return;
  }
  warning_reason_ = std::move(warning);
  emit warningChanged(!warning_reason_.isEmpty(), warning_reason_);
}

void WasmRobotModelLayer::noteRenderFailure(QString warning) {
  if (render_warning_ == warning) {
    return;
  }
  render_warning_ = std::move(warning);
  updateStatus();
}

void WasmRobotModelLayer::noteRenderSuccess() {
  if (render_warning_.isEmpty()) {
    return;
  }
  render_warning_.clear();
  updateStatus();
}

bool WasmRobotModelLayer::validateXml(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "robot_model"_L1 || !detail::isLeafPayload(element)) {
    return false;
  }
  const QString source = element.attribute(u"source_type"_s, u"topic"_s);
  const QString mode = element.attribute(u"display_mode"_s, u"auto"_s);
  bool visible = true;
  bool ignore = false;
  return (source == "topic"_L1 || source == "file"_L1 || source == "url"_L1) &&
         (mode == "auto"_L1 || mode == "visual"_L1 || mode == "collision"_L1) &&
         detail::parseTrueFalse(element, "visible", true, visible) &&
         detail::parseTrueFalse(element, "ignore_collada_up_axis", false, ignore) &&
         (!element.hasAttribute(u"color"_s) || QColor(element.attribute(u"color"_s)).isValid());
}

QDomElement WasmRobotModelLayer::xmlSaveState(QDomDocument& document) const {
  QDomElement element = document.createElement(u"robot_model"_s);
  element.setAttribute(u"source_type"_s, sourceTypeName(source_type_));
  element.setAttribute(u"source_value"_s, source_value_);
  element.setAttribute(u"frame_prefix"_s, frame_prefix_);
  element.setAttribute(u"display_mode"_s, displayModeName(display_mode_));
  element.setAttribute(u"visible"_s, visible_ ? u"true"_s : u"false"_s);
  element.setAttribute(u"color"_s, fallback_color_.name(QColor::HexRgb));
  element.setAttribute(u"ignore_collada_up_axis"_s, ignore_collada_up_axis_ ? u"true"_s : u"false"_s);
  return element;
}

bool WasmRobotModelLayer::xmlLoadState(const QDomElement& element) {
  if (!validateXml(element)) {
    return false;
  }
  const QString source = element.attribute(u"source_type"_s, u"topic"_s);
  source_type_ = source == "file"_L1 ? SourceType::kFile : source == "url"_L1 ? SourceType::kUrl : SourceType::kTopic;
  source_value_ = element.attribute(u"source_value"_s, source_value_);
  frame_prefix_ = element.attribute(u"frame_prefix"_s);
  const QString mode = element.attribute(u"display_mode"_s, u"auto"_s);
  display_mode_ = mode == "visual"_L1      ? DisplayMode::kVisual
                  : mode == "collision"_L1 ? DisplayMode::kCollision
                                           : DisplayMode::kAuto;
  detail::parseTrueFalse(element, "visible", true, visible_);
  detail::parseTrueFalse(element, "ignore_collada_up_axis", false, ignore_collada_up_axis_);
  fallback_color_ = QColor(element.attribute(u"color"_s, fallback_color_.name(QColor::HexRgb)));
  if (source_type_ == SourceType::kFile) {
    local_file_bytes_.clear();
  }
  loadFromCurrentSource();
  emit infoChanged();
  emit visibilityChanged(visible_);
  emit configurationChanged();
  return true;
}

QWidget* WasmRobotModelLayer::createConfigWidget(QWidget* parent) {
  auto* container = new QWidget(parent);
  auto* layout = new QVBoxLayout(container);
  layout->setContentsMargins(0, 0, 0, 0);
  auto* form = new QFormLayout();
  layout->addLayout(form);

  auto* source = new QLineEdit(source_value_, container);
  source->setObjectName(u"wasmRobotSource"_s);
  source->setReadOnly(true);
  form->addRow(tr("Source"), source);
  auto* status = new QLabel(status_text_, container);
  status->setObjectName(u"wasmRobotStatus"_s);
  status->setWordWrap(true);
  form->addRow(tr("Status"), status);

  auto* prefix = new QLineEdit(frame_prefix_, container);
  prefix->setObjectName(u"wasmRobotFramePrefix"_s);
  prefix->setPlaceholderText(tr("e.g. robot1/"));
  form->addRow(tr("Frame prefix"), prefix);
  auto* mode = new PJ::ComboBox(container);
  mode->setObjectName(u"wasmRobotDisplayMode"_s);
  mode->addItem(tr("Auto"), static_cast<int>(DisplayMode::kAuto));
  mode->addItem(tr("Visual"), static_cast<int>(DisplayMode::kVisual));
  mode->addItem(tr("Collision"), static_cast<int>(DisplayMode::kCollision));
  mode->setCurrentIndex(mode->findData(static_cast<int>(display_mode_)));
  form->addRow(tr("Display mode"), mode);
  auto* color = new PJ::ColorPickerWidget(container);
  color->setObjectName(u"wasmRobotFallbackColor"_s);
  color->setColor(fallback_color_);
  form->addRow(tr("Color"), color);
  auto* collada = new PJ::CheckButton(tr("Ignore COLLADA up_axis"), container);
  collada->setObjectName(u"wasmRobotIgnoreCollada"_s);
  collada->setChecked(ignore_collada_up_axis_);
  form->addRow(collada);
  auto* retry_button = new QPushButton(tr("Retry"), container);
  retry_button->setObjectName(u"wasmRobotRetry"_s);
  form->addRow(retry_button);
  if (source_type_ == SourceType::kFile && local_file_bytes_.isEmpty()) {
    auto* note = new QLabel(
        tr("A browser layout cannot reopen a host file automatically. Remove this row and select the URDF again."),
        container);
    note->setWordWrap(true);
    layout->addWidget(note);
  }
  layout->addStretch(1);

  connect(this, &WasmRobotModelLayer::statusTextChanged, status, &QLabel::setText);
  connect(prefix, &QLineEdit::editingFinished, this, [this, prefix]() { setFramePrefix(prefix->text()); });
  connect(mode, &QComboBox::currentIndexChanged, this, [this, mode](int) {
    setDisplayMode(static_cast<DisplayMode>(mode->currentData().toInt()));
  });
  connect(color, &PJ::ColorPickerWidget::colorChanged, this, &WasmRobotModelLayer::setFallbackColor);
  connect(collada, &PJ::CheckButton::toggled, this, &WasmRobotModelLayer::setIgnoreColladaUpAxis);
  connect(retry_button, &QPushButton::clicked, this, &WasmRobotModelLayer::retry);
  return container;
}

}  // namespace pj::scene3d
