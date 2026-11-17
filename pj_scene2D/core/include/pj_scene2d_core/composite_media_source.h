#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pj_scene2d_core/media_frame.h"
#include "pj_scene2d_core/media_source.h"

namespace PJ {

/// Owns N layer sources, polled in addition order, and fuses their frames:
///   - each layer keeps its last contribution, so a layer with nothing new this poll stays in the composite;
///   - `.pixel_layers` are stacked bottom to top in addition order (a layer that fills only `.base` becomes one pixel
///     layer at the layer's opacity); `.base` is the first pixel layer;
///   - `.overlays` are concatenated in addition order (later layers on top);
///   - pairing (`setLayerPairing`): an annotation that names its image (`image_topic` set) from a pairing layer is
///     shown only when the base layer has that topic and its timestamp is within one cadence of the delivered
///     frame's own stamp (`DecodedFrame::stamp`, else its pts). A rejected annotation stays retained and is checked
///     again at every composite. Other layers' annotations are shown as they are.
/// takeFrame() returns nullopt when no layer produced anything new.
class CompositeMediaSource : public MediaSource {
 public:
  CompositeMediaSource() = default;

  void addLayer(std::unique_ptr<MediaSource> layer);

  /// `opacity` multiplies the opacity of the pixel layers it produces.
  void addLayer(std::unique_ptr<MediaSource> layer, float opacity);

  /// `identity` (the underlying source pointer) matches the layer across a rebuild (adoptContributions(),
  /// setLayerPairing()); nullptr opts out.
  void addLayer(std::unique_ptr<MediaSource> layer, float opacity, const void* identity);

  /// Copies each layer's last contribution from the same-identity layer of `previous` and makes the next takeFrame()
  /// emit even with nothing new. Call right after rebuilding the layer set, or the viewer shows black until every
  /// source decodes again.
  void adoptContributions(const CompositeMediaSource& previous);

  /// Pairing for the layer tagged `identity` (no-op for an unknown one): `image_topic` is the store topic its pixels
  /// come from (what annotations name), `pairs_annotations` whether its own annotations name the image they were made
  /// from (an on-demand derived topic's) and are checked against the shown one, `cadence(entry_ts)` the image
  /// interval around the delivered frame's store entry (`DecodedFrame::entry_ts`, else its pts; empty: 100 ms), and
  /// `warning` receives, at every composite, why its annotations were hidden, or an empty string once none is.
  void setLayerPairing(
      const void* identity, std::string image_topic, bool pairs_annotations, std::function<int64_t(int64_t)> cadence,
      std::function<void(const std::string&)> warning);

  [[nodiscard]] size_t layerCount() const noexcept {
    return layers_.size();
  }

  void setTimestamp(int64_t ts_ns) override;
  std::optional<MediaFrame> takeFrame() override;
  void invalidate() override;
  void setGpuRectificationAvailable(bool available) override;

 private:
  struct Layer {
    std::unique_ptr<MediaSource> source;
    float opacity = 1.0f;
    const void* identity = nullptr;
    // Last contribution, layer opacity folded in: multi-rate layers are the norm, so a layer with nothing new is
    // composited from it (docs/ARCHITECTURE.md "Compositing").
    std::vector<PixelLayer> pixel_layers;
    std::vector<SceneFrame> overlays;
    std::string image_topic;
    bool pairs_annotations = false;
    std::function<int64_t(int64_t)> cadence;
    std::function<void(const std::string&)> warning;
  };

  std::vector<Layer> layers_;
  // Armed by adoptContributions(), cleared by the next takeFrame().
  bool force_emit_ = false;
};

}  // namespace PJ
