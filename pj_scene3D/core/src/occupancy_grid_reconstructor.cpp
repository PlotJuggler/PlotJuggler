// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_core/occupancy_grid_reconstructor.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <iterator>

namespace pj::scene3d {

namespace {
// Updates applied between successive snapshots (subject to the memory budget,
// which decimates the cache when exceeded). Tunes backward-seek replay cost vs.
// snapshot memory; correctness is independent of this value.
constexpr std::size_t kSnapshotStride = 64;
}  // namespace

OccupancyGridReconstructor::OccupancyGridReconstructor(std::size_t snapshot_budget_bytes)
    : snapshot_budget_bytes_(snapshot_budget_bytes) {}

void OccupancyGridReconstructor::invalidate() {
  grid_ = ReconstructedGrid{};
  base_cells_.clear();
  snapshots_.clear();
  dirty_rects_.clear();
  updates_since_snapshot_ = 0;
  have_epoch_ = false;
  last_t_ = 0;
}

void OccupancyGridReconstructor::invalidateAfter(PJ::Timestamp late_ts) {
  if (!have_epoch_) {
    return;
  }
  // A late entry at or before the base keyframe can't be re-included by a forward
  // replay within this epoch (applyRange's lower bound is the base ts), so fall
  // back to the full from-base rebuild on the next reconstructAt().
  if (late_ts <= grid_.base_timestamp_ns) {
    invalidate();
    return;
  }
  // Drop every snapshot that could have skipped the late entry: a snapshot at
  // ts >= late_ts was taken when the entry might already have been due but was
  // not yet ingested. Snapshots strictly before late_ts only embed updates that
  // genuinely predate it, so they remain trustworthy restore points.
  const auto first_invalid = std::lower_bound(
      snapshots_.begin(), snapshots_.end(), late_ts,
      [](const Snapshot& snapshot, PJ::Timestamp value) { return snapshot.ts < value; });
  snapshots_.erase(first_invalid, snapshots_.end());
  updates_since_snapshot_ = 0;
  // Rewind the forward cursor to the nearest surviving snapshot at-or-before the
  // late entry (or the pristine base). The next forward reconstructAt() replays
  // (last_t_, t] — which now re-includes the late entry and everything after it.
  restoreNearestAtOrBefore(late_ts - 1);
}

std::size_t OccupancyGridReconstructor::snapshotBytes() const {
  std::size_t total = 0;
  for (const auto& s : snapshots_) {
    total += s.cells.size();
  }
  return total;
}

void OccupancyGridReconstructor::resetToBase(const PJ::sdk::OccupancyGrid& base) {
  grid_.base_timestamp_ns = base.timestamp_ns;
  grid_.frame_id = base.frame_id;
  grid_.origin = base.origin;
  grid_.resolution = base.resolution;
  grid_.width = base.width;
  grid_.height = base.height;

  const std::size_t n = gridBytes();
  grid_.cells.assign(n, static_cast<int8_t>(-1));  // unknown by default if data is short
  const std::size_t copy_n = std::min(n, base.data.size());
  if (copy_n > 0) {
    std::memcpy(grid_.cells.data(), base.data.data(), copy_n);  // uint8 → int8, bit-identical
  }
  base_cells_ = grid_.cells;  // retain pristine keyframe for backward replay

  snapshots_.clear();
  updates_since_snapshot_ = 0;
  have_epoch_ = true;
  last_t_ = base.timestamp_ns;
}

void OccupancyGridReconstructor::applyUpdate(const PJ::sdk::OccupancyGridUpdate& update) {
  if (grid_.width == 0 || grid_.height == 0 || update.width == 0 || update.height == 0) {
    return;
  }
  // Guard against a malformed patch whose data is shorter than width*height.
  const std::size_t patch_cells = static_cast<std::size_t>(update.width) * update.height;
  if (update.data.size() < patch_cells) {
    return;
  }

  const int64_t gw = grid_.width;
  const int64_t gh = grid_.height;
  // Clamp the patch rectangle to the grid bounds.
  const int64_t x0 = std::max<int64_t>(0, update.x);
  const int64_t y0 = std::max<int64_t>(0, update.y);
  const int64_t x1 = std::min<int64_t>(gw, static_cast<int64_t>(update.x) + update.width);
  const int64_t y1 = std::min<int64_t>(gh, static_cast<int64_t>(update.y) + update.height);
  if (x1 <= x0 || y1 <= y0) {
    return;  // fully out of bounds
  }
  const int64_t copy_w = x1 - x0;

  for (int64_t y = y0; y < y1; ++y) {
    const int64_t src_row = y - update.y;   // row within the patch
    const int64_t src_col = x0 - update.x;  // column within the patch
    const uint8_t* src = update.data.data() + static_cast<std::size_t>(src_row * update.width + src_col);
    int8_t* dst = grid_.cells.data() + static_cast<std::size_t>(y * gw + x0);
    std::memcpy(dst, src, static_cast<std::size_t>(copy_w));
  }

  dirty_rects_.push_back(
      CellRect{
          static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), static_cast<uint32_t>(copy_w),
          static_cast<uint32_t>(y1 - y0)});
}

void OccupancyGridReconstructor::pushSnapshot(PJ::Timestamp ts) {
  // A forward pass that follows a backward seek can emit a snapshot OLDER than
  // already-cached ones: insert in ts order so restoreNearestAtOrBefore can
  // binary-search. An equal-ts snapshot would be byte-identical state — skip it.
  const auto pos = std::upper_bound(
      snapshots_.begin(), snapshots_.end(), ts,
      [](PJ::Timestamp value, const Snapshot& snapshot) { return value < snapshot.ts; });
  if (pos != snapshots_.begin() && std::prev(pos)->ts == ts) {
    return;
  }
  snapshots_.insert(pos, Snapshot{ts, grid_.cells});

  // Honor the memory budget: while over budget, decimate (drop every other
  // snapshot) so the survivors stay evenly spread across the timeline rather
  // than clustering — keeping worst-case backward-replay distance bounded.
  const std::size_t per = gridBytes();
  if (per == 0) {
    return;
  }
  while (snapshotBytes() > snapshot_budget_bytes_ && snapshots_.size() > 1) {
    std::vector<Snapshot> kept;
    kept.reserve(snapshots_.size() / 2 + 1);
    for (std::size_t i = 0; i < snapshots_.size(); i += 2) {
      kept.push_back(std::move(snapshots_[i]));
    }
    snapshots_ = std::move(kept);
  }
}

void OccupancyGridReconstructor::applyRange(
    const UpdatesProvider& updates_in, PJ::Timestamp lo, PJ::Timestamp hi, bool allow_snapshots) {
  if (hi <= lo) {
    return;
  }
  const std::vector<PJ::sdk::OccupancyGridUpdate> ups = updates_in(lo, hi);
  for (std::size_t i = 0; i < ups.size(); ++i) {
    const PJ::sdk::OccupancyGridUpdate& update = ups[i];
    applyUpdate(update);
    if (!allow_snapshots || ++updates_since_snapshot_ < kSnapshotStride) {
      continue;
    }
    // Defer the snapshot to a timestamp boundary: taken mid-way through a run of
    // equal-ts updates it would capture only a prefix of the group, and the
    // backward replay's exclusive lower bound (lo < ts) would never re-apply the
    // rest. The counter carries over, so the snapshot lands at the next boundary.
    const bool mid_equal_ts_run = (i + 1 < ups.size()) && ups[i + 1].timestamp_ns == update.timestamp_ns;
    if (!mid_equal_ts_run) {
      pushSnapshot(update.timestamp_ns);
      updates_since_snapshot_ = 0;
    }
  }
}

void OccupancyGridReconstructor::restoreNearestAtOrBefore(PJ::Timestamp t) {
  // snapshots_ is ts-sorted (pushSnapshot inserts in order): binary-search the
  // largest snapshot ts <= t, else fall back to the pristine base keyframe. The
  // full-buffer copy is the documented memory-vs-replay tradeoff of the design.
  const auto first_after = std::upper_bound(
      snapshots_.begin(), snapshots_.end(), t,
      [](PJ::Timestamp value, const Snapshot& snapshot) { return value < snapshot.ts; });
  if (first_after != snapshots_.begin()) {
    const Snapshot& best = *std::prev(first_after);
    grid_.cells = best.cells;
    last_t_ = best.ts;
  } else {
    grid_.cells = base_cells_;
    last_t_ = grid_.base_timestamp_ns;
  }
}

GridUpdate OccupancyGridReconstructor::reconstructAt(
    PJ::Timestamp t, const BaseProvider& base_at, const UpdatesProvider& updates_in) {
  dirty_rects_.clear();

  const std::optional<BaseSample> base = base_at(t);
  if (!base) {
    // No base grid at or before t — nothing to display.
    invalidate();
    return GridUpdate{grid_, GridUpdate::Kind::kEmpty, {}};
  }

  // Wire dims are untrusted (the canonical codec performs no width*height vs
  // payload cross-check): cap the cell count BEFORE resetToBase() allocates
  // from it, so a corrupt file cannot trigger a multi-GB / throwing assign.
  if (static_cast<uint64_t>(base->grid.width) * base->grid.height > kMaxGridCells) {
    invalidate();
    return GridUpdate{grid_, GridUpdate::Kind::kEmpty, {}};
  }

  // Exception barrier: this runs on the GUI thread (renderAt → reconstructAt);
  // an escaped bad_alloc/length_error from a hostile file would unwind through
  // the Qt event loop and terminate the app (same hazard class as the codec
  // barriers in pointcloud_codecs.cpp). Reset so one corrupt sample cannot
  // poison subsequent frames either.
  try {
    if (!have_epoch_ || base->identity != epoch_identity_) {
      // New epoch: a different (or first) base keyframe entry is now in effect.
      // Keyed on the entry identity, not the decoded timestamp — a replacement
      // keyframe at the SAME timestamp must still reset (see BaseSample).
      resetToBase(base->grid);
      epoch_identity_ = base->identity;
      applyRange(updates_in, grid_.base_timestamp_ns, t, /*allow_snapshots=*/true);
      last_t_ = t;
      return GridUpdate{grid_, GridUpdate::Kind::kFull, dirty_rects_};
    }

    if (t >= last_t_) {
      // Forward within the epoch: apply only the new deltas incrementally.
      applyRange(updates_in, last_t_, t, /*allow_snapshots=*/true);
      last_t_ = t;
      return GridUpdate{grid_, GridUpdate::Kind::kIncremental, dirty_rects_};
    }

    // Backward within the epoch: restore the nearest snapshot (or base) and replay.
    restoreNearestAtOrBefore(t);
    applyRange(updates_in, last_t_, t, /*allow_snapshots=*/false);
    last_t_ = t;
    return GridUpdate{grid_, GridUpdate::Kind::kFull, dirty_rects_};
  } catch (const std::exception&) {
    invalidate();
    return GridUpdate{grid_, GridUpdate::Kind::kEmpty, {}};
  } catch (...) {
    invalidate();
    return GridUpdate{grid_, GridUpdate::Kind::kEmpty, {}};
  }
}

}  // namespace pj::scene3d
