// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene2d_core/entry_thumbnail_cache.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace PJ {

namespace {
constexpr int64_t kOneSecondNs = 1'000'000'000;
}  // namespace

EntryThumbnailCache::EntryThumbnailCache(
    ObjectStore* store, ObjectTopicId topic, StreamingVideoDecoder::NalExtractor extractor)
    : EntryThumbnailCache(store, topic, std::move(extractor), Config{}) {}

EntryThumbnailCache::EntryThumbnailCache(
    ObjectStore* store, ObjectTopicId topic, StreamingVideoDecoder::NalExtractor extractor, Config cfg)
    : store_(store), topic_(topic), extractor_(std::move(extractor)), cfg_(cfg) {}

EntryThumbnailCache::~EntryThumbnailCache() {
  stop();
}

void EntryThumbnailCache::buildAsync(Timestamp start_ns, Timestamp end_ns) {
  stop();
  running_.store(true);
  thread_ = std::thread(&EntryThumbnailCache::buildThread, this, start_ns, end_ns);
}

void EntryThumbnailCache::stop() {
  running_.store(false);
  if (thread_.joinable()) {
    thread_.join();
  }
}

std::optional<DecodedFrame> EntryThumbnailCache::lookup(Timestamp ts_ns) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (tiles_.empty()) {
    return std::nullopt;
  }
  // Nearest at-or-before: first tile with ts > ts_ns, then step back one.
  auto it =
      std::upper_bound(tiles_.begin(), tiles_.end(), ts_ns, [](Timestamp t, const Tile& tile) { return t < tile.ts; });
  if (it == tiles_.begin()) {
    return std::nullopt;
  }
  --it;
  return decodeThumbnailJpeg(it->jpeg.data(), it->jpeg.size(), it->width, it->height);
}

void EntryThumbnailCache::prune(Timestamp before_ns) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto end = std::lower_bound(
      tiles_.begin(), tiles_.end(), before_ns, [](const Tile& tile, Timestamp v) { return tile.ts < v; });
  for (auto it = tiles_.begin(); it != end; ++it) {
    bytes_ -= it->jpeg.size();
  }
  tiles_.erase(tiles_.begin(), end);
}

std::size_t EntryThumbnailCache::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tiles_.size();
}

void EntryThumbnailCache::buildThread(Timestamp start_ns, Timestamp end_ns) {
  StreamingVideoDecoder decoder;
  decoder.attach(store_, topic_, extractor_);

  auto range = store_->timeRange(topic_);
  const Timestamp lo = (end_ns >= start_ns) ? start_ns : range.first;
  const Timestamp hi = (end_ns >= start_ns) ? end_ns : range.second;
  const int64_t span = std::max<int64_t>(1, hi - lo);
  // One per second, coarser when the span would need more than max_tiles.
  const int64_t interval =
      std::max<int64_t>(kOneSecondNs, span / static_cast<int64_t>(std::max<std::size_t>(1, cfg_.max_tiles)));

  bool resolution_checked = false;
  decoder.decodeSampled(interval, [&](const DecodedFrame& f) -> bool {
    if (!running_.load()) {
      return false;
    }
    if (!resolution_checked) {
      resolution_checked = true;
      // A source within the cap decodes cheaply on scrub: no thumbnails.
      if (f.width <= cfg_.max_width) {
        return false;
      }
    }
    JpegThumbnail thumb = encodeThumbnailJpeg(f, cfg_.max_width, cfg_.quality);
    if (thumb.jpeg.empty()) {
      return true;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (bytes_ + thumb.jpeg.size() > cfg_.max_bytes) {
      return false;
    }
    bytes_ += thumb.jpeg.size();
    tiles_.push_back({f.pts, std::move(thumb.jpeg), thumb.width, thumb.height});
    return tiles_.size() < cfg_.max_tiles;
  });

  // The drained tail of decodeSampled may be out of order; lookup() binary-searches.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::sort(tiles_.begin(), tiles_.end(), [](const Tile& a, const Tile& b) { return a.ts < b.ts; });
  }
  running_.store(false);
}

}  // namespace PJ
