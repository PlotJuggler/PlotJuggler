// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/DatastoreCurveAdapter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "pj_base/dataset.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"

namespace PJ {
namespace {

[[nodiscard]] QPointF invalidPoint() {
  return {0.0, std::numeric_limits<double>::quiet_NaN()};
}

[[nodiscard]] QRectF invalidRect() {
  return {1.0, 1.0, -2.0, -2.0};
}

// Thin Qwt-boundary adapters over the canonical conversions in Time.h, so the
// (raw - offset)/1e9 arithmetic lives in exactly one place. Caller is responsible
// for finite-checking display_sec — Qwt rects from real viewports are always
// bounded; the upstream call sites guard against NaN/inf.
[[nodiscard]] Timestamp displaySecondsToRawNs(double display_sec, DisplayOffset offset) noexcept {
  return displaySecondsToRaw(fromAxisDouble(display_sec), offset);
}

[[nodiscard]] double rawNsToDisplaySeconds(Timestamp raw_ns, DisplayOffset offset) noexcept {
  return toAxisDouble(rawToDisplaySeconds(raw_ns, offset));
}

[[nodiscard]] bool isAllRowsWindow(Timestamp t_min, Timestamp t_max) noexcept {
  return t_min == std::numeric_limits<Timestamp>::min() && t_max == std::numeric_limits<Timestamp>::max();
}

// Saturating widen helpers in unsigned space: a finite viewport wider than
// half the Timestamp domain (~±292 years in ns) must not overflow int64.
[[nodiscard]] Timestamp saturatingSubtract(Timestamp value, std::uint64_t amount) noexcept {
  constexpr Timestamp kMin = std::numeric_limits<Timestamp>::min();
  const auto headroom = static_cast<std::uint64_t>(value) - static_cast<std::uint64_t>(kMin);
  return amount >= headroom ? kMin : static_cast<Timestamp>(static_cast<std::uint64_t>(value) - amount);
}

[[nodiscard]] Timestamp saturatingAdd(Timestamp value, std::uint64_t amount) noexcept {
  constexpr Timestamp kMax = std::numeric_limits<Timestamp>::max();
  const auto headroom = static_cast<std::uint64_t>(kMax) - static_cast<std::uint64_t>(value);
  return amount >= headroom ? kMax : static_cast<Timestamp>(static_cast<std::uint64_t>(value) + amount);
}

}  // namespace

DatastoreCurveAdapter::DatastoreCurveAdapter(SessionManager* session, CurveDescriptor source)
    : session_(session), source_(std::move(source)), cached_full_bounding_rect_(invalidRect()) {}

std::size_t DatastoreCurveAdapter::size() const {
  ensureSampleWindow();
  return window_count_;
}

QPointF DatastoreCurveAdapter::sample(std::size_t index) const {
  ensureSampleWindow();
  if (index >= window_count_) {
    return invalidPoint();
  }

  const CachedSample& cached = window_samples_[window_offset_ + index];
  return toDisplayPoint(cached.timestamp, cached.value);
}

QRectF DatastoreCurveAdapter::boundingRect() const {
  if (full_bounding_rect_valid_) {
    return cached_full_bounding_rect_;
  }

  cached_full_bounding_rect_ = invalidRect();
  full_bounding_rect_valid_ = true;

  if (session_ == nullptr) {
    return cached_full_bounding_rect_;
  }

  auto series_or = session_->createReader().series(source_.topic_id, source_.column_index);
  if (!series_or.has_value()) {
    return cached_full_bounding_rect_;
  }

  const auto bounds = series_or->bounds();
  if (!bounds.has_value()) {
    return cached_full_bounding_rect_;
  }

  const DisplayOffset offset = displayOffsetNow();
  const double x_min = rawNsToDisplaySeconds(bounds->time.min, offset);
  const double x_max = rawNsToDisplaySeconds(bounds->time.max, offset);
  cached_full_bounding_rect_ =
      QRectF(QPointF(x_min, bounds->value.min), QPointF(x_max, bounds->value.max)).normalized();
  return cached_full_bounding_rect_;
}

void DatastoreCurveAdapter::setRectOfInterest(const QRectF& rect) {
  Timestamp next_min = 0;
  Timestamp next_max = 0;
  if (!std::isfinite(rect.left()) || !std::isfinite(rect.right())) {
    // Non-finite rect bounds (NaN / inf) — fall back to the all-rows window so
    // size()/sample() stay valid. Qwt occasionally hands us sentinel rects on
    // first paint or while axes are being reconfigured.
    next_min = std::numeric_limits<Timestamp>::min();
    next_max = std::numeric_limits<Timestamp>::max();
  } else {
    const DisplayOffset offset = displayOffsetNow();
    const Timestamp left = displaySecondsToRawNs(rect.left(), offset);
    const Timestamp right = displaySecondsToRawNs(rect.right(), offset);
    next_min = std::min(left, right);
    next_max = std::max(left, right);
  }

  if (visible_t_min_raw_ns_ == next_min && visible_t_max_raw_ns_ == next_max) {
    return;
  }

  visible_t_min_raw_ns_ = next_min;
  visible_t_max_raw_ns_ = next_max;
  window_dirty_ = true;
}

std::optional<Range<double>> DatastoreCurveAdapter::visibleYRange(Range<double> x_range_sec) const {
  if (session_ == nullptr) {
    return std::nullopt;
  }

  auto series_or = session_->createReader().series(source_.topic_id, source_.column_index);
  if (!series_or.has_value()) {
    return std::nullopt;
  }

  std::optional<SeriesBounds> bounds;
  if (!std::isfinite(x_range_sec.min) || !std::isfinite(x_range_sec.max)) {
    bounds = series_or->bounds();
  } else {
    const DisplayOffset offset = displayOffsetNow();
    const Timestamp raw_a = displaySecondsToRawNs(x_range_sec.min, offset);
    const Timestamp raw_b = displaySecondsToRawNs(x_range_sec.max, offset);
    bounds = series_or->bounds(Range<Timestamp>{.min = std::min(raw_a, raw_b), .max = std::max(raw_a, raw_b)});
  }

  if (!bounds.has_value()) {
    return std::nullopt;
  }
  return bounds->value;
}

void DatastoreCurveAdapter::onTopicCommitted() {
  data_dirty_ = true;
  window_dirty_ = true;
  full_bounding_rect_valid_ = false;
  cached_display_offset_valid_ = false;
}

void DatastoreCurveAdapter::onDataCleared() {
  window_samples_.clear();
  window_offset_ = 0;
  window_count_ = 0;
  data_dirty_ = true;
  window_dirty_ = true;
  full_bounding_rect_valid_ = false;
  cached_full_bounding_rect_ = invalidRect();
  cached_display_offset_valid_ = false;
}

void DatastoreCurveAdapter::onDisplayOffsetChanged() {
  // Samples didn't move — only their display->raw mapping. Clear the offset
  // cache and the bounding rect (its X extent shifts); leave the window flags
  // untouched so we don't pay a re-index. The visible raw window is re-derived
  // by Qwt on the replot that follows (updateScaleDiv -> setRectOfInterest
  // reconverts the display-seconds rect through the new offset).
  cached_display_offset_valid_ = false;
  full_bounding_rect_valid_ = false;
}

std::optional<QPointF> DatastoreCurveAdapter::sampleFromTime(double display_time_sec) const {
  if (session_ == nullptr) {
    return std::nullopt;
  }

  const Timestamp raw_time = displaySecondsToRawNs(display_time_sec, displayOffsetNow());
  auto series_or = session_->createReader().series(source_.topic_id, source_.column_index);
  if (!series_or.has_value()) {
    return std::nullopt;
  }

  const auto sample = series_or->sampleAtOrBeforeTime(raw_time);
  return sample.has_value() ? std::optional<QPointF>{readPoint(*sample)} : std::nullopt;
}

void DatastoreCurveAdapter::ensureSampleWindow() const {
  if (!window_dirty_ && !data_dirty_) {
    return;
  }

  // Content changed, but if the series generation still matches the build,
  // the only changes were strictly in-order appends and/or a retention-floor
  // rise — absorb them into the cached window instead of rebuilding. This is
  // the live-streaming hot path: one commit tick costs O(new samples), not
  // O(visible window).
  if (data_dirty_ && !window_samples_.empty() && session_ != nullptr) {
    if (auto series_or = session_->createReader().series(source_.topic_id, source_.column_index);
        series_or.has_value() && series_or->seriesGeneration() == built_series_generation_ &&
        absorbInOrderAppends(*series_or)) {
      data_dirty_ = false;
      if (!window_dirty_) {
        narrowToVisibleWindow();  // appended samples may extend the served slice
        return;
      }
    }
  }

  // The built vector still covers the new visible window: re-narrow without
  // touching the engine. This is the hot path while zooming/panning. An
  // all-rows build (sentinel range) is deliberately NOT reused for a finite
  // viewport: it would serve correctly but pin the whole series in memory, so
  // the first finite viewport pays one rebuild to shed it.
  const bool visible_all_rows = isAllRowsWindow(visible_t_min_raw_ns_, visible_t_max_raw_ns_);
  const bool built_all_rows = isAllRowsWindow(built_t_min_raw_ns_, built_t_max_raw_ns_);
  if (!data_dirty_ && built_all_rows == visible_all_rows && built_t_min_raw_ns_ <= visible_t_min_raw_ns_ &&
      visible_t_max_raw_ns_ <= built_t_max_raw_ns_) {
    narrowToVisibleWindow();
    window_dirty_ = false;
    return;
  }

  window_samples_.clear();
  window_offset_ = 0;
  window_count_ = 0;
  built_t_min_raw_ns_ = visible_t_min_raw_ns_;
  built_t_max_raw_ns_ = visible_t_max_raw_ns_;
  data_dirty_ = false;
  window_dirty_ = false;

  if (session_ == nullptr) {
    return;
  }

  auto series_or = session_->createReader().series(source_.topic_id, source_.column_index);
  if (!series_or.has_value()) {
    return;
  }

  // Widen the build window by one visible span per side (saturating), so the
  // following zoom/pan steps land inside it and take the re-narrow path above.
  if (!visible_all_rows) {
    const auto span =
        static_cast<std::uint64_t>(visible_t_max_raw_ns_) - static_cast<std::uint64_t>(visible_t_min_raw_ns_);
    built_t_min_raw_ns_ = saturatingSubtract(visible_t_min_raw_ns_, span);
    built_t_max_raw_ns_ = saturatingAdd(visible_t_max_raw_ns_, span);
  }

  const SeriesReader& series = *series_or;
  built_series_generation_ = series.seriesGeneration();
  // Sizing only: the virtual-index lookups assume chronological chunks and are
  // merely approximate when out-of-order chunks overlap.
  const auto first_inside = series.indexAtOrAfterTime(built_t_min_raw_ns_);
  const auto last_inside = series.indexAtOrBeforeTime(built_t_max_raw_ns_);
  if (first_inside.has_value() && last_inside.has_value() && *last_inside >= *first_inside) {
    window_samples_.reserve(*last_inside - *first_inside + 3);
  }

  // One sample STRICTLY outside each end of the built range (merge-aware over
  // overlapping chunks), so every window narrowed from this build can serve an
  // off-screen guard — including a window that starts exactly at the built edge.
  if (const auto left_guard = series.sampleBeforeTime(built_t_min_raw_ns_); left_guard.has_value()) {
    window_samples_.push_back({.timestamp = left_guard->timestamp, .value = left_guard->value});
  }

  auto cursor = series.samples(Range<Timestamp>{.min = built_t_min_raw_ns_, .max = built_t_max_raw_ns_});
  cursor.forEach([this](const SeriesSample& sample) {
    window_samples_.push_back({.timestamp = sample.timestamp, .value = sample.value});
  });

  if (const auto right_guard = series.sampleAfterTime(built_t_max_raw_ns_); right_guard.has_value()) {
    window_samples_.push_back({.timestamp = right_guard->timestamp, .value = right_guard->value});
  }

  narrowToVisibleWindow();
}

bool DatastoreCurveAdapter::absorbInOrderAppends(const SeriesReader& series) const {
  // Retention-floor rise: drop the logically evicted prefix of the cache.
  const Timestamp floor = series.retentionFloor();
  if (!window_samples_.empty() && window_samples_.front().timestamp < floor) {
    const auto keep_from = std::lower_bound(
        window_samples_.begin(), window_samples_.end(), floor,
        [](const CachedSample& sample, Timestamp t) { return sample.timestamp < t; });
    window_samples_.erase(window_samples_.begin(), keep_from);
  }
  if (window_samples_.empty()) {
    return false;  // everything evicted: rebuild decides what remains
  }

  const Timestamp last_cached = window_samples_.back().timestamp;
  if (last_cached > built_t_max_raw_ns_) {
    // The cache already ends in a right guard beyond the built range. An equal
    // generation means every append landed after the then-newest sample, i.e.
    // beyond that guard — nothing inside the window changed.
    return true;
  }

  // Strictly last_cached < built_t_max_: an equal boundary would produce the
  // inverted range (last+1, last), which SeriesCursor normalizes by SWAPPING —
  // re-reading and duplicating the boundary sample on every notification.
  if (last_cached < built_t_max_raw_ns_) {
    auto cursor = series.samples(Range<Timestamp>{.min = last_cached + 1, .max = built_t_max_raw_ns_});
    cursor.forEach([this](const SeriesSample& sample) {
      window_samples_.push_back({.timestamp = sample.timestamp, .value = sample.value});
    });
  }

  // Data beyond the built range supplies the right guard the build could not.
  // Guarded on the CURRENT back: once a guard is appended, the early-out above
  // keeps later notifications from appending it again.
  if (window_samples_.back().timestamp <= built_t_max_raw_ns_) {
    if (const auto right_guard = series.sampleAfterTime(built_t_max_raw_ns_); right_guard.has_value()) {
      window_samples_.push_back({.timestamp = right_guard->timestamp, .value = right_guard->value});
    }
  }
  return true;
}

void DatastoreCurveAdapter::narrowToVisibleWindow() const {
  if (isAllRowsWindow(visible_t_min_raw_ns_, visible_t_max_raw_ns_)) {
    window_offset_ = 0;
    window_count_ = window_samples_.size();
    return;
  }
  const auto begin = window_samples_.begin();
  const auto end = window_samples_.end();
  auto lo = std::lower_bound(
      begin, end, visible_t_min_raw_ns_, [](const CachedSample& sample, Timestamp t) { return sample.timestamp < t; });
  auto hi = std::upper_bound(
      begin, end, visible_t_max_raw_ns_, [](Timestamp t, const CachedSample& sample) { return t < sample.timestamp; });
  // One off-screen guard sample per side keeps the line segments that enter the
  // viewport from outside it.
  if (lo != begin) {
    --lo;
  }
  if (hi != end) {
    ++hi;
  }
  window_offset_ = static_cast<std::size_t>(lo - begin);
  window_count_ = static_cast<std::size_t>(hi - lo);
}

QPointF DatastoreCurveAdapter::toDisplayPoint(Timestamp timestamp, double value) const {
  return {rawNsToDisplaySeconds(timestamp, displayOffsetNow()), value};
}

QPointF DatastoreCurveAdapter::readPoint(const SeriesSample& sample) const {
  if (sample.chunk == nullptr) {
    return invalidPoint();
  }

  return toDisplayPoint(sample.timestamp, sample.value);
}

DisplayOffset DatastoreCurveAdapter::displayOffsetNow() const {
  if (cached_display_offset_valid_) {
    return cached_display_offset_;
  }

  // Live lookup via SessionManager::displayOffset — the single offset seam
  // (TimeDomain shift + the per-dataset "Use time offset" shift), never a
  // catalog-build-time snapshot. The cache below is invalidated by
  // onTopicCommitted / onDataCleared / onDisplayOffsetChanged so it tracks
  // offset reconfiguration through the same signals that drive sample
  // re-indexing (or, for a pure offset change, without re-indexing).
  DisplayOffset offset;
  if (session_ != nullptr) {
    offset = session_->displayOffset(source_.dataset_id);
  }

  cached_display_offset_ = offset;
  cached_display_offset_valid_ = true;
  return offset;
}

}  // namespace PJ
