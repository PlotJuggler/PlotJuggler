#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <cstdint>
#include <optional>
#include <unordered_set>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_widgets/Timeline.h"

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace PJ {

class AppSession;
class Timeline;

/// Binds the runtime-agnostic Timeline widget to AppSession: tracks from the catalog, ranges and offsets; widget
/// intents to SessionManager / PlaybackEngine. The only place that knows both: Timeline includes no pj_runtime header.
class SourceTimelineController : public QObject {
  Q_OBJECT
 public:
  SourceTimelineController(Timeline* widget, AppSession* session, QObject* parent = nullptr);

  /// Top to bottom, as drawn (only datasets with a bar). What a layout saves.
  [[nodiscard]] std::vector<DatasetId> currentTrackOrder() const;

  /// Top to bottom; datasets it does not list follow in catalog order.
  void setDisplayOrder(std::vector<DatasetId> order);

  /// Paints `anchor`'s bar in the merged color. Call after a merge from either entry point.
  void markDatasetMerged(DatasetId anchor);

 signals:
  /// One complete user edit of offsets/order has finished.
  void workspaceChangeCommitted();

  /// A dataset became a drawable track and must hydrate the current history tip.
  void trackAdded(PJ::DatasetId dataset_id);

 public slots:
  /// Aligns every source's displayed start to the earliest start.
  void alignStarts();

  /// Aligns every source's displayed center to the earliest center.
  void alignCenters();

  /// Aligns every source's displayed end to the latest end.
  void alignEnds();

  /// Zeroes every display offset and drops the drag order.
  void resetAll();

  /// In the playback frame (the widget shifts it into its bar frame); nullopt hides the needle.
  void setReferenceLine(std::optional<double> playback_seconds);

 private slots:
  void rebuildTracks();
  void onMergeRequested(const QList<quint64>& ids);

 private:
  /// The drag path: offsets only, skipping rebuildTracks()'s per-dataset range scan.
  void updateTrackOffsets();

  enum class AlignMode : std::uint8_t { kStarts, kCenters, kEnds };
  void applyAlignment(AlignMode mode);

  /// Coalesces the many offset writes of a drag into one playback-range recompute.
  void scheduleRangeRecompute();

  /// Assigned on first sight and kept, so a bar keeps its color across rebuilds.
  [[nodiscard]] QColor colorFor(DatasetId id);

  Timeline* widget_;
  AppSession* session_;
  QTimer* range_recompute_timer_;
  QHash<DatasetId, QColor> colors_;
  std::vector<TimelineTrack> tracks_;  // as drawn; alignment runs over it
  std::unordered_set<DatasetId> merged_ids_;
  // From a drag (Timeline::tracksReordered) or a layout (setDisplayOrder); unlisted datasets follow in catalog order.
  std::vector<DatasetId> display_order_;
};

}  // namespace PJ
