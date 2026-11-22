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

// Keeps a drag's range recomputes at <= 60 Hz however fast offsets are written.
constexpr int kRangeRecomputeThrottleMs = 16;

// 0xAARRGGBB, cycled by first sight.
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

// Outside kPalette, so a merge result stands apart.
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

  connect(widget_, &Timeline::offsetChangeRequested, this, [this, &sessions](quint64 id, qint64 new_offset) {
    sessions.setDisplayOffset(static_cast<DatasetId>(id), DisplayOffset{Duration{new_offset}});
    scheduleRangeRecompute();
  });
  connect(widget_, &Timeline::offsetChangeCommitted, this, &SourceTimelineController::workspaceChangeCommitted);
  connect(widget_, &Timeline::alignRequested, this, &SourceTimelineController::alignStarts);
  connect(widget_, &Timeline::mergeRequested, this, &SourceTimelineController::onMergeRequested);
  connect(widget_, &Timeline::tracksReordered, this, [this](const QList<quint64>& ids) {
    display_order_.clear();
    display_order_.reserve(static_cast<std::size_t>(ids.size()));
    for (const quint64 id : ids) {
      display_order_.push_back(static_cast<DatasetId>(id));
    }
    rebuildTracks();
    emit workspaceChangeCommitted();
  });
  connect(widget_, &Timeline::playheadSeeked, this, [&playback](double display_seconds) {
    // The widget already emits in the playback frame.
    playback.setCurrentTime(fromAxisDouble(display_seconds));
  });

  connect(&catalog, &CatalogModel::itemsAdded, this, [this](const std::vector<CatalogItem>&) { rebuildTracks(); });
  connect(&catalog, &CatalogModel::itemsRemoved, this, [this](const QStringList&) { rebuildTracks(); });
  connect(&catalog, &CatalogModel::cleared, this, [this]() {
    merged_ids_.clear();
    display_order_.clear();
    colors_.clear();  // ids are re-minted
    rebuildTracks();
  });
  connect(&sessions, qOverload<PJ::DatasetId>(&SessionManager::displayOffsetChanged), this, [this](DatasetId) {
    updateTrackOffsets();
    scheduleRangeRecompute();
  });
  connect(&sessions, qOverload<>(&SessionManager::displayOffsetChanged), this, [this, &sessions]() {
    // The global frame ("Use time offset") moves the labels, playhead and range, never the bars; MainWindow
    // re-seeds the playhead and range.
    widget_->setTimeFrameOffsetNs(sessions.globalTimeReference());
    widget_->setAbsoluteTimeLabels(!sessions.useTimeOffset());
    updateTrackOffsets();
    scheduleRangeRecompute();
  });
  // The coarse cursor, as the playback slider: see TimelineWidget::onEngineTimeChanged.
  connect(&playback, &PlaybackEngine::coarseTimeChanged, this, [this](double time) { widget_->setPlayhead(time); });
  connect(&playback, &PlaybackEngine::rangeChanged, this, [this](double min, double max) {
    // The only signal covering all data growth: object entries fire neither itemsAdded (after a topic's first) nor
    // samplesIngested. The frame offset goes first: a load moves the global reference, and a stale offset would
    // strand the needle.
    widget_->setTimeFrameOffsetNs(session_->sessionManager().globalTimeReference());
    widget_->setDisplayRange(min, max);
    rebuildTracks();
  });

  // Frame offset and label format first, so the range and playhead land in the right frame.
  widget_->setTimeFrameOffsetNs(sessions.globalTimeReference());
  widget_->setAbsoluteTimeLabels(!sessions.useTimeOffset());
  rebuildTracks();
  widget_->setDisplayRange(playback.rangeMin().value, playback.rangeMax().value);
  widget_->setPlayhead(playback.currentTime().value);
}

std::vector<DatasetId> SourceTimelineController::currentTrackOrder() const {
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
      continue;  // no time-bearing data yet
    }
    TimelineTrack track;
    track.id = static_cast<TimelineSourceId>(id);
    track.name = name;
    track.t_min_ns = static_cast<qint64>(raw->min);
    track.t_max_ns = static_cast<qint64>(raw->max);
    // The source's own offset only: bars never follow the global "Use time offset" frame.
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
  // The widget keys its extent and auto-zoom on the raw bounds, so re-feeding never resets the view.
  SessionManager& sessions = session_->sessionManager();
  for (TimelineTrack& track : tracks_) {
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
  SessionManager& mgr = session_->sessionManager();
  for (const auto& dataset : session_->catalogModel().datasets()) {
    mgr.setDisplayOffset(dataset.first, DisplayOffset{Duration{0}});
  }
  display_order_.clear();
  rebuildTracks();
  scheduleRangeRecompute();
  widget_->fitToContents();  // no-op when auto-zoom is off
  emit workspaceChangeCommitted();
}

void SourceTimelineController::alignStartsOf(const std::vector<DatasetId>& datasets) {
  setAlignedOffsets(AlignMode::kStarts, datasets);
}

void SourceTimelineController::applyAlignment(AlignMode mode) {
  std::vector<DatasetId> all;
  all.reserve(tracks_.size());
  for (const TimelineTrack& t : tracks_) {
    all.push_back(static_cast<DatasetId>(t.id));
  }
  setAlignedOffsets(mode, all);
  emit workspaceChangeCommitted();
}

void SourceTimelineController::setAlignedOffsets(AlignMode mode, const std::vector<DatasetId>& datasets) {
  // The widget keeps no offsets, so the controller computes them.
  TimelineScene scene;
  std::vector<TimelineSpanInput> spans;
  spans.reserve(tracks_.size());
  for (const TimelineTrack& t : tracks_) {
    if (std::find(datasets.begin(), datasets.end(), static_cast<DatasetId>(t.id)) != datasets.end()) {
      spans.push_back(
          TimelineSpanInput{.id = t.id, .t_min_ns = t.t_min_ns, .t_max_ns = t.t_max_ns, .offset_ns = t.offset_ns});
    }
  }
  if (spans.empty()) {
    return;
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
  widget_->fitToContents();  // no-op when auto-zoom is off
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
  confirmAndMergeDatasets(
      widget_, *session_, datasets, [guard = QPointer<SourceTimelineController>(this)](DatasetId anchor) {
        if (guard != nullptr) {
          guard->markDatasetMerged(anchor);
        }
      });
}

void SourceTimelineController::markDatasetMerged(DatasetId anchor) {
  merged_ids_.insert(anchor);
  rebuildTracks();
}

}  // namespace PJ
