// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/composite_media_source.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace PJ {

void CompositeMediaSource::addLayer(std::unique_ptr<MediaSource> layer) {
  addLayer(std::move(layer), 1.0f);
}

void CompositeMediaSource::addLayer(std::unique_ptr<MediaSource> layer, float opacity) {
  addLayer(std::move(layer), opacity, nullptr);
}

void CompositeMediaSource::addLayer(std::unique_ptr<MediaSource> layer, float opacity, const void* identity) {
  if (layer == nullptr) {
    return;
  }
  Layer entry;
  entry.source = std::move(layer);
  entry.opacity = std::clamp(opacity, 0.0f, 1.0f);
  entry.identity = identity;
  layers_.push_back(std::move(entry));
}

void CompositeMediaSource::setLayerPairing(
    const void* identity, std::string image_topic, bool pairs_annotations, std::function<int64_t(int64_t)> cadence,
    std::function<void(const std::string&)> warning) {
  for (auto& layer : layers_) {
    if (layer.identity == identity) {
      layer.image_topic = std::move(image_topic);
      layer.pairs_annotations = pairs_annotations;
      layer.cadence = std::move(cadence);
      layer.warning = std::move(warning);
      return;
    }
  }
}

void CompositeMediaSource::adoptContributions(const CompositeMediaSource& previous) {
  // A rebuild rewraps the same underlying sources in a fresh composite whose
  // layer caches are empty, so the first takeFrame() would drop to nullopt and
  // the viewer would blank until each source re-decodes. Copy each persisting
  // layer's last fused output (matched by identity) and arm a one-shot emit so
  // the rebuilt composite paints the current frame on its very next poll.
  for (auto& layer : layers_) {
    if (layer.identity == nullptr) {
      continue;
    }
    const auto match = std::find_if(previous.layers_.begin(), previous.layers_.end(), [&](const Layer& prev) {
      return prev.identity == layer.identity;
    });
    if (match == previous.layers_.end()) {
      continue;
    }
    layer.pixel_layers = match->pixel_layers;
    layer.overlays = match->overlays;
    force_emit_ = true;
  }
}

void CompositeMediaSource::setTimestamp(int64_t ts_ns) {
  for (auto& layer : layers_) {
    layer.source->setTimestamp(ts_ns);
  }
}

void CompositeMediaSource::invalidate() {
  // Fan out to every layer so they drop their per-timestamp dedup and re-decode
  // on the next setTimestamp — used after a composite rebuild re-seeds at the
  // (often unchanged) current tracker time.
  for (auto& layer : layers_) {
    layer.source->invalidate();
  }
}

void CompositeMediaSource::setGpuRectificationAvailable(bool available) {
  // Fan out so each rectifying layer (an ImagePipelineSource behind a
  // BorrowedMediaSource) learns whether to defer undistortion to the GPU.
  for (auto& layer : layers_) {
    layer.source->setGpuRectificationAvailable(available);
  }
}

std::optional<MediaFrame> CompositeMediaSource::takeFrame() {
  // Refresh the cached contribution of every layer that produced a new frame
  // this tick; layers that report nothing new keep their previous contribution.
  bool any_new = false;
  for (auto& layer : layers_) {
    auto fresh = layer.source->takeFrame();
    if (!fresh.has_value()) {
      continue;
    }
    any_new = true;
    layer.pixel_layers.clear();
    layer.overlays.clear();

    if (!fresh->pixel_layers.empty()) {
      for (auto& pixel_layer : fresh->pixel_layers) {
        pixel_layer.opacity = std::clamp(pixel_layer.opacity * layer.opacity, 0.0f, 1.0f);
        layer.pixel_layers.push_back(std::move(pixel_layer));
      }
    } else if (fresh->base.has_value()) {
      layer.pixel_layers.push_back(PixelLayer{std::move(*fresh->base), layer.opacity});
    }
    layer.overlays = std::move(fresh->overlays);
  }

  // Nothing new since the last poll: the consumer keeps the composite it has —
  // unless adoptContributions() armed a one-shot emit after a rebuild, in which
  // case we must hand the carried composite to the (just-repointed) consumer now
  // so it doesn't blank waiting for the next decode.
  if (!any_new && !force_emit_) {
    return std::nullopt;
  }
  force_emit_ = false;

  // Reassemble the full stack from every layer's retained contribution so a
  // layer that did not update this tick is still present in the composite.
  MediaFrame composite;
  const Layer* base_layer = nullptr;
  for (const auto& layer : layers_) {
    for (const auto& pixel_layer : layer.pixel_layers) {
      if (!composite.base.has_value()) {
        composite.base = pixel_layer.frame;
        base_layer = &layer;
      }
      composite.pixel_layers.push_back(pixel_layer);
    }
  }
  constexpr int64_t kDefaultCadence = 100'000'000;
  // Annotations carry the image's own stamp; the cadence is looked up around its store entry.
  const int64_t shown_pts = composite.base.has_value() ? composite.base->pts : -1;
  const int64_t shown_stamp = composite.base.has_value() ? composite.base->stamp.value_or(shown_pts) : -1;
  const int64_t shown_entry_ts = composite.base.has_value() ? composite.base->entry_ts.value_or(shown_pts) : -1;
  std::optional<int64_t> cadence;  // looked up once, by the first annotation that names its image
  const auto pairingCadence = [&] {
    if (!cadence) {
      cadence = base_layer != nullptr && base_layer->cadence ? base_layer->cadence(shown_entry_ts) : kDefaultCadence;
    }
    return *cadence;
  };
  for (const auto& layer : layers_) {
    std::string warning;
    for (const auto& overlay : layer.overlays) {
      SceneFrame shown = overlay;
      std::erase_if(shown.annotations, [&](const ImageAnnotation& annotation) {
        if (!layer.pairs_annotations || annotation.image_topic.empty()) {
          return false;
        }
        const bool paired =
            base_layer != nullptr && annotation.image_topic == base_layer->image_topic &&
            std::abs(static_cast<long double>(annotation.timestamp) - static_cast<long double>(shown_stamp)) <=
                static_cast<long double>(pairingCadence());
        if (!paired) {
          warning = "annotations for " + annotation.image_topic + " at " + std::to_string(annotation.timestamp) +
                    " ns are not paired with the shown image";
        }
        return !paired;
      });
      // An overlay that was empty (a cleared result) still clears; one emptied by the check is not drawn.
      if (overlay.annotations.empty() || !shown.annotations.empty()) {
        composite.overlays.push_back(std::move(shown));
      }
    }
    if (layer.warning) {
      layer.warning(warning);
    }
  }
  return composite;
}

}  // namespace PJ
