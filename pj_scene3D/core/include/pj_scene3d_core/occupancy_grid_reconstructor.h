// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pj_base/builtin/frame_transforms.hpp"  // sdk::Pose
#include "pj_base/builtin/occupancy_grid.hpp"
#include "pj_base/builtin/occupancy_grid_update.hpp"
#include "pj_base/types.hpp"

namespace pj::scene3d {

/// A fully reconstructed occupancy grid at a point in time: the base keyframe's
/// placement (frame / origin / resolution / dims) with all applicable updates
/// applied. `cells` is row-major, `width * height` signed-8-bit values
/// (-1 unknown, 0..100 occupancy %); empty when there is no base grid yet.
struct ReconstructedGrid {
  PJ::Timestamp base_timestamp_ns = 0;  ///< Epoch keyframe timestamp (0 when empty).
  std::string frame_id;
  PJ::sdk::Pose origin;
  double resolution = 0.0;
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<int8_t> cells;

  [[nodiscard]] bool empty() const {
    return cells.empty();
  }
};

/// Axis-aligned rectangle of cells, for incremental texture upload.
struct CellRect {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

/// Result of reconstructAt(): the grid plus how it changed, so the renderer can
/// pick a full vs. incremental texture upload without querying stashed state.
/// `grid` and `dirty` stay valid only until the next reconstructAt() call.
struct GridUpdate {
  /// How the grid changed since the previous reconstructAt().
  enum class Kind {
    kFull,         ///< Whole grid rebuilt (new epoch or backward seek) — full upload.
    kIncremental,  ///< Only `dirty` rects changed (forward seek) — partial upload.
    kEmpty,        ///< No base grid at this time — nothing to display.
  };

  const ReconstructedGrid& grid;
  Kind kind = Kind::kFull;
  std::span<const CellRect> dirty;  ///< Changed rects; meaningful only for Incremental.
};

/// Reconstructs the displayed occupancy grid at any time `t` from a base keyframe stream plus an
/// `OccupancyGridUpdate` delta stream, scrubbing forward or backward.
///
/// Pure: no Qt / GL / ObjectStore; the timeline is injected through two std::function providers.
///
///   displayed(t) = (latest base grid with ts <= t) + every update in (that base's ts, t], applied in ascending ts
///                  order.
///
/// reconstructAt(t) is order-independent PROVIDED the injected timeline is immutable between calls; if entries land
/// retroactively (live ingest behind the consumed cursor, dataset replace), call invalidate() first. Forward scrubbing
/// applies only the new deltas; a backward / jump / new-epoch seek restores the nearest snapshot (or the pristine
/// base) and replays a bounded number of deltas. The snapshot cache only bounds memory and time: a miss falls back
/// to base + full replay, so the result never depends on the cache.
class OccupancyGridReconstructor {
 public:
  /// One base keyframe plus the never-reused identity of the timeline entry it
  /// was decoded from (the ObjectStore SequentialUID value in production; any
  /// unique token in tests). The epoch check keys on `identity`, NOT the grid's
  /// decoded timestamp: the store permits duplicate timestamps, so a
  /// same-timestamp replacement keyframe must still start a new epoch.
  struct BaseSample {
    PJ::sdk::OccupancyGrid grid;
    uint64_t identity = 0;
  };
  /// Latest full grid with `ts <= t`, or nullopt if none exists yet.
  using BaseProvider = std::function<std::optional<BaseSample>(PJ::Timestamp t)>;
  /// Updates with `lo < ts <= hi`, returned in ascending ts order.
  using UpdatesProvider = std::function<std::vector<PJ::sdk::OccupancyGridUpdate>(PJ::Timestamp lo, PJ::Timestamp hi)>;

  static constexpr std::size_t kDefaultSnapshotBudgetBytes = 64u * 1024u * 1024u;  // 64 MiB

  /// Upper bound on width*height accepted from a base keyframe. Wire dims are
  /// untrusted (the canonical codec does no dims-vs-payload cross-check), so a
  /// corrupt file can declare absurd dims; anything beyond 256M cells (256 MB
  /// at one byte per cell) is treated as corrupt and yields Kind::Empty.
  static constexpr std::size_t kMaxGridCells = 256u * 1024u * 1024u;

  explicit OccupancyGridReconstructor(std::size_t snapshot_budget_bytes = kDefaultSnapshotBudgetBytes);

  /// Reconstructs the grid as displayed at `t` and describes how it changed; the returned grid/dirty references are
  /// valid until the next call.
  GridUpdate reconstructAt(PJ::Timestamp t, const BaseProvider& base_at, const UpdatesProvider& updates_in);

  /// Discards ALL cached epoch state (current grid, pristine base copy, every snapshot: snapshots may embed updates
  /// applied outside the live window and later superseded). Call when the injected timeline changed retroactively;
  /// the next reconstructAt() rebuilds from base + full replay. Invalidates the grid/dirty references returned by the
  /// previous reconstructAt().
  void invalidate();

  /// Partial invalidation for a retroactive entry at `late_ts` (<= the consumed forward cursor), keeping snapshots:
  /// one taken at S < late_ts embeds only updates with ts <= S, none of them the late entry, so snapshots strictly
  /// before `late_ts` stay valid and those with ts >= late_ts are dropped. The forward cursor (`last_t_`) rewinds to
  /// the nearest surviving snapshot at or before `late_ts` (or the pristine base), bounding the next replay to the
  /// window since `late_ts`. No-op without an epoch; a `late_ts` at or before the base keyframe degrades to
  /// invalidate(). Leaves `have_epoch_` set, so the next reconstructAt() takes the rewound forward path, not a
  /// new-epoch reset.
  void invalidateAfter(PJ::Timestamp late_ts);

  /// The current grid (as of the last reconstructAt()); valid until the next call.
  [[nodiscard]] const ReconstructedGrid& grid() const {
    return grid_;
  }

  [[nodiscard]] std::size_t snapshotCount() const {
    return snapshots_.size();
  }
  [[nodiscard]] std::size_t snapshotBytes() const;

 private:
  struct Snapshot {
    PJ::Timestamp ts = 0;
    std::vector<int8_t> cells;
  };

  void resetToBase(const PJ::sdk::OccupancyGrid& base);  // new epoch: clears snapshots
  void applyUpdate(const PJ::sdk::OccupancyGridUpdate& update);
  void applyRange(const UpdatesProvider& updates_in, PJ::Timestamp lo, PJ::Timestamp hi, bool allow_snapshots);
  void restoreNearestAtOrBefore(PJ::Timestamp t);  // backward seek within the epoch
  void pushSnapshot(PJ::Timestamp ts);
  [[nodiscard]] std::size_t gridBytes() const {
    return static_cast<std::size_t>(grid_.width) * grid_.height;
  }

  std::size_t snapshot_budget_bytes_;
  ReconstructedGrid grid_;
  std::vector<int8_t> base_cells_;  // pristine keyframe, retained for backward replay
  PJ::Timestamp last_t_ = 0;
  bool have_epoch_ = false;
  uint64_t epoch_identity_ = 0;      // BaseSample::identity of the current epoch's keyframe
  std::vector<Snapshot> snapshots_;  // ascending ts (pushSnapshot inserts sorted), excludes the base keyframe
  std::size_t updates_since_snapshot_ = 0;
  std::vector<CellRect> dirty_rects_;  // changed rects of the last reconstructAt()
};

}  // namespace pj::scene3d
