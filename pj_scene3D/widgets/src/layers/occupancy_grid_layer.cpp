// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_widgets/layers/occupancy_grid_layer.h"

#include <QComboBox>
#include <QDomElement>
#include <QFormLayout>
#include <QLoggingCategory>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <any>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "layer_xml_validation.h"
#include "pj_base/builtin/occupancy_grid.hpp"
#include "pj_base/builtin/occupancy_grid_update.hpp"
#include "pj_base/time.hpp"  // PJ::fromRaw, PJ::toRaw
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/camera/camera.h"  // AABB, occupancyGridBounds
#include "pj_scene3d_core/occupancy_grid_budget.h"
#include "pj_scene3d_core/pose_math.h"  // finitePose
#include "pj_scene3d_widgets/resolve_object.h"
#include "pj_widgets/ComboBox.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/FrameworkTokens.h"
using namespace Qt::StringLiterals;

namespace pj::scene3d {

namespace {
Q_LOGGING_CATEGORY(lcOccGrid, "pj.scene3d.occupancy_grid")
}  // namespace

OccupancyGridLayer::OccupancyGridLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent)
    : Scene3DLayer(parent), topic_id_(topic_id), display_name_(std::move(display_name)) {}

OccupancyGridLayer::~OccupancyGridLayer() = default;

PJ::SceneLayerInfo OccupancyGridLayer::info() const {
  return PJ::SceneLayerInfo{
      .topic_id = topic_id_,
      .object_type = PJ::sdk::BuiltinObjectType::kOccupancyGrid,
      .display_name = display_name_,
      .family_name = u"OccupancyGrid"_s,
      .visible = visible_,
  };
}

PJ::Range<PJ::Timepoint> OccupancyGridLayer::timeRange() const {
  const auto* store = ctx_.session != nullptr ? &ctx_.session->objectStore() : nullptr;
  PJ::Range<PJ::Timepoint> range = PJ::liveTopicTimeRange(store, topic_id_);
  if (updates_topic_.has_value()) {
    // This is the one two-topic layer: in live mapping the OccupancyGridUpdate
    // sibling routinely extends past the last full keyframe, so union its range
    // in. Without it timeRange().max stops at the base topic, the dock's scrub
    // clamp pins the tracker short of the newest patch, and the grid freezes at
    // the last keyframe while updates keep arriving. Inverted-empty ranges from
    // liveTopicTimeRange() fold away cleanly under min/max.
    const auto updates = PJ::liveTopicTimeRange(store, *updates_topic_);
    range.min = std::min(range.min, updates.min);
    range.max = std::max(range.max, updates.max);
  }
  return range;
}

QStringList OccupancyGridLayer::fallbackFrames() const {
  if (source_frame_.empty()) {
    return {};
  }
  return {QString::fromStdString(source_frame_)};
}

QString OccupancyGridLayer::sourceFrame() const {
  return QString::fromStdString(source_frame_);
}

QDomElement OccupancyGridLayer::xmlSaveState(QDomDocument& doc) const {
  QDomElement el = doc.createElement(u"occupancy_grid"_s);
  el.setAttribute(u"color_scheme"_s, color_scheme_ == OccupancyColorScheme::kCostmap ? u"costmap"_s : u"map"_s);
  el.setAttribute(u"opacity"_s, static_cast<double>(opacity_));
  return el;
}

std::optional<OccupancyGridLayer::ParsedSettings> OccupancyGridLayer::parseSettings(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "occupancy_grid"_L1 || !detail::isLeafPayload(element)) {
    return std::nullopt;
  }
  ParsedSettings settings;
  const QString scheme_text = element.attribute(u"color_scheme"_s, u"map"_s);
  if (scheme_text == "map"_L1) {
    settings.color_scheme = OccupancyColorScheme::kMap;
  } else if (scheme_text == "costmap"_L1) {
    settings.color_scheme = OccupancyColorScheme::kCostmap;
  } else {
    return std::nullopt;
  }
  if (!detail::parseFiniteFloat(element, "opacity", 0.7f, 0.0f, 1.0f, settings.opacity)) {
    return std::nullopt;
  }
  return settings;
}

bool OccupancyGridLayer::validateXml(const QDomElement& element) {
  return parseSettings(element).has_value();
}

bool OccupancyGridLayer::xmlLoadState(const QDomElement& element) {
  const auto settings = parseSettings(element);
  if (!settings.has_value()) {
    return false;
  }
  setColorScheme(settings->color_scheme);
  setOpacity(settings->opacity);
  return true;
}

bool OccupancyGridLayer::attach(const PJ::SceneLayerContext& ctx) {
  const auto& scene3d_ctx = static_cast<const Scene3DLayerContext&>(ctx);
  if (scene3d_ctx.session == nullptr) {
    qCWarning(lcOccGrid) << "attach: session is null";
    return false;
  }
  // A parser-backed topic, or a parser-less canonical one (resolveObject()).
  if (!scene3d_ctx.session->parserBindingForObjectTopic(topic_id_) &&
      !hasCanonical3DCodec(PJ::sdk::BuiltinObjectType::kOccupancyGrid)) {
    qCWarning(lcOccGrid) << "attach: no parser or canonical codec for occupancy-grid topic" << topic_id_.id;
    return false;
  }
  ctx_ = scene3d_ctx;

  // Discover the paired "<base>_updates" sibling in the same dataset (RViz
  // convention). Absent → base-only mode (each full grid is a keyframe).
  PJ::ObjectStore& store = scene3d_ctx.session->objectStore();
  const auto& desc = store.descriptor(topic_id_);
  updates_topic_ = store.findTopic(desc.dataset_id, desc.topic_name + "_updates");

  resetStreamingState();
  disconnect(reload_connection_);
  reload_connection_ = connect(
      ctx_.session, &PJ::SessionManager::datasetAboutToBeReplaced, this,
      &OccupancyGridLayer::onDatasetAboutToBeReplaced);
  // Streaming-tolerant attach, matching PointCloudLayer/SceneEntitiesLayer: a grid
  // topic can be attached before its first sample lands (layout restore at stream
  // start, catalog drag). bootstrap() only pre-warms source_frame_ from the first
  // keyframe — updateAt() self-heals it later — so a failure must NOT drop the
  // layer (SceneDockWidget::attach discards on false). updateAt no-ops on an empty
  // store, so we return true as long as session + parser binding exist.
  if (!bootstrap()) {
    qCWarning(lcOccGrid) << "attach: bootstrap failed for occupancy-grid topic" << topic_id_.id
                         << "— render will skip until a sample arrives";
  }
  return true;
}

void OccupancyGridLayer::detach() {
  disconnect(reload_connection_);
  reload_connection_ = {};
  stageEmpty();
  updates_topic_.reset();
  resetStreamingState();
  setDataWarning({});
  noteRenderSuccess();
  requested_time_.reset();
  tracker_dirty_ = false;
}

void OccupancyGridLayer::resetStreamingState() {
  base_cache_.reset();
  base_cache_uid_ = {};
  updates_cursor_ = {};
  last_consumed_time_.reset();
  skipped_update_warning_timestamp_.reset();
  skipped_update_warning_.clear();
  reconstructor_.invalidate();
  dirty_rects_.clear();
  budget_.reset();
}

void OccupancyGridLayer::onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id) {
  if (ctx_.session == nullptr || ctx_.session->objectStore().descriptor(topic_id_).dataset_id != dataset_id) {
    return;
  }
  stageEmpty();
  resetStreamingState();
  tracker_dirty_ = true;
  setDataWarning({});
  noteRenderSuccess();
  emit repaintRequested();
}

bool OccupancyGridLayer::bootstrap() {
  PJ::ObjectStore& store = ctx_.session->objectStore();
  auto first = store.at(topic_id_, 0);
  if (!first.has_value() || first->payload.bytes.empty() ||
      static_cast<std::uint64_t>(first->payload.bytes.size()) > kSceneLimits.max_occupancy_wire_bytes) {
    return false;
  }
  const auto binding = ctx_.session->parserBindingForObjectTopic(topic_id_);
  auto obj = resolveObject(binding, PJ::sdk::BuiltinObjectType::kOccupancyGrid, first->timestamp, first->payload);
  if (!obj.has_value()) {
    qCWarning(lcOccGrid) << "bootstrap: parseObject failed:" << QString::fromStdString(obj.error());
    return false;
  }
  const auto* grid = obj->object.get<PJ::sdk::OccupancyGrid>();
  if (grid == nullptr) {
    return false;
  }
  updateSourceFrame(grid->frame_id);
  return true;
}

void OccupancyGridLayer::updateSourceFrame(const std::string& frame) {
  if (frame == source_frame_) {
    return;
  }
  source_frame_ = frame;
  emit sourceFrameChanged(QString::fromStdString(source_frame_));
  emit fallbackFramesChanged(fallbackFrames());
}

bool OccupancyGridLayer::updateAt(int64_t time_ns) {
  if (ctx_.session == nullptr) {
    return false;
  }
  // Per-tick binding snapshots, alive for the whole reconstructAt call below.
  // An empty binding resolves canonical bytes (resolveObject()).
  const auto binding = ctx_.session->parserBindingForObjectTopic(topic_id_);
  const auto updates_binding = updates_topic_.has_value() ? ctx_.session->parserBindingForObjectTopic(*updates_topic_)
                                                          : PJ::SessionManager::ParserBinding{};
  PJ::ObjectStore& store = ctx_.session->objectStore();

  // Retroactive-ingest check (must run BEFORE reconstructAt). The reconstructor's
  // forward path consumes only (last_t_, t] and assumes immutable history, but the
  // live drive follows the fastest topic's edge — an update can be ingested later
  // with ts <= the already-consumed time and would be skipped forever. Detect it and
  // rewind. Conservative: a spurious invalidate only costs one rebuild, never
  // correctness.
  PJ::SequentialUID cursor_candidate = updates_cursor_;
  if (updates_topic_.has_value()) {
    // Post-reconstruct cursor: the MAX arrival UID among entries with ts <= t — NOT
    // latestAt(t)'s UID, whose value an out-of-order insert (old ts, newest UID) can
    // leave below a retained entry's, parking the cursor under the late entry and
    // re-detecting it every frame. maxUidAtOrBefore covers every entry reconstructAt
    // consumes at t. Captured pre-reconstruct so entries racing in mid-reconstruct
    // keep UIDs above it and are re-examined next tick.
    const PJ::SequentialUID high_at_t = store.maxUidAtOrBefore(*updates_topic_, time_ns);
    cursor_candidate = std::max(cursor_candidate, high_at_t);
    // Retroactive iff some entry with ts <= the consumed high-water carries an arrival
    // UID past the cursor — i.e. it was ingested after we already consumed that time.
    // (maxUidAtOrBefore, not the first-arrival entry: a late update can hide behind a
    // future-stamped arrival that the arrival cursor would reach first.) high_at_t is a
    // cheap necessary precondition — any such entry has ts <= t, so it is counted there
    // too — which short-circuits the second O(prefix) scan on a no-new-arrival frame.
    if (last_consumed_time_.has_value() && high_at_t > updates_cursor_ &&
        store.maxUidAtOrBefore(*updates_topic_, *last_consumed_time_) > updates_cursor_) {
      // Rewind to the EARLIEST such entry so the replay re-applies it. rangeByTime is
      // ascending by timestamp, so the first entry in (-inf, last_consumed] whose UID
      // is past the cursor is that earliest late update. invalidateAfter keeps the
      // snapshots strictly before it — a snapshot-preserving rewind, not a from-base
      // rebuild (which is cumulatively quadratic under a latched base whose updates
      // trail a faster topic's live edge).
      const auto retro =
          store.rangeByTime(*updates_topic_, std::numeric_limits<PJ::Timestamp>::min(), *last_consumed_time_);
      for (const auto& ref : retro) {
        if (ref.uid > updates_cursor_) {
          reconstructor_.invalidateAfter(ref.timestamp);
          ++retroactive_invalidate_count_;
          // Move the detector's reference to this render's t (the cursor below covers
          // every entry with ts <= t); a stale high-water could re-flag every tick.
          last_consumed_time_.reset();
          break;
        }
      }
    }
  }

  // Fail-closed reason for the whole grid (an undecodable base, or an over-limit
  // sample); a single bad update is only skipped, with a softer warning.
  QString rejection;

  // base_at(t): the latest full grid with ts <= t, decoded to sdk::OccupancyGrid
  // and paired with its entry UID (the reconstructor's epoch identity — a
  // same-timestamp replacement keyframe must still start a new epoch).
  // Memoized on the entry's SequentialUID: the common case is the same keyframe
  // tick after tick, and re-parsing deep-copies the full cell payload each time.
  // The memo is consulted metadata-first (latestEntryIdAt, a pure binary search):
  // resolving via latestAt before the identity check would re-fetch an evicted
  // pool seed (file read + whole-chunk decompress on the GUI thread) only to
  // discard the bytes on a match.
  using BaseSample = OccupancyGridReconstructor::BaseSample;
  // Memoized-base predicate shared by the metadata-first gate and the
  // post-resolve re-check (a racing insert can change the pick in between).
  const auto memoized_base = [this](PJ::SequentialUID uid) {
    return base_cache_.has_value() && uid == base_cache_uid_;
  };
  auto base_at = [this, &store, &binding, &memoized_base, &rejection](PJ::Timestamp t) -> std::optional<BaseSample> {
    const auto entry_id = store.latestEntryIdAt(topic_id_, t);
    if (!entry_id.has_value()) {
      return std::nullopt;
    }
    if (memoized_base(entry_id->uid)) {
      return BaseSample{*base_cache_, base_cache_uid_.value};  // no resolve, no re-parse
    }
    auto entry = store.latestAt(topic_id_, t);
    if (!entry.has_value() || entry->payload.bytes.empty()) {
      return std::nullopt;
    }
    if (memoized_base(entry->sequential_uid)) {
      return BaseSample{*base_cache_, base_cache_uid_.value};
    }
    if (static_cast<std::uint64_t>(entry->payload.bytes.size()) > kSceneLimits.max_occupancy_wire_bytes) {
      rejection = tr("Occupancy-grid payload is %1 MiB; the limit is %2 MiB")
                      .arg(
                          QString::number(static_cast<double>(entry->payload.bytes.size()) / (1024.0 * 1024.0), 'f', 1),
                          QString::number(kSceneLimits.max_occupancy_wire_bytes / kMiB));
      return std::nullopt;
    }
    auto obj = resolveObject(binding, PJ::sdk::BuiltinObjectType::kOccupancyGrid, entry->timestamp, entry->payload);
    if (!obj.has_value()) {
      rejection = tr("Occupancy grid could not be decoded: %1").arg(QString::fromStdString(obj.error()));
      return std::nullopt;
    }
    const auto* grid = obj->object.get<PJ::sdk::OccupancyGrid>();
    if (grid == nullptr) {
      rejection = tr("This layer accepts OccupancyGrid base samples only");
      return std::nullopt;
    }
    if (!occupancyDimensionsFit(grid->width, grid->height)) {
      rejection = tr("Occupancy grid is %1 x %2; the limit is %3 cells and %4 per dimension")
                      .arg(
                          QString::number(grid->width), QString::number(grid->height),
                          QString::number(kSceneLimits.max_occupancy_cells_per_layer),
                          QString::number(kSceneLimits.max_occupancy_texture_dimension));
      return std::nullopt;
    }
    const double width_m = grid->resolution * static_cast<double>(grid->width);
    const double height_m = grid->resolution * static_cast<double>(grid->height);
    if (!std::isfinite(grid->resolution) || grid->resolution <= 0.0 || !std::isfinite(width_m) ||
        !std::isfinite(height_m) || !finitePose(grid->origin)) {
      rejection = tr("Occupancy grid has an invalid resolution or origin pose");
      return std::nullopt;
    }
    base_cache_uid_ = entry->sequential_uid;
    base_cache_ = *grid;  // copy carries the anchor → bytes stay alive past the call
    return BaseSample{*base_cache_, base_cache_uid_.value};
  };

  // updates_in(lo, hi): the OccupancyGridUpdate patches with lo < ts <= hi, in
  // ASCENDING timestamp order (the reconstructor applies patches in time order).
  // A time-window query MUST go through ObjectStore::rangeByTime, NOT the
  // arrival-order UID cursor: an out-of-order update (older ts, newest UID — e.g.
  // multi-publisher interleaving) can sit at any UID position, so a UID walk would
  // skip it. rangeByTime returns a decode-free, ascending, eviction-safe snapshot
  // of (uid, timestamp); we resolve + parse each afterwards (an entry evicted
  // between the snapshot and the resolve simply comes back nullopt and is skipped).
  // A skipped patch leaves a soft warning naming the EARLIEST skip.
  QString update_warning;
  std::optional<PJ::Timestamp> skipped_update_timestamp;
  const auto note_skipped_update = [&update_warning, &skipped_update_timestamp](
                                       PJ::Timestamp timestamp, QString warning) {
    if (!skipped_update_timestamp.has_value() || timestamp < *skipped_update_timestamp) {
      skipped_update_timestamp = timestamp;
      update_warning = std::move(warning);
    }
  };
  std::uint64_t update_window_bytes = 0;
  auto updates_in = [this, &store, &updates_binding, &rejection, &update_window_bytes, &note_skipped_update](
                        PJ::Timestamp lo, PJ::Timestamp hi) -> std::vector<PJ::sdk::OccupancyGridUpdate> {
    std::vector<PJ::sdk::OccupancyGridUpdate> out;
    if (!updates_topic_.has_value()) {
      return out;
    }
    const PJ::ObjectTopicId id = *updates_topic_;
    const auto window = store.rangeByTime(id, lo, hi);
    out.reserve(window.size());
    std::size_t failed_fetches = 0;
    for (const auto& ref : window) {
      auto entry = store.at(id, ref.uid);
      if (entry.has_value() && entry->fetch_failed) {
        // Permanent loss: the retroactive cursor advances past this window, so
        // forward playback never re-reads it. Count and report, don't retry.
        ++failed_fetches;
        note_skipped_update(entry->timestamp, tr("Occupancy update lost: source re-read failed"));
        continue;
      }
      if (!entry.has_value() || entry->payload.bytes.empty()) {
        continue;  // evicted between the snapshot and the resolve, or empty payload
      }
      if (static_cast<std::uint64_t>(entry->payload.bytes.size()) > kSceneLimits.max_occupancy_wire_bytes) {
        rejection = tr("Occupancy update payload exceeds the %1 MiB sample limit")
                        .arg(kSceneLimits.max_occupancy_wire_bytes / kMiB);
        return {};
      }
      auto obj = resolveObject(
          updates_binding, PJ::sdk::BuiltinObjectType::kOccupancyGridUpdate, entry->timestamp, entry->payload);
      if (!obj.has_value()) {
        note_skipped_update(
            entry->timestamp,
            tr("Occupancy update could not be decoded and was skipped: %1").arg(QString::fromStdString(obj.error())));
        continue;
      }
      const auto* update = obj->object.get<PJ::sdk::OccupancyGridUpdate>();
      if (update == nullptr) {
        note_skipped_update(entry->timestamp, tr("A paired update sample was not OccupancyGridUpdate and was skipped"));
        continue;
      }
      const std::uint64_t declared = static_cast<std::uint64_t>(update->width) * update->height;
      if (declared > update->data.size()) {
        note_skipped_update(
            entry->timestamp, tr("Occupancy update data was shorter than its declared rectangle and was skipped"));
        continue;
      }
      const std::uint64_t window_limit = kSceneLimits.max_occupancy_update_window_bytes;
      if (declared > kSceneLimits.max_occupancy_update_window_bytes || declared > window_limit - update_window_bytes) {
        rejection = tr("Occupancy updates in this seek window exceed the %1 MiB limit").arg(window_limit / kMiB);
        return {};
      }
      update_window_bytes += declared;
      out.push_back(*update);
    }
    if (failed_fetches != 0) {
      qCWarning(lcOccGrid) << failed_fetches
                           << "grid update(s) LOST: lazy re-read failed (source file truncated, replaced, or corrupt)";
    }
    return out;
  };

  const GridUpdate update = reconstructor_.reconstructAt(time_ns, base_at, updates_in);
  // Advance the retroactive-ingest cursor. Safe even on an Empty result: entries
  // it covers are either applied, or re-read in full by the epoch rebuild the
  // next successful reconstructAt performs (the window always restarts at the
  // base keyframe after an invalidate).
  updates_cursor_ = cursor_candidate;
  last_consumed_time_ =
      last_consumed_time_.has_value() ? std::max(*last_consumed_time_, PJ::Timestamp{time_ns}) : time_ns;
  if (!rejection.isEmpty()) {
    reconstructor_.invalidate();
    stageEmpty();
    setDataWarning(std::move(rejection));
    return true;
  }
  if (skipped_update_warning_timestamp_.has_value() &&
      (time_ns < *skipped_update_warning_timestamp_ ||
       (!update.grid.empty() && update.grid.base_timestamp_ns > *skipped_update_warning_timestamp_))) {
    skipped_update_warning_timestamp_.reset();
    skipped_update_warning_.clear();
  }
  if (skipped_update_timestamp.has_value()) {
    skipped_update_warning_timestamp_ = skipped_update_timestamp;
    skipped_update_warning_ = std::move(update_warning);
  }
  const std::uint64_t revision_before = texture_revision_;
  const QString warning_before = dataWarning();
  if (update.grid.empty()) {
    stageEmpty();
  } else {
    updateSourceFrame(update.grid.frame_id);
    stageGrid(update);
  }
  setDataWarning(skipped_update_warning_);
  return texture_revision_ != revision_before || dataWarning() != warning_before;
}

void OccupancyGridLayer::stageGrid(const GridUpdate& update) {
  // Anything but an incremental forward step (new epoch / backward seek) re-uploads
  // the whole texture; an incremental update uploads just the changed rects, which
  // are only materialized on that path (potentially large after a backward-seek
  // replay).
  const bool incremental = update.kind == GridUpdate::Kind::kIncremental;
  if (incremental && update.dirty.empty() && grid_staged_) {
    return;
  }
  full_upload_pending_ = !incremental || !grid_staged_;
  if (full_upload_pending_) {
    dirty_rects_.clear();
  } else {
    dirty_rects_.assign(update.dirty.begin(), update.dirty.end());
  }
  grid_staged_ = true;
  ++texture_revision_;
}

void OccupancyGridLayer::stageEmpty() {
  dirty_rects_.clear();
  full_upload_pending_ = true;
  if (grid_staged_) {
    grid_staged_ = false;
    ++texture_revision_;
  }
}

void OccupancyGridLayer::setFixedFrame(const QString& frame) {
  // The grid is frame-relative; the renderer places it per-frame against the
  // fixed frame, so no re-decode is needed — just request a paint.
  fixed_frame_ = frame.toStdString();
  emit repaintRequested();
}

void OccupancyGridLayer::setTrackerTime(PJ::Timepoint time) {
  // Defer the reconstruction to the next painted frame — see tracker_dirty_. Also
  // stops a hidden layer from reconstructing on every tick.
  tracker_dirty_ = true;
  requested_time_ = time;
  emit repaintRequested();
}

void OccupancyGridLayer::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  // The view drops its GPU copy of a hidden grid, so un-hiding rebuilds it
  // from the base keyframe.
  if (!visible_) {
    budget_.reset();
    stageEmpty();
  } else if (requested_time_.has_value()) {
    reconstructor_.invalidate();
    base_cache_.reset();
    base_cache_uid_ = {};
    tracker_dirty_ = true;
  }
  emit visibilityChanged(visible);
  // Catch-up on un-hide is the dock's job: SceneDockWidget::setLayerVisible
  // re-delivers the last tracker time, marking tracker_dirty_ for the next paint.
  emit repaintRequested();
}

bool OccupancyGridLayer::drainPendingUpdate() {
  // One reconstruction per painted frame; the reconstructor's incremental path
  // keeps a forward step cheap. A layer hidden during the move keeps the flag
  // set and catches up on its first visible frame.
  if (!visible_ || !tracker_dirty_ || !requested_time_.has_value()) {
    return false;
  }
  tracker_dirty_ = false;
  return updateAt(PJ::toRaw(*requested_time_));
}

std::uint64_t OccupancyGridLayer::renderKey(PJ::Timepoint time) const {
  const PJ::ObjectStore* store = ctx_.session != nullptr ? &ctx_.session->objectStore() : nullptr;
  std::uint64_t key = PJ::latestEntryRenderKey(store, topic_id_, time);
  if (store != nullptr && updates_topic_.has_value()) {
    // Fingerprint the highest ARRIVAL UID in the consumed prefix, not merely
    // latestAt(time): a retroactive patch can have an older timestamp while
    // carrying the newest UID. That arrival must break the dock's repaint
    // coalescing gate so updateAt can run its late-update rewind.
    const std::uint64_t update_uid = store->maxUidAtOrBefore(*updates_topic_, PJ::toRaw(time)).value;
    key ^= update_uid + 0x9e3779b97f4a7c15ULL + (key << 6U) + (key >> 2U);
  }
  // Cursor-placed (entry clock): the drawn grid is a keyframe plus later patches,
  // with no single stamp to place it at.
  return foldFrameTransformIntoKey(key, ctx_.tf_buffer.get(), fixed_frame_, source_frame_, std::nullopt, time, nullptr);
}

bool OccupancyGridLayer::prepareForRender(std::uint64_t remaining_view_cells) {
  // The budget_ candidate: the base keyframe at the playhead, which sizes the grid.
  const auto candidate = [this] {
    return PJ::latestEntryRenderKey(
        ctx_.session != nullptr ? &ctx_.session->objectStore() : nullptr, topic_id_,
        requested_time_.value_or(PJ::Timepoint{}));
  };
  bool released = false;
  if (budget_.blocked(remaining_view_cells, candidate, released)) {
    return false;
  }
  if (released) {
    reconstructor_.invalidate();
    base_cache_.reset();
    base_cache_uid_ = {};
    tracker_dirty_ = true;
  }
  const bool changed = drainPendingUpdate();
  const std::uint64_t cells = static_cast<std::uint64_t>(grid().width) * grid().height;
  if (grid().empty() || cells <= remaining_view_cells) {
    return changed;
  }
  budget_.reject(cells, candidate());
  setDataWarning(tr("Layer needs %1 occupancy cells; only %2 remain in this view's budget")
                     .arg(QString::number(cells), QString::number(remaining_view_cells)));
  stageEmpty();
  reconstructor_.invalidate();
  return true;
}

std::optional<AABB> OccupancyGridLayer::worldBounds() const {
  // Bounds come from the grid as currently reconstructed (whatever the last
  // updateAt produced). Empty before any base keyframe has been seen.
  const ReconstructedGrid& grid = reconstructor_.grid();
  if (grid.empty()) {
    return std::nullopt;
  }
  const glm::vec3 origin{
      static_cast<float>(grid.origin.position.x), static_cast<float>(grid.origin.position.y),
      static_cast<float>(grid.origin.position.z)};
  const AABB box = occupancyGridBounds(origin, grid.resolution, grid.width, grid.height);
  if (!box.valid) {
    return std::nullopt;
  }
  return box;
}

void OccupancyGridLayer::setColorScheme(OccupancyColorScheme scheme) {
  if (color_scheme_ == scheme) {
    return;
  }
  color_scheme_ = scheme;
  emit configurationChanged();
  emit repaintRequested();
}

void OccupancyGridLayer::setOpacity(float opacity) {
  const float clamped = std::clamp(opacity, 0.0f, 1.0f);
  if (opacity_ == clamped) {
    return;
  }
  opacity_ = clamped;
  emit configurationChanged();
  emit repaintRequested();
}

QWidget* OccupancyGridLayer::createConfigWidget(QWidget* parent) {
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

  auto* scheme_combo = new PJ::ComboBox(container);
  scheme_combo->addItem(tr("Map (grayscale)"), static_cast<int>(OccupancyColorScheme::kMap));
  scheme_combo->addItem(tr("Costmap"), static_cast<int>(OccupancyColorScheme::kCostmap));
  scheme_combo->setCurrentIndex(color_scheme_ == OccupancyColorScheme::kCostmap ? 1 : 0);
  form->addRow(tr("Colors:"), scheme_combo);
  QObject::connect(scheme_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
    setColorScheme(idx == 1 ? OccupancyColorScheme::kCostmap : OccupancyColorScheme::kMap);
  });

  auto* opacity_spin = new PJ::DoubleScrubber(container);
  opacity_spin->setRange(0.0, 1.0);
  opacity_spin->setDecimals(2);
  opacity_spin->setSingleStep(0.05);
  opacity_spin->setValue(static_cast<double>(opacity_));
  form->addRow(tr("Opacity:"), opacity_spin);
  QObject::connect(
      opacity_spin, &PJ::DoubleScrubber::valueChanged, this, [this](double v) { setOpacity(static_cast<float>(v)); });

  return container;
}

}  // namespace pj::scene3d
