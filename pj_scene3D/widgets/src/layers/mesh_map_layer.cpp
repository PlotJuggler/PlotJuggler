// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_widgets/layers/mesh_map_layer.h"

#include <QDomElement>
#include <QFormLayout>
#include <QLoggingCategory>
#include <QPointer>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

#include "layer_xml_validation.h"
#include "pj_base/builtin/mesh_map_codec.hpp"         // validateMeshMap
#include "pj_base/builtin/mesh_map_update_codec.hpp"  // validateMeshMapUpdate
#include "pj_base/time.hpp"                           // PJ::toRaw
#include "pj_scene3d_core/pose_math.h"                // finitePose
#include "pj_scene3d_core/tf/object_placement.h"
#include "pj_scene3d_widgets/object_topic_metadata.h"  // builtinObjectTypeFor
#include "pj_scene3d_widgets/resolve_object.h"
#include "pj_tf/tf_buffer.h"
#include "pj_widgets/ComboBox.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/ToggleSwitch.h"
using namespace Qt::StringLiterals;

namespace pj::scene3d {

namespace {
Q_LOGGING_CATEGORY(lcMeshMap, "pj.scene3d.mesh_map")

// The MeshMapUpdate `resolved` (a resolveObject() result) holds, when it decoded
// and passes validation; nullptr otherwise. Valid while `resolved` lives.
const PJ::sdk::MeshMapUpdate* validUpdate(const auto& resolved) {
  const auto* update = resolved.has_value() ? resolved->object.template get<PJ::sdk::MeshMapUpdate>() : nullptr;
  return update != nullptr && PJ::validateMeshMapUpdate(*update) ? update : nullptr;
}

}  // namespace

MeshMapLayer::MeshMapLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent)
    : Scene3DLayer(parent), topic_id_(topic_id), display_name_(std::move(display_name)) {}

MeshMapLayer::~MeshMapLayer() = default;

PJ::SceneLayerInfo MeshMapLayer::info() const {
  return PJ::SceneLayerInfo{
      .topic_id = topic_id_,
      .object_type = PJ::sdk::BuiltinObjectType::kMeshMap,
      .display_name = display_name_,
      .family_name = u"MeshMap"_s,
      .visible = visible_,
  };
}

PJ::Range<PJ::Timepoint> MeshMapLayer::timeRange() const {
  const auto* store = ctx_.session != nullptr ? &ctx_.session->objectStore() : nullptr;
  PJ::Range<PJ::Timepoint> range = PJ::liveTopicTimeRange(store, topic_id_);
  for (const UpdateTopic& topic : update_topics_) {
    // Like OccupancyGridLayer: live updates routinely extend past the last full
    // map, so union their range in, or the scrub clamp would pin the tracker at
    // the last map while updates keep arriving. A geometry-paired topic joins
    // once one of its updates applied, and stays joined across later maps.
    if (!reconstructor_.paired(topic.id.id)) {
      continue;
    }
    const auto updates = PJ::liveTopicTimeRange(store, topic.id);
    range.min = std::min(range.min, updates.min);
    range.max = std::max(range.max, updates.max);
  }
  return range;
}

QStringList MeshMapLayer::fallbackFrames() const {
  if (source_frame_.empty()) {
    return {};
  }
  return {QString::fromStdString(source_frame_)};
}

QString MeshMapLayer::sourceFrame() const {
  return QString::fromStdString(source_frame_);
}

uint64_t MeshMapLayer::renderKey(PJ::Timepoint time) const {
  const PJ::ObjectStore* store = ctx_.session != nullptr ? &ctx_.session->objectStore() : nullptr;
  uint64_t key = 0x51ed2701f3a5c9b7ULL ^ PJ::latestEntryRenderKey(store, topic_id_, time);
  if (store != nullptr && !update_topics_.empty()) {
    // The set of updates at or before the time (updateKey()): a late update with
    // an older stamp must still break the repaint coalescing gate.
    const std::uint64_t update_uid = updateKey(PJ::toRaw(time));
    key ^= update_uid + 0x9e3779b97f4a7c15ULL + (key << 6U) + (key >> 2U);
  }
  // Cursor-placed (entry clock), as OccupancyGridLayer: the drawn mesh is a map
  // plus later updates, with no single stamp to place it at.
  return foldFrameTransformIntoKey(key, ctx_.tf_buffer.get(), fixed_frame_, source_frame_, std::nullopt, time, nullptr);
}

QDomElement MeshMapLayer::xmlSaveState(QDomDocument& doc) const {
  QDomElement el = doc.createElement(u"mesh_map"_s);
  el.setAttribute(u"color_by"_s, QString::fromStdString(color_request_));
  coloring_.saveXml(el);
  el.setAttribute(u"lighting"_s, lighting_ ? 1 : 0);
  el.setAttribute(u"opacity"_s, opacity_);
  el.setAttribute(u"links"_s, show_links_ ? 1 : 0);
  return el;
}

bool MeshMapLayer::xmlLoadState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "mesh_map"_L1 || !detail::isLeafPayload(element)) {
    return false;
  }
  const auto coloring = ChannelColoring::parseXml(element);
  bool lighting = true;
  double opacity = 1.0;
  bool links = true;
  if (!coloring.has_value() || !detail::parseZeroOne(element, "lighting", true, lighting) ||
      !detail::parseFiniteDouble(element, "opacity", 1.0, 0.0, 1.0, opacity) ||
      !detail::parseZeroOne(element, "links", true, links)) {
    return false;
  }
  // The request is restored verbatim: a missing channel falls back at render
  // time and the request is honoured again once a map carrying it arrives.
  setColorBy(element.attribute(u"color_by"_s));
  setColormap(coloring->colormap());
  setAutoRange(coloring->autoRange());
  setManualRange(coloring->manualRange().first, coloring->manualRange().second);
  setLighting(lighting);
  setOpacity(opacity);
  setShowLinks(links);
  return true;
}

bool MeshMapLayer::attach(const PJ::SceneLayerContext& ctx) {
  const auto& scene3d_ctx = static_cast<const Scene3DLayerContext&>(ctx);
  if (scene3d_ctx.session == nullptr) {
    qCWarning(lcMeshMap) << "attach: session is null";
    return false;
  }
  if (!scene3d_ctx.session->parserBindingForObjectTopic(topic_id_) &&
      !hasCanonical3DCodec(PJ::sdk::BuiltinObjectType::kMeshMap)) {
    qCWarning(lcMeshMap) << "attach: no parser and no canonical codec for topic" << topic_id_.id;
    return false;
  }
  ctx_ = scene3d_ctx;
  resetStreamingState();
  refreshUpdateTopics();
  disconnect(reload_connection_);
  reload_connection_ = connect(
      ctx_.session, &PJ::SessionManager::datasetAboutToBeReplaced, this, &MeshMapLayer::onDatasetAboutToBeReplaced);
  // Streaming-tolerant: the topic may have no sample yet; renderAt self-heals.
  if (!bootstrap()) {
    qCWarning(lcMeshMap) << "attach: bootstrap deferred for mesh-map topic" << topic_id_.id;
  }
  return true;
}

void MeshMapLayer::detach() {
  disconnect(reload_connection_);
  reload_connection_ = {};
  clearStaged();
  resetStreamingState();
  ctx_ = {};
}

void MeshMapLayer::resetStreamingState() {
  // The pairings go only with the topic's history: a new dataset, a new attach.
  // The streams themselves are listed again by the next refreshUpdateTopics().
  reconstructor_.reset();
  update_topics_.clear();
  known_topics_.clear();
  staged_update_key_ = 0;
  staged_base_uid_ = {};
  colors_dirty_ = true;
  bounds_.reset();
  source_frame_.clear();
  color_sources_.clear();
  resolved_color_.clear();
  update_warning_.clear();
  base_warning_.clear();
  color_warning_.clear();
  coloring_.resetAutoRange();
  setDataWarning(QString());
}

void MeshMapLayer::onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id) {
  if (ctx_.session == nullptr || ctx_.session->objectStore().descriptor(topic_id_).dataset_id != dataset_id) {
    return;
  }
  // The topic id survives the swap but every UID is minted fresh: a stale memo
  // must never serve the old bytes.
  clearStaged();
  resetStreamingState();
  tracker_dirty_ = true;
  emit repaintRequested();
}

bool MeshMapLayer::bootstrap() {
  PJ::ObjectStore& store = ctx_.session->objectStore();
  auto first = store.at(topic_id_, size_t{0});
  if (!first.has_value() || first->payload.bytes.empty()) {
    return false;
  }
  return rebase(*first);
}

bool MeshMapLayer::rebase(const PJ::ResolvedObjectEntry& entry) {
  ++rebase_count_;
  const auto binding = ctx_.session->parserBindingForObjectTopic(topic_id_);
  auto obj = resolveObject(binding, PJ::sdk::BuiltinObjectType::kMeshMap, entry.timestamp, entry.payload);
  if (!obj.has_value()) {
    base_warning_ = tr("Mesh map decode failed: %1").arg(QString::fromStdString(obj.error()));
    return false;
  }
  const auto* mesh = obj->object.get<PJ::sdk::MeshMap>();
  if (mesh == nullptr) {
    base_warning_ = tr("Mesh map decode returned an object of another type");
    return false;
  }
  // Wire layouts are untrusted until validated, whichever route decoded them.
  if (const auto valid = PJ::validateMeshMap(*mesh); !valid) {
    base_warning_ = tr("Invalid mesh map: %1").arg(QString::fromStdString(valid.error()));
    return false;
  }
  if (!finitePose(mesh->origin)) {
    base_warning_ = tr("Mesh map has a non-finite origin pose");
    return false;
  }
  if (mesh->face_count > kMaxRenderableMeshMapFaces) {
    base_warning_ = tr("Mesh map too large to display: %1 faces (limit %2)")
                        .arg(QString::number(mesh->face_count), QString::number(kMaxRenderableMeshMapFaces));
    return false;
  }
  // decodeMeshMap copies everything it needs: the object (and its anchor, which
  // may live in the plugin DSO) dies with `obj` here.
  reconstructor_.rebase(entry.sequential_uid.value, entry.timestamp, decodeMeshMap(*mesh));
  base_warning_.clear();
  // What is stamped before the map is never applied to it: only a later arrival
  // can be late.
  for (UpdateTopic& topic : update_topics_) {
    topic.applied_max_uid = ctx_.session->objectStore().maxUidAtOrBefore(topic.id, reconstructor_.appliedUntil()).value;
  }
  colors_dirty_ = true;
  setBounds(meshMapBounds(*reconstructor_.state()));
  if (mesh->frame_id != source_frame_) {
    source_frame_ = mesh->frame_id;
    emit sourceFrameChanged(QString::fromStdString(source_frame_));
    emit fallbackFramesChanged(fallbackFrames());
  }
  resolveColorSource();
  return true;
}

void MeshMapLayer::refreshUpdateTopics() {
  if (ctx_.session == nullptr) {
    return;
  }
  PJ::ObjectStore& store = ctx_.session->objectStore();
  const auto& descriptor = store.descriptor(topic_id_);
  auto topics = store.listTopics(descriptor.dataset_id);
  if (topics == known_topics_) {
    return;  // the common case: nothing new in the dataset
  }
  known_topics_ = std::move(topics);
  const auto sibling = store.findTopic(descriptor.dataset_id, descriptor.topic_name + "_updates");
  std::vector<UpdateTopic> next;
  std::vector<MeshMapReconstructor::Stream> streams;
  for (const PJ::ObjectTopicId candidate : known_topics_) {
    const bool is_sibling = sibling.has_value() && candidate == *sibling;
    if (!is_sibling &&
        builtinObjectTypeFor(store.descriptor(candidate)) != PJ::sdk::BuiltinObjectType::kMeshMapUpdate) {
      continue;
    }
    const auto known = std::find_if(
        update_topics_.begin(), update_topics_.end(),
        [candidate](const UpdateTopic& topic) { return topic.id == candidate; });
    next.push_back(known != update_topics_.end() ? *known : UpdateTopic{.id = candidate});
    streams.push_back({.key = candidate.id, .sibling = is_sibling});
  }
  update_topics_ = std::move(next);
  reconstructor_.setStreams(streams);  // a changed set rebuilds from the map
}

std::uint64_t MeshMapLayer::updateKey(int64_t time_ns, std::vector<std::uint64_t>* max_uids) const {
  if (ctx_.session == nullptr) {
    return 0;
  }
  const PJ::ObjectStore& store = ctx_.session->objectStore();
  // The highest arrival UID alone does not identify the set of updates at or
  // before the time when arrival and stamp order differ (update@300, then
  // update@200: the same maximum at 250 and at 350); the latest-by-stamp entry
  // tells those prefixes apart.
  std::uint64_t key = 0;
  for (const UpdateTopic& topic : update_topics_) {
    const auto latest = store.latestEntryIdAt(topic.id, time_ns);
    const std::uint64_t max_uid = store.maxUidAtOrBefore(topic.id, time_ns).value;
    key = (key * 0x100000001b3ULL) ^ max_uid;
    key = (key * 0x100000001b3ULL) ^ (latest.has_value() ? latest->uid.value : 0);
    if (max_uids != nullptr) {
      max_uids->push_back(max_uid);
    }
  }
  return key;
}

bool MeshMapLayer::lateUpdateArrived() {
  PJ::ObjectStore& store = ctx_.session->objectStore();
  const PJ::Timestamp applied_until = reconstructor_.appliedUntil();
  for (UpdateTopic& topic : update_topics_) {
    const auto oldest = store.oldestArrivalAfter(topic.id, PJ::SequentialUID{topic.applied_max_uid});
    if (!oldest.has_value() || *oldest > applied_until) {
      continue;
    }
    if (reconstructor_.paired(topic.id.id)) {
      return true;
    }
    // A topic not paired yet: rebuild only for a late update the map claims, so
    // another mesh's late updates leave this layer alone. Its first matching
    // update is routinely late on a live source. What predates the map never
    // applies, so only the window since it is examined.
    const PJ::SequentialUID cursor = store.maxUidAtOrBefore(topic.id, applied_until);
    const auto binding = ctx_.session->parserBindingForObjectTopic(topic.id);
    for (const auto& ref : store.rangeByTime(topic.id, reconstructor_.appliedFrom(), applied_until)) {
      if (ref.uid.value <= topic.applied_max_uid || !reconstructor_.wants(topic.id.id, ref.uid.value)) {
        continue;
      }
      auto entry = store.at(topic.id, ref.uid);
      if (!entry.has_value() || entry->payload.bytes.empty()) {
        continue;
      }
      const auto obj =
          resolveObject(binding, PJ::sdk::BuiltinObjectType::kMeshMapUpdate, entry->timestamp, entry->payload);
      const auto* update = validUpdate(obj);
      if (update != nullptr && reconstructor_.claims(topic.id.id, ref.uid.value, *update)) {
        return true;
      }
    }
    // None was ours: they are not examined again.
    topic.applied_max_uid = std::max(topic.applied_max_uid, cursor.value);
  }
  return false;
}

void MeshMapLayer::feedUpdates(PJ::Timestamp lo, PJ::Timestamp hi, MeshMapReconstructor::Sink& sink) {
  PJ::ObjectStore& store = ctx_.session->objectStore();
  // Every topic's window, merged in time order (stable by topic order on ties).
  struct Pending {
    PJ::ObjectTopicId topic;
    PJ::ObjectStore::TimeRangeEntry ref;
  };
  std::vector<Pending> pending;
  for (const UpdateTopic& topic : update_topics_) {
    for (const auto& ref : store.rangeByTime(topic.id, lo, hi)) {
      pending.push_back({topic.id, ref});
    }
  }
  std::stable_sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) {
    return a.ref.timestamp < b.ref.timestamp;
  });
  for (const Pending& item : pending) {
    const std::uint64_t stream = item.topic.id;
    const std::uint64_t identity = item.ref.uid.value;
    if (!sink.wants(stream, identity)) {
      continue;  // another mesh's update, known as such: not fetched again
    }
    auto entry = store.at(item.topic, item.ref.uid);
    if (entry.has_value() && entry->fetch_failed) {
      // Permanent: the applied cursor moves past it, so playback never re-reads it.
      sink.lost(stream);
      continue;
    }
    if (!entry.has_value() || entry->payload.bytes.empty()) {
      continue;  // evicted between the snapshot and the resolve, or a lost re-read
    }
    const auto binding = ctx_.session->parserBindingForObjectTopic(item.topic);
    const auto obj =
        resolveObject(binding, PJ::sdk::BuiltinObjectType::kMeshMapUpdate, entry->timestamp, entry->payload);
    const auto* update = validUpdate(obj);
    if (update == nullptr) {
      sink.invalid(stream, identity, obj.has_value() ? tr("not a valid MeshMapUpdate").toStdString() : obj.error());
      continue;
    }
    sink.apply(stream, identity, *update);
  }
}

void MeshMapLayer::noteReplay(const MeshMapReplay& replay) {
  QStringList warnings;
  if (replay.lost > 0) {
    warnings << tr("%n mesh map update(s) lost: source re-read failed", nullptr, replay.lost);
  }
  if (replay.refused > 0) {
    warnings
        << (replay.decode_error.empty()
                ? tr("%n mesh map update(s) for another geometry were skipped", nullptr, replay.refused)
                : tr("%n mesh map update(s) were skipped: %1", nullptr, replay.refused)
                      .arg(QString::fromStdString(replay.decode_error)));
  }
  if (replay.skipped_patches > 0) {
    warnings << tr(
        "%n update patch(es) did not fit the map and were skipped", nullptr,
        static_cast<int>(std::min<uint32_t>(replay.skipped_patches, std::numeric_limits<int>::max())));
  }
  if (replay.skipped_indices > 0) {
    warnings << tr(
        "%n sparse update index(es) named elements the map does not have", nullptr,
        static_cast<int>(std::min<uint32_t>(replay.skipped_indices, std::numeric_limits<int>::max())));
  }
  if (!warnings.isEmpty()) {
    update_warning_ = warnings.join(u"; "_s);
  }
  applied_update_count_ += replay.applied;
}

void MeshMapLayer::resolveColorSource() {
  const MeshMapState* state = reconstructor_.state();
  std::vector<std::string> sources =
      state != nullptr ? meshMapColorSources(*state) : std::vector<std::string>{std::string(kMeshMapSourceSolid)};
  const bool requested_available =
      !color_request_.empty() && std::find(sources.begin(), sources.end(), color_request_) != sources.end();
  std::string resolved;
  if (requested_available) {
    resolved = color_request_;
  } else if (state != nullptr) {
    resolved = defaultMeshMapColorSource(*state);
  }
  color_warning_ = !color_request_.empty() && !requested_available && state != nullptr
                       ? tr("Color source '%1' not found").arg(QString::fromStdString(color_request_))
                       : QString();
  if (sources != color_sources_ || resolved != resolved_color_) {
    color_sources_ = std::move(sources);
    resolved_color_ = std::move(resolved);
    colors_dirty_ = true;
    emit colorSourcesChanged();
  }
}

void MeshMapLayer::renderAt(int64_t time_ns) {
  if (ctx_.session == nullptr) {
    return;
  }
  refreshUpdateTopics();
  PJ::ObjectStore& store = ctx_.session->objectStore();
  const auto base_id = store.latestEntryIdAt(topic_id_, time_ns);
  // Per topic, the newest arrival at or before the time: the applied cursors of
  // the replay below. Taken before the updates are read (as OccupancyGridLayer):
  // an entry that lands meanwhile keeps a UID above its cursor and is examined
  // next tick, never counted as applied without having been.
  std::vector<std::uint64_t> cursors;
  cursors.reserve(update_topics_.size());
  const std::uint64_t update_key = updateKey(time_ns, &cursors);
  const bool same_base = base_id.has_value() && base_id->uid == staged_base_uid_;
  // Metadata-first gate: the same map, and either it was refused (no mesh; its
  // warning stays up, and decoding it again would only refuse it again) or the
  // same updates with nothing to re-colour.
  if (same_base && (reconstructor_.state() == nullptr ||
                    (update_key == staged_update_key_ && !colors_dirty_ && staged_mesh_.has_value()))) {
    return;
  }
  const auto nothing_to_show = [this] {
    clearStaged();
    staged_base_uid_ = {};
    setBounds(std::nullopt);
    setDataWarning(QString());
  };
  if (!base_id.has_value()) {
    nothing_to_show();
    return;
  }

  // The staged positions stay valid while the map entry is the same one.
  const bool geometry_changed = !staged_mesh_.has_value() || !same_base;
  // A late update stamped at or before what was already applied invalidates the
  // incremental state as surely as a rewind does.
  if (reconstructor_.needsBase(base_id->uid.value, time_ns) || lateUpdateArrived()) {
    auto base = store.at(topic_id_, base_id->uid);
    if (!base.has_value() || base->payload.bytes.empty()) {
      nothing_to_show();  // evicted since the lookup, or a lost re-read: the next move tries again
      return;
    }
    update_warning_.clear();
    if (!rebase(*base)) {
      reconstructor_.clear();
      nothing_to_show();
      staged_base_uid_ = base_id->uid;  // with no mesh: the gate's "refused"
      setDataWarning(base_warning_);
      return;
    }
  }

  bool values_changed = false;
  if (time_ns > reconstructor_.appliedUntil()) {
    const MeshMapReplay replay = reconstructor_.advance(
        time_ns,
        [this](PJ::Timestamp lo, PJ::Timestamp hi, MeshMapReconstructor::Sink& sink) { feedUpdates(lo, hi, sink); });
    noteReplay(replay);
    values_changed = replay.applied > 0;
    if (values_changed) {
      resolveColorSource();  // an update may add a channel
    }
    for (size_t i = 0; i < update_topics_.size(); ++i) {
      update_topics_[i].applied_max_uid = cursors[i];
    }
  }

  staged_base_uid_ = base_id->uid;
  staged_update_key_ = update_key;
  stage(geometry_changed, values_changed);
  QStringList warnings;
  for (const QString& warning : {base_warning_, color_warning_, update_warning_}) {
    if (!warning.isEmpty()) {
      warnings << warning;
    }
  }
  setDataWarning(warnings.join(u"; "_s));
}

void MeshMapLayer::stage(bool geometry_changed, bool values_changed) {
  const MeshMapState* state = reconstructor_.state();
  if (state == nullptr) {
    return;
  }
  // The channels' dirty spans are what the last replay wrote on top of the
  // staged colours: with new positions, another source or a rebuild from the
  // map, colour whole instead.
  std::optional<ElementRange> delta;
  if (!geometry_changed && !colors_dirty_) {
    if (!values_changed) {
      return;
    }
    delta = refreshMeshMapColors(*state, resolved_color_, *staged_mesh_);
    if (delta.has_value() && delta->empty()) {
      return;  // the updates wrote other channels: the staged mesh is current
    }
  }
  MeshMapUpload upload = staged_mesh_.has_value() ? std::move(*staged_mesh_) : MeshMapUpload{};
  if (geometry_changed) {
    fillMeshMapGeometry(*state, upload);
    upload.geometry_revision = ++geometry_revision_;
  }
  if (!delta.has_value()) {
    fillMeshMapColors(*state, resolved_color_, upload);
  }
  if (upload.color_mode == MeshMapColorMode::kChannel) {
    coloring_.noteAutoRange(upload.color_range);
  }
  colors_dirty_ = false;
  staged_mesh_ = std::move(upload);
  staged_color_delta_ = delta;
  ++staged_revision_;
}

void MeshMapLayer::clearStaged() {
  if (staged_mesh_.has_value()) {
    staged_mesh_.reset();
    staged_color_delta_.reset();
    ++staged_revision_;
  }
}

void MeshMapLayer::setFixedFrame(const QString& frame) {
  std::string next = frame.toStdString();
  if (fixed_frame_ == next) {
    return;
  }
  fixed_frame_ = std::move(next);
  emit repaintRequested();
}

void MeshMapLayer::setBounds(std::optional<AABB> bounds) {
  if (bounds.has_value() && !bounds->valid) {
    bounds.reset();
  }
  if (bounds == bounds_) {
    return;
  }
  bounds_ = bounds;
  emit repaintRequested();  // re-fits the camera via Scene3DDockWidget::updateSceneBounds
}

void MeshMapLayer::setTrackerTime(PJ::Timepoint time) {
  requested_time_ = time;
  tracker_dirty_ = true;
  emit repaintRequested();
}

void MeshMapLayer::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  emit visibilityChanged(visible);
  emit repaintRequested();
}

void MeshMapLayer::prepareForRender() {
  if (!visible_ || !tracker_dirty_ || !requested_time_.has_value()) {
    return;
  }
  tracker_dirty_ = false;
  renderAt(PJ::toRaw(*requested_time_));
}

std::optional<AABB> MeshMapLayer::worldBounds() const {
  return bounds_;  // setBounds() keeps only valid boxes
}

void MeshMapLayer::setColorBy(const QString& source) {
  const std::string next = source.toStdString();
  if (color_request_ == next) {
    return;
  }
  color_request_ = next;
  resolveColorSource();
  colors_dirty_ = true;
  tracker_dirty_ = true;
  emit configurationChanged();
  emit repaintRequested();
}

void MeshMapLayer::setColormap(PJ::Colormap colormap) {
  if (coloring_.setColormap(colormap)) {
    coloringChanged();
  }
}

void MeshMapLayer::setAutoRange(bool on) {
  if (coloring_.setAutoRange(on)) {
    coloringChanged();
  }
}

void MeshMapLayer::setManualRange(double lo, double hi) {
  if (coloring_.setManualRange(lo, hi)) {
    coloringChanged();
  }
}

void MeshMapLayer::coloringChanged() {
  emit configurationChanged();
  emit repaintRequested();
}

void MeshMapLayer::setLighting(bool on) {
  if (lighting_ == on) {
    return;
  }
  lighting_ = on;
  emit configurationChanged();
  emit repaintRequested();
}

void MeshMapLayer::setOpacity(double opacity) {
  const double clamped = std::clamp(opacity, 0.0, 1.0);
  if (opacity_ == clamped) {
    return;
  }
  opacity_ = clamped;
  emit configurationChanged();
  emit repaintRequested();
}

void MeshMapLayer::setShowLinks(bool on) {
  if (show_links_ == on) {
    return;
  }
  show_links_ = on;
  emit configurationChanged();
  emit repaintRequested();
}

QWidget* MeshMapLayer::createConfigWidget(QWidget* parent) {
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

  // The selector lists the last decoded map's channels plus the two tokens; the
  // current item is the EFFECTIVE pick. It rebuilds (silently: not a user edit)
  // whenever a decoded map or an update changes either.
  auto* color_combo = new PJ::ComboBox(container);
  color_combo->setObjectName(u"mesh_map_color_by"_s);
  const auto rebuild = [this, color_combo]() {
    const QSignalBlocker block(color_combo);
    color_combo->clear();
    for (const std::string& source : color_sources_) {
      // "face:cost" reads as "cost", "vertex:cost" as "cost (vertex)".
      QString label = QString::fromStdString(source);
      if (source == kMeshMapSourceVertexColor) {
        label = tr("Vertex color");
      } else if (source == kMeshMapSourceSolid) {
        label = tr("Solid");
      } else if (label.startsWith(u"face:"_s)) {
        label = label.mid(5);
      } else if (label.startsWith(u"vertex:"_s)) {
        label = tr("%1 (vertex)").arg(label.mid(7));
      }
      color_combo->addItem(label, QString::fromStdString(source));
    }
    color_combo->setCurrentIndex(std::max(0, color_combo->findData(QString::fromStdString(resolved_color_))));
    color_combo->setEnabled(color_combo->count() > 0);
  };
  rebuild();
  form->addRow(tr("Color by:"), color_combo);
  QObject::connect(color_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, color_combo](int) {
    setColorBy(color_combo->currentData().toString());
  });
  QPointer<PJ::ComboBox> safe_combo(color_combo);
  QObject::connect(this, &MeshMapLayer::colorSourcesChanged, container, [safe_combo, rebuild]() {
    if (safe_combo) {
      rebuild();
    }
  });

  coloring_.addConfigRows(form, container, this, u"mesh_map"_s, [this] { coloringChanged(); });

  auto* lighting_toggle = new PJ::ToggleSwitch(container);
  lighting_toggle->setChecked(lighting_, /*animate=*/false);
  form->addRow(tr("Lighting:"), lighting_toggle);
  QObject::connect(lighting_toggle, &PJ::ToggleSwitch::toggled, this, [this](bool on) { setLighting(on); });

  auto* links_toggle = new PJ::ToggleSwitch(container);
  links_toggle->setObjectName(u"mesh_map_links"_s);
  links_toggle->setChecked(show_links_, /*animate=*/false);
  form->addRow(tr("Links:"), links_toggle);
  QObject::connect(links_toggle, &PJ::ToggleSwitch::toggled, this, [this](bool on) { setShowLinks(on); });

  auto* opacity_spin = new PJ::DoubleScrubber(container);
  opacity_spin->setRange(0.0, 1.0);
  opacity_spin->setDecimals(2);
  opacity_spin->setSingleStep(0.05);
  opacity_spin->setValue(opacity_);
  form->addRow(tr("Opacity:"), opacity_spin);
  QObject::connect(opacity_spin, &PJ::DoubleScrubber::valueChanged, this, [this](double v) { setOpacity(v); });

  return container;
}

}  // namespace pj::scene3d
