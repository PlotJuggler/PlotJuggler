// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "script_media.h"

#include <algorithm>
#include <utility>

namespace PJ::scripting {

ScriptMedia::ScriptMedia(
    std::shared_ptr<const IImageDecoder> decoder, native::NativeLimits limits, Timestamp result_stamp)
    : decoder_(std::move(decoder)), limits_(std::move(limits)), result_stamp_(result_stamp) {}

void ScriptMedia::noteInput(const void* identity, std::string topic, std::string alias, Timestamp stamp) {
  inputs_.push_back({identity, std::move(topic), std::move(alias), stamp});
}

Expected<DecodedPixels> ScriptMedia::decode(const void* identity, const sdk::Image& image) {
  const auto known = std::find_if(
      decoded_.begin(), decoded_.end(), [identity](const Decoded& decoded) { return decoded.identity == identity; });
  if (known != decoded_.end()) {
    return known->pixels;
  }
  if (decoder_ == nullptr) {
    return unexpected("no image decoder");
  }
  Expected<DecodedPixels> pixels = decoder_->decode(image, limits_.budget);
  if (!pixels) {
    return pixels;
  }
  const auto input = std::find_if(
      inputs_.begin(), inputs_.end(), [identity](const Input& input) { return input.identity == identity; });
  if (input != inputs_.end()) {
    pixels->image_topic = input->topic.empty() ? input->alias : input->topic;  // as pairing() names it
  } else {
    pixels->image_topic.clear();
  }
  pixels->timestamp_ns = input != inputs_.end() ? input->stamp : image.timestamp_ns;
  decoded_.push_back({identity, *pixels});
  return pixels;
}

std::pair<std::string, Timestamp> ScriptMedia::pairing(std::string_view image_topic) const {
  for (const Input& input : inputs_) {
    if (input.topic == image_topic || input.alias == image_topic) {
      // An input with no store topic (an evaluation's upstream object) keeps the name the script gave.
      return {input.topic.empty() ? std::string(image_topic) : input.topic, input.stamp};
    }
  }
  return {std::string(image_topic), result_stamp_};
}

}  // namespace PJ::scripting
