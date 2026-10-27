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
#include <array>
#include <cmath>
#include <exception>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "layer_xml_validation.h"
#include "mesh_import.h"
#include "mesh_load_queue.h"
#include "pj_base/time.hpp"
#include "pj_runtime/HttpUrl.h"
#include "pj_runtime/ResultHistory.h"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/model_budget.h"
#include "pj_scene3d_core/pose_math.h"
#include "pj_scene3d_core/scene_entities_decode.h"
#include "pj_scene3d_widgets/object_topic_metadata.h"
#include "pj_scene3d_widgets/resolve_object.h"
#include "pj_widgets/ColorPickerWidget.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/Fnv1a.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/ToggleSwitch.h"
using namespace Qt::StringLiterals;

namespace pj::scene3d {

namespace {
Q_LOGGING_CATEGORY(lcSceneEntitiesLayer, "pj.scene3d.entity.markers")

// At most this many per-model failure lines in the notice.
constexpr qsizetype kMaxNoticeLines = 8;

// The source frame of a batch is the message-level frame: the frame_id of its
// first entity (per-entity frames are handled by the renderers).
std::string batchSourceFrame(const PJ::sdk::SceneEntities& batch) {
  return batch.entities.empty() ? std::string{} : batch.entities.front().frame_id;
}

std::string meshKey(PJ::ObjectTopicId topic_id, const std::string& entity_id, std::size_t model_index) {
  return std::to_string(topic_id.id) + ":" + entity_id + ":" + std::to_string(model_index);
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
           std::to_string(PJ::fnv1a(primitive.data));
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
// per newly-seen model source.
bool remoteModelFetchAllowed() {
  return QSettings().value(u"pj_scene3d/allow_remote_model_fetch"_s, true).toBool();
}

// Heap footprint of a decoded batch (saturating), for the snapshot-cache budget.
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

}  // namespace

SceneEntitiesLayer::SceneEntitiesLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent)
    : Scene3DLayer(parent),
      topic_id_(topic_id),
      display_name_(std::move(display_name)),
      model_queue_(std::make_unique<MeshLoadQueue>(this)) {}

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

std::optional<SceneEntitiesLayer::ParsedSettings> SceneEntitiesLayer::parseSettings(const QDomElement& element) {
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
    if (!settings.override_color->isValid()) {
      return std::nullopt;
    }
  }
  return settings;
}

bool SceneEntitiesLayer::validateXml(const QDomElement& element) {
  return parseSettings(element).has_value();
}

bool SceneEntitiesLayer::xmlLoadState(const QDomElement& element) {
  const std::optional<ParsedSettings> settings = parseSettings(element);
  if (!settings.has_value()) {
    return false;
  }
  setOpacity(settings->opacity);
  if (settings->override_color.has_value()) {
    setOverrideColor(*settings->override_color);
  }
  setColorOverrideEnabled(settings->color_override);
  setWireframe(settings->wireframe);
  return true;
}

bool SceneEntitiesLayer::attach(const PJ::SceneLayerContext& ctx) {
  const auto& scene3d_ctx = static_cast<const Scene3DLayerContext&>(ctx);
  if (scene3d_ctx.session == nullptr) {
    qCWarning(lcSceneEntitiesLayer) << "attach: session is null";
    return false;
  }
  if (!scene3d_ctx.session->parserBindingForObjectTopic(topic_id_) &&
      !hasCanonical3DCodec(PJ::sdk::BuiltinObjectType::kSceneEntities)) {
    qCWarning(lcSceneEntitiesLayer) << "attach: no parser or canonical codec for topic_id=" << topic_id_.id;
    return false;
  }
  ctx_ = scene3d_ctx;
  snapshot_stream_ = isSnapshotStream(ctx_.session->objectStore().descriptor(topic_id_));
  // A dataset reload may re-attach without a detach(): start from the pristine
  // baseline so prior-generation state never skips or anchors the new replay.
  resetReplayState();
  disconnect(reload_connection_);
  reload_connection_ = connect(
      ctx_.session, &PJ::SessionManager::datasetAboutToBeReplaced, this,
      &SceneEntitiesLayer::onDatasetAboutToBeReplaced);
  disconnect(samples_connection_);
  samples_connection_ = connect(ctx_.session, &PJ::SessionManager::samplesIngested, this, [this](const auto&, bool) {
    if (visible_ && requested_time_.has_value()) {
      decode_dirty_ = true;
      emit repaintRequested();
    }
  });
  // Seed only when a first sample exists; gate on presence, not on a 0 sentinel
  // (t=0 is a valid first timestamp under ROS sim time).
  PJ::ObjectStore& store = ctx_.session->objectStore();
  if (store.entryCount(topic_id_) > 0) {
    const PJ::Timepoint first = PJ::fromRaw(store.timeRange(topic_id_).first);
    requested_time_ = first;
    renderAt(PJ::toRaw(first));
    rebuildModelStateAt(first);
  }
  return true;
}

void SceneEntitiesLayer::detach() {
  disconnect(reload_connection_);
  reload_connection_ = {};
  disconnect(samples_connection_);
  samples_connection_ = {};
  resetReplayState();
  ctx_ = {};
}

void SceneEntitiesLayer::onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id) {
  if (ctx_.session == nullptr || ctx_.session->objectStore().descriptor(topic_id_).dataset_id != dataset_id) {
    return;
  }
  resetReplayState();
}

void SceneEntitiesLayer::resetReplayState() {
  decode_dirty_ = false;
  invalidateDecodeState();
  clearGeometry();
  requested_time_.reset();
  last_marker_uid_ = {};
  shown_store_ns_.reset();
  model_state_.clear();
  state_built_at_.reset();
  applied_uid_high_ = {};
  snapshot_cache_.clear();
  snapshot_cache_bytes_ = 0;
  model_frames_.clear();
  setSourceFrame({});
  // Running imports keep their slots until they settle; their results are dropped.
  model_queue_->cancelAll();
  model_loads_.clear();
  while (!model_mesh_bytes_.empty()) {
    releaseModelMesh(std::string(model_mesh_bytes_.begin()->first));  // a copy: the call erases the entry
  }
  model_draw_calls_.clear();
  ++model_revision_;
  batch_warning_.clear();
  model_state_warning_.clear();
  model_draw_warning_.clear();
  noteRenderSuccess();
  updateRemoteFetchNotice();  // re-combines the data warning
}

void SceneEntitiesLayer::setTrackerTime(PJ::Timepoint time) {
  requested_time_ = time;
  if (!visible_) {
    return;
  }
  renderAt(PJ::toRaw(time));
  ensureModelStateAt(time);
}

PlacementHold* SceneEntitiesLayer::placementHoldFor(const std::string& frame) const {
  return &frame_holds_[frame];  // ponytail: one per frame ever drawn; prune when entity frames churn by the thousand
}

uint64_t SceneEntitiesLayer::renderKey(PJ::Timepoint time) const {
  const PJ::ObjectStore* store = ctx_.session != nullptr ? &ctx_.session->objectStore() : nullptr;
  return PJ::fnv1aMix(
      PJ::fnv1aMix(PJ::kFnv1aOffsetBasis, PJ::ISceneLayer::renderKey(time)),
      PJ::latestEntryRenderKey(store, topic_id_, time));
}

void SceneEntitiesLayer::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  if (!visible_) {
    // Release the expanded geometry while hidden; the dock re-delivers the
    // tracker time on un-hide, which re-selects the batch.
    invalidateDecodeState();
    clearGeometry();
    noteRenderSuccess();
  }
  emit visibilityChanged(visible);
  emit infoChanged();
  emit repaintRequested();
}

void SceneEntitiesLayer::setSourceFrame(std::string frame) {
  if (frame == source_frame_) {
    return;
  }
  source_frame_ = std::move(frame);
  emit sourceFrameChanged(QString::fromStdString(source_frame_));
  emit fallbackFramesChanged(fallbackFrames());
}

std::shared_ptr<PJ::sdk::SceneEntities> SceneEntitiesLayer::decodeEntry(
    std::int64_t timestamp, const PJ::sdk::PayloadView& payload, QString& error) const {
  auto object = resolveObject(
      ctx_.session->parserBindingForObjectTopic(topic_id_), PJ::sdk::BuiltinObjectType::kSceneEntities, timestamp,
      payload);
  if (!object.has_value()) {
    error = tr("SceneEntities could not be decoded: %1").arg(QString::fromStdString(object.error()));
    return nullptr;
  }
  auto* decoded = object->object.get<PJ::sdk::SceneEntities>();
  if (decoded == nullptr) {
    error = tr("Object parser returned the wrong type for SceneEntities");
    return nullptr;
  }
  // Moved, not copied: the ObjectRecord is discarded right after.
  return std::make_shared<PJ::sdk::SceneEntities>(std::move(*decoded));
}

SceneEntitiesLayer::MarkerPick SceneEntitiesLayer::selectMarkerBatch(
    std::int64_t time_ns, std::shared_ptr<const PJ::sdk::SceneEntities>& batch) {
  if (ctx_.session == nullptr) {
    return MarkerPick::kNoSample;
  }
  PJ::ObjectStore& store = ctx_.session->objectStore();
  // Checked metadata-first (latestEntryIdAt) so an unchanged batch never
  // resolves payload bytes: a resolve can be a cold refetch once a pool seed
  // is evicted. SequentialUID is stable across front eviction. Re-checked after
  // the resolve, since a racing insert can change the pick in between.
  const auto entry_id = store.latestEntryIdAt(topic_id_, time_ns);
  if (!entry_id.has_value()) {
    return MarkerPick::kNoSample;
  }
  if (entry_id->uid == last_marker_uid_) {
    return MarkerPick::kUnchanged;
  }
  auto resolved = store.latestAt(topic_id_, time_ns);
  if (!resolved.has_value() || resolved->payload.bytes.empty()) {
    return MarkerPick::kNoSample;
  }
  if (resolved->sequential_uid == last_marker_uid_) {
    return MarkerPick::kUnchanged;
  }
  last_marker_uid_ = resolved->sequential_uid;
  shown_store_ns_ = resolved->timestamp;
  finding_stamp_.reset();
  if (resolved->timestamp == PJ::kFindingTimestamp) {
    if (const auto stamp = PJ::derivedTopicPresentation(ctx_.session, topic_id_).finding_stamp) {
      finding_stamp_ = PJ::fromRaw(*stamp);
    }
  }
  if (static_cast<std::uint64_t>(resolved->payload.bytes.size()) > kSceneLimits.max_marker_wire_bytes) {
    setBatchWarning(
        tr("SceneEntities payload is %1 MiB; the limit is %2 MiB")
            .arg(QString::number(static_cast<double>(resolved->payload.bytes.size()) / (1024.0 * 1024.0), 'f', 1))
            .arg(kSceneLimits.max_marker_wire_bytes / (1024ULL * 1024ULL)));
    return MarkerPick::kRejected;
  }
  if (const auto cached = snapshot_cache_.find(resolved->sequential_uid); cached != snapshot_cache_.end()) {
    batch = cached->second.batch;
  } else {
    QString error;
    auto decoded = decodeEntry(resolved->timestamp, resolved->payload, error);
    if (decoded == nullptr) {
      qCWarning(lcSceneEntitiesLayer) << "renderAt:" << error;
      setBatchWarning(std::move(error));
      return MarkerPick::kRejected;
    }
    batch = decoded;
    // Seed the model path's cache so the following model fold reuses this batch
    // instead of re-parsing the same entry.
    cacheSnapshot(resolved->sequential_uid, std::move(decoded), resolved->timestamp);
  }
  setBatchWarning({});
  setSourceFrame(batchSourceFrame(*batch));
  return MarkerPick::kBatch;
}

void SceneEntitiesLayer::renderAt(std::int64_t time_ns) {
  std::shared_ptr<const PJ::sdk::SceneEntities> batch;
  const MarkerPick pick = selectMarkerBatch(time_ns, batch);
  switch (pick) {
    case MarkerPick::kUnchanged:
      return;
    case MarkerPick::kNoSample:
      if (shown_store_ns_.has_value() && time_ns >= *shown_store_ns_) {
        return;  // the shown entry was evicted under the cursor: it stays
      }
      // Nothing at/before the playhead: drop stale geometry and any in-flight
      // expansion of the previous pick.
      shown_store_ns_.reset();
      invalidateDecodeState();
      clearGeometry();
      finding_stamp_.reset();
      return;
    case MarkerPick::kRejected: {
      // Keep the rejected UID memoized; only the geometry and in-flight work go.
      const PJ::SequentialUID rejected = last_marker_uid_;
      invalidateDecodeState();
      last_marker_uid_ = rejected;
      clearGeometry();
      emit repaintRequested();
      return;
    }
    case MarkerPick::kBatch:
      decode_worker_.request(DecodeRequest{std::move(batch), last_marker_uid_});
      return;
  }
}

void SceneEntitiesLayer::applyOverrides() {
  emit repaintRequested();
}

bool SceneEntitiesLayer::applySnapshot(const PJ::sdk::SceneEntities& snapshot, std::int64_t store_ns) {
  const auto projected = model_state_.projectedRetainedBytes(snapshot);
  if (!projected.has_value() || *projected > kSceneLimits.max_model_state_bytes_per_layer) {
    model_state_warning_ = tr("SceneEntities model state would retain more than %1 MiB")
                               .arg(kSceneLimits.max_model_state_bytes_per_layer / (1024ULL * 1024ULL));
    return false;
  }
  // An anchor at the end of time never expires: a finding stays until replaced or removed.
  const std::int64_t anchor = store_ns == PJ::kFindingTimestamp ? std::numeric_limits<std::int64_t>::max() : store_ns;
  return model_state_.applySnapshot(snapshot, anchor);
}

bool SceneEntitiesLayer::foldEntry(const PJ::ObjectStore::TimeRangeEntry& reference, std::size_t& failed_fetches) {
  if (const auto cached = snapshot_cache_.find(reference.uid); cached != snapshot_cache_.end()) {
    return applySnapshot(*cached->second.batch, cached->second.store_ns);
  }
  PJ::ObjectStore& store = ctx_.session->objectStore();
  const auto entry = store.at(topic_id_, reference.uid);
  if (entry.has_value() && entry->fetch_failed) {
    // Permanent loss: the incremental fold marks this window applied, so
    // forward playback never re-reads it. Report, don't retry.
    ++failed_fetches;
    return false;
  }
  if (!entry.has_value() || entry->payload.bytes.empty()) {
    return false;  // evicted between the snapshot and the resolve
  }
  if (static_cast<std::uint64_t>(entry->payload.bytes.size()) > kSceneLimits.max_marker_wire_bytes) {
    model_state_warning_ = tr("A SceneEntities model snapshot exceeds the %1 MiB payload limit")
                               .arg(kSceneLimits.max_marker_wire_bytes / (1024ULL * 1024ULL));
    return false;
  }
  QString error;
  auto snapshot = decodeEntry(entry->timestamp, entry->payload, error);
  if (snapshot == nullptr) {
    qCWarning(lcSceneEntitiesLayer) << "foldEntry:" << error;
    model_state_warning_ = std::move(error);
    return false;
  }
  const bool applied = applySnapshot(*snapshot, entry->timestamp);
  cacheSnapshot(reference.uid, std::move(snapshot), entry->timestamp);
  return applied;
}

bool SceneEntitiesLayer::applyModelWindow(std::int64_t lo_ns, std::int64_t hi_ns) {
  PJ::ObjectStore& store = ctx_.session->objectStore();
  pruneSnapshotCacheBelow(store.firstSequentialUID(topic_id_));
  bool applied = false;
  std::size_t failed_fetches = 0;
  // rangeByTime is the out-of-order-safe fold source: a decode-free, ascending,
  // eviction-safe snapshot of (lo, hi]. An arrival-order UID walk would skip a
  // late (older-ts, newest-UID) batch inside the window.
  for (const auto& reference : store.rangeByTime(topic_id_, lo_ns, hi_ns)) {
    applied = foldEntry(reference, failed_fetches) || applied;
  }
  if (failed_fetches != 0) {
    qCWarning(lcSceneEntitiesLayer) << failed_fetches
                                    << "marker batch(es) LOST: lazy re-read failed (source file truncated,"
                                    << "replaced, or corrupt)";
    model_state_warning_ = tr("A SceneEntities model snapshot was lost: source re-read failed");
  }
  return applied;
}

void SceneEntitiesLayer::cacheSnapshot(
    PJ::SequentialUID uid, std::shared_ptr<const PJ::sdk::SceneEntities> batch, std::int64_t store_ns) {
  if (!uid.valid() || batch == nullptr || snapshot_cache_.contains(uid)) {
    return;
  }
  CachedSnapshot cached;
  cached.bytes = estimateSnapshotBytes(*batch);
  if (cached.bytes > kSceneLimits.max_model_snapshot_cache_bytes) {
    // The caller may still use this batch, but it must not become an
    // over-budget retained cache entry by itself.
    return;
  }
  cached.batch = std::move(batch);
  cached.store_ns = store_ns;
  snapshot_cache_bytes_ = saturatedAdd(snapshot_cache_bytes_, cached.bytes);
  snapshot_cache_.emplace(uid, std::move(cached));
  while (snapshot_cache_bytes_ > kSceneLimits.max_model_snapshot_cache_bytes && snapshot_cache_.size() > 1U) {
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
  model_state_.clear();
  model_state_warning_.clear();
  state_built_at_ = time;
  applied_uid_high_ = {};
  if (ctx_.session != nullptr && snapshot_stream_) {
    PJ::ObjectStore& store = ctx_.session->objectStore();
    if (const auto shown = store.latestEntryIdAt(topic_id_, PJ::toRaw(time))) {
      pruneSnapshotCacheBelow(store.firstSequentialUID(topic_id_));
      std::size_t failed_fetches = 0;
      (void)foldEntry(*shown, failed_fetches);
      if (failed_fetches != 0) {
        model_state_warning_ = tr("A SceneEntities model snapshot was lost: source re-read failed");
      }
      applied_uid_high_ = shown->uid;
      (void)model_state_.dropExpired(PJ::toRaw(time));
    }
  } else if (ctx_.session != nullptr) {
    PJ::ObjectStore& store = ctx_.session->objectStore();
    const std::int64_t time_ns = PJ::toRaw(time);
    // High-water arrival UID among ts <= time — NOT latestAt()'s UID, which an
    // out-of-order insert can leave below a retained entry's.
    const PJ::SequentialUID high = store.maxUidAtOrBefore(topic_id_, time_ns);
    if (high.valid()) {
      applyModelWindow(std::numeric_limits<std::int64_t>::min(), time_ns);
      applied_uid_high_ = high;
      (void)model_state_.dropExpired(time_ns);
    }
  }
  updateModelFrames();
  syncModelSources();
}

void SceneEntitiesLayer::ensureModelStateAt(PJ::Timepoint time) {
  if (ctx_.session == nullptr) {
    if (!state_built_at_.has_value() || *state_built_at_ != time) {
      rebuildModelStateAt(time);
    }
    return;
  }
  PJ::ObjectStore& store = ctx_.session->objectStore();
  const std::int64_t time_ns = PJ::toRaw(time);
  if (snapshot_stream_) {
    // The same entry, forward: only lifetimes move. Another entry, or a step back (an entity a lifetime dropped
    // shows again): fold the shown entry alone.
    const auto shown = store.latestEntryIdAt(topic_id_, time_ns);
    // renderAt() ran first: an entry evicted under the cursor keeps its marker stamp, and its snapshot stays too,
    // also on a step back that stays at or after its stamp (there is nothing to rebuild it from).
    const bool evicted = !shown && shown_store_ns_.has_value() && time_ns >= *shown_store_ns_;
    if (state_built_at_.has_value() &&
        (evicted || (time >= *state_built_at_ && (shown ? shown->uid : PJ::SequentialUID{}) == applied_uid_high_))) {
      state_built_at_ = time;
      if (model_state_.dropExpired(time_ns)) {
        updateModelFrames();
        syncModelSources();
        emit repaintRequested();
      }
      return;
    }
    rebuildModelStateAt(time);
    emit repaintRequested();
    return;
  }
  const PJ::SequentialUID high_now = store.maxUidAtOrBefore(topic_id_, time_ns);
  if (state_built_at_.has_value() && *state_built_at_ == time && high_now == applied_uid_high_) {
    // Already built at this playhead and no batch at/before it arrived since.
    return;
  }
  // Incremental forward fold when the playhead only advanced AND nothing changed
  // at or below the previous build time (a late insert there raises
  // maxUidAtOrBefore above what was folded and forces the full rebuild below).
  const bool can_incremental = state_built_at_.has_value() && applied_uid_high_.valid() && time >= *state_built_at_;
  if (can_incremental) {
    const PJ::SequentialUID high_at_built = store.maxUidAtOrBefore(topic_id_, PJ::toRaw(*state_built_at_));
    if (high_at_built == applied_uid_high_) {
      const bool applied_new = applyModelWindow(PJ::toRaw(*state_built_at_), time_ns);
      applied_uid_high_ = high_now;
      state_built_at_ = time;
      const bool dropped = model_state_.dropExpired(time_ns);
      // The marker UID guard skips its per-tick repaint while the batch is
      // unchanged, so model-state changes request their own frame. Gated on a
      // real change: prepareForRender() also lands here.
      if (applied_new || dropped) {
        updateModelFrames();
        syncModelSources();
        emit repaintRequested();
      }
      return;
    }
  }
  rebuildModelStateAt(time);
  // No loop risk: the next ensure at this playhead takes the early return.
  emit repaintRequested();
}

void SceneEntitiesLayer::updateModelFrames() {
  QStringList frames;
  for (const auto& [_, entity] : model_state_.entities()) {
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

void SceneEntitiesLayer::syncModelSources() {
  struct Desired {
    std::string entity_id;
    std::size_t model_index = 0;
  };
  std::unordered_map<std::string, Desired> desired;
  for (const auto& [entity_id, entity] : model_state_.entities()) {
    for (std::size_t index = 0; index < entity.models.size(); ++index) {
      desired.emplace(meshKey(topic_id_, entity_id, index), Desired{.entity_id = entity_id, .model_index = index});
    }
  }
  for (auto iterator = model_loads_.begin(); iterator != model_loads_.end();) {
    if (!desired.contains(iterator->first)) {
      model_queue_->cancel(iterator->second.job);
      releaseModelMesh(iterator->first);
      iterator = model_loads_.erase(iterator);
    } else {
      ++iterator;
    }
  }
  for (const auto& [key, wanted] : desired) {
    startModelLoad(
        key, wanted.entity_id, wanted.model_index,
        model_state_.entities().at(wanted.entity_id).models[wanted.model_index]);
  }
  refreshModelDrawCalls();
  model_queue_->pump();
  updateRemoteFetchNotice();
}

void SceneEntitiesLayer::startModelLoad(
    const std::string& key, const std::string& entity_id, std::size_t model_index,
    const PJ::sdk::ModelPrimitive& primitive) {
  const std::string source_signature = sourceSignature(primitive);
  const std::string identity = source_signature.empty() ? "invalid" : source_signature;
  const auto existing = model_loads_.find(key);
  if (existing != model_loads_.end()) {
    if (existing->second.identity == identity) {
      return;  // loaded, loading, or recorded as blocked/failed
    }
    // Same key, new source bytes: blank its mesh until the reload lands; only the
    // newest source loads.
    model_queue_->cancel(existing->second.job);
  }
  releaseModelMesh(key);
  ModelLoad& load = model_loads_[key];
  load = {};
  load.identity = identity;
  load.source_label = !primitive.data.empty() ? tr("(embedded model)") : QString::fromStdString(primitive.url);
  if (source_signature.empty()) {
    load.error = tr("model has neither embedded data nor a URL");
    load.phase = ModelLoad::Phase::kFailed;
    return;
  }
  if (static_cast<std::uint64_t>(primitive.data.size()) > kSceneLimits.max_model_source_bytes) {
    load.error =
        tr("model source exceeds the %1 MiB limit").arg(kSceneLimits.max_model_source_bytes / (1024ULL * 1024ULL));
    load.phase = ModelLoad::Phase::kFailed;
    return;
  }
  if (primitive.data.empty()) {
    const QUrl url(QString::fromStdString(primitive.url));
    // Local sources (bare paths / file:// URLs) stay ungated: reading the
    // user's disk is not network egress. Fetched bytes are capped by UrlFetcher.
    const bool remote = PJ::isHttpUrl(url);
    if (remote && !remoteModelFetchAllowed()) {
      // Decided once per (key, signature), never re-checked per tick.
      load.phase = ModelLoad::Phase::kBlocked;
      qCWarning(lcSceneEntitiesLayer) << "blocked remote model fetch for" << url.toString()
                                      << "- enable pj_scene3d/allow_remote_model_fetch to allow";
      return;
    }
  }
  // Resolved when the job gets a slot, so waiting jobs hold no copy of the bytes.
  load.job = model_queue_->enqueue(
      [this, entity_id, model_index]() -> std::optional<MeshLoadQueue::Source> {
        const auto entity = model_state_.entities().find(entity_id);
        if (entity == model_state_.entities().end() || model_index >= entity->second.models.size()) {
          return std::nullopt;  // syncModelSources cancels a gone model's job; stay defensive
        }
        const PJ::sdk::ModelPrimitive& model = entity->second.models[model_index];
        MeshLoadQueue::Source source;
        source.format_hint = hintFromMediaType(model.media_type, model.url);
        if (!model.data.empty()) {
          source.kind = MeshLoadQueue::Source::Kind::kBytes;
          source.bytes =
              QByteArray(reinterpret_cast<const char*>(model.data.data()), static_cast<qsizetype>(model.data.size()));
        } else {
          source.kind = MeshLoadQueue::Source::Kind::kUrl;
          source.location = QString::fromStdString(model.url);
        }
        return source;
      },
      [this, key](MeshLoadQueue::Outcome outcome) { admitModel(key, std::move(outcome.mesh), outcome.fetch_failed); });
}

void SceneEntitiesLayer::admitModel(const std::string& key, MeshData mesh, bool fetch_failed) {
  const auto current = model_loads_.find(key);
  if (current == model_loads_.end()) {
    return;
  }
  ModelLoad& load = current->second;
  QString error;
  if (!mesh.ok) {
    error = std::move(mesh.error);
  } else {
    const std::uint64_t retained = estimateRetainedMeshBytes(mesh);
    if (!retainedMeshFits(retained, model_retained_bytes_)) {
      error = tr("model would exceed the %1 MiB layer memory limit")
                  .arg(kSceneLimits.max_model_retained_bytes_per_layer / (1024ULL * 1024ULL));
    } else {
      try {
        storeModelMesh(key, std::move(mesh), retained);
      } catch (const std::exception& exception) {
        error = tr("could not retain model: %1").arg(QString::fromUtf8(exception.what()));
      }
    }
  }
  if (error.isEmpty()) {
    load.error.clear();
    load.phase = ModelLoad::Phase::kReady;
  } else {
    qCWarning(lcSceneEntitiesLayer) << (fetch_failed ? "ModelPrimitive fetch failed for" : "model import failed for")
                                    << load.source_label << ":" << error;
    load.phase = ModelLoad::Phase::kFailed;
    load.fetch_failed = fetch_failed;
    load.error = std::move(error);
  }
  // The settle requests its own repaint, so a landed mesh shows without waiting
  // for an unrelated paint.
  updateRemoteFetchNotice();
  emit repaintRequested();
}

std::size_t SceneEntitiesLayer::modelImportsOutstandingForTest() const {
  return model_queue_->importsRunning();
}

void SceneEntitiesLayer::storeModelMesh(const std::string& key, MeshData mesh, std::uint64_t bytes) {
  releaseModelMesh(key);
  model_meshes_[key] = std::make_shared<const MeshData>(std::move(mesh));
  ++model_revision_;
  model_mesh_bytes_[key] = bytes;
  model_retained_bytes_ = saturatedAdd(model_retained_bytes_, bytes);
}

void SceneEntitiesLayer::releaseModelMesh(const std::string& key) {
  const auto found = model_mesh_bytes_.find(key);
  if (found == model_mesh_bytes_.end()) {
    return;
  }
  model_retained_bytes_ = found->second <= model_retained_bytes_ ? model_retained_bytes_ - found->second : 0U;
  model_mesh_bytes_.erase(found);
  model_meshes_.erase(key);
  ++model_revision_;
}

void SceneEntitiesLayer::updateRemoteFetchNotice() {
  int blocked = 0;
  QStringList lines;
  for (const auto& [key, load] : model_loads_) {
    if (load.phase == ModelLoad::Phase::kBlocked) {
      ++blocked;  // aggregated into one summary line
    } else if (load.phase == ModelLoad::Phase::kFailed && lines.size() < kMaxNoticeLines) {
      lines << (load.fetch_failed ? tr("Failed to fetch %1: %2") : tr("Failed to load model %1: %2"))
                   .arg(load.source_label, load.error);
    }
  }
  lines.sort();  // hash-map order would reshuffle the text on every refresh
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
  }
  updateWarning();
}

void SceneEntitiesLayer::setBatchWarning(QString warning) {
  if (batch_warning_ == warning) {
    return;
  }
  batch_warning_ = std::move(warning);
  updateWarning();
}

void SceneEntitiesLayer::updateWarning() {
  QStringList warnings;
  for (const QString* part : {&batch_warning_, &model_state_warning_, &model_draw_warning_, &remote_fetch_notice_}) {
    if (!part->isEmpty()) {
      warnings.push_back(*part);
    }
  }
  setDataWarning(warnings.join(QLatin1Char('\n')));
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

bool SceneEntitiesLayer::prepareForRender() {
  if (!visible_ || !decode_dirty_ || !requested_time_.has_value()) {
    return false;
  }
  decode_dirty_ = false;
  const std::uint64_t revision = geometry_revision_;
  renderAt(PJ::toRaw(*requested_time_));
  ensureModelStateAt(*requested_time_);
  return geometry_revision_ != revision;
}

SceneEntitiesLayer::DecodeResult SceneEntitiesLayer::expandBatch(const DecodeRequest& request) {
  DecodeResult result{.uid = request.uid, .built = {}};
  if (request.scene == nullptr) {
    result.built.error = QObject::tr("SceneEntities decode received no snapshot");
  } else {
    result.built = buildMarkerGeometry(*request.scene);
  }
  return result;
}

void SceneEntitiesLayer::onDecodeFinished(DecodeResult result) {
  // A newer batch may have been picked while this one expanded.
  if (!visible_ || result.uid != last_marker_uid_) {
    return;
  }
  if (!result.built.error.isEmpty()) {
    clearGeometry();
    setBatchWarning(std::move(result.built.error));
  } else {
    geometry_ = std::move(result.built.geometry);
    ++geometry_revision_;
    setBatchWarning({});
  }
  emit repaintRequested();
}

void SceneEntitiesLayer::invalidateDecodeState() {
  decode_worker_.invalidate();
  last_marker_uid_ = {};
}

void SceneEntitiesLayer::clearGeometry() {
  if (geometry_.empty() && geometry_.frames.empty()) {
    return;
  }
  geometry_ = {};
  ++geometry_revision_;
}

void SceneEntitiesLayer::refreshModelDrawCalls() {
  model_draw_calls_.clear();
  std::uint64_t invalid = 0;
  for (const auto& [entity_id, entity] : model_state_.entities()) {
    for (std::size_t index = 0; index < entity.models.size(); ++index) {
      const PJ::sdk::ModelPrimitive& primitive = entity.models[index];
      const auto pose = poseMatrix(primitive.pose);
      const glm::dvec3 scale{primitive.scale.x, primitive.scale.y, primitive.scale.z};
      const glm::dmat4 model = pose.has_value() ? *pose * glm::scale(glm::dmat4(1.0), scale) : glm::dmat4(1.0);
      if (!pose.has_value() || !finite(model)) {
        ++invalid;
        continue;
      }
      model_draw_calls_.push_back(
          ModelDrawCall{
              .mesh_key = meshKey(topic_id_, entity_id, index),
              .frame_id = entity.frame_id,
              .model = model,
              .override_color = toVec4(primitive.color),
              .use_material = !primitive.override_color,
          });
    }
  }
  if (model_draw_calls_.size() > kSceneLimits.max_model_draws_per_layer) {
    model_draw_calls_.clear();
    model_draw_warning_ =
        tr("SceneEntities has more than %1 live model draws").arg(kSceneLimits.max_model_draws_per_layer);
  } else if (invalid != 0U) {
    model_draw_warning_ = tr("Skipped %n model(s) with an invalid pose or scale", nullptr, static_cast<int>(invalid));
  } else {
    model_draw_warning_.clear();
  }
  ++model_revision_;
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

  // Override color: a sliding switch + swatch on one row. Picking a color
  // auto-enables it.
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

  // Wireframe (always active): draws marker primitives as edges.
  auto* wire_toggle = new PJ::ToggleSwitch(container);
  wire_toggle->setChecked(overrides_.wireframe, /*animate=*/false);
  form->addRow(tr("Wireframe:"), wire_toggle);
  QObject::connect(wire_toggle, &PJ::ToggleSwitch::toggled, this, [this](bool on) { setWireframe(on); });

  // Data / model-load warnings. Hidden while there is nothing to surface.
  auto* warning_label = new QLabel(statusWarning(), container);
  warning_label->setWordWrap(true);
  warning_label->setVisible(!statusWarning().isEmpty());
  outer->addWidget(warning_label);
  QObject::connect(this, &Scene3DLayer::statusWarningChanged, warning_label, [this, warning_label]() {
    warning_label->setText(statusWarning());
    warning_label->setVisible(!statusWarning().isEmpty());
  });

  return container;
}

}  // namespace pj::scene3d
