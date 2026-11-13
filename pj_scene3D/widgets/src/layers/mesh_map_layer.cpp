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
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

#include "layer_xml_validation.h"
#include "pj_base/builtin/mesh_map_codec.hpp"         // validateMeshMap
#include "pj_base/builtin/mesh_map_update_codec.hpp"  // validateMeshMapUpdate
#include "pj_base/time.hpp"                           // PJ::toRaw
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

// The manual colormap range the pass can store: finite, float-representable,
// ordered. The setter normalizes to this; xmlLoadState rejects what violates it.
constexpr double kManualRangeLimit = std::numeric_limits<float>::max();

bool isValidManualRange(double lo, double hi) {
  return std::isfinite(lo) && std::isfinite(hi) && std::abs(lo) <= kManualRangeLimit &&
         std::abs(hi) <= kManualRangeLimit && lo <= hi;
}

QString token(std::string_view text) {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
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
    // once one of its updates applied to this map.
    if (!topic.by_name && !topic.matched) {
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
    // The highest ARRIVAL UIDs at or before the time, as OccupancyGridLayer: a
    // late update with an older stamp must still break the repaint coalescing gate.
    const std::uint64_t update_uid = updateKey(PJ::toRaw(time));
    key ^= update_uid + 0x9e3779b97f4a7c15ULL + (key << 6U) + (key >> 2U);
  }
  // Cursor-placed (entry clock), as OccupancyGridLayer: the drawn mesh is a map
  // plus later updates, with no single stamp to place it at.
  return foldFrameTransformIntoKey(
      key, ctx_.tf_buffer.get(), fixed_frame_.toStdString(), source_frame_, std::nullopt, time, nullptr);
}

QDomElement MeshMapLayer::xmlSaveState(QDomDocument& doc) const {
  QDomElement el = doc.createElement(u"mesh_map"_s);
  el.setAttribute(u"color_by"_s, QString::fromStdString(color_request_));
  el.setAttribute(u"colormap"_s, static_cast<int>(colormap_));
  el.setAttribute(u"auto_range"_s, auto_range_ ? 1 : 0);
  el.setAttribute(u"range_lo"_s, manual_lo_);
  el.setAttribute(u"range_hi"_s, manual_hi_);
  el.setAttribute(u"lighting"_s, lighting_ ? 1 : 0);
  el.setAttribute(u"opacity"_s, opacity_);
  el.setAttribute(u"links"_s, show_links_ ? 1 : 0);
  return el;
}

bool MeshMapLayer::xmlLoadState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "mesh_map"_L1 || !detail::isLeafPayload(element)) {
    return false;
  }
  int colormap = 0;
  bool auto_range = true;
  double range_lo = 0.0;
  double range_hi = 1.0;
  bool lighting = true;
  double opacity = 1.0;
  bool links = true;
  if (!detail::parseBoundedInt(element, "colormap", 0, 0, PJ::kColormapCount - 1, colormap) ||
      !detail::parseZeroOne(element, "auto_range", true, auto_range) ||
      !detail::parseFiniteDouble(element, "range_lo", 0.0, -kManualRangeLimit, kManualRangeLimit, range_lo) ||
      !detail::parseFiniteDouble(element, "range_hi", 1.0, -kManualRangeLimit, kManualRangeLimit, range_hi) ||
      !isValidManualRange(range_lo, range_hi) || !detail::parseZeroOne(element, "lighting", true, lighting) ||
      !detail::parseFiniteDouble(element, "opacity", 1.0, 0.0, 1.0, opacity) ||
      !detail::parseZeroOne(element, "links", true, links)) {
    return false;
  }
  // The request is restored verbatim: a missing channel falls back at render
  // time and the request is honoured again once a map carrying it arrives.
  setColorBy(element.attribute(u"color_by"_s));
  setColormap(static_cast<PJ::Colormap>(colormap));
  setAutoRange(auto_range);
  setManualRange(range_lo, range_hi);
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
  update_topics_.clear();
  known_topic_count_ = 0;
  ctx_ = {};
}

void MeshMapLayer::resetStreamingState() {
  state_.reset();
  base_uid_ = {};
  base_timestamp_ = 0;
  applied_until_ = 0;
  for (UpdateTopic& topic : update_topics_) {
    topic.applied_max_uid = 0;
    topic.matched = false;
  }
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
  last_auto_range_ = {};
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
  if (!loadBase(*first)) {
    return false;
  }
  return true;
}

bool MeshMapLayer::loadBase(const PJ::ResolvedObjectEntry& entry) {
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
  if (mesh->face_count > kMaxRenderableMeshMapFaces) {
    base_warning_ = tr("Mesh map too large to display: %1 faces (limit %2)")
                        .arg(QString::number(mesh->face_count), QString::number(kMaxRenderableMeshMapFaces));
    return false;
  }
  // decodeMeshMap copies everything it needs: the object (and its anchor, which
  // may live in the plugin DSO) dies with `obj` here.
  state_ = decodeMeshMap(*mesh);
  base_warning_.clear();
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
  const auto topics = store.listTopics(descriptor.dataset_id);
  if (topics.size() == known_topic_count_) {
    return;  // the common case: nothing new in the dataset
  }
  known_topic_count_ = topics.size();
  const auto by_name = store.findTopic(descriptor.dataset_id, descriptor.topic_name + "_updates");
  std::vector<UpdateTopic> next;
  for (const PJ::ObjectTopicId candidate : topics) {
    const bool sibling = by_name.has_value() && candidate == *by_name;
    if (!sibling && builtinObjectTypeFor(store.descriptor(candidate)) != PJ::sdk::BuiltinObjectType::kMeshMapUpdate) {
      continue;
    }
    const auto known = std::find_if(
        update_topics_.begin(), update_topics_.end(),
        [candidate](const UpdateTopic& topic) { return topic.id == candidate; });
    UpdateTopic topic = known != update_topics_.end() ? *known : UpdateTopic{.id = candidate};
    topic.by_name = sibling;
    next.push_back(topic);
  }
  if (next.size() != update_topics_.size()) {
    state_.reset();  // a new update topic may hold updates inside the applied window: rebuild
  }
  update_topics_ = std::move(next);
}

std::uint64_t MeshMapLayer::updateKey(int64_t time_ns) const {
  if (ctx_.session == nullptr) {
    return 0;
  }
  const PJ::ObjectStore& store = ctx_.session->objectStore();
  std::uint64_t key = 0;
  for (const UpdateTopic& topic : update_topics_) {
    key = (key * 0x100000001b3ULL) ^ store.maxUidAtOrBefore(topic.id, time_ns).value;
  }
  return key;
}

bool MeshMapLayer::lateUpdateArrived() const {
  const PJ::ObjectStore& store = ctx_.session->objectStore();
  return std::any_of(update_topics_.begin(), update_topics_.end(), [&](const UpdateTopic& topic) {
    const auto oldest = store.oldestArrivalAfter(topic.id, PJ::SequentialUID{topic.applied_max_uid});
    return oldest.has_value() && *oldest <= applied_until_;
  });
}

int MeshMapLayer::applyUpdates(int64_t from, int64_t to) {
  if (update_topics_.empty() || !state_.has_value() || to <= from) {
    return 0;
  }
  PJ::ObjectStore& store = ctx_.session->objectStore();
  // Every topic's window, merged in time order (stable by topic order on ties).
  struct Pending {
    size_t topic = 0;
    PJ::ObjectStore::TimeRangeEntry ref;
  };
  std::vector<Pending> pending;
  for (size_t t = 0; t < update_topics_.size(); ++t) {
    for (const auto& ref : store.rangeByTime(update_topics_[t].id, from, to)) {
      pending.push_back({t, ref});
    }
  }
  std::stable_sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) {
    return a.ref.timestamp < b.ref.timestamp;
  });

  int applied = 0;
  uint32_t skipped_records = 0;
  uint32_t skipped_patches = 0;
  int refused = 0;
  QString decode_error;
  for (const Pending& item : pending) {
    UpdateTopic& topic = update_topics_[item.topic];
    auto entry = store.at(topic.id, item.ref.uid);
    if (!entry.has_value() || entry->payload.bytes.empty()) {
      continue;  // evicted between the snapshot and the resolve, or a lost re-read
    }
    const auto binding = ctx_.session->parserBindingForObjectTopic(topic.id);
    auto obj = resolveObject(binding, PJ::sdk::BuiltinObjectType::kMeshMapUpdate, entry->timestamp, entry->payload);
    const auto* update = obj.has_value() ? obj->object.get<PJ::sdk::MeshMapUpdate>() : nullptr;
    if (update == nullptr || !PJ::validateMeshMapUpdate(*update)) {
      if (topic.by_name) {  // another dataset topic's decode trouble is not this layer's
        decode_error = obj.has_value() ? tr("not a valid MeshMapUpdate") : QString::fromStdString(obj.error());
        ++refused;
      }
      continue;
    }
    // A geometry-paired topic applies only updates that name this map's geometry.
    const bool ours = topic.by_name || (!update->geometry_id.empty() && update->geometry_id == state_->geometry_id);
    if (!ours) {
      continue;
    }
    const MeshMapUpdateResult result = applyMeshMapUpdate(*state_, *update);
    if (!result.applied) {
      ++refused;
      continue;
    }
    topic.matched = true;
    skipped_records += result.skipped_records;
    skipped_patches += result.skipped_patches;
    ++applied;
  }
  QStringList warnings;
  if (refused > 0) {
    warnings
        << (decode_error.isEmpty() ? tr("%n mesh map update(s) for another geometry were skipped", nullptr, refused)
                                   : tr("%n mesh map update(s) were skipped: %1", nullptr, refused).arg(decode_error));
  }
  if (skipped_patches > 0) {
    warnings << tr(
        "%n update patch(es) did not fit the map and were skipped", nullptr,
        static_cast<int>(std::min<uint32_t>(skipped_patches, std::numeric_limits<int>::max())));
  }
  if (skipped_records > 0) {
    warnings << tr(
        "%n update record(s) named elements the map does not have", nullptr,
        static_cast<int>(std::min<uint32_t>(skipped_records, std::numeric_limits<int>::max())));
  }
  if (!warnings.isEmpty()) {
    update_warning_ = warnings.join(u"; "_s);
  }
  applied_update_count_ += applied;
  return applied;
}

void MeshMapLayer::resolveColorSource() {
  std::vector<std::string> sources =
      state_.has_value() ? meshMapColorSources(*state_) : std::vector<std::string>{std::string(kMeshMapSourceSolid)};
  const bool requested_available =
      !color_request_.empty() && std::find(sources.begin(), sources.end(), color_request_) != sources.end();
  std::string resolved;
  if (requested_available) {
    resolved = color_request_;
  } else if (state_.has_value()) {
    resolved = defaultMeshMapColorSource(*state_);
  }
  color_warning_ = !color_request_.empty() && !requested_available && state_.has_value()
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
  const std::uint64_t update_key = updateKey(time_ns);
  // Metadata-first gate: same map, same latest updates, nothing re-coloured.
  if (base_id.has_value() && base_id->uid == staged_base_uid_ && update_key == staged_update_key_ && !colors_dirty_ &&
      staged_mesh_.has_value()) {
    return;
  }
  auto base = base_id.has_value() ? store.latestAt(topic_id_, time_ns) : std::nullopt;
  if (!base.has_value() || base->payload.bytes.empty()) {
    clearStaged();
    staged_base_uid_ = {};
    setBounds(std::nullopt);
    setDataWarning(QString());
    return;
  }

  bool geometry_changed = false;
  const bool rewind = time_ns < applied_until_;
  // A late update stamped at or before what was already applied invalidates the
  // incremental state as surely as a rewind does.
  const bool late_arrival = state_.has_value() && base->sequential_uid == base_uid_ && lateUpdateArrived();
  if (!state_.has_value() || base->sequential_uid != base_uid_ || rewind || late_arrival) {
    update_warning_.clear();
    if (!loadBase(*base)) {
      state_.reset();
      base_uid_ = base->sequential_uid;
      staged_base_uid_ = base->sequential_uid;
      staged_update_key_ = update_key;
      clearStaged();
      setBounds(std::nullopt);
      setDataWarning(base_warning_);
      return;
    }
    base_uid_ = base->sequential_uid;
    base_timestamp_ = base->timestamp;
    // Updates stamped with the map itself apply too: rangeByTime's lower bound is exclusive.
    applied_until_ = base_timestamp_ - 1;
    for (UpdateTopic& topic : update_topics_) {
      topic.applied_max_uid = 0;
      topic.matched = false;
    }
    geometry_changed = true;
    colors_dirty_ = true;
    ++rebuild_count_;
    setBounds(meshMapBounds(*state_));
  }

  if (time_ns > applied_until_ && applyUpdates(applied_until_, time_ns) > 0) {
    colors_dirty_ = true;
    resolveColorSource();  // an update may add a channel
  }
  if (time_ns > applied_until_) {
    applied_until_ = time_ns;
    for (UpdateTopic& topic : update_topics_) {
      topic.applied_max_uid = store.maxUidAtOrBefore(topic.id, time_ns).value;
    }
  }

  staged_base_uid_ = base_uid_;
  staged_update_key_ = update_key;
  stage(geometry_changed);
  QStringList warnings;
  for (const QString& warning : {base_warning_, color_warning_, update_warning_}) {
    if (!warning.isEmpty()) {
      warnings << warning;
    }
  }
  setDataWarning(warnings.join(u"; "_s));
}

void MeshMapLayer::stage(bool geometry_changed) {
  if (!state_.has_value()) {
    return;
  }
  MeshMapUpload upload = staged_mesh_.has_value() ? std::move(*staged_mesh_) : MeshMapUpload{};
  if (geometry_changed || !staged_mesh_.has_value() || upload.positions.empty()) {
    fillMeshMapGeometry(*state_, upload);
    upload.geometry_revision = ++geometry_revision_;
  }
  fillMeshMapColors(*state_, resolved_color_, upload);
  if (upload.color_mode == MeshMapColorMode::kChannel) {
    last_auto_range_ = upload.color_range;
  }
  colors_dirty_ = false;
  staged_mesh_ = std::move(upload);
  ++staged_revision_;
}

void MeshMapLayer::clearStaged() {
  if (staged_mesh_.has_value()) {
    staged_mesh_.reset();
    ++staged_revision_;
  }
}

void MeshMapLayer::setFixedFrame(const QString& frame) {
  if (fixed_frame_ == frame) {
    return;
  }
  fixed_frame_ = frame;
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
  if (!bounds_.has_value() || !bounds_->valid) {
    return std::nullopt;
  }
  return bounds_;
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
  if (colormap_ == colormap) {
    return;
  }
  colormap_ = colormap;
  emit configurationChanged();
  emit repaintRequested();
}

void MeshMapLayer::setAutoRange(bool on) {
  if (auto_range_ == on) {
    return;
  }
  auto_range_ = on;
  emit configurationChanged();
  emit repaintRequested();
}

void MeshMapLayer::setManualRange(double lo, double hi) {
  if (!std::isfinite(lo) || !std::isfinite(hi)) {
    return;
  }
  lo = std::clamp(lo, -kManualRangeLimit, kManualRangeLimit);
  hi = std::clamp(hi, -kManualRangeLimit, kManualRangeLimit);
  if (lo > hi) {
    std::swap(lo, hi);
  }
  if (manual_lo_ == lo && manual_hi_ == hi) {
    return;
  }
  manual_lo_ = lo;
  manual_hi_ = hi;
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

  auto* colormap_combo = new PJ::ComboBox(container);
  colormap_combo->setObjectName(u"mesh_map_colormap"_s);
  for (int idx = 0; idx < PJ::kColormapCount; ++idx) {
    colormap_combo->addItem(tr(PJ::colormapName(static_cast<PJ::Colormap>(idx))), idx);
  }
  colormap_combo->setCurrentIndex(static_cast<int>(colormap_));
  form->addRow(tr("Colors:"), colormap_combo);
  QObject::connect(colormap_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
    setColormap(idx >= 0 && idx < PJ::kColormapCount ? static_cast<PJ::Colormap>(idx) : PJ::Colormap::kTurbo);
  });

  buildRangeRows(form, container);

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

void MeshMapLayer::buildRangeRows(QFormLayout* form, QWidget* parent) {
  auto* auto_toggle = new PJ::ToggleSwitch(parent);
  auto_toggle->setChecked(auto_range_, /*animate=*/false);
  form->addRow(tr("Auto range:"), auto_toggle);

  const auto add_scrubber = [this, form, parent](const QString& label, const QString& object_name, double value) {
    auto* spin = new PJ::DoubleScrubber(parent);
    spin->setObjectName(object_name);
    spin->setRange(-1.0e9, 1.0e9);
    spin->setDecimals(3);
    spin->setValue(value);
    spin->setEnabled(!auto_range_);
    form->addRow(label, spin);
    return spin;
  };
  auto* range_lo_spin = add_scrubber(tr("Range min:"), u"mesh_map_range_min"_s, manual_lo_);
  auto* range_hi_spin = add_scrubber(tr("Range max:"), u"mesh_map_range_max"_s, manual_hi_);

  QObject::connect(auto_toggle, &PJ::ToggleSwitch::toggled, this, [this, range_lo_spin, range_hi_spin](bool on) {
    // Switching to manual starts from the range the user was just looking at.
    if (!on && last_auto_range_.valid) {
      const QSignalBlocker block_lo(range_lo_spin);
      const QSignalBlocker block_hi(range_hi_spin);
      range_lo_spin->setValue(last_auto_range_.lo);
      range_hi_spin->setValue(last_auto_range_.hi);
      setManualRange(last_auto_range_.lo, last_auto_range_.hi);
    }
    setAutoRange(on);
    range_lo_spin->setEnabled(!on);
    range_hi_spin->setEnabled(!on);
  });
  QObject::connect(
      range_lo_spin, &PJ::DoubleScrubber::valueChanged, this, [this, range_lo_spin, range_hi_spin](double v) {
        if (v > range_hi_spin->value()) {
          const QSignalBlocker block(range_hi_spin);
          range_hi_spin->setValue(v);
        }
        setManualRange(range_lo_spin->value(), range_hi_spin->value());
      });
  QObject::connect(
      range_hi_spin, &PJ::DoubleScrubber::valueChanged, this, [this, range_lo_spin, range_hi_spin](double v) {
        if (v < range_lo_spin->value()) {
          const QSignalBlocker block(range_lo_spin);
          range_lo_spin->setValue(v);
        }
        setManualRange(range_lo_spin->value(), range_hi_spin->value());
      });
}

}  // namespace pj::scene3d
