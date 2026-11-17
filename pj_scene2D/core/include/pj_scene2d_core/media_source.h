#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <optional>

#include "pj_scene2d_core/media_frame.h"

namespace PJ {

/// Pull-based frame delivery to MediaViewerWidget. Every method is called on the GUI thread: setTimestamp() when the
/// time changes, takeFrame() at render rate. Frames never travel through Qt signals (docs/ARCHITECTURE.md "Frame
/// delivery").
class MediaSource {
 public:
  virtual ~MediaSource() = default;

  MediaSource() = default;
  MediaSource(const MediaSource&) = delete;
  MediaSource& operator=(const MediaSource&) = delete;
  MediaSource(MediaSource&&) = delete;
  MediaSource& operator=(MediaSource&&) = delete;

  /// Decodes synchronously or posts to the source's worker.
  virtual void setTimestamp(int64_t ts_ns) = 0;

  /// The latest frame, or nullopt when nothing is new since the last call.
  virtual std::optional<MediaFrame> takeFrame() = 0;

  /// Makes the next setTimestamp() decode even at an unchanged time. Called after a layer rebuild, which re-seeds the
  /// sources at the same time; without it they dedup the request and keep a stale or empty frame.
  virtual void invalidate() {}

  /// Whether the widget rectifies on the GPU. True: a rectifying source may deliver the raw frame with
  /// DecodedFrame::rectify_map set. False: it must deliver an already rectified frame, or the image shows distorted.
  virtual void setGpuRectificationAvailable(bool /*available*/) {}
};

}  // namespace PJ
