#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <unordered_set>
#include <vector>

#include "pj_scene3d_core/camera/camera.h"

namespace pj::scene3d {

/// When a Scene3D view refits its camera to the scene bounds. The rule, shared
/// by both renderers:
///  - once per newly added layer, on the first frame that layer contributes
///    placed bounds (a layer re-added after removal counts as new);
///  - never over a camera restored from a layout: the restore drops every
///    pending fit and suppresses fits for layers added later (deferred restores
///    included), until the view's layer set becomes empty again.
/// Which layers contribute, and to what bounds, is the view's decision; the
/// policy only tracks layer identity.
class CameraFitPolicy {
 public:
  using LayerKey = const void*;

  /// Adopts the view's current layer set (every layer, visible or not).
  void setLayers(const std::vector<LayerKey>& layers) {
    std::erase_if(
        pending_, [&layers](LayerKey key) { return std::find(layers.begin(), layers.end(), key) == layers.end(); });
    if (layers.empty()) {
      restored_ = false;
    }
    for (LayerKey key : layers) {
      if (!restored_ && std::find(known_.begin(), known_.end(), key) == known_.end()) {
        pending_.insert(key);
      }
    }
    known_ = layers;
  }

  /// The camera was just restored from a layout.
  void noteCameraRestored() {
    pending_.clear();
    restored_ = true;
  }

  [[nodiscard]] bool hasPending() const {
    return !pending_.empty();
  }

  /// Called once per frame with the layers whose placed bounds make up
  /// `bounds`. True (and those layers stop being pending) when `bounds` is valid
  /// and at least one contributor is still pending: the caller fits now.
  [[nodiscard]] bool takeFit(const std::vector<LayerKey>& contributors, const AABB& bounds) {
    if (!bounds.valid || std::none_of(contributors.begin(), contributors.end(), [this](LayerKey key) {
          return pending_.contains(key);
        })) {
      return false;
    }
    for (LayerKey key : contributors) {
      pending_.erase(key);
    }
    return true;
  }

 private:
  std::vector<LayerKey> known_;
  std::unordered_set<LayerKey> pending_;
  bool restored_ = false;
};

}  // namespace pj::scene3d
