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

/// Multi-layer MediaSource. Fans setTimestamp() out to all owned layers and
/// fuses their MediaFrames into one composite frame returned by takeFrame().
///
/// Fusion rules:
///   - Pixel base: the FIRST layer that produces pixels wins for legacy `.base`.
///     The ordered `.pixel_layers` vector is populated bottom-to-top in add
///     order. Layers that only populate legacy `.base` are adapted into one
///     PixelLayer using this composite layer's opacity.
///   - Overlays: every layer's `.overlays` are concatenated in layer order
///     (so overlays from layer 0 render under overlays from layer 1).
///   - Pairing: an annotation naming its image (`image_topic` not empty) from a layer
///     set to pair its annotations (`setLayerPairing`) is shown only over the image
///     it names, at its time: the base layer's image topic must match, and its
///     timestamp must lie within one cadence of the delivered frame's own stamp
///     (`DecodedFrame::stamp`, else its pts), so an image still decoding never pairs
///     with the tracker's instant. A rejected annotation stays retained and is
///     checked again at every composite, so it appears once its image arrives. The
///     annotations of any other layer (a parser's, whose `image_topic` is only a
///     header frame id) are shown as they are.
///
/// Returns nullopt if no layer produced new data on this poll.
///
/// Ownership: the compositor takes ownership of every added layer. Layers
/// are polled in the order they were added.
class CompositeMediaSource : public MediaSource {
 public:
  CompositeMediaSource() = default;

  /// Append a layer. Layers are polled in addition order during takeFrame().
  void addLayer(std::unique_ptr<MediaSource> layer);

  /// Append a layer with opacity applied to pixel layers produced by it.
  void addLayer(std::unique_ptr<MediaSource> layer, float opacity);

  /// Append a layer tagged with a stable `identity` (the underlying source
  /// pointer). The identity lets a rebuilt composite inherit this layer's last
  /// fused output from its predecessor via adoptContributions(). `identity` may
  /// be nullptr (the layer then never participates in carry-over).
  void addLayer(std::unique_ptr<MediaSource> layer, float opacity, const void* identity);

  /// Seed each layer's retained contribution from the matching layer (same
  /// identity) in `previous`, and arm a one-shot emission so the very next
  /// takeFrame() returns the carried composite even though no source produced a
  /// new frame this tick. Call right after rebuilding the layer set so the
  /// viewer keeps showing the current frame instead of going black until each
  /// source re-decodes (the "black until play" symptom on add/remove/hide).
  void adoptContributions(const CompositeMediaSource& previous);

  /// Pairing for the layer tagged `identity` (no-op for an unknown one): `image_topic` is the store topic its pixels
  /// come from (what annotations name), `pairs_annotations` whether its own annotations name the image they were made
  /// from (an on-demand derived topic's) and are checked against the shown one, `cadence(entry_ts)` the image
  /// interval around the delivered frame's store entry (`DecodedFrame::entry_ts`, else its pts; empty: 100 ms), and
  /// `warning` receives, at every composite, why its annotations were hidden, or an empty string once none is.
  void setLayerPairing(
      const void* identity, std::string image_topic, bool pairs_annotations, std::function<int64_t(int64_t)> cadence,
      std::function<void(const std::string&)> warning);

  /// Number of layers currently owned.
  [[nodiscard]] size_t layerCount() const noexcept {
    return layers_.size();
  }

  void setTimestamp(int64_t ts_ns) override;
  std::optional<MediaFrame> takeFrame() override;
  void invalidate() override;
  /// Fans the capability out to every owned layer (each rectifying layer decides
  /// individually whether to defer undistortion to the GPU).
  void setGpuRectificationAvailable(bool available) override;

 private:
  struct Layer {
    std::unique_ptr<MediaSource> source;
    float opacity = 1.0f;
    // Stable handle to the underlying (borrowed) source, used to match this
    // layer against the same layer in a predecessor composite during a rebuild.
    // nullptr opts the layer out of carry-over.
    const void* identity = nullptr;
    // Last contribution (layer opacity already folded into the pixel layers),
    // retained so a tick where this layer reports nothing new still composites
    // its most recent output instead of dropping it (REQUIREMENTS §4.8 —
    // multi-rate layers are the norm; the compositor never drops a layer).
    std::vector<PixelLayer> pixel_layers;
    std::vector<SceneFrame> overlays;
    // Pairing (setLayerPairing).
    std::string image_topic;
    bool pairs_annotations = false;
    std::function<int64_t(int64_t)> cadence;
    std::function<void(const std::string&)> warning;
  };

  std::vector<Layer> layers_;
  // One-shot: armed by adoptContributions() so the next takeFrame() emits the
  // carried composite even when no layer produced a new frame this tick.
  bool force_emit_ = false;
};

}  // namespace PJ
