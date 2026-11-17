#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "pj_base/builtin/camera_info.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_scene2d_core/async_frame_worker.h"
#include "pj_scene2d_core/codec_pipeline.h"
#include "pj_scene2d_core/codecs.h"
#include "pj_scene2d_core/media_source.h"
#include "pj_scene2d_core/undistort_remap.h"

namespace PJ {

class MessageParserPluginBase;
namespace sdk {
struct Image;
}  // namespace sdk

/// MediaSource for non-depth image topics, decoded and optionally rectified on an AsyncFrameWorker. Three routes: the
/// topic's MessageParser (production), the canonical pj_image_v1 blob (CanonicalImageCodec), or a CodecPipeline over
/// the entry bytes (tests and demos).
///
/// setTimestamp(), takeFrame() and setFrameReadyCallback() are GUI-thread only; the frame-ready callback runs on the
/// worker thread. `store` and `parser` are not owned; `store` must be non-null and outlive the source.
class ImagePipelineSource : public MediaSource {
 public:
  /// @param parser_mutex  Serializes parseObject across every consumer of the same parser: MessageParser plugins are
  ///   not thread-safe, so sources sharing a parser must share the mutex. nullptr only for a private parser (tests).
  /// @param parser_keepalive  Owns the parser instance and its plugin library for the source's lifetime, so neither
  ///   is unloaded under an in-flight parseObject. From SessionManager::parserKeepaliveForObjectTopic.
  ImagePipelineSource(
      ObjectStore* store, ObjectTopicId topic, MessageParserPluginBase* parser,
      std::shared_ptr<std::mutex> parser_mutex, std::shared_ptr<void> parser_keepalive);

  ImagePipelineSource(ObjectStore* store, ObjectTopicId topic, std::unique_ptr<CodecPipeline> pipeline);

  /// Each entry is a serialized sdk::Image (pj_image_v1), on topics registered with metadata image_codec=pj_image_v1
  /// and no parser.
  struct CanonicalImageCodec {};

  ImagePipelineSource(ObjectStore* store, ObjectTopicId topic, CanonicalImageCodec tag);

  ~ImagePipelineSource() override;

  ImagePipelineSource(const ImagePipelineSource&) = delete;
  ImagePipelineSource& operator=(const ImagePipelineSource&) = delete;
  ImagePipelineSource(ImagePipelineSource&&) = delete;
  ImagePipelineSource& operator=(ImagePipelineSource&&) = delete;

  void setTimestamp(int64_t ts_ns) override;
  std::optional<MediaFrame> takeFrame() override;
  void invalidate() override;

  /// Thread-safe; a change re-decodes the current frame in the new mode.
  void setGpuRectificationAvailable(bool available) override;

  /// Fired on the worker thread whenever takeFrame() has something new. nullptr clears it.
  void setFrameReadyCallback(std::function<void()> cb);

  /// Calibration keyed by CameraInfo.frame_id; empty disables rectification. GUI thread, before the first
  /// setTimestamp(): the worker then reads the map unlocked, so a later call is refused.
  void setCameraInfoMap(std::unordered_map<std::string, sdk::CameraInfo> by_frame_id);

  /// Default on: a frame whose frame_id has a calibration is undistorted. Off forces raw passthrough, the operator's
  /// override for an already rectified stream (docs/TECHNICAL_NOTES.md "Rectification assumption"). Thread-safe; a
  /// change re-decodes the current frame.
  void setRectifyEnabled(bool enabled);

  /// Calibration eligibility of the latest decoded image, whatever the toggle. Published by the worker; read it on the
  /// GUI thread after a frame-ready notification.
  enum class RectificationStatus {
    kWaitingForImage,
    kMissingFrameId,
    kMissingCalibration,
    kInvalidCalibration,
    kReady
  };
  [[nodiscard]] RectificationStatus rectificationStatus() const {
    return rectification_status_.load(std::memory_order_relaxed);
  }

 private:
  /// The frame of the entry at or before `ts_ns`: nullopt for no update (nothing there, already delivered, a
  /// decode failure), a null frame to clear what is shown (an empty image, or a seek before the shown entry with
  /// nothing there). Worker thread.
  std::optional<DecodedFrame> decodeAt(int64_t ts_ns);

  // Raw and Bayer encodings (including grayscale-PNG-wrapped raw buffers) are reinterpreted at the image's geometry;
  // everything else goes through the jpeg/png/auto cascade.
  std::optional<DecodedFrame> decodeCanonicalImage(const sdk::Image& img, int64_t pts);

  // With an enabled, usable calibration for the frame_id: on the GPU path attaches rectify_map, otherwise resamples
  // to the camera's calibrated resolution. Assumes the image is raw and its annotations are in rectified space; a
  // pre-rectified stream is rectified twice (docs/TECHNICAL_NOTES.md "Rectification assumption").
  void rectifyIfCalibrated(DecodedFrame& df);

  // Worker thread.
  const sdk::CameraInfo* cameraInfoFor(const std::string& frame_id);

  // Worker-thread state after construction; the GUI thread only reads it.
  ObjectStore* store_;
  ObjectTopicId topic_;
  std::string source_key_;
  MessageParserPluginBase* parser_ = nullptr;
  std::shared_ptr<std::mutex> parser_mutex_;
  // Released only after the worker joins, so parseObject never runs on an unloaded parser.
  std::shared_ptr<void> parser_keepalive_;
  std::unique_ptr<CodecPipeline> pipeline_;
  bool canonical_image_codec_ = false;
  JpegCodec jpeg_codec_;
  PngCodec png_codec_;
  NormalizeMono16 normalize_mono16_;
  AutoImageCodec auto_image_codec_;
  /// The last delivered entry, worker-thread only; invalid = none (force_redecode resets to it).
  SequentialUID last_entry_uid_;
  /// The last delivered entry's store stamp, worker-thread only: a seek before it with nothing there clears.
  std::optional<int64_t> last_entry_stamp_;

  // Per-camera maps are built on first use on the worker. The UndistortMap is shared so the GPU path can hand it to
  // the widget in DecodedFrame; the fixed-point table serves only the CPU path.
  std::unordered_map<std::string, sdk::CameraInfo> camera_info_by_frame_;
  std::unordered_map<std::string, std::shared_ptr<const UndistortMap>> undistort_by_frame_;
  std::unordered_map<std::string, UndistortMapFast> fast_by_frame_;
  std::atomic<bool> gpu_rectify_available_{false};
  std::atomic<bool> rectify_enabled_{true};
  std::atomic<RectificationStatus> rectification_status_{RectificationStatus::kWaitingForImage};
  // Set by the first setTimestamp(); setCameraInfoMap() is refused afterwards.
  bool timestamp_requested_ = false;

  // Declared LAST: its stop() joins the worker before any member the decode closure touches is destroyed.
  AsyncFrameWorker worker_;
};

}  // namespace PJ
