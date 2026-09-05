// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/wasm/scene_entities_layer_wasm.h"

#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLoggingCategory>
#include <QSettings>
#include <QStringList>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrentRun>
#include <algorithm>
#include <any>
#include <array>
#include <cmath>
#include <exception>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <limits>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "layers/layer_xml_validation.h"
#include "mesh_loader.h"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/model_budget.h"
#include "pj_scene3d_widgets/resolve_object.h"
#include "pj_widgets/ColorPickerWidget.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/ToggleSwitch.h"
#include "url_fetcher.h"

using namespace Qt::StringLiterals;

namespace pj::scene3d {
namespace {

Q_LOGGING_CATEGORY(lcWasmSceneEntities, "pj.scene3d.wasm.scene_entities")

constexpr std::uint64_t kSaturated = std::numeric_limits<std::uint64_t>::max();

std::uint64_t saturatedAdd(std::uint64_t left, std::uint64_t right);
std::uint64_t saturatedMultiply(std::uint64_t left, std::uint64_t right);

std::uint64_t fnv1a(const std::vector<std::uint8_t>& bytes) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const std::uint8_t byte : bytes) {
    hash = (hash ^ byte) * 1099511628211ULL;
  }
  return hash;
}

std::string modelSourceSignature(const PJ::sdk::ModelPrimitive& primitive) {
  if (!primitive.data.empty()) {
    return "data:" + primitive.media_type + ':' + std::to_string(primitive.data.size()) + ':' +
           std::to_string(fnv1a(primitive.data));
  }
  if (!primitive.url.empty()) {
    return "url:" + primitive.url + ':' + primitive.media_type;
  }
  return {};
}

QString modelFormatHint(const PJ::sdk::ModelPrimitive& primitive) {
  const QString media = QString::fromStdString(primitive.media_type).toLower();
  if (media == "model/gltf-binary"_L1) {
    return u"glb"_s;
  }
  if (media == "model/gltf+json"_L1 || media == "model/gltf"_L1) {
    return u"gltf"_s;
  }
  if (media == "model/vnd.collada+xml"_L1) {
    return u"dae"_s;
  }
  if (media == "model/stl"_L1) {
    return u"stl"_s;
  }
  if (media == "model/obj"_L1) {
    return u"obj"_s;
  }
  const QUrl url(QString::fromStdString(primitive.url));
  return QFileInfo(url.isValid() ? url.path() : QString::fromStdString(primitive.url)).suffix();
}

std::string modelKey(PJ::ObjectTopicId topic, const std::string& entity_id, std::size_t index) {
  return std::to_string(topic.id) + ':' + entity_id + ':' + std::to_string(index);
}

std::uint64_t estimateSnapshotBytes(const PJ::sdk::SceneEntities& snapshot) {
  std::uint64_t bytes = sizeof(snapshot);
  bytes = saturatedAdd(bytes, saturatedMultiply(snapshot.entities.capacity(), sizeof(PJ::sdk::SceneEntity)));
  bytes = saturatedAdd(bytes, saturatedMultiply(snapshot.deletions.capacity(), sizeof(PJ::sdk::SceneEntityDeletion)));
  for (const PJ::sdk::SceneEntity& entity : snapshot.entities) {
    bytes = saturatedAdd(bytes, entity.id.capacity());
    bytes = saturatedAdd(bytes, entity.frame_id.capacity());
    bytes = saturatedAdd(bytes, saturatedMultiply(entity.metadata.capacity(), sizeof(PJ::sdk::KeyValuePair)));
    for (const PJ::sdk::KeyValuePair& pair : entity.metadata) {
      bytes = saturatedAdd(bytes, pair.key.capacity());
      bytes = saturatedAdd(bytes, pair.value.capacity());
    }
    bytes = saturatedAdd(bytes, saturatedMultiply(entity.arrows.capacity(), sizeof(PJ::sdk::ArrowPrimitive)));
    bytes = saturatedAdd(bytes, saturatedMultiply(entity.cubes.capacity(), sizeof(PJ::sdk::CubePrimitive)));
    bytes = saturatedAdd(bytes, saturatedMultiply(entity.spheres.capacity(), sizeof(PJ::sdk::SpherePrimitive)));
    bytes = saturatedAdd(bytes, saturatedMultiply(entity.cylinders.capacity(), sizeof(PJ::sdk::CylinderPrimitive)));
    bytes = saturatedAdd(bytes, saturatedMultiply(entity.lines.capacity(), sizeof(PJ::sdk::LinePrimitive)));
    for (const PJ::sdk::LinePrimitive& line : entity.lines) {
      bytes = saturatedAdd(bytes, saturatedMultiply(line.points.capacity(), sizeof(PJ::sdk::Point3)));
      bytes = saturatedAdd(bytes, saturatedMultiply(line.colors.capacity(), sizeof(PJ::sdk::ColorRGBA)));
      bytes = saturatedAdd(bytes, saturatedMultiply(line.indices.capacity(), sizeof(std::uint32_t)));
    }
    bytes = saturatedAdd(bytes, saturatedMultiply(entity.triangles.capacity(), sizeof(PJ::sdk::TrianglePrimitive)));
    for (const PJ::sdk::TrianglePrimitive& triangle : entity.triangles) {
      bytes = saturatedAdd(bytes, saturatedMultiply(triangle.points.capacity(), sizeof(PJ::sdk::Point3)));
      bytes = saturatedAdd(bytes, saturatedMultiply(triangle.colors.capacity(), sizeof(PJ::sdk::ColorRGBA)));
      bytes = saturatedAdd(bytes, saturatedMultiply(triangle.indices.capacity(), sizeof(std::uint32_t)));
    }
    bytes = saturatedAdd(bytes, saturatedMultiply(entity.texts.capacity(), sizeof(PJ::sdk::TextPrimitive)));
    for (const PJ::sdk::TextPrimitive& text : entity.texts) {
      bytes = saturatedAdd(bytes, text.text.capacity());
    }
    bytes = saturatedAdd(bytes, saturatedMultiply(entity.models.capacity(), sizeof(PJ::sdk::ModelPrimitive)));
    for (const PJ::sdk::ModelPrimitive& model : entity.models) {
      bytes = saturatedAdd(bytes, model.data.capacity());
      bytes = saturatedAdd(bytes, model.url.capacity());
      bytes = saturatedAdd(bytes, model.media_type.capacity());
    }
    bytes = saturatedAdd(bytes, saturatedMultiply(entity.axes.capacity(), sizeof(PJ::sdk::AxesPrimitive)));
  }
  for (const PJ::sdk::SceneEntityDeletion& deletion : snapshot.deletions) {
    bytes = saturatedAdd(bytes, deletion.id.capacity());
  }
  return bytes;
}

std::uint64_t estimateMeshBytes(const MeshData& mesh) {
  std::uint64_t texture_bytes = 0;
  std::unordered_set<const Material*> counted_materials;
  for (const SubMesh& submesh : mesh.submeshes) {
    if (submesh.material == nullptr || !counted_materials.insert(submesh.material.get()).second) {
      continue;
    }
    const std::array<const TextureSource*, 5> textures{
        &submesh.material->base_color, &submesh.material->metallic_roughness, &submesh.material->normal,
        &submesh.material->occlusion, &submesh.material->emissive};
    for (const TextureSource* texture : textures) {
      if (!texture->bytes.empty()) {
        texture_bytes = saturatedAdd(texture_bytes, texture->bytes.size());
      }
    }
  }
  const auto retained = browserModelRetainedBytes(
      mesh.vertices.size(), sizeof(Vertex), mesh.indices.size(), mesh.submeshes.size(), sizeof(SubMesh), texture_bytes);
  return retained.value_or(kSaturated);
}

bool remoteModelFetchAllowed() {
  return QSettings().value(u"pj_scene3d/allow_remote_model_fetch"_s, true).toBool();
}

std::uint64_t saturatedAdd(std::uint64_t left, std::uint64_t right) {
  return right > kSaturated - left ? kSaturated : left + right;
}

std::uint64_t saturatedMultiply(std::uint64_t left, std::uint64_t right) {
  return left != 0U && right > kSaturated / left ? kSaturated : left * right;
}

bool finite(const glm::dmat4& matrix) {
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      if (!std::isfinite(matrix[column][row])) {
        return false;
      }
    }
  }
  return true;
}

bool finite(const glm::dvec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

std::optional<glm::dmat4> poseMatrix(const PJ::sdk::Pose& pose) {
  const glm::dvec3 translation{pose.position.x, pose.position.y, pose.position.z};
  const glm::dquat orientation{pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z};
  const double norm = glm::length(orientation);
  if (!finite(translation) || !std::isfinite(norm) || norm < 1e-15) {
    return std::nullopt;
  }
  const glm::dmat4 matrix = glm::translate(glm::dmat4(1.0), translation) * glm::mat4_cast(orientation / norm);
  return finite(matrix) ? std::optional<glm::dmat4>{matrix} : std::nullopt;
}

glm::vec4 colorVector(const PJ::sdk::ColorRGBA& color) {
  return {
      color.r / 255.0F,
      color.g / 255.0F,
      color.b / 255.0F,
      color.a / 255.0F,
  };
}

glm::dvec3 pointVector(const PJ::sdk::Point3& point) {
  return {point.x, point.y, point.z};
}

glm::dvec3 sizeVector(const PJ::sdk::Vector3& size) {
  return {size.x, size.y, size.z};
}

void expandBounds(WasmMarkerBounds& bounds, const glm::dvec3& point) {
  if (!finite(point)) {
    return;
  }
  if (!bounds.valid) {
    bounds.min = point;
    bounds.max = point;
    bounds.valid = true;
    return;
  }
  bounds.min = glm::min(bounds.min, point);
  bounds.max = glm::max(bounds.max, point);
}

void expandUnitBounds(
    WasmMarkerBounds& bounds, const glm::dmat4& model, const glm::dvec3& minimum, const glm::dvec3& maximum) {
  for (int corner = 0; corner < 8; ++corner) {
    const glm::dvec3 point{
        (corner & 1) != 0 ? maximum.x : minimum.x,
        (corner & 2) != 0 ? maximum.y : minimum.y,
        (corner & 4) != 0 ? maximum.z : minimum.z,
    };
    expandBounds(bounds, glm::dvec3(model * glm::dvec4(point, 1.0)));
  }
}

struct Preflight {
  std::uint64_t instances = 0;
  std::uint64_t stream_vertices = 0;
  std::uint64_t frames = 0;
  std::uint64_t retained_bytes = 0;
  std::uint64_t cubes = 0;
  std::uint64_t spheres = 0;
  std::uint64_t cylinders = 0;
  std::uint64_t arrows = 0;
  std::uint64_t axes_arms = 0;
  std::uint64_t line_batches = 0;
  std::uint64_t triangle_batches = 0;
  std::uint64_t frame_string_bytes = 0;
  QString error;
};

std::uint64_t lineExpandedVertexCount(const PJ::sdk::LinePrimitive& line) {
  const std::uint64_t count = line.indices.empty() ? line.points.size() : line.indices.size();
  switch (line.type) {
    case PJ::sdk::LineType::kLineList:
      return count - count % 2U;
    case PJ::sdk::LineType::kLineStrip:
      return count >= 2U ? saturatedMultiply(count - 1U, 2U) : 0U;
    case PJ::sdk::LineType::kLineLoop:
      return count >= 2U ? saturatedMultiply(count, 2U) : 0U;
  }
  return 0U;
}

Preflight preflight(const PJ::sdk::SceneEntities& scene) {
  Preflight output;
  std::unordered_map<std::string, std::uint32_t> frames;
  for (const PJ::sdk::SceneEntity& entity : scene.entities) {
    const bool inserted = frames.try_emplace(entity.frame_id, static_cast<std::uint32_t>(frames.size())).second;
    if (inserted) {
      output.frame_string_bytes = saturatedAdd(output.frame_string_bytes, saturatedAdd(entity.frame_id.size(), 1U));
    }
    if (frames.size() > kBrowserMaxMarkerFramesPerLayer) {
      output.frames = frames.size();
      output.error = QObject::tr("SceneEntities exceeds the browser limit of %1 distinct frames")
                         .arg(kBrowserMaxMarkerFramesPerLayer);
      return output;
    }
    output.cubes = saturatedAdd(output.cubes, entity.cubes.size());
    output.spheres = saturatedAdd(output.spheres, entity.spheres.size());
    output.cylinders = saturatedAdd(output.cylinders, entity.cylinders.size());
    output.arrows = saturatedAdd(output.arrows, entity.arrows.size());
    output.axes_arms = saturatedAdd(output.axes_arms, saturatedMultiply(entity.axes.size(), 3U));

    for (const PJ::sdk::LinePrimitive& line : entity.lines) {
      const std::uint64_t vertices = lineExpandedVertexCount(line);
      output.stream_vertices = saturatedAdd(output.stream_vertices, vertices);
      output.line_batches = saturatedAdd(output.line_batches, vertices == 0U ? 0U : 1U);
    }
    for (const PJ::sdk::TrianglePrimitive& triangles : entity.triangles) {
      const std::uint64_t count = triangles.indices.empty() ? triangles.points.size() : triangles.indices.size();
      const std::uint64_t triangle_count = count / 3U;
      // Reserve for the larger WebGL wireframe representation: six line
      // vertices for every three triangle vertices.
      output.stream_vertices = saturatedAdd(output.stream_vertices, saturatedMultiply(triangle_count, 6U));
      output.triangle_batches = saturatedAdd(output.triangle_batches, triangle_count == 0U ? 0U : 1U);
    }
  }
  output.instances = saturatedAdd(
      saturatedAdd(output.cubes, output.spheres),
      saturatedAdd(saturatedAdd(output.cylinders, output.arrows), output.axes_arms));
  output.frames = frames.size();
  output.retained_bytes = saturatedMultiply(output.instances, sizeof(WasmMarkerInstanceSource));
  output.retained_bytes =
      saturatedAdd(output.retained_bytes, saturatedMultiply(output.stream_vertices, sizeof(WasmMarkerStreamVertex)));
  output.retained_bytes = saturatedAdd(
      output.retained_bytes, saturatedMultiply(output.frames, sizeof(WasmMarkerBounds) + sizeof(std::string)));
  output.retained_bytes = saturatedAdd(output.retained_bytes, output.frame_string_bytes);
  output.retained_bytes = saturatedAdd(
      output.retained_bytes,
      saturatedMultiply(saturatedAdd(output.line_batches, output.triangle_batches), sizeof(WasmMarkerStreamBatch)));
  if (!browserMarkerLayerCountsFit(output.instances, output.stream_vertices, output.frames)) {
    output.error = QObject::tr("SceneEntities exceeds browser limits (%1 instances, %2 expanded vertices, %3 frames)")
                       .arg(
                           QString::number(output.instances), QString::number(output.stream_vertices),
                           QString::number(output.frames));
  } else if (!browserMarkerRetainedBytesFit(output.retained_bytes)) {
    output.error = QObject::tr("SceneEntities would retain more than %1 MiB in the browser")
                       .arg(kBrowserMaxMarkerRetainedBytesPerLayer / (1024ULL * 1024ULL));
  }
  return output;
}

std::uint32_t internFrame(WasmMarkerGeometry& geometry, const std::string& frame) {
  for (std::uint32_t index = 0; index < geometry.frames.size(); ++index) {
    if (geometry.frames[index] == frame) {
      return index;
    }
  }
  geometry.frames.push_back(frame);
  geometry.frame_bounds.emplace_back();
  return static_cast<std::uint32_t>(geometry.frames.size() - 1U);
}

template <typename Primitive>
void appendSolid(
    const Primitive& primitive, std::uint32_t frame_index, std::vector<WasmMarkerInstanceSource>& destination,
    WasmMarkerBounds& bounds, std::uint64_t& skipped_invalid) {
  const auto pose = poseMatrix(primitive.pose);
  const glm::dvec3 size = sizeVector(primitive.size);
  if (!pose.has_value() || !finite(size)) {
    ++skipped_invalid;
    return;
  }
  const glm::dmat4 model = *pose * glm::scale(glm::dmat4(1.0), size);
  if (!finite(model)) {
    ++skipped_invalid;
    return;
  }
  destination.push_back({model, colorVector(primitive.color), frame_index, 1.0F, 1.0F});
  expandUnitBounds(bounds, model, glm::dvec3(-0.5), glm::dvec3(0.5));
}

}  // namespace

WasmSceneEntitiesLayer::WasmSceneEntitiesLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent)
    : PJ::ISceneLayer(parent),
      topic_id_(topic_id),
      display_name_(std::move(display_name)),
      mesh_loader_(std::make_unique<MeshLoader>()) {
  decode_watcher_ = new QFutureWatcher<DecodeResult>(this);
  connect(decode_watcher_, &QFutureWatcher<DecodeResult>::finished, this, &WasmSceneEntitiesLayer::onDecodeFinished);
}

WasmSceneEntitiesLayer::~WasmSceneEntitiesLayer() = default;

PJ::SceneLayerInfo WasmSceneEntitiesLayer::info() const {
  return PJ::SceneLayerInfo{
      .topic_id = topic_id_,
      .object_type = PJ::sdk::BuiltinObjectType::kSceneEntities,
      .display_name = display_name_,
      .family_name = u"Markers"_s,
      .visible = visible_,
  };
}

PJ::Range<PJ::Timepoint> WasmSceneEntitiesLayer::timeRange() const {
  return PJ::liveTopicTimeRange(session_ != nullptr ? &session_->objectStore() : nullptr, topic_id_);
}

bool WasmSceneEntitiesLayer::attach(const PJ::SceneLayerContext& context) {
  if (context.session == nullptr) {
    qCWarning(lcWasmSceneEntities) << "attach: session is unavailable for topic" << topic_id_.id;
    return false;
  }
  session_ = context.session;
  if (!session_->parserBindingForObjectTopic(topic_id_) &&
      !hasCanonical3DCodec(PJ::sdk::BuiltinObjectType::kSceneEntities)) {
    qCWarning(lcWasmSceneEntities) << "attach: no parser or canonical codec for topic" << topic_id_.id;
    session_ = nullptr;
    return false;
  }
  invalidateDecodeState();
  clearGeometry();
  resetModelState();
  disconnect(reload_connection_);
  disconnect(samples_connection_);
  reload_connection_ = connect(
      session_, &PJ::SessionManager::datasetAboutToBeReplaced, this,
      &WasmSceneEntitiesLayer::onDatasetAboutToBeReplaced);
  samples_connection_ = connect(session_, &PJ::SessionManager::samplesIngested, this, [this](const auto&, bool) {
    if (visible_ && requested_time_.has_value()) {
      decode_dirty_ = true;
      emit repaintRequested();
    }
  });
  const PJ::Range<PJ::Timepoint> range = timeRange();
  if (range.max >= range.min) {
    requested_time_ = range.min;
    decode_dirty_ = true;
  }
  return true;
}

void WasmSceneEntitiesLayer::detach() {
  disconnect(reload_connection_);
  disconnect(samples_connection_);
  reload_connection_ = {};
  samples_connection_ = {};
  session_ = nullptr;
  requested_time_.reset();
  decode_dirty_ = false;
  invalidateDecodeState();
  clearGeometry();
  resetModelState();
  setDataWarning({});
  render_warning_.clear();
  updateWarning();
}

void WasmSceneEntitiesLayer::setTrackerTime(PJ::Timepoint time) {
  requested_time_ = time;
  decode_dirty_ = true;
  if (visible_) {
    emit repaintRequested();
  }
}

std::uint64_t WasmSceneEntitiesLayer::renderKey(PJ::Timepoint time) const {
  if (session_ == nullptr) {
    return PJ::kNoSampleRenderKey;
  }
  const auto index = session_->objectStore().indexAt(topic_id_, PJ::toRaw(time));
  if (!index.has_value()) {
    return PJ::kNoSampleRenderKey;
  }
  const auto timestamps = session_->objectStore().entryTimestamps(topic_id_);
  return *index < timestamps.size() ? static_cast<std::uint64_t>(timestamps[*index]) : PJ::kNoSampleRenderKey;
}

void WasmSceneEntitiesLayer::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  if (!visible_) {
    invalidateDecodeState();
    clearGeometry();
    resetModelState();
    render_warning_.clear();
    updateWarning();
  } else {
    decode_dirty_ = requested_time_.has_value();
  }
  emit visibilityChanged(visible_);
  emit repaintRequested();
}

bool WasmSceneEntitiesLayer::prepareForRender() {
  if (!visible_ || !decode_dirty_ || !requested_time_.has_value()) {
    return false;
  }
  decode_dirty_ = false;
  return decodeAt(*requested_time_);
}

bool WasmSceneEntitiesLayer::decodeAt(PJ::Timepoint time) {
  if (session_ == nullptr) {
    return false;
  }
  ensureModelStateAt(time);
  const auto resolved = session_->objectStore().latestAt(topic_id_, PJ::toRaw(time));
  if (!resolved.has_value() || resolved->payload.bytes.empty()) {
    clearGeometry();
    return true;
  }
  const SampleId sample{resolved->timestamp, resolved->payload.bytes.size(), resolved->sequential_uid};
  wanted_sample_ = sample;
  if (sample == active_sample_) {
    return false;
  }
  if (!browserMarkerWirePayloadFits(resolved->payload.bytes.size())) {
    active_sample_ = sample;
    clearGeometry();
    setDataWarning(
        tr("SceneEntities payload is %1 MiB; the browser limit is %2 MiB")
            .arg(QString::number(static_cast<double>(resolved->payload.bytes.size()) / (1024.0 * 1024.0), 'f', 1))
            .arg(kMaxWireBytesPerSample / (1024ULL * 1024ULL)));
    return true;
  }
  std::shared_ptr<const PJ::sdk::SceneEntities> scene;
  if (const auto cached = snapshot_cache_.find(resolved->sequential_uid); cached != snapshot_cache_.end()) {
    scene = cached->second.batch;
  } else {
    auto object = resolveObject(
        session_->parserBindingForObjectTopic(topic_id_), PJ::sdk::BuiltinObjectType::kSceneEntities,
        resolved->timestamp, resolved->payload);
    if (!object.has_value()) {
      active_sample_ = sample;
      clearGeometry();
      setDataWarning(tr("SceneEntities could not be decoded: %1").arg(QString::fromStdString(object.error())));
      return true;
    }
    auto* decoded = object->object.get<PJ::sdk::SceneEntities>();
    if (decoded == nullptr) {
      active_sample_ = sample;
      clearGeometry();
      setDataWarning(tr("Object parser returned the wrong type for SceneEntities"));
      return true;
    }
    scene = std::make_shared<PJ::sdk::SceneEntities>(std::move(*decoded));
    cacheSnapshot(resolved->sequential_uid, scene, resolved->timestamp);
  }
  queueDecode(DecodeRequest{std::move(scene), sample, decode_generation_});
  return false;
}

void WasmSceneEntitiesLayer::queueDecode(DecodeRequest request) {
  wanted_sample_ = request.sample;
  if (inflight_sample_.has_value()) {
    if (*inflight_sample_ == request.sample && inflight_generation_ == request.generation) {
      pending_.reset();
    } else {
      pending_ = std::move(request);
    }
    return;
  }
  startDecode(std::move(request));
}

void WasmSceneEntitiesLayer::startDecode(DecodeRequest request) {
  inflight_sample_ = request.sample;
  inflight_generation_ = request.generation;
  ++decode_starts_;
  QThread* const main_thread = QThread::currentThread();
  decode_watcher_->setFuture(QtConcurrent::run([request = std::move(request), main_thread]() mutable {
    return decode(std::move(request), main_thread);
  }));
}

WasmSceneEntitiesLayer::DecodeResult WasmSceneEntitiesLayer::decode(DecodeRequest request, QThread* main_thread) {
  DecodeResult result{
      .sample = request.sample,
      .generation = request.generation,
      .geometry = {},
      .error = {},
      .ran_off_main_thread = QThread::currentThread() != main_thread,
  };
  try {
    if (request.scene == nullptr) {
      result.error = QObject::tr("SceneEntities decode received no snapshot");
      return result;
    }
    const Preflight counts = preflight(*request.scene);
    if (!counts.error.isEmpty()) {
      result.error = counts.error;
      return result;
    }
    WasmMarkerGeometry& geometry = result.geometry;
    geometry.frames.reserve(static_cast<std::size_t>(counts.frames));
    geometry.frame_bounds.reserve(static_cast<std::size_t>(counts.frames));
    geometry.cubes.reserve(static_cast<std::size_t>(counts.cubes));
    geometry.spheres.reserve(static_cast<std::size_t>(counts.spheres));
    geometry.cylinders.reserve(static_cast<std::size_t>(counts.cylinders));
    geometry.arrows.reserve(static_cast<std::size_t>(counts.arrows));
    geometry.axes.reserve(static_cast<std::size_t>(counts.axes_arms));
    geometry.lines.reserve(static_cast<std::size_t>(counts.line_batches));
    geometry.triangles.reserve(static_cast<std::size_t>(counts.triangle_batches));

    for (const PJ::sdk::SceneEntity& entity : request.scene->entities) {
      const std::uint32_t frame_index = internFrame(geometry, entity.frame_id);
      WasmMarkerBounds& bounds = geometry.frame_bounds[frame_index];

      for (const PJ::sdk::CubePrimitive& cube : entity.cubes) {
        appendSolid(cube, frame_index, geometry.cubes, bounds, geometry.skipped_invalid);
      }
      for (const PJ::sdk::SpherePrimitive& sphere : entity.spheres) {
        appendSolid(sphere, frame_index, geometry.spheres, bounds, geometry.skipped_invalid);
      }
      for (const PJ::sdk::CylinderPrimitive& cylinder : entity.cylinders) {
        const auto pose = poseMatrix(cylinder.pose);
        const glm::dvec3 size = sizeVector(cylinder.size);
        if (!pose.has_value() || !finite(size) || !std::isfinite(cylinder.bottom_scale) ||
            !std::isfinite(cylinder.top_scale)) {
          ++geometry.skipped_invalid;
          continue;
        }
        const glm::dmat4 model = *pose * glm::scale(glm::dmat4(1.0), size);
        const float bottom_scale = static_cast<float>(cylinder.bottom_scale);
        const float top_scale = static_cast<float>(cylinder.top_scale);
        const double radius = 0.5 * std::max(std::abs(cylinder.bottom_scale), std::abs(cylinder.top_scale));
        if (!finite(model) || !std::isfinite(bottom_scale) || !std::isfinite(top_scale) || !std::isfinite(radius)) {
          ++geometry.skipped_invalid;
          continue;
        }
        geometry.cylinders.push_back({model, colorVector(cylinder.color), frame_index, bottom_scale, top_scale});
        expandUnitBounds(bounds, model, {-radius, -radius, -0.5}, {radius, radius, 0.5});
      }
      for (const PJ::sdk::ArrowPrimitive& arrow : entity.arrows) {
        const auto pose = poseMatrix(arrow.pose);
        const double total = std::max(arrow.shaft_length + arrow.head_length, 1e-4);
        const double diameter = std::max({arrow.shaft_diameter, arrow.head_diameter, 1e-4});
        if (!pose.has_value() || !std::isfinite(total) || !std::isfinite(diameter)) {
          ++geometry.skipped_invalid;
          continue;
        }
        const glm::dmat4 model = *pose * glm::scale(glm::dmat4(1.0), glm::dvec3(total, diameter, diameter));
        if (!finite(model)) {
          ++geometry.skipped_invalid;
          continue;
        }
        geometry.arrows.push_back({model, colorVector(arrow.color), frame_index, 1.0F, 1.0F});
        expandUnitBounds(bounds, model, {0.0, -0.5, -0.5}, {1.0, 0.5, 0.5});
      }
      for (const PJ::sdk::AxesPrimitive& axes : entity.axes) {
        const auto pose = poseMatrix(axes.pose);
        const double length = std::max(axes.length, 1e-3);
        const double thickness = std::max(axes.thickness, 1e-3);
        if (!pose.has_value() || !std::isfinite(length) || !std::isfinite(thickness)) {
          ++geometry.skipped_invalid;
          continue;
        }
        const glm::dmat4 scale = glm::scale(glm::dmat4(1.0), glm::dvec3(length, thickness, thickness));
        const std::array<std::pair<glm::dmat4, glm::vec4>, 3> arms{{
            {glm::dmat4(1.0), {1.0F, 0.0F, 0.0F, 1.0F}},
            {glm::rotate(glm::dmat4(1.0), glm::radians(90.0), glm::dvec3(0.0, 0.0, 1.0)), {0.0F, 1.0F, 0.0F, 1.0F}},
            {glm::rotate(glm::dmat4(1.0), glm::radians(-90.0), glm::dvec3(0.0, 1.0, 0.0)), {0.0F, 0.0F, 1.0F, 1.0F}},
        }};
        for (const auto& [rotation, color] : arms) {
          const glm::dmat4 model = *pose * rotation * scale;
          if (!finite(model)) {
            ++geometry.skipped_invalid;
            continue;
          }
          geometry.axes.push_back({model, color, frame_index, 1.0F, 1.0F});
          expandUnitBounds(bounds, model, {0.0, -0.5, -0.5}, {1.0, 0.5, 0.5});
        }
      }

      for (const PJ::sdk::LinePrimitive& line : entity.lines) {
        const auto model = poseMatrix(line.pose);
        if (!model.has_value()) {
          ++geometry.skipped_invalid;
          continue;
        }
        WasmMarkerStreamBatch batch{.model = *model, .vertices = {}, .frame_index = frame_index};
        batch.vertices.reserve(static_cast<std::size_t>(lineExpandedVertexCount(line)));
        const bool per_vertex_color = line.colors.size() == line.points.size();
        const std::size_t count = line.indices.empty() ? line.points.size() : line.indices.size();
        const auto resolve = [&line](std::size_t index) {
          return line.indices.empty() ? index : static_cast<std::size_t>(line.indices[index]);
        };
        const auto emit_segment = [&](std::size_t left, std::size_t right) {
          const std::size_t a = resolve(left);
          const std::size_t b = resolve(right);
          if (a >= line.points.size() || b >= line.points.size()) {
            ++geometry.skipped_invalid;
            return;
          }
          const glm::dvec3 pa = pointVector(line.points[a]);
          const glm::dvec3 pb = pointVector(line.points[b]);
          if (!finite(pa) || !finite(pb)) {
            ++geometry.skipped_invalid;
            return;
          }
          batch.vertices.push_back({pa, colorVector(per_vertex_color ? line.colors[a] : line.color)});
          batch.vertices.push_back({pb, colorVector(per_vertex_color ? line.colors[b] : line.color)});
          expandBounds(bounds, glm::dvec3(*model * glm::dvec4(pa, 1.0)));
          expandBounds(bounds, glm::dvec3(*model * glm::dvec4(pb, 1.0)));
        };
        switch (line.type) {
          case PJ::sdk::LineType::kLineList:
            for (std::size_t index = 0; index + 1U < count; index += 2U) {
              emit_segment(index, index + 1U);
            }
            break;
          case PJ::sdk::LineType::kLineStrip:
            for (std::size_t index = 0; index + 1U < count; ++index) {
              emit_segment(index, index + 1U);
            }
            break;
          case PJ::sdk::LineType::kLineLoop:
            for (std::size_t index = 0; index + 1U < count; ++index) {
              emit_segment(index, index + 1U);
            }
            if (count >= 2U) {
              emit_segment(count - 1U, 0U);
            }
            break;
        }
        if (!batch.vertices.empty()) {
          geometry.line_vertices += batch.vertices.size();
          geometry.lines.push_back(std::move(batch));
        }
      }

      for (const PJ::sdk::TrianglePrimitive& triangles : entity.triangles) {
        const auto model = poseMatrix(triangles.pose);
        if (!model.has_value()) {
          ++geometry.skipped_invalid;
          continue;
        }
        WasmMarkerStreamBatch batch{.model = *model, .vertices = {}, .frame_index = frame_index};
        const bool per_vertex_color = triangles.colors.size() == triangles.points.size();
        const std::size_t count = triangles.indices.empty() ? triangles.points.size() : triangles.indices.size();
        const auto resolve = [&triangles](std::size_t index) {
          return triangles.indices.empty() ? index : static_cast<std::size_t>(triangles.indices[index]);
        };
        batch.vertices.reserve(count - count % 3U);
        for (std::size_t index = 0; index + 2U < count; index += 3U) {
          const std::size_t ia = resolve(index);
          const std::size_t ib = resolve(index + 1U);
          const std::size_t ic = resolve(index + 2U);
          if (ia >= triangles.points.size() || ib >= triangles.points.size() || ic >= triangles.points.size()) {
            ++geometry.skipped_invalid;
            continue;
          }
          const glm::dvec3 a = pointVector(triangles.points[ia]);
          const glm::dvec3 b = pointVector(triangles.points[ib]);
          const glm::dvec3 c = pointVector(triangles.points[ic]);
          const glm::dvec3 cross = glm::cross(b - a, c - a);
          if (!finite(a) || !finite(b) || !finite(c) || !finite(cross) || glm::length(cross) < 1e-15) {
            ++geometry.skipped_invalid;
            continue;
          }
          batch.vertices.push_back({a, colorVector(per_vertex_color ? triangles.colors[ia] : triangles.color)});
          batch.vertices.push_back({b, colorVector(per_vertex_color ? triangles.colors[ib] : triangles.color)});
          batch.vertices.push_back({c, colorVector(per_vertex_color ? triangles.colors[ic] : triangles.color)});
          expandBounds(bounds, glm::dvec3(*model * glm::dvec4(a, 1.0)));
          expandBounds(bounds, glm::dvec3(*model * glm::dvec4(b, 1.0)));
          expandBounds(bounds, glm::dvec3(*model * glm::dvec4(c, 1.0)));
        }
        if (!batch.vertices.empty()) {
          geometry.triangle_vertices += batch.vertices.size();
          geometry.triangles.push_back(std::move(batch));
        }
      }

      geometry.skipped_texts += entity.texts.size();
      geometry.skipped_models += entity.models.size();
    }
  } catch (const std::bad_alloc&) {
    result.geometry = {};
    result.error = QObject::tr("Not enough browser memory to build these SceneEntities markers");
  } catch (const std::exception& error) {
    result.geometry = {};
    result.error = QObject::tr("SceneEntities conversion failed: %1").arg(QString::fromUtf8(error.what()));
  }
  return result;
}

void WasmSceneEntitiesLayer::onDecodeFinished() {
  DecodeResult result = decode_watcher_->result();
  ++decode_completions_;
  if (result.ran_off_main_thread) {
    ++decode_off_main_completions_;
  }
  inflight_sample_.reset();
  if (visible_ && result.generation == decode_generation_ && result.sample == wanted_sample_) {
    applyResult(std::move(result));
  }
  if (pending_.has_value()) {
    DecodeRequest next = std::move(*pending_);
    pending_.reset();
    if (visible_ && next.generation == decode_generation_ && next.sample == wanted_sample_) {
      startDecode(std::move(next));
    }
  }
}

void WasmSceneEntitiesLayer::applyResult(DecodeResult result) {
  active_sample_ = result.sample;
  if (!result.error.isEmpty()) {
    clearGeometry();
    setDataWarning(std::move(result.error));
  } else {
    geometry_ = std::move(result.geometry);
    ++geometry_revision_;
    setDataWarning({});
    updateSourceFrame();
  }
  emit repaintRequested();
}

void WasmSceneEntitiesLayer::invalidateDecodeState() {
  ++decode_generation_;
  pending_.reset();
  wanted_sample_ = {};
  active_sample_ = {};
}

void WasmSceneEntitiesLayer::clearGeometry() {
  if (geometry_.empty() && geometry_.frames.empty()) {
    return;
  }
  geometry_ = {};
  ++geometry_revision_;
  updateSourceFrame();
}

void WasmSceneEntitiesLayer::resetModelState() {
  url_fetcher_.reset();
  active_model_fetch_.reset();
  active_model_import_.reset();
  model_loads_.clear();
  mesh_loader_->clearCache();
  model_state_.clear();
  model_state_built_at_.reset();
  model_applied_uid_high_ = {};
  snapshot_cache_.clear();
  snapshot_cache_bytes_ = 0;
  model_draw_calls_.clear();
  if (!model_meshes_.empty() || model_retained_bytes_ != 0U) {
    model_meshes_.clear();
    model_retained_bytes_ = 0;
    ++model_revision_;
  }
  model_state_warning_.clear();
  model_warning_.clear();
  updateSourceFrame();
  updateWarning();
}

void WasmSceneEntitiesLayer::rebuildModelStateAt(PJ::Timepoint time) {
  model_state_.clear();
  model_state_warning_.clear();
  model_state_built_at_ = time;
  model_applied_uid_high_ = {};
  if (session_ == nullptr) {
    syncModelSources();
    refreshModelDrawCalls();
    return;
  }
  PJ::ObjectStore& store = session_->objectStore();
  const std::int64_t time_ns = PJ::toRaw(time);
  const PJ::SequentialUID high = store.maxUidAtOrBefore(topic_id_, time_ns);
  if (high.valid()) {
    applyModelWindow(std::numeric_limits<std::int64_t>::min(), time_ns);
    model_applied_uid_high_ = high;
    (void)model_state_.dropExpired(time_ns);
  }
  syncModelSources();
  refreshModelDrawCalls();
  updateSourceFrame();
}

void WasmSceneEntitiesLayer::ensureModelStateAt(PJ::Timepoint time) {
  if (session_ == nullptr) {
    if (!model_state_built_at_.has_value() || *model_state_built_at_ != time) {
      rebuildModelStateAt(time);
    }
    return;
  }
  PJ::ObjectStore& store = session_->objectStore();
  const std::int64_t time_ns = PJ::toRaw(time);
  const PJ::SequentialUID high_now = store.maxUidAtOrBefore(topic_id_, time_ns);
  if (model_state_built_at_.has_value() && *model_state_built_at_ == time && high_now == model_applied_uid_high_) {
    return;
  }
  const bool can_incremental =
      model_state_built_at_.has_value() && model_applied_uid_high_.valid() && time >= *model_state_built_at_;
  if (can_incremental) {
    const PJ::SequentialUID high_at_built = store.maxUidAtOrBefore(topic_id_, PJ::toRaw(*model_state_built_at_));
    if (high_at_built == model_applied_uid_high_) {
      const bool applied = applyModelWindow(PJ::toRaw(*model_state_built_at_), time_ns);
      const bool expired = model_state_.dropExpired(time_ns);
      model_state_built_at_ = time;
      model_applied_uid_high_ = high_now;
      if (applied || expired) {
        syncModelSources();
        refreshModelDrawCalls();
        updateSourceFrame();
        emit repaintRequested();
      }
      return;
    }
  }
  rebuildModelStateAt(time);
  emit repaintRequested();
}

bool WasmSceneEntitiesLayer::applyModelWindow(std::int64_t lo_ns, std::int64_t hi_ns) {
  PJ::ObjectStore& store = session_->objectStore();
  pruneSnapshotCacheBelow(store.firstSequentialUID(topic_id_));
  bool applied = false;
  const auto apply_snapshot = [this, &applied](const PJ::sdk::SceneEntities& snapshot, std::int64_t store_ns) {
    const auto projected = model_state_.projectedRetainedBytes(snapshot);
    if (!projected.has_value() || *projected > kBrowserMaxModelStateBytesPerLayer) {
      model_state_warning_ = tr("SceneEntities model state would retain more than %1 MiB in the browser")
                                 .arg(kBrowserMaxModelStateBytesPerLayer / (1024ULL * 1024ULL));
      return;
    }
    applied = model_state_.applySnapshot(snapshot, store_ns) || applied;
  };
  for (const auto& reference : store.rangeByTime(topic_id_, lo_ns, hi_ns)) {
    if (const auto cached = snapshot_cache_.find(reference.uid); cached != snapshot_cache_.end()) {
      apply_snapshot(*cached->second.batch, cached->second.store_ns);
      continue;
    }
    const auto entry = store.at(topic_id_, reference.uid);
    if (entry.has_value() && entry->fetch_failed) {
      // Permanent loss (re-read failed); surface via the model warning, don't retry.
      model_state_warning_ = tr("A SceneEntities model snapshot was lost: source re-read failed");
      continue;
    }
    if (!entry.has_value() || entry->payload.bytes.empty()) {
      continue;
    }
    if (!browserMarkerWirePayloadFits(entry->payload.bytes.size())) {
      model_state_warning_ = tr("A SceneEntities model snapshot exceeds the %1 MiB browser payload limit")
                                 .arg(kMaxWireBytesPerSample / (1024ULL * 1024ULL));
      continue;
    }
    auto object = resolveObject(
        session_->parserBindingForObjectTopic(topic_id_), PJ::sdk::BuiltinObjectType::kSceneEntities, entry->timestamp,
        entry->payload);
    if (!object.has_value()) {
      model_state_warning_ =
          tr("SceneEntities model history could not be decoded: %1").arg(QString::fromStdString(object.error()));
      continue;
    }
    auto* decoded = object->object.get<PJ::sdk::SceneEntities>();
    if (decoded == nullptr) {
      model_state_warning_ = tr("Object parser returned the wrong type for SceneEntities model history");
      continue;
    }
    auto snapshot = std::make_shared<PJ::sdk::SceneEntities>(std::move(*decoded));
    apply_snapshot(*snapshot, entry->timestamp);
    cacheSnapshot(reference.uid, std::move(snapshot), entry->timestamp);
  }
  updateModelWarning();
  return applied;
}

void WasmSceneEntitiesLayer::cacheSnapshot(
    PJ::SequentialUID uid, std::shared_ptr<const PJ::sdk::SceneEntities> batch, std::int64_t store_ns) {
  if (!uid.valid() || batch == nullptr || snapshot_cache_.contains(uid)) {
    return;
  }
  CachedSnapshot cached;
  cached.bytes = estimateSnapshotBytes(*batch);
  if (cached.bytes > kBrowserMaxModelSnapshotCacheBytes) {
    // The current decode/model fold may still use this owned snapshot, but it
    // must not become an over-budget retained cache entry by itself.
    return;
  }
  cached.batch = std::move(batch);
  cached.store_ns = store_ns;
  snapshot_cache_bytes_ = saturatedAdd(snapshot_cache_bytes_, cached.bytes);
  snapshot_cache_.emplace(uid, std::move(cached));
  while (snapshot_cache_bytes_ > kBrowserMaxModelSnapshotCacheBytes && snapshot_cache_.size() > 1U) {
    const auto oldest = snapshot_cache_.begin();
    snapshot_cache_bytes_ -= oldest->second.bytes;
    snapshot_cache_.erase(oldest);
  }
}

void WasmSceneEntitiesLayer::pruneSnapshotCacheBelow(PJ::SequentialUID first_retained_uid) {
  if (!first_retained_uid.valid()) {
    return;
  }
  const auto retained = snapshot_cache_.lower_bound(first_retained_uid);
  for (auto iterator = snapshot_cache_.begin(); iterator != retained; ++iterator) {
    snapshot_cache_bytes_ -= iterator->second.bytes;
  }
  snapshot_cache_.erase(snapshot_cache_.begin(), retained);
}

void WasmSceneEntitiesLayer::syncModelSources() {
  std::unordered_map<std::string, const PJ::sdk::ModelPrimitive*> desired;
  for (const auto& [entity_id, entity] : model_state_.entities()) {
    for (std::size_t index = 0; index < entity.models.size(); ++index) {
      desired.emplace(modelKey(topic_id_, entity_id, index), &entity.models[index]);
    }
  }
  bool meshes_changed = false;
  for (auto iterator = model_loads_.begin(); iterator != model_loads_.end();) {
    if (!desired.contains(iterator->first)) {
      if (const auto mesh = model_meshes_.find(iterator->first); mesh != model_meshes_.end()) {
        const std::uint64_t released = estimateMeshBytes(*mesh->second);
        model_retained_bytes_ = released <= model_retained_bytes_ ? model_retained_bytes_ - released : 0U;
        model_meshes_.erase(mesh);
        meshes_changed = true;
      }
      iterator = model_loads_.erase(iterator);
    } else {
      ++iterator;
    }
  }
  if (active_model_fetch_.has_value()) {
    const auto desired_source = desired.find(active_model_fetch_->key);
    const auto current_load = model_loads_.find(active_model_fetch_->key);
    if (desired_source == desired.end() || current_load == model_loads_.end() ||
        current_load->second.generation != active_model_fetch_->generation ||
        modelSourceSignature(*desired_source->second) != active_model_fetch_->identity) {
      // There is only one layer fetch at a time, so replacing the fetcher is a
      // targeted cancellation of a source that is no longer live.
      url_fetcher_.reset();
      active_model_fetch_.reset();
    }
  }
  for (const auto& [key, primitive] : desired) {
    startModelLoad(key, *primitive);
  }
  if (meshes_changed) {
    ++model_revision_;
  }
  updateModelWarning();
  pumpModelLoads();
}

void WasmSceneEntitiesLayer::refreshModelDrawCalls() {
  model_draw_calls_.clear();
  std::uint64_t invalid = 0;
  for (const auto& [entity_id, entity] : model_state_.entities()) {
    for (std::size_t index = 0; index < entity.models.size(); ++index) {
      const PJ::sdk::ModelPrimitive& primitive = entity.models[index];
      const auto pose = poseMatrix(primitive.pose);
      const glm::dvec3 scale = sizeVector(primitive.scale);
      if (!pose.has_value() || !finite(scale)) {
        ++invalid;
        continue;
      }
      const glm::dmat4 model = *pose * glm::scale(glm::dmat4(1.0), scale);
      if (!finite(model)) {
        ++invalid;
        continue;
      }
      model_draw_calls_.push_back(
          WasmModelDrawCall{
              .mesh_key = modelKey(topic_id_, entity_id, index),
              .frame_id = entity.frame_id,
              .model = model,
              .override_color = colorVector(primitive.color),
              .use_material = !primitive.override_color,
          });
    }
  }
  if (model_draw_calls_.size() > kBrowserMaxModelDrawsPerLayer) {
    model_draw_calls_.clear();
    model_state_warning_ =
        tr("SceneEntities has more than %1 live browser model draws").arg(kBrowserMaxModelDrawsPerLayer);
  } else if (invalid != 0U) {
    model_state_warning_ = tr("Skipped %n model(s) with an invalid pose or scale", nullptr, static_cast<int>(invalid));
  } else if (
      model_state_warning_.startsWith(u"Skipped "_s) || model_state_warning_.startsWith(u"SceneEntities has more"_s)) {
    model_state_warning_.clear();
  }
  updateModelWarning();
}

void WasmSceneEntitiesLayer::startModelLoad(const std::string& key, const PJ::sdk::ModelPrimitive& primitive) {
  const std::string source_signature = modelSourceSignature(primitive);
  const std::string identity = source_signature.empty() ? "invalid" : source_signature;
  if (const auto existing = model_loads_.find(key);
      existing != model_loads_.end() && existing->second.identity == identity) {
    return;
  }
  if (const auto old_mesh = model_meshes_.find(key); old_mesh != model_meshes_.end()) {
    const std::uint64_t released = estimateMeshBytes(*old_mesh->second);
    model_retained_bytes_ = released <= model_retained_bytes_ ? model_retained_bytes_ - released : 0U;
    model_meshes_.erase(old_mesh);
    ++model_revision_;
  }
  ModelLoad& load = model_loads_[key];
  load = {};
  load.identity = identity;
  load.generation = next_model_load_generation_++;
  if (next_model_load_generation_ == 0U) {
    // Reserve zero as the never-issued value. A wrap is not reachable in
    // practice, but keeping the invariant costs nothing.
    next_model_load_generation_ = 1U;
  }
  if (!primitive.data.empty()) {
    load.source_label = tr("(embedded model)");
  } else {
    load.source_label = QString::fromStdString(primitive.url);
  }
  if (source_signature.empty()) {
    load.error = tr("model has neither embedded data nor a URL");
    load.phase = ModelLoad::Phase::kFailed;
  }
}

void WasmSceneEntitiesLayer::pumpModelLoads() {
  if (active_model_fetch_.has_value() || active_model_import_ != nullptr) {
    return;
  }
  for (const auto& [entity_id, entity] : model_state_.entities()) {
    for (std::size_t index = 0; index < entity.models.size(); ++index) {
      const PJ::sdk::ModelPrimitive& primitive = entity.models[index];
      const std::string key = modelKey(topic_id_, entity_id, index);
      auto current = model_loads_.find(key);
      if (current == model_loads_.end() || current->second.phase != ModelLoad::Phase::kWaiting) {
        continue;
      }
      const std::string identity = current->second.identity;
      const std::uint64_t generation = current->second.generation;
      const QString hint = modelFormatHint(primitive);
      if (!primitive.data.empty()) {
        if (!browserModelSourceFits(primitive.data.size())) {
          current->second.error = tr("model source exceeds the %1 MiB browser limit")
                                      .arg(kBrowserMaxModelSourceBytes / (1024ULL * 1024ULL));
          current->second.phase = ModelLoad::Phase::kFailed;
          continue;
        }
        try {
          const QByteArray bytes(
              reinterpret_cast<const char*>(primitive.data.data()), static_cast<qsizetype>(primitive.data.size()));
          if (startModelImport(key, identity, generation, bytes, hint)) {
            updateModelWarning();
            return;
          }
        } catch (const std::exception& error) {
          current->second.error = tr("could not queue model import: %1").arg(QString::fromUtf8(error.what()));
          current->second.phase = ModelLoad::Phase::kFailed;
        } catch (...) {
          current->second.error = tr("could not queue model import");
          current->second.phase = ModelLoad::Phase::kFailed;
        }
        continue;
      }

      const QUrl url(QString::fromStdString(primitive.url));
      const bool remote = url.scheme() == "http"_L1 || url.scheme() == "https"_L1;
      if (remote && !remoteModelFetchAllowed()) {
        current->second.phase = ModelLoad::Phase::kBlocked;
        current->second.error = tr("remote model fetch is disabled");
        continue;
      }
      if (url_fetcher_ == nullptr) {
        url_fetcher_ = std::make_unique<UrlFetcher>();
      }
      current->second.phase = ModelLoad::Phase::kFetching;
      active_model_fetch_ = ActiveModelFetch{key, identity, generation};
      url_fetcher_->fetch(url, [this, key, identity, generation, hint](FetchResult result) {
        if (!active_model_fetch_.has_value() || active_model_fetch_->key != key ||
            active_model_fetch_->identity != identity || active_model_fetch_->generation != generation) {
          return;
        }
        active_model_fetch_.reset();
        const auto active = model_loads_.find(key);
        if (active == model_loads_.end() || active->second.identity != identity ||
            active->second.generation != generation) {
          pumpModelLoads();
          return;
        }
        if (!result.ok) {
          active->second.phase = ModelLoad::Phase::kFailed;
          active->second.error = std::move(result.error);
          updateModelWarning();
          emit repaintRequested();
          pumpModelLoads();
          return;
        }
        if (!startModelImport(key, identity, generation, result.bytes, hint)) {
          pumpModelLoads();
        }
      });
      updateModelWarning();
      return;
    }
  }
  updateModelWarning();
}

bool WasmSceneEntitiesLayer::startModelImport(
    const std::string& key, const std::string& identity, std::uint64_t generation, const QByteArray& bytes,
    const QString& format_hint) {
  auto current = model_loads_.find(key);
  if (current == model_loads_.end() || current->second.identity != identity ||
      current->second.generation != generation || active_model_import_ != nullptr) {
    return false;
  }
  try {
    auto active = std::make_unique<ActiveModelImport>();
    active->key = key;
    active->identity = identity;
    active->generation = generation;
    active->future = mesh_loader_->loadFromMemory(bytes, format_hint);
    active->watcher = std::make_unique<QFutureWatcher<MeshData>>();
    connect(active->watcher.get(), &QFutureWatcher<MeshData>::finished, this, [this, key, identity, generation]() {
      finishModelImport(key, identity, generation);
    });
    QFutureWatcher<MeshData>* watcher = active->watcher.get();
    current->second.phase = ModelLoad::Phase::kImporting;
    active_model_import_ = std::move(active);
    watcher->setFuture(active_model_import_->future);
    return true;
  } catch (const std::exception& error) {
    current->second.error = tr("could not start model import: %1").arg(QString::fromUtf8(error.what()));
  } catch (...) {
    current->second.error = tr("could not start model import");
  }
  active_model_import_.reset();
  current->second.phase = ModelLoad::Phase::kFailed;
  updateModelWarning();
  emit repaintRequested();
  return false;
}

void WasmSceneEntitiesLayer::finishModelImport(
    const std::string& key, const std::string& identity, std::uint64_t generation) {
  if (active_model_import_ == nullptr || active_model_import_->key != key ||
      active_model_import_->identity != identity || active_model_import_->generation != generation ||
      !active_model_import_->future.isFinished()) {
    return;
  }
  MeshData data;
  std::unique_ptr<ActiveModelImport> completed_job = std::move(active_model_import_);
  QFuture<MeshData> completed = std::move(completed_job->future);
  if (QFutureWatcher<MeshData>* watcher = completed_job->watcher.release(); watcher != nullptr) {
    watcher->deleteLater();
  }
  try {
    data = completed.takeResult();
  } catch (const std::exception& error) {
    data.error = tr("mesh load threw: %1").arg(QString::fromUtf8(error.what()));
  } catch (...) {
    data.error = tr("mesh load threw an unknown exception");
  }
  auto current = model_loads_.find(key);
  if (current == model_loads_.end() || current->second.identity != identity ||
      current->second.generation != generation) {
    pumpModelLoads();
    return;
  }
  if (!data.ok) {
    current->second.phase = ModelLoad::Phase::kFailed;
    current->second.error = std::move(data.error);
    updateModelWarning();
    emit repaintRequested();
    pumpModelLoads();
    return;
  }
  const std::uint64_t retained = estimateMeshBytes(data);
  const auto old = model_meshes_.find(key);
  const std::uint64_t old_bytes = old != model_meshes_.end() ? estimateMeshBytes(*old->second) : 0U;
  const std::uint64_t retained_without_old =
      old_bytes <= model_retained_bytes_ ? model_retained_bytes_ - old_bytes : kSaturated;
  if (retained == kSaturated || retained > kBrowserMaxModelRetainedBytesPerMesh || retained_without_old == kSaturated ||
      retained_without_old > kBrowserMaxModelRetainedBytesPerLayer ||
      retained > kBrowserMaxModelRetainedBytesPerLayer ||
      retained > kBrowserMaxModelRetainedBytesPerLayer - retained_without_old) {
    current->second.error = tr("model would exceed the %1 MiB browser layer memory limit")
                                .arg(kBrowserMaxModelRetainedBytesPerLayer / (1024ULL * 1024ULL));
    current->second.phase = ModelLoad::Phase::kFailed;
    updateModelWarning();
    emit repaintRequested();
    pumpModelLoads();
    return;
  }
  try {
    model_meshes_[key] = std::make_shared<MeshData>(std::move(data));
  } catch (const std::exception& error) {
    current->second.error = tr("could not retain model: %1").arg(QString::fromUtf8(error.what()));
    current->second.phase = ModelLoad::Phase::kFailed;
    updateModelWarning();
    emit repaintRequested();
    pumpModelLoads();
    return;
  } catch (...) {
    current->second.error = tr("could not retain model");
    current->second.phase = ModelLoad::Phase::kFailed;
    updateModelWarning();
    emit repaintRequested();
    pumpModelLoads();
    return;
  }
  model_retained_bytes_ = model_retained_bytes_ - old_bytes + retained;
  current->second.error.clear();
  current->second.phase = ModelLoad::Phase::kReady;
  ++model_revision_;
  updateModelWarning();
  emit repaintRequested();
  pumpModelLoads();
}

void WasmSceneEntitiesLayer::updateModelWarning() {
  QStringList lines;
  if (!model_state_warning_.isEmpty()) {
    lines.push_back(model_state_warning_);
  }
  int blocked = 0;
  for (const auto& [_, load] : model_loads_) {
    if (load.phase == ModelLoad::Phase::kBlocked) {
      ++blocked;
    } else if (!load.error.isEmpty() && lines.size() < 8) {
      lines.push_back(tr("Failed to load %1: %2").arg(load.source_label, load.error));
    }
  }
  if (blocked != 0) {
    lines.push_front(tr("Remote model fetch is disabled — %n URL(s) blocked", nullptr, blocked));
  }
  const QString next = lines.join(QLatin1Char('\n'));
  if (next == model_warning_) {
    return;
  }
  model_warning_ = next;
  updateWarning();
}

void WasmSceneEntitiesLayer::updateSourceFrame() {
  std::string next;
  const auto iterator = std::find_if(
      geometry_.frames.cbegin(), geometry_.frames.cend(), [](const std::string& frame) { return !frame.empty(); });
  if (iterator != geometry_.frames.cend()) {
    next = *iterator;
  }
  if (next.empty()) {
    const auto model = std::find_if(
        model_state_.entities().cbegin(), model_state_.entities().cend(),
        [](const auto& item) { return !item.second.frame_id.empty(); });
    if (model != model_state_.entities().cend()) {
      next = model->second.frame_id;
    }
  }
  if (source_frame_ == next) {
    return;
  }
  source_frame_ = std::move(next);
  emit sourceFrameChanged(QString::fromStdString(source_frame_));
}

void WasmSceneEntitiesLayer::setDataWarning(QString warning) {
  data_warning_ = std::move(warning);
  updateWarning();
}

void WasmSceneEntitiesLayer::noteRenderFailure(QString warning) {
  render_warning_ = std::move(warning);
  updateWarning();
}

void WasmSceneEntitiesLayer::noteRenderSuccess() {
  if (render_warning_.isEmpty()) {
    return;
  }
  render_warning_.clear();
  updateWarning();
}

void WasmSceneEntitiesLayer::updateWarning() {
  QStringList warnings;
  if (!data_warning_.isEmpty()) {
    warnings.push_back(data_warning_);
  }
  if (!model_warning_.isEmpty()) {
    warnings.push_back(model_warning_);
  }
  if (!render_warning_.isEmpty()) {
    warnings.push_back(render_warning_);
  }
  const QString next = warnings.join(QLatin1Char('\n'));
  if (warning_reason_ == next) {
    return;
  }
  warning_reason_ = next;
  emit warningChanged(!warning_reason_.isEmpty(), warning_reason_);
}

void WasmSceneEntitiesLayer::setOpacity(float opacity) {
  const float next = std::clamp(opacity, 0.0F, 1.0F);
  if (opacity_ == next) {
    return;
  }
  opacity_ = next;
  emit configurationChanged();
  emit repaintRequested();
}

void WasmSceneEntitiesLayer::setColorOverrideEnabled(bool enabled) {
  if (color_override_enabled_ == enabled) {
    return;
  }
  color_override_enabled_ = enabled;
  emit configurationChanged();
  emit repaintRequested();
}

void WasmSceneEntitiesLayer::setOverrideColor(QColor color) {
  if (!color.isValid() || override_color_ == color) {
    return;
  }
  override_color_ = color;
  emit configurationChanged();
  emit repaintRequested();
}

void WasmSceneEntitiesLayer::setWireframe(bool enabled) {
  if (wireframe_ == enabled) {
    return;
  }
  wireframe_ = enabled;
  emit configurationChanged();
  emit repaintRequested();
}

std::optional<WasmSceneEntitiesLayer::ParsedSettings> WasmSceneEntitiesLayer::parseSettings(
    const QDomElement& element) {
  if (element.isNull() || element.tagName() != "markers"_L1 || !detail::isLeafPayload(element)) {
    return std::nullopt;
  }
  ParsedSettings settings;
  if (!detail::parseFiniteFloat(element, "opacity", 1.0F, 0.0F, 1.0F, settings.opacity) ||
      !detail::parseTrueFalse(element, "color_override", false, settings.color_override) ||
      !detail::parseTrueFalse(element, "wireframe", false, settings.wireframe)) {
    return std::nullopt;
  }
  if (element.hasAttribute(u"override_color"_s)) {
    settings.override_color = QColor(element.attribute(u"override_color"_s));
    if (!settings.override_color.isValid()) {
      return std::nullopt;
    }
  }
  return settings;
}

bool WasmSceneEntitiesLayer::validateXml(const QDomElement& element) {
  return parseSettings(element).has_value();
}

QDomElement WasmSceneEntitiesLayer::xmlSaveState(QDomDocument& document) const {
  QDomElement element = document.createElement(u"markers"_s);
  element.setAttribute(u"opacity"_s, QString::number(static_cast<double>(opacity_), 'g', 6));
  element.setAttribute(u"color_override"_s, color_override_enabled_ ? u"true"_s : u"false"_s);
  element.setAttribute(u"override_color"_s, override_color_.name(QColor::HexRgb));
  element.setAttribute(u"wireframe"_s, wireframe_ ? u"true"_s : u"false"_s);
  return element;
}

bool WasmSceneEntitiesLayer::xmlLoadState(const QDomElement& element) {
  const auto settings = parseSettings(element);
  if (!settings.has_value()) {
    return false;
  }
  setOpacity(settings->opacity);
  setOverrideColor(settings->override_color);
  setColorOverrideEnabled(settings->color_override);
  setWireframe(settings->wireframe);
  return true;
}

QWidget* WasmSceneEntitiesLayer::createConfigWidget(QWidget* parent) {
  auto* container = new QWidget(parent);
  auto* outer = new QVBoxLayout(container);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(PJ::theme::space(PJ::theme::Space::Snug));
  auto* form = new QFormLayout();
  form->setContentsMargins(0, 0, 0, 0);
  form->setHorizontalSpacing(PJ::theme::space(PJ::theme::Space::Comfortable));
  form->setVerticalSpacing(PJ::theme::space(PJ::theme::Space::Snug));
  outer->addLayout(form);

  auto* opacity = new PJ::DoubleScrubber(container);
  opacity->setObjectName(u"wasmMarkerOpacity"_s);
  opacity->setRange(0.0, 1.0);
  opacity->setDecimals(2);
  opacity->setSingleStep(0.05);
  opacity->setValue(opacity_);
  form->addRow(tr("Opacity:"), opacity);

  auto* override_row = new QWidget(container);
  auto* override_layout = new QHBoxLayout(override_row);
  override_layout->setContentsMargins(0, 0, 0, 0);
  override_layout->setSpacing(PJ::theme::space(PJ::theme::Space::Comfortable));
  auto* override_toggle = new PJ::ToggleSwitch(override_row);
  override_toggle->setObjectName(u"wasmMarkerColorOverride"_s);
  override_toggle->setChecked(color_override_enabled_, false);
  auto* swatch = new PJ::ColorPickerWidget(override_row);
  swatch->setObjectName(u"wasmMarkerOverrideColor"_s);
  swatch->setColor(override_color_);
  override_layout->addWidget(override_toggle);
  override_layout->addWidget(swatch);
  override_layout->addStretch();
  form->addRow(tr("Override color:"), override_row);

  auto* wireframe = new PJ::ToggleSwitch(container);
  wireframe->setObjectName(u"wasmMarkerWireframe"_s);
  wireframe->setChecked(wireframe_, false);
  form->addRow(tr("Wireframe:"), wireframe);

  connect(opacity, &PJ::DoubleScrubber::valueChanged, this, [this](double value) {
    setOpacity(static_cast<float>(value));
  });
  connect(override_toggle, &PJ::ToggleSwitch::toggled, this, &WasmSceneEntitiesLayer::setColorOverrideEnabled);
  connect(swatch, &PJ::ColorPickerWidget::colorChanged, this, [this, override_toggle](QColor color) {
    setOverrideColor(color);
    if (!override_toggle->isChecked()) {
      override_toggle->setChecked(true);
    }
    setColorOverrideEnabled(true);
  });
  connect(wireframe, &PJ::ToggleSwitch::toggled, this, &WasmSceneEntitiesLayer::setWireframe);
  return container;
}

void WasmSceneEntitiesLayer::onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id) {
  if (session_ == nullptr || session_->objectStore().descriptor(topic_id_).dataset_id != dataset_id) {
    return;
  }
  requested_time_.reset();
  decode_dirty_ = false;
  invalidateDecodeState();
  clearGeometry();
  resetModelState();
  setDataWarning({});
  render_warning_.clear();
  updateWarning();
}

}  // namespace pj::scene3d
