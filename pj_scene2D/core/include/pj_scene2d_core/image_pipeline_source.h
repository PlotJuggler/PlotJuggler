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

/// MediaSource for image topics (JPEG, PNG, depth, segmentation).
///
/// Production object topics use the parser-driven constructor: raw bytes from
/// ObjectStore are translated by the bound MessageParser into canonical
/// sdk::Image, then decoded by Image::encoding. Demo utilities may still use
/// the pipeline-driven constructor when they push already-prepared image bytes.
///
/// Decode runs on a dedicated worker thread (mirrors StreamingVideoSource).
/// setTimestamp() posts a request to the worker and returns immediately;
/// takeFrame() polls the latest decoded result. New requests during decode
/// overwrite the pending target — latest-target-wins coalescing keeps the
/// queue at depth one regardless of slider drag rate.
///
/// Notification: when a new frame is ready, the worker invokes the callback
/// installed via setFrameReadyCallback() (called FROM the worker thread, so
/// implementers must hop back to the GUI thread themselves — typically with
/// QMetaObject::invokeMethod(..., Qt::QueuedConnection)).
///
/// Thread safety: setTimestamp(), takeFrame(), and setFrameReadyCallback()
/// must be called from the same (main) thread. The worker thread is internal.
///
/// Ownership: `store` and `parser` are NOT owned. `pipeline` is owned.
/// Precondition: `store` is non-null and outlives this source.
/// See ARCHITECTURE.md §5.1.
class ImagePipelineSource : public MediaSource {
 public:
  /// @param store  ObjectStore to query (not owned, must be non-null and outlive this source)
  /// @param topic  Topic ID to query via latestAt()
  /// @param parser  Parser to drive canonical-image decode (not owned)
  /// @param parser_mutex  Shared mutex serialising parseObject across consumers
  ///   of the same parser. MessageParser plugins aren't thread-safe (stateful
  ///   scratch), so sources sharing a parser pointer MUST share a mutex. Pass
  ///   nullptr only when the parser is private to this source (e.g. unit tests).
  /// @param parser_keepalive  Opaque shared owner of the parser handle (parser
  ///   instance + plugin DSO). Held for this source's whole lifetime so the
  ///   parser can't be torn down / dlclosed under an in-flight parseObject on the
  ///   worker thread. Get it from SessionManager::parserKeepaliveForObjectTopic.
  ImagePipelineSource(
      ObjectStore* store, ObjectTopicId topic, MessageParserPluginBase* parser,
      std::shared_ptr<std::mutex> parser_mutex, std::shared_ptr<void> parser_keepalive);

  /// @param store  ObjectStore to query (not owned, must be non-null and outlive this source)
  /// @param topic  Topic ID to query via latestAt()
  /// @param pipeline  Codec pipeline for decode (owned, moved in)
  ImagePipelineSource(ObjectStore* store, ObjectTopicId topic, std::unique_ptr<CodecPipeline> pipeline);

  /// Tag selecting the per-frame canonical-image route: each ObjectStore entry's
  /// bytes are a serialized sdk::Image (pj_base pj_image_v1 codec). Used by
  /// producers (e.g. the mosaico toolbox) that push canonical Image blobs via
  /// pushOwnedObject and register the topic with metadata image_codec=pj_image_v1
  /// — there is no MessageParser, so the source deserializes each blob itself.
  struct CanonicalImageCodec {};

  /// @param store  ObjectStore to query (not owned, must be non-null and outlive this source)
  /// @param topic  Topic ID to query via latestAt()
  ImagePipelineSource(ObjectStore* store, ObjectTopicId topic, CanonicalImageCodec tag);

  ~ImagePipelineSource() override;

  ImagePipelineSource(const ImagePipelineSource&) = delete;
  ImagePipelineSource& operator=(const ImagePipelineSource&) = delete;
  ImagePipelineSource(ImagePipelineSource&&) = delete;
  ImagePipelineSource& operator=(ImagePipelineSource&&) = delete;

  void setTimestamp(int64_t ts_ns) override;
  std::optional<MediaFrame> takeFrame() override;
  void invalidate() override;

  /// Switch between GPU (raw frame + rectify_map) and CPU (resampled frame)
  /// rectification. Thread-safe: callable from the widget thread at any time; a
  /// change re-decodes the current frame in the new mode via invalidate().
  void setGpuRectificationAvailable(bool available) override;

  /// Install a notification fired (from the worker thread) every time
  /// takeFrame() has new data to return. Pass nullptr to clear.
  void setFrameReadyCallback(std::function<void()> cb);

  /// Provide the camera calibration used to rectify decoded frames, keyed by
  /// CameraInfo.frame_id. The owner (which has the session's parser registry)
  /// parses each "<ns>/camera_info" topic and passes the result here. Must be
  /// called on the main thread before the first setTimestamp(); empty disables
  /// rectification (frames pass through unmodified).
  void setCameraInfoMap(std::unordered_map<std::string, sdk::CameraInfo> by_frame_id);

  /// Enable or disable lens rectification for this source. Default enabled: a frame
  /// whose frame_id matches an injected CameraInfo is undistorted automatically.
  /// Disabling forces raw passthrough even when calibration exists — the operator's
  /// override for the always-on auto-decision (e.g. an already-rectified stream that
  /// would otherwise be undistorted twice; see docs/TECHNICAL_NOTES.md §12).
  /// Thread-safe; a change re-decodes the current frame so even a paused view updates.
  void setRectifyEnabled(bool enabled);

  /// Calibration eligibility for the latest decoded image, independent of the user's toggle.
  /// Published by the decode worker; safe to read from the UI thread after frame-ready notification.
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

  // Decode one canonical sdk::Image (from a parser or a deserialized blob) into a
  // display frame: raw/bayer encodings via the raw-or-bayer path (incl. grayscale
  // PNG-wrapped recovery), otherwise the jpeg/png/auto compressed cascade.
  std::optional<DecodedFrame> decodeCanonicalImage(const sdk::Image& img, int64_t pts);

  // Lens-undistort a decoded frame in place when calibration exists for its
  // frame_id, emitting it at the camera's native (calibrated) resolution so 2D
  // annotation overlays authored in that space line up. No-op without calibration.
  //
  // ASSUMPTION (not a universal law): a usable CameraInfo for this frame implies
  // the image is still in raw sensor space and its annotations are authored in the
  // native *rectified* space (the Waymo / Foxglove / ROS image_proc convention).
  // CameraInfo describes the lens, NOT whether THIS stream was already rectified,
  // so a producer that logs a pre-rectified image alongside its CameraInfo would
  // be rectified twice. See docs/TECHNICAL_NOTES.md §12 for the rationale and the
  // known blind spot. The operator can override this auto-decision per layer via
  // setRectifyEnabled(false), which short-circuits this method to raw passthrough.
  void rectifyIfCalibrated(DecodedFrame& df);

  // Return the injected CameraInfo whose frame_id matches, or nullptr when none.
  // A plain lookup into the map set by setCameraInfoMap(); worker-thread-only.
  const sdk::CameraInfo* cameraInfoFor(const std::string& frame_id);

  // I/O config and codec state — read/written by worker thread only after
  // construction finishes. The main thread treats them as read-only after
  // the constructor returns.
  ObjectStore* store_;
  ObjectTopicId topic_;
  std::string source_key_;
  MessageParserPluginBase* parser_ = nullptr;
  std::shared_ptr<std::mutex> parser_mutex_;
  // Keeps the parser handle (instance + plugin DSO) alive for this source's whole
  // lifetime — see SessionManager::parserKeepaliveForObjectTopic. Dropped after
  // the worker joins (destructor body), so parseObject can't run on a freed/
  // dlclosed parser during teardown. Null when no parser is used.
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

  // Rectification state. camera_info_by_frame_ is injected once (main thread,
  // before first decode) via setCameraInfoMap; each camera's reverse map is then
  // built on first use and reused thereafter (worker-thread-only). The map is held
  // by shared_ptr so the GPU path can hand it to the widget via DecodedFrame; the
  // fast (fixed-point) table is derived lazily and only used by the CPU path.
  std::unordered_map<std::string, sdk::CameraInfo> camera_info_by_frame_;
  std::unordered_map<std::string, std::shared_ptr<const UndistortMap>> undistort_by_frame_;
  std::unordered_map<std::string, UndistortMapFast> fast_by_frame_;
  // True once the widget reports GPU rectification is available; then decode keeps
  // frames raw and attaches the map. Atomic: written from the widget thread, read
  // on the worker thread (the setter's invalidate() also re-decodes in the new mode).
  std::atomic<bool> gpu_rectify_available_{false};
  // Whether lens rectification is applied when a CameraInfo matches the frame.
  // Default true (the historical always-on behaviour); setRectifyEnabled(false) is
  // the per-layer operator override that forces raw passthrough. Atomic: written from
  // the widget/UI thread, read on the worker thread in rectifyIfCalibrated();
  // cross-thread publication mirrors gpu_rectify_available_.
  std::atomic<bool> rectify_enabled_{true};
  std::atomic<RectificationStatus> rectification_status_{RectificationStatus::kWaitingForImage};
  // Set true by the first setTimestamp() (main thread). Guards setCameraInfoMap:
  // once a decode has been requested the worker may read camera_info_by_frame_
  // unlocked, so a later injection would be a data race — it is refused instead.
  bool timestamp_requested_ = false;

  // The shared latest-wins worker engine (request/result/callback channels +
  // thread + lost-wakeup-safe teardown). Declared LAST so its implicit stop()
  // joins the worker before any member the decode closure touches goes away;
  // the explicit stop() in the destructor body is the primary safety net.
  AsyncFrameWorker worker_;
};

}  // namespace PJ
