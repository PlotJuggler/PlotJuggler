// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "../rhi_scene_pass.h"
#include "pj_scene3d_core/camera/camera.h"

namespace pj::scene3d {

/// Asynchronous GPU min/max of point instance buffers (PointVertex xyz), for
/// backends with QRhi::Compute (never WebGL2): a compute pass reduces the
/// resident buffer, a buffer readback delivers the box a frame or more later.
/// One reduction per key in flight; results carry the key and the revision they
/// describe, so a caller drops any that no longer match its data.
class PointAabbReducer {
 public:
  struct Result {
    const void* key = nullptr;
    std::uint64_t revision = 0;
    AABB bounds;  ///< invalid when the cloud had no finite point
  };

  PointAabbReducer();
  ~PointAabbReducer();
  PointAabbReducer(const PointAabbReducer&) = delete;
  PointAabbReducer& operator=(const PointAabbReducer&) = delete;

  /// False when the backend has no compute: the caller keeps CPU bounds.
  [[nodiscard]] bool initialize(QRhi* rhi);
  [[nodiscard]] bool available() const {
    return pipeline_ != nullptr;
  }
  /// Asks for the bounds of `count` PointVertex in `points` (created with
  /// StorageBuffer usage). Ignored while this key has a reduction in flight.
  void request(const void* key, std::uint64_t revision, QRhiBuffer* points, quint32 count);
  /// Records every requested reduction in one compute pass, which consumes
  /// `updates` (so it must be the frame's first pass). False when nothing was
  /// recorded; `updates` is then untouched.
  bool record(QRhiCommandBuffer* command_buffer, QRhiResourceUpdateBatch* updates);
  /// The reductions completed since the last call.
  [[nodiscard]] std::vector<Result> takeResults();
  /// Forgets `key`: an in-flight reduction for it completes unseen.
  void forget(const void* key);
  /// Waits for in-flight readbacks (outside a frame; skipped once the QRhi is
  /// gone, which drops them), then releases every resource.
  void release();

 private:
  struct Slot;

  QRhi* rhi_ = nullptr;
  QRhiComputePipeline* pipeline_ = nullptr;
  QRhiBuffer* layout_points_ = nullptr;
  QRhiBuffer* layout_result_ = nullptr;
  QRhiBuffer* layout_params_ = nullptr;
  QRhiShaderResourceBindings* layout_bindings_ = nullptr;
  std::vector<std::unique_ptr<Slot>> slots_;
  std::vector<Result> results_;
};

}  // namespace pj::scene3d
