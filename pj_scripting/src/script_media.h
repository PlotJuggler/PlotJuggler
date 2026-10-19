// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "native/point_cloud_ops.h"
#include "pj_base/builtin/image.hpp"
#include "pj_base/expected.hpp"
#include "pj_base/types.hpp"
#include "pj_scripting/image_decoder.h"

// One run's media state (docs/derived_objects/ARCHITECTURE.md §6): the injected decoder, the image inputs the run
// read, the pixels it decoded, and the pairing of the annotations it makes. Luau-free, so the Luau and Python binders
// share it. One per run, on the run's thread.

namespace PJ::scripting {

class ScriptMedia {
 public:
  /// `decoder` null: every decode fails with "no image decoder". `result_stamp` stamps annotations for no image
  /// input.
  ScriptMedia(std::shared_ptr<const IImageDecoder> decoder, native::NativeLimits limits, Timestamp result_stamp);

  /// Records an image or depth input: `identity` is the struct the script reads, `topic` its store topic, `alias` the
  /// name the recipe gives it, `stamp` its own stamp on the dataset clock.
  void noteInput(const void* identity, std::string topic, std::string alias, Timestamp stamp);

  /// Decodes `image` once per `identity` (the struct the script holds) and run: a second call returns the same
  /// pixels, whose charge the run keeps until it ends. Paired with the input `identity` was noted as (named by
  /// its store topic, else its alias), else unpaired with `image`'s stamp. A failure is not remembered.
  [[nodiscard]] Expected<DecodedPixels> decode(const void* identity, const sdk::Image& image);

  /// What annotations built for `image_topic` pair with: the store topic and stamp of the image input it names (by
  /// its store topic or by its alias; `image_topic` itself when that input has no store topic), else `image_topic`
  /// itself at the result stamp.
  [[nodiscard]] std::pair<std::string, Timestamp> pairing(std::string_view image_topic) const;

  [[nodiscard]] const native::NativeLimits& limits() const noexcept {
    return limits_;
  }

 private:
  struct Input {
    const void* identity = nullptr;
    std::string topic;
    std::string alias;
    Timestamp stamp = 0;
  };
  struct Decoded {
    const void* identity = nullptr;
    DecodedPixels pixels;
  };

  std::shared_ptr<const IImageDecoder> decoder_;
  native::NativeLimits limits_;
  Timestamp result_stamp_ = 0;
  std::vector<Input> inputs_;
  std::vector<Decoded> decoded_;
};

}  // namespace PJ::scripting
