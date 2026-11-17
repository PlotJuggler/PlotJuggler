#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <climits>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "pj_base/types.hpp"  // PJ::Range
#include "pj_datastore/object_store.hpp"
#include "pj_scene2d_core/async_frame_worker.h"
#include "pj_scene2d_core/media_source.h"

namespace PJ {

class MessageParserPluginBase;

namespace sdk {
struct DepthImage;
}  // namespace sdk

/// MediaSource for depth images: an sdk::Image with a depth encoding (16UC1 mm, 32FC1 m, or compressedDepth PNG).
/// An AsyncFrameWorker decodes it (the compressedDepth inflate is too heavy for the GUI thread) into metric float32
/// (PixelFormat::kDepthR32F) plus DepthColorParams; the colormap, range and invert are applied on the GPU.
///
/// Every public method is GUI-thread only; the frame-ready callback runs on the worker thread.
class DepthPipelineSource : public MediaSource {
 public:
  /// Each store entry is a serialized pj_image_v1 sdk::Image. `store` is not owned.
  DepthPipelineSource(ObjectStore* store, ObjectTopicId topic);

  /// Each store entry is a raw message for the topic's MessageParser; the arguments are those of
  /// ImagePipelineSource's parser constructor (SessionManager::parserBindingForObjectTopic).
  DepthPipelineSource(
      ObjectStore* store, ObjectTopicId topic, MessageParserPluginBase* parser,
      std::shared_ptr<std::mutex> parser_mutex, std::shared_ptr<void> parser_keepalive);

  ~DepthPipelineSource() override;

  void setTimestamp(int64_t ts_ns) override;
  std::optional<MediaFrame> takeFrame() override;
  void invalidate() override;

  /// Fired on the worker thread after each decode. nullptr clears it.
  void setFrameReadyCallback(std::function<void()> cb);

  /// The id is pj_widgets' `Colormap` value and the GPU LUT row, opaque here so the core does not depend on
  /// pj_widgets.
  void setColormap(uint8_t colormap_id);
  void setInvert(bool invert);  ///< Mirror the colormap (t -> 1 - t).
  void setRange(float near_m, float far_m);
  void setOpacity(float opacity);

  /// The lo/hi percentiles of the last decoded frame's valid depths (`min` = near, `max` = far, metres), for the
  /// layer's Auto-fit; does not change the range. nullopt before a decode or without valid samples.
  [[nodiscard]] std::optional<Range<float>> autoRange(float lo_frac = 0.02f, float hi_frac = 0.98f) const;

 private:
  void startWorker();

  /// Worker thread. The entry at or before `ts_ns`: nullopt for no data, an unchanged entry or an undecodable
  /// payload; a null frame (a clear) for an empty image or a seek before the shown entry with nothing there.
  [[nodiscard]] std::optional<DecodedFrame> decodeAt(int64_t ts_ns);
  [[nodiscard]] std::optional<DecodedFrame> decodeDepthImage(const sdk::DepthImage& depth, int64_t pts) const;

  ObjectStore* store_;
  ObjectTopicId topic_;
  MessageParserPluginBase* parser_ = nullptr;  ///< null for canonical blobs
  std::shared_ptr<std::mutex> parser_mutex_;
  std::shared_ptr<void> parser_keepalive_;
  bool canonical_ = false;

  // Guarded by params_mutex_: the setters write them and the decode worker snapshots them under it.
  mutable std::mutex params_mutex_;
  uint8_t colormap_ = 0;  ///< 0 = turbo
  bool invert_ = false;
  float near_m_ = 0.0f;
  float far_m_ = 4.0f;
  float opacity_ = 1.0f;

  /// Worker-thread dedup, keyed by UID because the store allows equal stamps and a same-stamp replacement must
  /// still decode. Invalid = none (force_redecode resets it).
  SequentialUID last_entry_uid_;
  /// The last delivered entry's store stamp, worker-thread only: a seek before it with nothing there clears.
  std::optional<int64_t> last_entry_stamp_;

  // The last decoded depth frame for autoRange(): written by the worker, read on the GUI thread. The pixels are
  // shared and never mutated.
  mutable std::mutex last_frame_mutex_;
  std::shared_ptr<const std::vector<uint8_t>> last_depth_pixels_;
  int last_depth_width_ = 0;
  int last_depth_height_ = 0;

  // Declared last and stopped first in the destructor, so the decode closure never touches a destroyed member.
  AsyncFrameWorker worker_;
};

}  // namespace PJ
