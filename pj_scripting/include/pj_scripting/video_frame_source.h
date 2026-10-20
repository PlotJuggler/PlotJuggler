// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <memory>

#include "pj_base/builtin/image.hpp"
#include "pj_base/expected.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_scripting/memory_budget.h"

// The headless video seam of the object scripts (docs/derived_objects/ARCHITECTURE.md §6): `video:frame_at(t)` decodes
// the frame presented at `t` through a source the session injects (`RunOptions::video_source`), so scripting links no
// codec.

namespace PJ::scripting {

/// A full-resolution decoded frame, never a scrub preview.
struct VideoFrameResult {
  /// rgb8, tightly packed, stamped with its presentation time; its anchor holds the pixels and their charge.
  sdk::Image image;
  Timestamp presentation_ns = 0;
};

class IVideoFrameSource {
 public:
  virtual ~IVideoFrameSource() = default;
  /// Any thread. The frame of the video topic `topic` presented at `t` (the greatest presentation time at or before
  /// it), its pixels reserved on `budget` (null: unbounded) before they are allocated. `cancelled` (empty: never) is
  /// polled between packets: a cancelled call fails "cancelled" at the next one instead of finishing its decode. An
  /// implementation serializes the calls on one topic and runs those on different topics at once.
  [[nodiscard]] virtual Expected<VideoFrameResult> frameAt(
      ObjectTopicId topic, Timestamp t, const std::shared_ptr<native::MemoryBudget>& budget,
      const std::function<bool()>& cancelled) = 0;
  /// GUI thread, before the dataset's storage changes (a reload, a merge, a topic removal, a clear): forgets the
  /// decoders of its topics without waiting for a call in flight on one, which fails "video source generation changed"
  /// when it completes.
  virtual void retireDataset(DatasetId dataset) = 0;
};

}  // namespace PJ::scripting
