// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/layers/scene_entities_layer.h"

#include <QByteArray>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLoggingCategory>
#include <QSettings>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "layer_xml_validation.h"
#include "mesh_load_set.h"
#include "mesh_loader.h"
#include "pj_base/builtin/scene_entities.hpp"
#include "pj_base/time.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/scene_entities_decode.h"
#include "pj_scene3d_core/scene_entities_model_state.h"
#include "pj_scene3d_widgets/parse_locked.h"
#include "pj_widgets/ColorPickerWidget.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/ToggleSwitch.h"
#include "url_fetcher.h"
using namespace Qt::StringLiterals;

namespace pj::scene3d {

namespace {
Q_LOGGING_CATEGORY(lcSceneEntitiesLayer, "pj.scene3d.entity.markers")

// The source frame of a batch is the message-level frame: the frame_id of its
// first entity (per-entity frame overrides are a v2 refinement).
std::string batchSourceFrame(const PJ::sdk::SceneEntities& batch) {
  return batch.entities.empty() ? std::string{} : batch.entities.front().frame_id;
}

std::string meshKey(PJ::ObjectTopicId topic_id, const std::string& entity_id, std::size_t model_index) {
  return std::to_string(topic_id.id) + ":" + entity_id + ":" + std::to_string(model_index);
}

std::uint64_t fnv1a(const std::vector<std::uint8_t>& data) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (std::uint8_t byte : data) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

QString hintFromMediaType(const std::string& media_type, const std::string& url) {
  const QString media = QString::fromStdString(media_type).toLower();
  if (media == QLatin1String("model/gltf-binary") || media == QLatin1String("application/octet-stream+glb")) {
    return u"glb"_s;
  }
  if (media == QLatin1String("model/gltf+json") || media == QLatin1String("model/gltf")) {
    return u"gltf"_s;
  }
  if (media == QLatin1String("model/vnd.collada+xml")) {
    return u"dae"_s;
  }
  if (media == QLatin1String("model/stl")) {
    return u"stl"_s;
  }
  if (media == QLatin1String("model/obj")) {
    return u"obj"_s;
  }
  if (!media.isEmpty()) {
    const qsizetype slash = media.lastIndexOf('/');
    const QString suffix = slash >= 0 ? media.mid(slash + 1) : media;
    if (!suffix.isEmpty() && !suffix.contains('+')) {
      return suffix;
    }
  }
  const QUrl parsed(QString::fromStdString(url));
  const QString path = parsed.isValid() && !parsed.path().isEmpty() ? parsed.path() : QString::fromStdString(url);
  return QFileInfo(path).suffix();
}

// Identity of a model's source bytes: content hash for embedded data, URL+type
// for remote sources. A changed signature for the same mesh key forces a reload.
std::string sourceSignature(const PJ::sdk::ModelPrimitive& primitive) {
  if (!primitive.data.empty()) {
    return "data:" + primitive.media_type + ":" + std::to_string(primitive.data.size()) + ":" +
           std::to_string(fnv1a(primitive.data));
  }
  if (!primitive.url.empty()) {
    return "url:" + primitive.url + ":" + primitive.media_type;
  }
  return {};
}

// Policy gate for DATA-SUPPLIED model URLs. Remote model fetch defaults ON
// (datasets routinely reference their mesh by URL, and a fetch is bounded by
// UrlFetcher's redirect/size/timeout limits and cached on disk); a user who
// wants no dataset-driven network egress opts OUT via this QSettings key. Read
// per newly-seen model source — a URL blocked under the old value stays recorded
// (no per-tick re-check) until the layer re-attaches.
bool remoteModelFetchAllowed() {
  return QSettings().value(u"pj_scene3d/allow_remote_model_fetch"_s, true).toBool();
}

// Rough heap footprint of a decoded batch — the buffers that dominate (embedded
// model bytes, line/triangle geometry), not exact allocator accounting.
std::size_t estimateSnapshotBytes(const PJ::sdk::SceneEntities& batch) {
  std::size_t bytes = sizeof(batch);
  for (const auto& entity : batch.entities) {
    bytes += sizeof(entity);
    for (const auto& model : entity.models) {
      bytes += model.data.size() + model.url.size() + model.media_type.size();
    }
    for (const auto& line : entity.lines) {
      bytes += line.points.size() * sizeof(line.points[0]) + line.colors.size() * sizeof(line.colors[0]) +
               line.indices.size() * sizeof(uint32_t);
    }
    for (const auto& triangle : entity.triangles) {
      bytes += triangle.points.size() * sizeof(triangle.points[0]) +
               triangle.colors.size() * sizeof(triangle.colors[0]) + triangle.indices.size() * sizeof(uint32_t);
    }
    for (const auto& text : entity.texts) {
      bytes += sizeof(text) + text.text.size();
    }
    bytes += entity.arrows.size() * sizeof(PJ::sdk::ArrowPrimitive) +
             entity.cubes.size() * sizeof(PJ::sdk::CubePrimitive) +
             entity.spheres.size() * sizeof(PJ::sdk::SpherePrimitive) +
             entity.cylinders.size() * sizeof(PJ::sdk::CylinderPrimitive) +
             entity.axes.size() * sizeof(PJ::sdk::AxesPrimitive);
  }
  bytes += batch.deletions.size() * sizeof(PJ::sdk::SceneEntityDeletion);
  return bytes;
}

// Decoded-batch cache budget. Past it, the lowest-UID batches are evicted and a
// backward rebuild degrades to re-parsing them — bounded memory over speed
// (file sessions never evict from the store, so the cache cannot rely on
// retention pruning alone).
constexpr std::size_t kSnapshotCacheMaxBytes = 256u * 1024u * 1024u;
}  // namespace

// One async mesh load per mesh key. The entry's `identity` is the source-byte
// signature so a re-published model with new content replaces it; URL-sourced
// entries are inserted BEFORE their bytes exist — `future` stays
// default-constructed (invalid) while the fetch is in flight, or forever when
// the URL was blocked (blocked_by_policy) or its fetch failed (fetch_failed) —
// so per-tick re-entry dedupes on (key, signature) and drain() skips entries
// without a valid future. See MeshLoadEntry in mesh_load_set.h.

SceneEntitiesLayer::SceneEntitiesLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent)
    : Scene3DLayer(parent),
      topic_id_(topic_id),
      display_name_(std::move(display_name)),
      mesh_loader_(std::make_unique<MeshLoader>()),
      mesh_pass_(std::make_unique<MeshRenderPass>()),
      mesh_loads_(std::make_unique<MeshLoadSet>()) {}

SceneEntitiesLayer::~SceneEntitiesLayer() = default;

PJ::SceneLayerInfo SceneEntitiesLayer::info() const {
  return PJ::SceneLayerInfo{
      .topic_id = topic_id_,
      .object_type = PJ::sdk::BuiltinObjectType::kSceneEntities,
      .display_name = display_name_,
      .family_name = u"Markers"_s,
      .visible = visible_,
  };
}

PJ::Range<PJ::Timepoint> SceneEntitiesLayer::timeRange() const {
  return PJ::liveTopicTimeRange(ctx_.session != nullptr ? &ctx_.session->objectStore() : nullptr, topic_id_);
}

QStringList SceneEntitiesLayer::fallbackFrames() const {
  // The marker batch's source frame plus every distinct model-entity frame.
  QStringList out;
  if (!source_frame_.empty()) {
    out.append(QString::fromStdString(source_frame_));
  }
  for (const QString& frame : model_frames_) {
    if (!out.contains(frame)) {
      out.append(frame);
    }
  }
  return out;
}

QString SceneEntitiesLayer::sourceFrame() const {
  return QString::fromStdString(source_frame_);
}

QDomElement SceneEntitiesLayer::xmlSaveState(QDomDocument& doc) const {
  QDomElement el = doc.createElement(u"markers"_s);
  el.setAttribute(u"opacity"_s, QString::number(static_cast<double>(overrides_.opacity), 'g', 6));
  el.setAttribute(u"color_override"_s, overrides_.color_override ? u"true"_s : u"false"_s);
  el.setAttribute(u"override_color"_s, overrideColor().name(QColor::HexRgb));
  el.setAttribute(u"wireframe"_s, overrides_.wireframe ? u"true"_s : u"false"_s);
  return el;
}

bool SceneEntitiesLayer::xmlLoadState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "markers"_L1 || !detail::isLeafPayload(element)) {
    return false;
  }
  float restored_opacity = 0.0f;
  bool restored_color_override = false;
  bool restored_wireframe = false;
  if (!detail::parseFiniteFloat(element, "opacity", 1.0f, 0.0f, 1.0f, restored_opacity) ||
      !detail::parseTrueFalse(element, "color_override", false, restored_color_override) ||
      !detail::parseTrueFalse(element, "wireframe", false, restored_wireframe)) {
    return false;
  }
  QColor restored_color;
  const bool has_override_color = element.hasAttribute(u"override_color"_s);
  if (has_override_color) {
    restored_color = QColor(element.attribute(u"override_color"_s));
    if (!restored_color.isValid()) {
      return false;
    }
  }
  setOpacity(restored_opacity);
  if (has_override_color) {
    setOverrideColor(restored_color);
  }
  setColorOverrideEnabled(restored_color_override);
  setWireframe(restored_wireframe);
  return true;
}

bool SceneEntitiesLayer::attach(const PJ::SceneLayerContext& ctx) {
  const auto& scene3d_ctx = static_cast<const Scene3DLayerContext&>(ctx);
  if (scene3d_ctx.session == nullptr) {
    qCWarning(lcSceneEntitiesLayer) << "attach: session is null";
    return false;
  }
  ctx_ = scene3d_ctx;
  if (!ctx_.session->parserBindingForObjectTopic(topic_id_)) {
    qCWarning(lcSceneEntitiesLayer) << "attach: no parser for topic_id=" << topic_id_.id;
    return false;
  }
  // A dataset reload (SessionManager::replaceDataset) re-attaches without a
  // detach(): start from the pristine baseline so prior-generation state never
  // skips or anchors the new generation's replay (or leaks out of accessors).
  resetReplayState();
  PJ::ObjectStore& store = ctx_.session->objectStore();
  if (store.entryCount(topic_id_) > 0) {
    ts_first_ = store.timeRange(topic_id_).first;
  }
  if (!bootstrap()) {
    qCWarning(lcSceneEntitiesLayer) << "attach: bootstrap failed for topic_id=" << topic_id_.id;
    // Continue anyway — render will silently skip until a sample arrives.
  }
  // Seed only when a first sample actually exists; gate on presence, not on a
  // 0 sentinel (t=0 is a valid first timestamp under ROS sim time).
  if (ts_first_.has_value()) {
    renderAt(*ts_first_);
    rebuildModelStateAt(PJ::fromRaw(*ts_first_));
  }
  return true;
}

void SceneEntitiesLayer::detach() {
  ctx_ = {};
  resetReplayState();
}

void SceneEntitiesLayer::resetReplayState() {
  // Drop the active marker batch too: on a detach-less re-attach with the topic
  // still empty, renderAt() never runs, so a batch left here would keep drawing
  // prior-generation markers. setActive only swaps a shared_ptr — no GL.
  pass_.setActive(nullptr);
  source_frame_.clear();
  last_marker_uid_ = {};
  ts_first_.reset();
  entities_.clear();
  entity_expiry_anchor_ns_.clear();
  model_frames_.clear();
  state_built_at_.reset();
  applied_uid_high_ = {};
  snapshot_cache_.clear();
  snapshot_cache_bytes_ = 0;
  // Aborts in-flight model-URL fetches and drops their callbacks; the records
  // they would have targeted die with mesh_loads_ right below.
  url_fetcher_.reset();
  mesh_loads_->clear();
  updateRemoteFetchNotice();  // no records left -> notice clears
  mesh_pass_->clearMeshes();
}

void SceneEntitiesLayer::setTrackerTime(PJ::Timepoint time) {
  if (visible_) {
    renderAt(PJ::toRaw(time));
    ensureModelStateAt(time);
  }
}

void SceneEntitiesLayer::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  pass_.setVisible(visible);
  emit visibilityChanged(visible);
  // Catch-up on un-hide is the dock's job: SceneDockWidget::setLayerVisible
  // re-delivers the last tracker time (hidden layers receive no ticks), which
  // setTrackerTime decodes at the playhead — so no refreshNow() here.
  emit repaintRequested();
}

void SceneEntitiesLayer::initializeGL() {
  pass_.initializeGL();
  mesh_pass_->initializeGL();
}

void SceneEntitiesLayer::render(const ViewParams& view_params, const FrameContext& frame_ctx) {
  pass_.render(view_params, frame_ctx);
  // MarkerRenderPass tracks visibility internally (setVisible); the model path
  // guards here. Mesh loads resolve on the thread pool, so drain them per frame.
  if (!visible_) {
    return;
  }
  ensureModelStateAt(frame_ctx.time);
  pollMeshLoads();
  mesh_pass_->renderVisuals(view_params, modelDrawCallsForFrame(frame_ctx), overrides_.opacity);
}

void SceneEntitiesLayer::releaseGL() {
  pass_.releaseGL();
  mesh_pass_->releaseGL();
}

std::optional<AABB> SceneEntitiesLayer::meshShadowBounds(const FrameContext& frame_ctx) {
  if (!visible_) {
    return std::nullopt;
  }
  ensureModelStateAt(frame_ctx.time);
  pollMeshLoads();
  const AABB bounds = mesh_pass_->worldBoundsOfDraws(modelDrawCallsForFrame(frame_ctx));
  return bounds.valid ? std::optional<AABB>(bounds) : std::nullopt;
}

void SceneEntitiesLayer::renderShadowCasters(const glm::mat4& light_view_proj, const FrameContext& frame_ctx) {
  if (!visible_) {
    return;
  }
  ensureModelStateAt(frame_ctx.time);
  mesh_pass_->renderDepthOnly(light_view_proj, modelDrawCallsForFrame(frame_ctx));
}

bool SceneEntitiesLayer::bootstrap() {
  PJ::ObjectStore& store = ctx_.session->objectStore();
  auto first = store.at(topic_id_, 0);
  if (!first.has_value() || first->payload.bytes.empty()) {
    return false;
  }
  const auto binding = ctx_.session->parserBindingForObjectTopic(topic_id_);
  if (!binding) {
    return false;
  }
  auto obj = parseLocked(binding, first->timestamp, first->payload);
  if (!obj.has_value()) {
    qCWarning(lcSceneEntitiesLayer) << "bootstrap parseObject failed:" << QString::fromStdString(obj.error());
    return false;
  }
  const auto* batch = obj->object.get<PJ::sdk::SceneEntities>();
  if (batch == nullptr) {
    return false;
  }
  const std::string frame = batchSourceFrame(*batch);
  if (frame != source_frame_) {
    source_frame_ = frame;
    emit sourceFrameChanged(QString::fromStdString(source_frame_));
  }
  if (!source_frame_.empty()) {
    emit fallbackFramesChanged(fallbackFrames());
  }
  return true;
}

void SceneEntitiesLayer::renderAt(int64_t time_ns) {
  if (ctx_.session == nullptr) {
    return;
  }
  PJ::ObjectStore& store = ctx_.session->objectStore();
  // Skip re-parsing + re-decoding when the active batch hasn't changed (scrubbing
  // within one message's time window). SequentialUID is stable across ObjectStore
  // front eviction, unlike a current deque index; checked metadata-first
  // (latestEntryIdAt) so an unchanged batch never resolves payload bytes — a
  // resolve can be a cold refetch once a pool seed is evicted. Viewer color
  // overrides apply at render time (setOverrides), not here, so they are
  // unaffected by this guard. Shared by the gate and the post-resolve re-check
  // (a racing insert can change the pick in between).
  const auto already_active = [this](PJ::SequentialUID uid) { return uid == last_marker_uid_; };
  const auto entry_id = store.latestEntryIdAt(topic_id_, time_ns);
  if (!entry_id.has_value() || already_active(entry_id->uid)) {
    return;
  }
  auto resolved = store.latestAt(topic_id_, time_ns);
  if (!resolved.has_value() || resolved->payload.bytes.empty()) {
    return;
  }
  if (already_active(resolved->sequential_uid)) {
    return;
  }
  const auto binding = ctx_.session->parserBindingForObjectTopic(topic_id_);
  if (!binding) {
    return;
  }
  auto obj = parseLocked(binding, resolved->timestamp, resolved->payload);
  if (!obj.has_value()) {
    qCWarning(lcSceneEntitiesLayer) << "renderAt parseObject failed:" << QString::fromStdString(obj.error());
    return;
  }
  auto* batch = obj->object.get<PJ::sdk::SceneEntities>();
  if (batch == nullptr) {
    return;
  }
  const std::string frame = batchSourceFrame(*batch);
  if (frame != source_frame_) {
    source_frame_ = frame;
    emit sourceFrameChanged(QString::fromStdString(source_frame_));
    emit fallbackFramesChanged(fallbackFrames());
  }
  pass_.setActive(std::make_shared<const DecodedSceneEntities>(decodeSceneEntities(*batch)));
  last_marker_uid_ = resolved->sequential_uid;
  // Seed the model path's cache with this just-decoded batch so the subsequent
  // ensureModelStateAt fold reuses it instead of re-parsing the SAME entry — a
  // topic carrying both markers and a model (embedded GLB) would otherwise decode
  // the message twice per tracker change (once here, once for the model state).
  // Moved, not copied: the ObjectRecord is discarded right after.
  cacheSnapshot(
      resolved->sequential_uid, std::make_shared<PJ::sdk::SceneEntities>(std::move(*batch)), resolved->timestamp);
  emit repaintRequested();
}

void SceneEntitiesLayer::applyOverrides() {
  pass_.setOverrides(overrides_);
  emit repaintRequested();
}

std::vector<MeshRenderPass::DrawCall> SceneEntitiesLayer::modelDrawCallsForFrame(const FrameContext& frame_ctx) const {
  std::vector<MeshRenderPass::DrawCall> draws;
  for (const auto& [entity_id, entity] : entities_) {
    const auto tf = frame_ctx.lookup(entity.frame_id);
    if (!tf.has_value()) {
      continue;
    }
    const glm::mat4 frame_model = glm::mat4(tf->matrix());
    for (std::size_t i = 0; i < entity.models.size(); ++i) {
      const PJ::sdk::ModelPrimitive& primitive = entity.models[i];
      MeshRenderPass::DrawCall draw;
      draw.kind = MeshRenderPass::GeometryKind::kMesh;
      draw.mesh_key = meshKey(topic_id_, entity_id, i);
      draw.model = frame_model * poseToMat4(primitive.pose);
      draw.model = glm::scale(draw.model, toVec3(primitive.scale));
      if (primitive.override_color) {
        draw.color = toVec4(primitive.color);
        draw.use_vertex_color = false;
      } else {
        draw.color = glm::vec4(1.0f);
        draw.use_vertex_color = true;
      }
      // The viewer-side recolor (config widget) trumps the message's own color,
      // same as the marker path; the alpha channel is preserved.
      if (overrides_.color_override) {
        draw.color = glm::vec4(
            overrides_.override_color.r, overrides_.override_color.g, overrides_.override_color.b, draw.color.a);
        draw.use_vertex_color = false;
      }
      draws.push_back(std::move(draw));
    }
  }
  return draws;
}

bool SceneEntitiesLayer::applyWindow(int64_t lo_ns, int64_t hi_ns) {
  PJ::ObjectStore& store = ctx_.session->objectStore();
  pruneSnapshotCacheBelow(store.firstSequentialUID(topic_id_));

  bool applied = false;
  std::size_t failed_fetches = 0;
  // rangeByTime is the out-of-order-safe fold source: a decode-free, ascending,
  // eviction-safe snapshot of (lo, hi]. An arrival-order UID walk would skip a late
  // (older-ts, newest-UID) batch that sits at a high UID but an in-window timestamp;
  // this walks by timestamp and cannot. Each ref is resolved afterwards (an entry
  // evicted between the snapshot and the resolve simply comes back nullopt).
  const auto window = store.rangeByTime(topic_id_, lo_ns, hi_ns);
  for (const auto& ref : window) {
    // Cache hit: re-fold the decoded batch (backward scrub / rebuild) without
    // re-parsing — the protobuf decode of heavy embedded models is what hitched.
    if (const auto cached = snapshot_cache_.find(ref.uid); cached != snapshot_cache_.end()) {
      applySnapshot(*cached->second.batch, cached->second.store_ns);
      applied = true;
      continue;
    }
    auto entry = store.at(topic_id_, ref.uid);
    if (entry.has_value() && entry->fetch_failed) {
      // Permanent loss: the incremental fold marks this window applied, so
      // forward playback never re-reads it. Count and report, don't retry.
      ++failed_fetches;
      continue;
    }
    if (!entry.has_value() || entry->payload.bytes.empty()) {
      continue;  // evicted between the snapshot and the resolve
    }
    const auto binding = ctx_.session->parserBindingForObjectTopic(topic_id_);
    if (!binding) {
      continue;
    }
    auto obj = parseLocked(binding, entry->timestamp, entry->payload);
    if (!obj.has_value()) {
      qCWarning(lcSceneEntitiesLayer) << "applyWindow parseObject failed:" << QString::fromStdString(obj.error());
      continue;
    }
    auto* snapshot = obj->object.get<PJ::sdk::SceneEntities>();
    if (snapshot != nullptr) {
      applySnapshot(*snapshot, entry->timestamp);
      // Moved, not copied: the ObjectRecord is discarded at the end of this step.
      cacheSnapshot(ref.uid, std::make_shared<PJ::sdk::SceneEntities>(std::move(*snapshot)), entry->timestamp);
      applied = true;
    }
  }
  if (failed_fetches != 0) {
    qCWarning(lcSceneEntitiesLayer) << failed_fetches
                                    << "marker batch(es) LOST: lazy re-read failed (source file truncated,"
                                    << "replaced, or corrupt)";
  }
  return applied;
}

void SceneEntitiesLayer::cacheSnapshot(
    PJ::SequentialUID uid, std::shared_ptr<const PJ::sdk::SceneEntities> batch, int64_t store_ns) {
  if (!uid.valid() || batch == nullptr) {
    return;
  }
  const auto [it, inserted] = snapshot_cache_.try_emplace(uid);
  if (!inserted) {
    return;
  }
  it->second.bytes = estimateSnapshotBytes(*batch);
  it->second.batch = std::move(batch);
  it->second.store_ns = store_ns;
  snapshot_cache_bytes_ += it->second.bytes;
  // Evict lowest-UID first; keep at least one entry so the just-decoded batch
  // is never thrown away by its own insertion.
  while (snapshot_cache_bytes_ > kSnapshotCacheMaxBytes && snapshot_cache_.size() > 1) {
    const auto oldest = snapshot_cache_.begin();
    snapshot_cache_bytes_ -= oldest->second.bytes;
    snapshot_cache_.erase(oldest);
  }
}

void SceneEntitiesLayer::pruneSnapshotCacheBelow(PJ::SequentialUID first_retained_uid) {
  if (!first_retained_uid.valid()) {
    return;
  }
  const auto retained_begin = snapshot_cache_.lower_bound(first_retained_uid);
  for (auto it = snapshot_cache_.begin(); it != retained_begin; ++it) {
    snapshot_cache_bytes_ -= it->second.bytes;
  }
  snapshot_cache_.erase(snapshot_cache_.begin(), retained_begin);
}

void SceneEntitiesLayer::rebuildModelStateAt(PJ::Timepoint time) {
  entities_.clear();
  entity_expiry_anchor_ns_.clear();
  state_built_at_ = time;
  applied_uid_high_ = {};

  if (ctx_.session == nullptr) {
    updateModelFrames();
    return;
  }
  PJ::ObjectStore& store = ctx_.session->objectStore();
  const int64_t time_ns = PJ::toRaw(time);
  // High-water arrival UID among ts <= time — NOT latestAt()'s UID, which an
  // out-of-order insert can leave below a retained entry's (see applied_uid_high_).
  const PJ::SequentialUID high = store.maxUidAtOrBefore(topic_id_, time_ns);
  if (!high.valid()) {
    updateModelFrames();  // no batch at/before time
    return;
  }
  applyWindow(std::numeric_limits<int64_t>::min(), time_ns);
  applied_uid_high_ = high;
  dropExpiredEntities(time);
  updateModelFrames();
  startMeshLoadsForCurrentEntities();
}

void SceneEntitiesLayer::ensureModelStateAt(PJ::Timepoint time) {
  if (ctx_.session == nullptr) {
    if (!state_built_at_.has_value() || *state_built_at_ != time) {
      rebuildModelStateAt(time);
    }
    return;
  }

  PJ::ObjectStore& store = ctx_.session->objectStore();
  const int64_t time_ns = PJ::toRaw(time);
  const PJ::SequentialUID high_now = store.maxUidAtOrBefore(topic_id_, time_ns);

  if (state_built_at_.has_value() && *state_built_at_ == time && high_now == applied_uid_high_) {
    // Already built at this exact playhead and no batch at/before it arrived since
    // (equal high-water; the both-empty case is invalid == invalid).
    return;
  }

  // Incremental forward fold: when the playhead only advanced AND nothing changed at
  // or below the previous build time, fold just the (state_built_at_, time] window
  // instead of replaying the whole history (which would re-parse — and re-hash heavy
  // embedded models in — every frame). A late (out-of-order) batch inserted at
  // ts <= state_built_at_ raises maxUidAtOrBefore THERE above what we folded, and a
  // forward window cannot reach it — so fall back to a full rebuild. This is the same
  // retroactive-ingest guard the occupancy layer uses. Backward scrubs / jumps also
  // fall through to rebuild.
  const bool can_incremental = state_built_at_.has_value() && applied_uid_high_.valid() && time >= *state_built_at_;
  if (can_incremental) {
    const PJ::SequentialUID high_at_built = store.maxUidAtOrBefore(topic_id_, PJ::toRaw(*state_built_at_));
    if (high_at_built == applied_uid_high_) {
      const bool applied_new = applyWindow(PJ::toRaw(*state_built_at_), time_ns);
      applied_uid_high_ = high_now;
      state_built_at_ = time;
      const bool dropped = dropExpiredEntities(time);
      updateModelFrames();
      if (applied_new) {
        startMeshLoadsForCurrentEntities();
      }
      // renderAt's UID guard skips its per-tick repaint while the marker batch is
      // unchanged, so model-state changes must request their own frame — without
      // this, an entity erased by lifetime expiry lingers on screen. Gated on real
      // change: render() also lands here, and an unconditional emit would request
      // frames forever.
      if (applied_new || dropped) {
        emit repaintRequested();
      }
      return;
    }
  }
  rebuildModelStateAt(time);
  // A full rebuild can change the state arbitrarily; renderAt may not repaint
  // (same active marker batch), so request the frame here. No loop risk: the
  // next ensure at this playhead takes the built-at early-return above.
  emit repaintRequested();
}

void SceneEntitiesLayer::applySnapshot(const PJ::sdk::SceneEntities& snapshot, int64_t ingest_ns) {
  pj::scene3d::applySceneEntityDeletions(entities_, snapshot, [this](auto it) { return eraseEntity(it); });
  if (!snapshot.deletions.empty()) {
    // A deletion may have pruned a failed model's record — drop it from the notice.
    updateRemoteFetchNotice();
  }
  for (const PJ::sdk::SceneEntity& entity : snapshot.entities) {
    entities_[entity.id] = entity;
    // Anchor lifetime expiry on the ingest (tracker-clock) timestamp, not the
    // entity's embedded sensor epoch — see sceneEntityExpiredAt(). Written in lockstep with
    // every entities_ insert; eraseEntity() drops both together.
    entity_expiry_anchor_ns_[entity.id] = ingest_ns;
  }
}

std::map<std::string, PJ::sdk::SceneEntity>::iterator SceneEntitiesLayer::eraseEntity(
    std::map<std::string, PJ::sdk::SceneEntity>::iterator it) {
  // Drop the entity's model mesh-load records too, so a failed model stops
  // feeding the status notice once its entity is gone. The caller refreshes the
  // notice after the erase batch (updateRemoteFetchNotice is no-op if unchanged).
  for (std::size_t i = 0; i < it->second.models.size(); ++i) {
    mesh_loads_->erase(meshKey(topic_id_, it->first, i));
  }
  entity_expiry_anchor_ns_.erase(it->first);
  return entities_.erase(it);
}

bool SceneEntitiesLayer::dropExpiredEntities(PJ::Timepoint time) {
  const int64_t time_ns = PJ::toRaw(time);
  bool dropped = false;
  for (auto it = entities_.begin(); it != entities_.end();) {
    // .at(): the anchor is written in lockstep with every entities_ insert
    // (applySnapshot) and erased via eraseEntity, so a miss is a broken invariant.
    if (sceneEntityExpiredAt(it->second, entity_expiry_anchor_ns_.at(it->first), time_ns)) {
      it = eraseEntity(it);
      dropped = true;
    } else {
      ++it;
    }
  }
  if (dropped) {
    // An expired entity's failed-model record was pruned — refresh the notice.
    updateRemoteFetchNotice();
  }
  return dropped;
}

void SceneEntitiesLayer::updateModelFrames() {
  QStringList frames;
  for (const auto& [_, entity] : entities_) {
    if (entity.frame_id.empty()) {
      continue;
    }
    const QString frame = QString::fromStdString(entity.frame_id);
    if (!frames.contains(frame)) {
      frames.append(frame);
    }
  }
  if (frames != model_frames_) {
    model_frames_ = std::move(frames);
    emit fallbackFramesChanged(fallbackFrames());
  }
}

void SceneEntitiesLayer::startMeshLoadsForCurrentEntities() {
  for (const auto& [entity_id, entity] : entities_) {
    for (std::size_t i = 0; i < entity.models.size(); ++i) {
      startMeshLoadIfNeeded(meshKey(topic_id_, entity_id, i), entity.models[i]);
    }
  }
}

void SceneEntitiesLayer::startMeshLoadIfNeeded(const std::string& key, const PJ::sdk::ModelPrimitive& primitive) {
  const std::string signature = sourceSignature(primitive);
  if (signature.empty()) {
    return;
  }
  const MeshLoadEntry* existing = mesh_loads_->find(key);
  if (existing != nullptr && existing->identity == signature) {
    return;  // loaded, loading, fetch in flight, or recorded as blocked/failed
  }
  // Same key, new source bytes: blank the stale mesh until the reload lands.
  if (existing != nullptr) {
    mesh_pass_->setMeshData(key, MeshData{});
  }

  const QString hint = hintFromMediaType(primitive.media_type, primitive.url);

  if (!primitive.data.empty()) {
    MeshLoadEntry& entry = mesh_loads_->insertOrReplace(key, signature);
    entry.source_label = tr("(embedded model)");
    const QByteArray bytes(
        reinterpret_cast<const char*>(primitive.data.data()), static_cast<qsizetype>(primitive.data.size()));
    startRecordImport(entry, bytes, hint);
    updateRemoteFetchNotice();
    return;
  }

  const QString url_text = QString::fromStdString(primitive.url);
  const QUrl url(url_text);
  // Local sources (bare paths / file:// URLs) stay ungated: reading the user's
  // disk is not network egress. Only data-supplied http(s) URLs need consent.
  const bool is_remote = url.scheme() == "http"_L1 || url.scheme() == "https"_L1;
  if (is_remote && !remoteModelFetchAllowed()) {
    // Recorded once as consumed+failed so the gate is decided per (key,
    // signature), never re-checked per tracker tick.
    MeshLoadEntry& entry = mesh_loads_->insertOrReplace(key, signature);
    entry.consumed = true;
    entry.failed = true;
    entry.blocked_by_policy = true;
    entry.source_label = url_text;
    qCWarning(lcSceneEntitiesLayer) << "blocked remote model fetch for" << url_text
                                    << "- enable pj_scene3d/allow_remote_model_fetch to allow";
    updateRemoteFetchNotice();
    return;
  }

  // Insert the entry BEFORE kicking the fetch (future stays default-invalid =
  // pending) so per-tick re-entry dedupes on (key, signature) while the bytes
  // are still in flight. source_label feeds the failure notice if it fails.
  mesh_loads_->insertOrReplace(key, signature).source_label = url_text;
  updateRemoteFetchNotice();
  if (!url_fetcher_) {
    url_fetcher_ = std::make_unique<UrlFetcher>();
  }
  // The fetcher is owned by this layer, so no callback can fire after the layer
  // (or resetReplayState) destroys it. The entry may have been REPLACED by a
  // newer publish meanwhile — re-locate it by key AND signature and drop the
  // result on mismatch.
  url_fetcher_->fetch(url, [this, key, signature, hint, url_text](const FetchResult& fetched) {
    MeshLoadEntry* target = mesh_loads_->find(key);
    if (target == nullptr || target->identity != signature) {
      return;
    }
    if (!fetched.ok) {
      qCWarning(lcSceneEntitiesLayer) << "ModelPrimitive fetch failed for" << url_text << ":" << fetched.error;
      target->consumed = true;
      target->failed = true;
      target->fetch_failed = true;
      target->error = fetched.error;
      updateRemoteFetchNotice();
      return;
    }
    startRecordImport(*target, fetched.bytes, hint);
  });
}

void SceneEntitiesLayer::startRecordImport(MeshLoadEntry& entry, const QByteArray& bytes, const QString& format_hint) {
  entry.future = mesh_loader_->loadFromMemory(bytes, format_hint);
  // pollMeshLoads() drains the result and emits repaintRequested(), exactly as a
  // render()-driven poll would. arm() connects BEFORE setFuture so an
  // already-finished load still fires.
  mesh_loads_->arm(entry, this, [this]() { pollMeshLoads(); });
}

void SceneEntitiesLayer::updateRemoteFetchNotice() {
  int blocked = 0;
  QStringList lines;
  for (const auto& [key, entry] : *mesh_loads_) {
    if (entry.blocked_by_policy) {
      ++blocked;  // aggregated into one summary line (count, not per-URL)
    } else if (entry.fetch_failed) {
      lines << tr("Failed to fetch %1: %2").arg(entry.source_label, entry.error);
    } else if (entry.consumed && entry.failed) {
      // Bytes arrived but the import rejected them (malformed mesh / unsupported
      // format / embedded stub) — distinct from a fetch failure.
      lines << tr("Failed to load model %1: %2").arg(entry.source_label, entry.error);
    }
  }
  QString notice;
  if (blocked > 0) {
    notice =
        tr("Remote model fetch is disabled — %n model URL(s) blocked. "
           "Enable pj_scene3d/allow_remote_model_fetch to allow.",
           nullptr, blocked);
  }
  if (!lines.isEmpty()) {
    if (!notice.isEmpty()) {
      notice += QLatin1Char('\n');
    }
    notice += lines.join(QLatin1Char('\n'));
  }
  if (notice != remote_fetch_notice_) {
    remote_fetch_notice_ = notice;
    emit remoteFetchNoticeChanged(remote_fetch_notice_);
    // Drives the dock's layer-row warning combine (icon + tooltip) — see
    // Scene3DLayer::statusWarning / Scene3DDockWidget::recomputeOrphanStates.
    emit statusWarningChanged();
  }
}

void SceneEntitiesLayer::pollMeshLoads() {
  // No per-failure eviction here: SceneEntities loads are in-memory (no
  // path-keyed loader cache to evict) and a failed entry stays recorded so the
  // (key, signature) dedup keeps it from re-importing each tick. Log import
  // failures so the three model-load failure modes (blocked / fetch / import)
  // are all visible in the application log, not only in the config-widget notice.
  const auto log_import_failure = [](const std::string& /*key*/, const MeshLoadEntry& entry) {
    qCWarning(lcSceneEntitiesLayer) << "model import failed for" << entry.source_label << ":" << entry.error;
  };
  if (mesh_loads_->drain(*mesh_pass_, log_import_failure).changed) {
    // A drain can flip an entry to failed (import rejected the bytes), so refresh
    // the notice before repainting — otherwise a parse failure stays silent.
    updateRemoteFetchNotice();
    emit repaintRequested();
  }
}

void SceneEntitiesLayer::setOpacity(float opacity) {
  const float clamped = std::clamp(opacity, 0.0f, 1.0f);
  if (overrides_.opacity == clamped) {
    return;
  }
  overrides_.opacity = clamped;
  emit configurationChanged();
  applyOverrides();
}

void SceneEntitiesLayer::setColorOverrideEnabled(bool enabled) {
  if (overrides_.color_override == enabled) {
    return;
  }
  overrides_.color_override = enabled;
  emit configurationChanged();
  applyOverrides();
}

void SceneEntitiesLayer::setOverrideColor(QColor color) {
  if (!color.isValid()) {
    return;
  }
  const glm::vec4 next{
      static_cast<float>(color.redF()), static_cast<float>(color.greenF()), static_cast<float>(color.blueF()), 1.0F};
  if (overrides_.override_color.r == next.r && overrides_.override_color.g == next.g &&
      overrides_.override_color.b == next.b) {
    return;
  }
  overrides_.override_color = next;
  emit configurationChanged();
  applyOverrides();
}

void SceneEntitiesLayer::setWireframe(bool enabled) {
  if (overrides_.wireframe == enabled) {
    return;
  }
  overrides_.wireframe = enabled;
  emit configurationChanged();
  applyOverrides();
}

QWidget* SceneEntitiesLayer::createConfigWidget(QWidget* parent) {
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
  form->setHorizontalSpacing(PJ::theme::space(PJ::theme::Space::Comfortable));
  form->setVerticalSpacing(PJ::theme::space(PJ::theme::Space::Snug));
  outer->addLayout(form);

  // Opacity (always active): multiplies every primitive's alpha.
  auto* opacity_spin = new PJ::DoubleScrubber(container);
  opacity_spin->setRange(0.0, 1.0);
  opacity_spin->setDecimals(2);
  opacity_spin->setSingleStep(0.05);
  opacity_spin->setValue(static_cast<double>(overrides_.opacity));
  form->addRow(tr("Opacity:"), opacity_spin);
  QObject::connect(
      opacity_spin, &PJ::DoubleScrubber::valueChanged, this, [this](double v) { setOpacity(static_cast<float>(v)); });

  // Override color: a sliding switch + swatch on one row. The marker protocol
  // has no override field — this is a viewer-only recolor of every primitive.
  // Picking a color auto-enables it.
  auto* override_row = new QWidget(container);
  auto* override_layout = new QHBoxLayout(override_row);
  override_layout->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
  override_layout->setSpacing(PJ::theme::space(PJ::theme::Space::Comfortable));
  auto* override_toggle = new PJ::ToggleSwitch(override_row);
  override_toggle->setChecked(overrides_.color_override, /*animate=*/false);
  auto* swatch = new PJ::ColorPickerWidget(override_row);
  swatch->setColor(overrideColor());
  override_layout->addWidget(override_toggle);
  override_layout->addWidget(swatch);
  override_layout->addStretch();
  form->addRow(tr("Override color:"), override_row);

  QObject::connect(override_toggle, &PJ::ToggleSwitch::toggled, this, [this](bool on) { setColorOverrideEnabled(on); });
  QObject::connect(swatch, &PJ::ColorPickerWidget::colorChanged, this, [this, override_toggle](QColor c) {
    setOverrideColor(c);
    if (!override_toggle->isChecked()) {
      override_toggle->setChecked(true);  // picking implies enable (visual slide)
    }
    setColorOverrideEnabled(true);  // apply now; the toggled-driven call lags the slide animation
  });

  // Wireframe (always active): draws mesh primitives as edges.
  auto* wire_toggle = new PJ::ToggleSwitch(container);
  wire_toggle->setChecked(overrides_.wireframe, /*animate=*/false);
  form->addRow(tr("Wireframe:"), wire_toggle);
  QObject::connect(wire_toggle, &PJ::ToggleSwitch::toggled, this, [this](bool on) { setWireframe(on); });

  // Remote-fetch notice (consent-gate blocks / failed model-URL fetches).
  // Hidden while there is nothing to surface.
  auto* fetch_notice = new QLabel(remote_fetch_notice_, container);
  fetch_notice->setWordWrap(true);
  fetch_notice->setVisible(!remote_fetch_notice_.isEmpty());
  outer->addWidget(fetch_notice);
  QObject::connect(
      this, &SceneEntitiesLayer::remoteFetchNoticeChanged, fetch_notice, [fetch_notice](const QString& text) {
        fetch_notice->setText(text);
        fetch_notice->setVisible(!text.isEmpty());
      });

  return container;
}

}  // namespace pj::scene3d
