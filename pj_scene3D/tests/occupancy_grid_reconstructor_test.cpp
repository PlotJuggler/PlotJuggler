// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_core/occupancy_grid_reconstructor.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <vector>

#include "pj_base/span.hpp"

namespace PJ {
namespace {

using pj::scene3d::GridUpdate;
using pj::scene3d::OccupancyGridReconstructor;
using pj::scene3d::ReconstructedGrid;

// --- Builders: own the cell bytes via a shared_ptr anchor so the Span stays valid. ---

sdk::OccupancyGrid makeBase(Timestamp ts, uint32_t w, uint32_t h, std::vector<uint8_t> bytes) {
  auto buf = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
  sdk::OccupancyGrid g;
  g.timestamp_ns = ts;
  g.frame_id = "map";
  g.resolution = 0.05;
  g.width = w;
  g.height = h;
  g.data = Span<const uint8_t>(buf->data(), buf->size());
  g.anchor = buf;
  return g;
}

sdk::OccupancyGridUpdate makeUpdate(
    Timestamp ts, int32_t x, int32_t y, uint32_t w, uint32_t h, std::vector<uint8_t> bytes) {
  auto buf = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
  sdk::OccupancyGridUpdate u;
  u.timestamp_ns = ts;
  u.frame_id = "map";
  u.x = x;
  u.y = y;
  u.width = w;
  u.height = h;
  u.data = Span<const uint8_t>(buf->data(), buf->size());
  u.anchor = buf;
  return u;
}

// --- Independent reference: cell-by-cell replay (different code path than the
//     reconstructor's row-memcpy), so equivalence is a genuine cross-check. ---

std::vector<int8_t> refReplay(
    const sdk::OccupancyGrid& base, const std::vector<sdk::OccupancyGridUpdate>& updates, Timestamp t) {
  const int64_t gw = base.width;
  const int64_t gh = base.height;
  std::vector<int8_t> cells(static_cast<std::size_t>(gw * gh), static_cast<int8_t>(-1));
  const std::size_t copy_n = std::min(cells.size(), base.data.size());
  for (std::size_t i = 0; i < copy_n; ++i) {
    cells[i] = static_cast<int8_t>(base.data.data()[i]);
  }
  for (const auto& u : updates) {
    if (u.timestamp_ns <= base.timestamp_ns || u.timestamp_ns > t) {
      continue;
    }
    for (uint32_t r = 0; r < u.height; ++r) {
      for (uint32_t c = 0; c < u.width; ++c) {
        const int64_t y = static_cast<int64_t>(u.y) + r;
        const int64_t x = static_cast<int64_t>(u.x) + c;
        if (x < 0 || y < 0 || x >= gw || y >= gh) {
          continue;
        }
        cells[static_cast<std::size_t>(y * gw + x)] =
            static_cast<int8_t>(u.data.data()[static_cast<std::size_t>(r) * u.width + c]);
      }
    }
  }
  return cells;
}

// --- Providers over in-memory timelines. ---

// BaseSample::identity is the base's position in the timeline vector (+1, so 0
// stays "none") — a unique per-entry token, standing in for the production
// SequentialUID. Equal-ts bases: the LAST one wins (>=), matching the store's
// indexAtOrBefore pick over duplicate timestamps.
OccupancyGridReconstructor::BaseProvider baseProviderFor(const std::vector<sdk::OccupancyGrid>& bases) {
  return [&bases](Timestamp t) -> std::optional<OccupancyGridReconstructor::BaseSample> {
    std::optional<OccupancyGridReconstructor::BaseSample> best;
    for (size_t i = 0; i < bases.size(); ++i) {
      const auto& b = bases[i];
      if (b.timestamp_ns <= t && (!best || b.timestamp_ns >= best->grid.timestamp_ns)) {
        best = OccupancyGridReconstructor::BaseSample{b, static_cast<uint64_t>(i) + 1};
      }
    }
    return best;
  };
}

OccupancyGridReconstructor::UpdatesProvider updatesProviderFor(const std::vector<sdk::OccupancyGridUpdate>& updates) {
  return [&updates](Timestamp lo, Timestamp hi) {
    std::vector<sdk::OccupancyGridUpdate> out;
    for (const auto& u : updates) {
      if (u.timestamp_ns > lo && u.timestamp_ns <= hi) {
        out.push_back(u);
      }
    }
    // stable_sort: equal-ts updates keep insertion order, matching refReplay's
    // vector-order application so the cross-check stays well-defined.
    std::stable_sort(out.begin(), out.end(), [](const sdk::OccupancyGridUpdate& a, const sdk::OccupancyGridUpdate& b) {
      return a.timestamp_ns < b.timestamp_ns;
    });
    return out;
  };
}

// Build a base + N in-bounds updates with deterministic pseudo-random placement.
struct Timeline {
  std::vector<sdk::OccupancyGrid> bases;
  std::vector<sdk::OccupancyGridUpdate> updates;
};

Timeline makeTimeline(uint32_t grid = 16, int n_updates = 200, uint32_t seed = 12345) {
  Timeline tl;
  std::vector<uint8_t> base_bytes(static_cast<std::size_t>(grid) * grid);
  for (std::size_t i = 0; i < base_bytes.size(); ++i) {
    base_bytes[i] = static_cast<uint8_t>(i % 101);
  }
  tl.bases.push_back(makeBase(1000, grid, grid, std::move(base_bytes)));

  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> pos(0, static_cast<int>(grid) - 3);
  std::uniform_int_distribution<int> dim(1, 3);
  for (int i = 1; i <= n_updates; ++i) {
    const uint32_t w = static_cast<uint32_t>(dim(rng));
    const uint32_t h = static_cast<uint32_t>(dim(rng));
    const int32_t x = pos(rng);
    const int32_t y = pos(rng);
    std::vector<uint8_t> bytes(static_cast<std::size_t>(w) * h, static_cast<uint8_t>(i % 101));
    tl.updates.push_back(makeUpdate(1000 + 10LL * i, x, y, w, h, std::move(bytes)));
  }
  return tl;
}

TEST(OccupancyGridReconstructorTest, BaseOnlyNoUpdates) {
  const Timeline tl = makeTimeline(8, 0);
  OccupancyGridReconstructor r;
  const GridUpdate update = r.reconstructAt(2000, baseProviderFor(tl.bases), updatesProviderFor(tl.updates));
  const ReconstructedGrid& g = update.grid;
  EXPECT_EQ(g.width, 8u);
  EXPECT_EQ(g.height, 8u);
  EXPECT_EQ(g.cells, refReplay(tl.bases.front(), tl.updates, 2000));
  // First call with a base in effect → a full (re)build, not incremental.
  EXPECT_EQ(update.kind, GridUpdate::Kind::kFull);
}

// A replacement base at the SAME timestamp — a NEW timeline entry (fresh
// identity), possible because the store permits duplicate stamps — must start a
// new epoch. Keyed on the decoded timestamp alone it would be invisible and the
// forward path would keep the old keyframe's cells.
TEST(OccupancyGridReconstructorTest, SameTimestampReplacementBaseResetsEpoch) {
  std::vector<sdk::OccupancyGrid> bases;
  bases.push_back(makeBase(1000, 4, 4, std::vector<uint8_t>(16, 10)));
  const std::vector<sdk::OccupancyGridUpdate> no_updates;
  const auto up = updatesProviderFor(no_updates);
  OccupancyGridReconstructor r;
  ASSERT_EQ(r.reconstructAt(1500, baseProviderFor(bases), up).grid.cells[0], static_cast<int8_t>(10));

  bases.push_back(makeBase(1000, 4, 4, std::vector<uint8_t>(16, 33)));  // same stamp, new entry
  const GridUpdate update = r.reconstructAt(1500, baseProviderFor(bases), up);
  EXPECT_EQ(update.kind, GridUpdate::Kind::kFull) << "replacement base must rebuild, not increment";
  EXPECT_EQ(update.grid.cells[0], static_cast<int8_t>(33))
      << "same-timestamp replacement keyframe did not reset the epoch";
}

TEST(OccupancyGridReconstructorTest, NoBaseYieldsEmpty) {
  const Timeline tl = makeTimeline(8, 5);
  OccupancyGridReconstructor r;
  const GridUpdate update = r.reconstructAt(500, baseProviderFor(tl.bases), updatesProviderFor(tl.updates));
  EXPECT_TRUE(update.grid.empty());
  EXPECT_EQ(update.kind, GridUpdate::Kind::kEmpty);
}

// THE crux: any sequence of scrub targets (forward, backward, jumps, repeats)
// yields the same grid as an independent from-scratch replay for that time.
TEST(OccupancyGridReconstructorTest, SeekEquivalenceUnderRandomScrubbing) {
  const Timeline tl = makeTimeline(16, 200);
  const auto base_provider = baseProviderFor(tl.bases);
  const auto updates_provider = updatesProviderFor(tl.updates);
  OccupancyGridReconstructor r;

  std::mt19937 rng(999);
  std::uniform_int_distribution<Timestamp> time_dist(900, 1000 + 10 * 210);
  for (int iter = 0; iter < 400; ++iter) {
    const Timestamp t = time_dist(rng);
    const ReconstructedGrid& g = r.reconstructAt(t, base_provider, updates_provider).grid;
    if (base_provider(t)) {
      EXPECT_EQ(g.cells, refReplay(tl.bases.front(), tl.updates, t)) << "mismatch at t=" << t << " iter=" << iter;
    } else {
      EXPECT_TRUE(g.empty()) << "expected empty before base at t=" << t;
    }
  }
}

TEST(OccupancyGridReconstructorTest, BackwardSeekMatchesReplay) {
  const Timeline tl = makeTimeline(16, 150);
  const auto bp = baseProviderFor(tl.bases);
  const auto up = updatesProviderFor(tl.updates);
  OccupancyGridReconstructor r;
  // Go to the end, then step backward to several earlier times.
  r.reconstructAt(1000 + 10 * 151, bp, up);
  for (const Timestamp t :
       {Timestamp{1000 + 10 * 120}, Timestamp{1000 + 10 * 40}, Timestamp{1000 + 10 * 5}, Timestamp{1000}}) {
    const ReconstructedGrid& g = r.reconstructAt(t, bp, up).grid;
    EXPECT_EQ(g.cells, refReplay(tl.bases.front(), tl.updates, t)) << "backward mismatch at t=" << t;
  }
}

TEST(OccupancyGridReconstructorTest, NewEpochResetsAndIsReversible) {
  Timeline tl = makeTimeline(8, 20);
  // A second, differently-sized base keyframe at a later time.
  std::vector<uint8_t> b2(6u * 6u, 42u);
  tl.bases.push_back(makeBase(5000, 6, 6, std::move(b2)));
  const auto bp = baseProviderFor(tl.bases);
  const auto up = updatesProviderFor(tl.updates);
  OccupancyGridReconstructor r;

  const ReconstructedGrid& g_after = r.reconstructAt(6000, bp, up).grid;
  EXPECT_EQ(g_after.width, 6u);
  EXPECT_EQ(g_after.cells, refReplay(tl.bases.back(), tl.updates, 6000));

  // Seek back into the first epoch.
  const ReconstructedGrid& g_before = r.reconstructAt(1000 + 10 * 10, bp, up).grid;
  EXPECT_EQ(g_before.width, 8u);
  EXPECT_EQ(g_before.cells, refReplay(tl.bases.front(), tl.updates, 1000 + 10 * 10));
}

TEST(OccupancyGridReconstructorTest, OutOfBoundsPatchClampedNoCrash) {
  std::vector<sdk::OccupancyGrid> bases{makeBase(1000, 8, 8, std::vector<uint8_t>(64, 0))};
  std::vector<sdk::OccupancyGridUpdate> updates;
  // Patch straddling the top-left corner and the right edge.
  updates.push_back(makeUpdate(1100, -2, -2, 4, 4, std::vector<uint8_t>(16, 50)));
  updates.push_back(makeUpdate(1200, 6, 6, 5, 5, std::vector<uint8_t>(25, 75)));
  OccupancyGridReconstructor r;
  const ReconstructedGrid& g = r.reconstructAt(1300, baseProviderFor(bases), updatesProviderFor(updates)).grid;
  EXPECT_EQ(g.cells, refReplay(bases.front(), updates, 1300));
}

TEST(OccupancyGridReconstructorTest, SnapshotMemoryBudgetHonored) {
  const Timeline tl = makeTimeline(32, 1000);  // 32*32 = 1024 B per snapshot
  const auto bp = baseProviderFor(tl.bases);
  const auto up = updatesProviderFor(tl.updates);
  OccupancyGridReconstructor r(/*snapshot_budget_bytes=*/4096);  // ≤ 4 snapshots
  // Drive a full forward pass to accumulate snapshots.
  r.reconstructAt(1000 + 10 * 1001, bp, up);
  EXPECT_LE(r.snapshotBytes(), 4096u);
  // Correctness is independent of the (now sparse) snapshot cache.
  for (const Timestamp t : {1000 + 10 * 800, 1000 + 10 * 300, 1000 + 10 * 50}) {
    EXPECT_EQ(r.reconstructAt(t, bp, up).grid.cells, refReplay(tl.bases.front(), tl.updates, t));
  }
}

// H.13 contract at the core boundary: when the timeline gains an entry BEHIND
// the consumed cursor (live ingest jitter), the forward fast-path cannot see it
// by design; invalidate() must force a full rebuild that picks it up.
TEST(OccupancyGridReconstructorTest, LateEntryBehindCursorForcesRebuild) {
  Timeline tl = makeTimeline(16, 50);  // updates at ts 1010..1500
  const auto bp = baseProviderFor(tl.bases);
  const auto up = updatesProviderFor(tl.updates);
  OccupancyGridReconstructor r;

  const Timestamp t_high = 1700;
  r.reconstructAt(t_high, bp, up);  // forward cursor now at t_high

  // The providers close over tl.updates by reference, so this append mutates
  // the live timeline mid-test: a retroactive entry at ts 1600 < t_high.
  const std::vector<sdk::OccupancyGridUpdate> updates_before = tl.updates;
  tl.updates.push_back(makeUpdate(1600, 5, 5, 3, 3, std::vector<uint8_t>(9, 99)));

  // Forward fast-path window (last_t_, t] is empty — the grid stays stale.
  EXPECT_EQ(r.reconstructAt(t_high, bp, up).grid.cells, refReplay(tl.bases.front(), updates_before, t_high));

  // invalidate() discards the epoch (and the snapshots that pre-date the late
  // entry): the next call rebuilds from base + full replay and includes it.
  r.invalidate();
  const GridUpdate rebuilt = r.reconstructAt(t_high, bp, up);
  EXPECT_EQ(rebuilt.kind, GridUpdate::Kind::kFull);
  EXPECT_EQ(rebuilt.grid.cells, refReplay(tl.bases.front(), tl.updates, t_high));
  // Sanity: the late update actually changes the grid, so the check above is meaningful.
  EXPECT_NE(rebuilt.grid.cells, refReplay(tl.bases.front(), updates_before, t_high));
}

// M.13: several updates share a timestamp straddling the snapshot stride (64).
// A snapshot taken mid-way through the equal-ts run captures only a prefix of
// the group, and the backward replay's exclusive lower bound (lo < ts) never
// re-applies the rest — the restored grid diverges from a from-scratch replay.
TEST(OccupancyGridReconstructorTest, DuplicateTimestampSnapshotBoundary) {
  constexpr uint32_t kGrid = 16;
  Timeline tl;
  tl.bases.push_back(makeBase(1000, kGrid, kGrid, std::vector<uint8_t>(kGrid * kGrid, 0)));
  // 100 updates in equal-ts groups of 5 (ts 1010, 1010, ..., 1020, ...): the
  // 64th update — where the stride-driven snapshot fires — falls mid-group.
  for (int i = 1; i <= 100; ++i) {
    const Timestamp ts = 1000 + 10 * ((i - 1) / 5 + 1);
    const int32_t x = (i * 3) % (kGrid - 2);
    const int32_t y = (i * 7) % (kGrid - 2);
    tl.updates.push_back(makeUpdate(ts, x, y, 2, 2, std::vector<uint8_t>(4, static_cast<uint8_t>(i))));
  }
  const auto bp = baseProviderFor(tl.bases);
  const auto up = updatesProviderFor(tl.updates);

  OccupancyGridReconstructor r;
  r.reconstructAt(2000, bp, up);  // forward pass applies all 100, snapshotting en route

  // Backward seeks landing on / just after the straddled group (ts 1130) must
  // match the reference replay, which includes the WHOLE equal-ts group.
  for (const Timestamp t : {Timestamp{1130}, Timestamp{1135}, Timestamp{1090}, Timestamp{1010}}) {
    EXPECT_EQ(r.reconstructAt(t, bp, up).grid.cells, refReplay(tl.bases.front(), tl.updates, t))
        << "backward mismatch at t=" << t;
  }
}

// M.11/M.12: wire dims are untrusted — absurd width*height must yield Empty
// instead of throwing a multi-exabyte allocation through the GUI thread, and
// must not poison reconstruction of a later sane base on the same timeline.
TEST(OccupancyGridReconstructorTest, OversizedDimsYieldEmpty) {
  std::vector<sdk::OccupancyGrid> bases{makeBase(1000, 0xFFFFFFFFu, 0xFFFFFFFFu, std::vector<uint8_t>(16, 7))};
  std::vector<sdk::OccupancyGridUpdate> updates;
  const auto bp = baseProviderFor(bases);
  const auto up = updatesProviderFor(updates);
  OccupancyGridReconstructor r;

  const GridUpdate update = r.reconstructAt(2000, bp, up);
  EXPECT_EQ(update.kind, GridUpdate::Kind::kEmpty);
  EXPECT_TRUE(update.grid.empty());

  // A sane base later on the same timeline still reconstructs.
  bases.push_back(makeBase(3000, 4, 4, std::vector<uint8_t>(16, 25)));
  const GridUpdate recovered = r.reconstructAt(3500, bp, up);
  EXPECT_EQ(recovered.kind, GridUpdate::Kind::kFull);
  EXPECT_EQ(recovered.grid.cells, refReplay(bases.back(), updates, 3500));
}

}  // namespace
}  // namespace PJ
