#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_scene2d_core/decoded_frame.h"
#include "pj_scene2d_core/streaming_video_decoder.h"
#include "pj_scene2d_core/thumbnail_codec.h"

namespace PJ {

/// Scrub thumbnails of a VideoFrame topic: a build thread samples about one frame per adaptive interval
/// (StreamingVideoDecoder::decodeSampled, bounded by the tile budget) into small JPEGs; lookup() serves the one at or
/// before a time while the real decode settles. Only bounded topics build one (StreamingVideoSource); prune() and
/// the build window are not used by any caller.
///
/// lookup()/prune()/size() are safe during a build; the destructor joins. The extractor runs only on the build thread
/// and must not be shared with another decoder (its keepalive slot is single-threaded).
class EntryThumbnailCache {
 public:
  struct Config {
    int max_width = kThumbnailMaxWidth;  ///< a source no wider than this builds no cache
    int quality = kThumbnailJpegQuality;
    std::size_t max_tiles = 80;  ///< sets the sampling interval
    std::size_t max_bytes = 48u * 1024 * 1024;
  };

  EntryThumbnailCache(ObjectStore* store, ObjectTopicId topic, StreamingVideoDecoder::NalExtractor extractor);
  EntryThumbnailCache(
      ObjectStore* store, ObjectTopicId topic, StreamingVideoDecoder::NalExtractor extractor, Config cfg);
  ~EntryThumbnailCache();

  EntryThumbnailCache(const EntryThumbnailCache&) = delete;
  EntryThumbnailCache& operator=(const EntryThumbnailCache&) = delete;
  EntryThumbnailCache(EntryThumbnailCache&&) = delete;
  EntryThumbnailCache& operator=(EntryThumbnailCache&&) = delete;

  /// Builds over [start_ns, end_ns], or the whole topic when end_ns < start_ns. Stops a running build first.
  void buildAsync(Timestamp start_ns = 0, Timestamp end_ns = -1);

  /// Idempotent.
  void stop();

  /// The thumbnail at or before ts_ns as YUV420P; nullopt before the first tile.
  [[nodiscard]] std::optional<DecodedFrame> lookup(Timestamp ts_ns) const;

  /// Drops tiles before before_ns.
  void prune(Timestamp before_ns);

  [[nodiscard]] std::size_t size() const;

 private:
  void buildThread(Timestamp start_ns, Timestamp end_ns);

  struct Tile {
    Timestamp ts;
    std::vector<std::uint8_t> jpeg;
    int width;
    int height;
  };

  ObjectStore* store_;
  ObjectTopicId topic_;
  StreamingVideoDecoder::NalExtractor extractor_;
  Config cfg_;

  mutable std::mutex mutex_;  // guards tiles_ and bytes_
  std::vector<Tile> tiles_;   // ascending ts
  std::size_t bytes_ = 0;

  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace PJ
