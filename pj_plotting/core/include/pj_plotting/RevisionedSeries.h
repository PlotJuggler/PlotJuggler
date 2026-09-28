#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <atomic>
#include <cstdint>

namespace PJ {

// A plot series (a QwtSeriesData) whose served samples change only together with
// contentRevision(): every path that can change what size()/sample() return,
// including a new rect of interest, bumps it first. Plot canvases use it to keep
// retained geometry across a replot of unchanged data; a series without it is
// rebuilt on every replot. Implemented by the PJ-owned adapters.
//
// Revisions are unique for the process lifetime, not per series: a canvas keys
// retained geometry on the revisions alone, so a series freed and replaced by a
// new one at the same address (a transform applied in place) cannot repeat the
// old key. A series inherits this class and calls bumpContentRevision().
class RevisionedSeries {
 public:
  virtual ~RevisionedSeries() = default;
  [[nodiscard]] std::uint64_t contentRevision() const noexcept {
    return revision_;
  }

 protected:
  /// Marks the served samples changed: a revision no series in this process has
  /// had. Thread-safe.
  void bumpContentRevision() noexcept {
    revision_ = nextRevision();
  }

 private:
  [[nodiscard]] static std::uint64_t nextRevision() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  std::uint64_t revision_ = nextRevision();
};

}  // namespace PJ
