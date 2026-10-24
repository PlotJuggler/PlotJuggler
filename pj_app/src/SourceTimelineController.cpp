// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "SourceTimelineController.h"

#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <algorithm>
#include <array>
#include <utility>

#include "DatasetMergeActions.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_widgets/Timeline.h"

namespace PJ {

namespace {

// Coalesce window for the live-drag range recompute. ~16 ms keeps a sustained
// drag's recompute work at <=60 Hz regardless of how fast offset writes arrive.
constexpr int kRangeRecomputeThrottleMs = 16;

// Stable per-source palette (0xAARRGGBB), cycled by insertion order. Bars stay
// the same color across rebuilds so a source is visually trackable while dragged.
constexpr std::array<uint32_t, 8> kPalette = {
    0xFF4FC3F7u,  // light blue
    0xFF81C784u,  // green
    0xFFFFB74Du,  // orange
    0xFFBA68C8u,  // purple
    0xFFE57373u,  // red
    0xFF4DD0E1u,  // cyan
    0xFFFFF176u,  // yellow
    0xFFA1887Fu,  // brown
};

// Reserved bar color for a merged dataset — deliberately outside kPalette so a
// merge result reads as distinct from the per-source bars.
constexpr uint32_t kMergedColor = 0xFFFFD740u;  // amber/gold

}  // namespace

SourceTimelineController::SourceTimelineController(Timeline* widget, AppSession* session, QObject* parent)
    : QObject(parent), widget_(widget), session_(session) {
  range_recompute_timer_ = new QTimer(this);
  range_recompute_timer_->setSingleShot(true);
  range_recompute_timer_->setInterval(kRangeRecomputeThrottleMs);
  connect(range_recompute_timer_, &QTimer::timeout, this, [this]() { session_->recomputeRange(); });

  SessionManager& sessions = session_->sessionManager();
  CatalogModel& catalog = session_->catalogModel();
  PlaybackEngine& playback = session_->playbackEngine();

  // --- widget intents -> runtime writes ---
  connect(widget_, &Timeline::offsetChangeRequested, this, [this, &sessions](quint64 id, qint64 new_offset) {
    sessions.setDisplayOffset(static_cast<DatasetId>(id), DisplayOffset{Duration{new_offset}});
    scheduleRangeRecompute();
  });
  connect(widget_, &Timeline::offsetChangeCommitted, this, &SourceTimelineController::workspaceChangeCommitted);
  connect(widget_, &Timeline::alignRequested, this, &SourceTimelineController::alignStarts);
  connect(widget_, &Timeline::mergeRequested, this, &SourceTimelineController::onMergeRequested);
  connect(widget_, &Timeline::tracksReordered, this, [this](const QList<quint64>& ids) {
    // Adopt the user's drag order so it survives later rebuilds (offset edits, etc.).
    display_order_.clear();
    display_order_.reserve(static_cast<std::size_t>(ids.size()));
    for (const quint64 id : ids) {
      display_order_.push_back(static_cast<DatasetId>(id));
    }
    rebuildTracks();
    emit workspaceChangeCommitted();
  });
  connect(widget_, &Timeline::playheadSeeked, this, [&playback](double display_seconds) {
    // The widget already emits in the playback frame (it undoes its time-frame
    // offset in integer ns), so this is a direct, precise hand-off — wrapped
    // through the canonical axis-double door rather than constructed raw.
    playback.setCurrentTime(fromAxisDouble(display_seconds));
  });

  // --- runtime state -> widget ---
  connect(&catalog, &CatalogModel::itemsAdded, this, [this](const std::vector<CatalogItem>&) { rebuildTracks(); });
  connect(&catalog, &CatalogModel::itemsRemoved, this, [this](const QStringList&) { rebuildTracks(); });
  connect(&catalog, &CatalogModel::cleared, this, [this]() {
    merged_ids_.clear();     // fresh session: nothing is a merge result anymore
    display_order_.clear();  // ...and no user track ordering to honor
    colors_.clear();         // ...so palette assignment restarts at slot 0 (ids are re-minted)
    rebuildTracks();
  });
  connect(&sessions, qOverload<PJ::DatasetId>(&SessionManager::displayOffsetChanged), this, [this](DatasetId) {
    // Offset-only change (drag/align/reset): refresh just the offsets, NOT a full
    // rebuildTracks() — the raw ranges are unchanged, so re-scanning every
    // dataset's datasetRawTimeRange on every drag tick would be wasted work.
    updateTrackOffsets();
    scheduleRangeRecompute();
  });
  connect(&sessions, qOverload<>(&SessionManager::displayOffsetChanged), this, [this, &sessions]() {
    // Global frame change (the "Use time offset" toggle). It does NOT move the bars
    // (those follow only the per-source alignment, refreshed here for safety); it
    // reframes the NUMBERS (relative-to-epoch vs absolute unix seconds) and shifts
    // the playhead/range frame. Push the new global reference to the widget as the
    // integer-ns frame offset; the playhead/range re-arrive (re-seeded by MainWindow).
    widget_->setTimeFrameOffsetNs(sessions.globalTimeReference());
    widget_->setAbsoluteTimeLabels(!sessions.useTimeOffset());
    updateTrackOffsets();
    scheduleRangeRecompute();
  });
  connect(&playback, &PlaybackEngine::currentTimeChanged, this, [this](double time) { widget_->setPlayhead(time); });
  connect(&playback, &PlaybackEngine::rangeChanged, this, [this](double min, double max) {
    // The PlaybackEngine range is the single authoritative data extent — recomputed
    // over BOTH scalar and (lazily-ingested) object topics, so it is the one signal
    // that aggregates ALL growth, including object/image entries that fire neither
    // catalog itemsAdded (after the topic first appears) nor samplesIngested
    // (scalar-only). Rebuild the bars from it so the bar extent — and thus the
    // needle's clamp — always matches what playback traverses. rebuildTracks only
    // re-fits the view when the raw bounds actually changed (an offset shift leaves
    // them identical), so this is safe on the live-drag recompute path too. The
    // widget shifts these playback-frame values into the bar frame in integer ns.
    //
    // Refresh the frame offset FIRST: a data load changes the global reference (the
    // global earliest sample), and the range is a function of it — so whenever it
    // moves, this signal fires. Re-syncing here (before setDisplayRange, and ahead of
    // the currentTimeChanged that follows a load) keeps the needle in the bars' frame
    // instead of stranded at the small playback value under a stale (e.g. 0) offset.
    widget_->setTimeFrameOffsetNs(session_->sessionManager().globalTimeReference());
    widget_->setDisplayRange(min, max);
    rebuildTracks();
  });

  // (The timeline's align/reset actions live on the align rail — pj_app's
  // buildTimelineAlignRail wires align starts/centers/ends + reset to the slots
  // below; the name-column header has no kebab menu.)

  // Seed the widget with whatever is already loaded + the current playback state.
  // The time-frame offset (the global "use time offset" reference, in ns) and the
  // number format go first, so the playback-frame range/playhead below land correctly.
  widget_->setTimeFrameOffsetNs(sessions.globalTimeReference());
  widget_->setAbsoluteTimeLabels(!sessions.useTimeOffset());
  rebuildTracks();
  widget_->setDisplayRange(playback.rangeMin().value, playback.rangeMax().value);
  widget_->setPlayhead(playback.currentTime().value);
}

std::vector<DatasetId> SourceTimelineController::currentTrackOrder() const {
  // tracks_ is already the resolved display order (see rebuildTracks): the
  // exact top-to-bottom arrangement the user sees and we must persist.
  std::vector<DatasetId> order;
  order.reserve(tracks_.size());
  for (const TimelineTrack& t : tracks_) {
    order.push_back(static_cast<DatasetId>(t.id));
  }
  return order;
}

void SourceTimelineController::setDisplayOrder(std::vector<DatasetId> order) {
  display_order_ = std::move(order);
  rebuildTracks();
}

void SourceTimelineController::rebuildTracks() {
  SessionManager& sessions = session_->sessionManager();
  std::unordered_set<DatasetId> previous_track_ids;
  previous_track_ids.reserve(tracks_.size());
  for (const TimelineTrack& track : tracks_) {
    previous_track_ids.insert(static_cast<DatasetId>(track.id));
  }
  // Honor the user's drag order first (those still loaded), then any remaining
  // datasets in catalog (load) order.
  const auto catalog = session_->catalogModel().datasets();
  std::vector<std::pair<DatasetId, QString>> ordered;
  ordered.reserve(catalog.size());
  for (const DatasetId id : display_order_) {
    const auto it = std::find_if(catalog.begin(), catalog.end(), [id](const auto& p) { return p.first == id; });
    if (it != catalog.end()) {
      ordered.push_back(*it);
    }
  }
  for (const auto& entry : catalog) {
    if (std::find(display_order_.begin(), display_order_.end(), entry.first) == display_order_.end()) {
      ordered.push_back(entry);
    }
  }

  std::vector<TimelineTrack> tracks;
  for (const auto& [id, name] : ordered) {
    const auto raw = session_->datasetRawTimeRange(id);
    if (!raw.has_value()) {
      continue;  // no time-bearing data yet — nothing to draw a bar for
    }
    TimelineTrack track;
    track.id = static_cast<TimelineSourceId>(id);
    track.name = name;
    track.t_min_ns = static_cast<qint64>(raw->min);
    track.t_max_ns = static_cast<qint64>(raw->max);
    // Align-only offset: bars must NOT follow the global "Use time offset" frame.
    track.offset_ns = static_cast<qint64>(sessions.sourceDisplayOffset(id).value.count());
    track.color = merged_ids_.count(id) != 0 ? QColor::fromRgba(kMergedColor) : colorFor(id);
    tracks.push_back(std::move(track));
  }
  tracks_ = std::move(tracks);
  widget_->setTracks(tracks_);
  for (const TimelineTrack& track : tracks_) {
    const DatasetId id = static_cast<DatasetId>(track.id);
    if (previous_track_ids.count(id) == 0) {
      emit trackAdded(id);
    }
  }
}

void SourceTimelineController::updateTrackOffsets() {
  // Refresh ONLY the cached tracks' offsets from the SessionManager and re-feed
  // them. Used on the offset-only path (displayOffsetChanged from a live drag /
  // align / reset): the raw time ranges, names, colors, and order are unchanged,
  // so this avoids rebuildTracks()'s per-dataset datasetRawTimeRange catalog
  // scan. The widget keys its extent/auto-zoom on the (unchanged) raw bounds, so
  // re-feeding never resets the view.
  SessionManager& sessions = session_->sessionManager();
  for (TimelineTrack& track : tracks_) {
    // Align-only offset (see rebuildTracks): the global frame never moves a bar.
    track.offset_ns = static_cast<qint64>(sessions.sourceDisplayOffset(static_cast<DatasetId>(track.id)).value.count());
  }
  widget_->setTracks(tracks_);
}

void SourceTimelineController::alignStarts() {
  applyAlignment(AlignMode::kStarts);
}

void SourceTimelineController::alignCenters() {
  applyAlignment(AlignMode::kCenters);
}

void SourceTimelineController::alignEnds() {
  applyAlignment(AlignMode::kEnds);
}

void SourceTimelineController::resetAll() {
  // Undo every timeline edit for all sources: zero each display offset and drop the
  // user's drag order back to catalog order.
  SessionManager& mgr = session_->sessionManager();
  for (const auto& dataset : session_->catalogModel().datasets()) {
    mgr.setDisplayOffset(dataset.first, DisplayOffset{Duration{0}});
  }
  display_order_.clear();
  rebuildTracks();
  scheduleRangeRecompute();
  widget_->fitToContents();  // re-frame after reset (no-op if auto-zoom is off)
  emit workspaceChangeCommitted();
}

void SourceTimelineController::applyAlignment(AlignMode mode) {
  // Compute aligned offsets via the math engine over the current track list,
  // then write each through the SessionManager. The widget is intentionally
  // stateless about offsets, so the controller owns this computation.
  TimelineScene scene;
  std::vector<TimelineSpanInput> spans;
  spans.reserve(tracks_.size());
  for (const TimelineTrack& t : tracks_) {
    spans.push_back(
        TimelineSpanInput{.id = t.id, .t_min_ns = t.t_min_ns, .t_max_ns = t.t_max_ns, .offset_ns = t.offset_ns});
  }
  scene.setTracks(std::move(spans));
  std::vector<std::pair<TimelineSourceId, qint64>> offsets;
  switch (mode) {
    case AlignMode::kStarts:
      offsets = scene.alignStartsToCommonOrigin();
      break;
    case AlignMode::kCenters:
      offsets = scene.alignCentersToCommonOrigin();
      break;
    case AlignMode::kEnds:
      offsets = scene.alignEndsToCommonOrigin();
      break;
  }
  SessionManager& sessions = session_->sessionManager();
  for (const auto& [id, offset] : offsets) {
    sessions.setDisplayOffset(static_cast<DatasetId>(id), DisplayOffset{Duration{offset}});
  }
  scheduleRangeRecompute();
  widget_->fitToContents();  // re-frame the realigned extent (no-op if auto-zoom is off)
  emit workspaceChangeCommitted();
}

void SourceTimelineController::scheduleRangeRecompute() {
  if (!range_recompute_timer_->isActive()) {
    range_recompute_timer_->start();
  }
}

QColor SourceTimelineController::colorFor(DatasetId id) {
  const auto it = colors_.constFind(id);
  if (it != colors_.constEnd()) {
    return it.value();
  }
  const QColor color = QColor::fromRgba(kPalette[static_cast<std::size_t>(colors_.size()) % kPalette.size()]);
  colors_.insert(id, color);
  return color;
}

void SourceTimelineController::setReferenceLine(std::optional<double> playback_seconds) {
  // The widget shifts the playback-frame value into the bar frame in integer ns
  // (via the time-frame offset), so pass it straight through.
  if (playback_seconds.has_value()) {
    widget_->setReferenceLine(*playback_seconds, /*visible=*/true);
  } else {
    widget_->setReferenceLine(0.0, /*visible=*/false);
  }
}

void SourceTimelineController::onMergeRequested(const QList<quint64>& ids) {
  std::vector<DatasetId> datasets;
  datasets.reserve(static_cast<std::size_t>(ids.size()));
  for (const quint64 id : ids) {
    datasets.push_back(static_cast<DatasetId>(id));
  }
  // confirmAndMergeDatasets filters to data-bearing datasets and gates on ≥2.
  confirmAndMergeDatasets(
      widget_, *session_, datasets, [guard = QPointer<SourceTimelineController>(this)](DatasetId anchor) {
        if (guard != nullptr) {
          guard->markDatasetMerged(anchor);
        }
      });
}

void SourceTimelineController::markDatasetMerged(DatasetId anchor) {
  merged_ids_.insert(anchor);
  rebuildTracks();  // re-color the merged bar (catalog signals rebuild it too)
}

}  // namespace PJ
