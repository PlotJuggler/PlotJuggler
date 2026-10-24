// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "script_media.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "pj_base/builtin/depth_image.hpp"
#include "pj_base/builtin/video_frame.hpp"
#include "script_contract.h"

namespace PJ::scripting {

ScriptMedia::ScriptMedia(
    std::shared_ptr<const IImageDecoder> decoder, native::NativeLimits limits, Timestamp result_stamp,
    std::shared_ptr<IVideoFrameSource> video)
    : decoder_(std::move(decoder)), video_(std::move(video)), limits_(std::move(limits)), result_stamp_(result_stamp) {}

void ScriptMedia::noteInput(
    const void* identity, std::string topic, std::string alias, Timestamp stamp, ObjectTopicId topic_id) {
  inputs_.push_back({identity, std::move(topic), std::move(alias), stamp, topic_id});
}

void ScriptMedia::noteInput(const sdk::BuiltinObject& object, const NamedValue& input) {
  const void* identity = object.get<sdk::Image>();
  if (identity == nullptr) {
    identity = object.get<sdk::DepthImage>();
  }
  if (identity == nullptr) {
    identity = object.get<sdk::VideoFrame>();
  }
  if (identity != nullptr) {
    noteInput(identity, input.topic, input.name, detail::ownStamp(object).value_or(0), input.topic_id);
    held_.push_back(object);
  }
}

const ScriptMedia::Input* ScriptMedia::findInput(const void* identity) const {
  const auto input = std::find_if(
      inputs_.begin(), inputs_.end(), [identity](const Input& candidate) { return candidate.identity == identity; });
  return input == inputs_.end() ? nullptr : &*input;
}

ScriptMedia::Frame* ScriptMedia::findFrame(const void* identity, const sdk::Image& image) {
  const auto frame = std::find_if(frames_.begin(), frames_.end(), [&](const Frame& candidate) {
    return candidate.image == identity && image.anchor != nullptr && candidate.tag.lock() == image.anchor;
  });
  return frame == frames_.end() ? nullptr : &*frame;
}

Expected<sdk::BuiltinObject> ScriptMedia::frameAt(const void* identity, Timestamp t) {
  if (video_ == nullptr) {
    return unexpected("no video frame source");
  }
  const Input* input = findInput(identity);
  if (input == nullptr) {
    return unexpected("video is not an input of this run");
  }
  if (input->topic_id.id == 0) {
    return unexpected("video input '" + input->alias + "' has no store topic");
  }
  Expected<VideoFrameResult> frame = video_->frameAt(input->topic_id, t, limits_.budget, limits_.cancelled);
  if (!frame) {
    return unexpected(std::move(frame).error());
  }
  frame->image.timestamp_ns = frame->presentation_ns;
  const auto tag = std::make_shared<sdk::BufferAnchor>(std::move(frame->image.anchor));  // holds the pixels
  frame->image.anchor = tag;
  sdk::BuiltinObject image(std::move(frame->image));
  frames_.push_back(
      {image.get<sdk::Image>(), tag, static_cast<std::size_t>(input - inputs_.data()), frame->presentation_ns, {}});
  return image;
}

Expected<DecodedPixels> ScriptMedia::decode(const void* identity, const sdk::Image& image) {
  const auto known = std::find_if(
      decoded_.begin(), decoded_.end(), [identity](const Decoded& decoded) { return decoded.identity == identity; });
  if (known != decoded_.end()) {
    return known->pixels;
  }
  if (const Frame* frame = findFrame(identity, image); frame != nullptr && frame->decoded) {
    return *frame->decoded;
  }
  if (decoder_ == nullptr) {
    return unexpected("no image decoder");
  }
  Expected<DecodedPixels> pixels = decoder_->decode(image, limits_.budget);
  if (!pixels) {
    return pixels;
  }
  const auto paired_name = [](const Input& paired) { return paired.topic.empty() ? paired.alias : paired.topic; };
  if (const Input* input = findInput(identity)) {
    pixels->image_topic = paired_name(*input);  // as pairing() names it
    pixels->timestamp_ns = input->stamp;
    decoded_.push_back({identity, *pixels});
  } else if (Frame* frame = findFrame(identity, image)) {
    pixels->image_topic = paired_name(inputs_[frame->input]);
    pixels->timestamp_ns = frame->presentation;
    frame->decoded = *pixels;
  } else {  // no identity that outlives this call: decoded, not remembered
    pixels->image_topic.clear();
    pixels->timestamp_ns = image.timestamp_ns;
  }
  return pixels;
}

std::pair<std::string, Timestamp> ScriptMedia::pairing(std::string_view image_topic) const {
  for (std::size_t index = 0; index < inputs_.size(); ++index) {
    const Input& input = inputs_[index];
    if (input.topic != image_topic && input.alias != image_topic) {
      continue;
    }
    std::optional<Timestamp> shown;
    bool agree = true;
    for (const Frame& frame : frames_) {
      if (frame.input == index) {
        agree = agree && (!shown || *shown == frame.presentation);
        shown = frame.presentation;
      }
    }
    // An input with no store topic (an evaluation's upstream object) keeps the name the script gave.
    return {input.topic.empty() ? std::string(image_topic) : input.topic, shown && agree ? *shown : input.stamp};
  }
  return {std::string(image_topic), result_stamp_};
}

}  // namespace PJ::scripting
