// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "native/point_cloud_ops.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/image.hpp"
#include "pj_base/expected.hpp"
#include "pj_base/types.hpp"
#include "pj_scripting/image_decoder.h"
#include "pj_scripting/object_script.h"
#include "pj_scripting/video_frame_source.h"

// One run's media state (docs/derived_objects/ARCHITECTURE.md §6): the injected decoder and video source, the image
// and video inputs the run read, the pixels and frames it decoded, and the pairing of the annotations it makes.
// Luau-free, so the Luau and Python binders share it. One per run, on the run's thread.

namespace PJ::scripting {

class ScriptMedia {
 public:
  /// `decoder` null: every decode fails with "no image decoder"; `video` null: every `frameAt` fails with "no video
  /// frame source". `result_stamp` stamps annotations for no image input.
  ScriptMedia(
      std::shared_ptr<const IImageDecoder> decoder, native::NativeLimits limits, Timestamp result_stamp,
      std::shared_ptr<IVideoFrameSource> video = nullptr);

  /// Records an image, depth or video input: `identity` is the struct the script reads, which the caller keeps alive
  /// for the run (so no other object takes its address), `topic` its store topic, `alias` the name the recipe gives
  /// it, `stamp` its own stamp on the dataset clock, `topic_id` its store topic's id (what a video's frames decode
  /// from).
  void noteInput(
      const void* identity, std::string topic, std::string alias, Timestamp stamp, ObjectTopicId topic_id = {});
  /// `noteInput` for `object`, the value of `input` the script reads, when it is an image, depth image or video; holds
  /// it until the media ends, so its address names that input for the whole run even after the script drops it.
  void noteInput(const sdk::BuiltinObject& object, const NamedValue& input);

  /// The frame of the video input `identity` presented at `t`, an rgb8 image whose pixels are charged to the run's
  /// budget; `t` and its presentation time are on the dataset clock. It pairs with the input's topic at its
  /// presentation time, when decoded and when an annotations builder names the topic (see `pairing`).
  [[nodiscard]] Expected<sdk::BuiltinObject> frameAt(const void* identity, Timestamp t);

  /// Decodes `image` once per `identity` (the struct the script holds) and run, when that is an input or a live
  /// frame: a second call returns the same pixels, whose charge the run keeps until it ends. Paired with the input
  /// `identity` was noted as (named by its store topic, else its alias) or the frame's input at its presentation time,
  /// else unpaired with `image`'s stamp and not remembered. A failure is not remembered.
  [[nodiscard]] Expected<DecodedPixels> decode(const void* identity, const sdk::Image& image);

  /// What annotations built for `image_topic` pair with: the store topic and stamp of the image input it names (by
  /// its store topic or by its alias; `image_topic` itself when that input has no store topic), else `image_topic`
  /// itself at the result stamp. A video input's stamp is the presentation time of its frames decoded so far when they
  /// all agree, else its own.
  [[nodiscard]] std::pair<std::string, Timestamp> pairing(std::string_view image_topic) const;

  /// Gives `stamp`, when 0, the result's stamp: a builder's output is born unstamped.
  void stampUnset(Timestamp& stamp) const noexcept {
    if (stamp == 0) {
      stamp = result_stamp_;
    }
  }

  [[nodiscard]] const native::NativeLimits& limits() const noexcept {
    return limits_;
  }

 private:
  struct Input {
    const void* identity = nullptr;
    std::string topic;
    std::string alias;
    Timestamp stamp = 0;
    ObjectTopicId topic_id;
  };
  /// A frame `frameAt` made: its image's struct, the input it came from (an index into `inputs_`), when it presents.
  /// The script may drop the image and another take its address, so it is the frame only while `tag`, an anchor only
  /// this frame's image holds, is alive and is that image's anchor.
  struct Frame {
    const void* image = nullptr;
    std::weak_ptr<const void> tag;
    std::size_t input = 0;
    Timestamp presentation = 0;
    std::optional<DecodedPixels> decoded;
  };
  struct Decoded {
    const void* identity = nullptr;
    DecodedPixels pixels;
  };

  /// The input noted for the struct `identity`, or null.
  [[nodiscard]] const Input* findInput(const void* identity) const;
  /// The live frame whose image is `image` at `identity`, or null.
  [[nodiscard]] Frame* findFrame(const void* identity, const sdk::Image& image);

  std::shared_ptr<const IImageDecoder> decoder_;
  std::shared_ptr<IVideoFrameSource> video_;
  native::NativeLimits limits_;
  Timestamp result_stamp_ = 0;
  std::vector<Input> inputs_;
  std::vector<sdk::BuiltinObject> held_;  ///< the media inputs, alive for the run: `inputs_` names them by address
  std::vector<Decoded> decoded_;
  std::vector<Frame> frames_;
};

}  // namespace PJ::scripting
