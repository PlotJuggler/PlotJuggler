// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "SceneVideoFrameSource.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_base/builtin/video_frame.hpp"
#include "pj_scene2d_core/decoded_frame.h"
#ifdef PJ_HAS_FFMPEG
#include "pj_scene2d_core/ffmpeg_decoder.h"
#include "pj_scene2d_core/video_frame_nal_extractor.h"
#endif

namespace PJ {
#ifdef PJ_HAS_FFMPEG
namespace {

constexpr const char* kRetired = "video source generation changed";

/// The pixels of a frame and their charge, in the image's anchor.
struct ChargedPixels {
  std::vector<uint8_t> rgb;
  scripting::native::MemoryReservation charge;
};

/// `frame` as tightly packed rgb8, its bytes reserved on `budget` before they are allocated.
Expected<scripting::VideoFrameResult> toRgb(
    const DecodedFrame& frame, const std::shared_ptr<scripting::native::MemoryBudget>& budget) {
  const bool yuv = frame.format == PixelFormat::kYUV420P || frame.format == PixelFormat::kNV12;
  if (!frame.isValid() || (!yuv && frame.format != PixelFormat::kRGB888)) {
    return unexpected("the video decoder returned no rgb or 4:2:0 frame");
  }
  const auto width = static_cast<std::size_t>(frame.width);
  const auto height = static_cast<std::size_t>(frame.height);
  auto held = std::make_shared<ChargedPixels>(ChargedPixels{{}, scripting::native::MemoryReservation(budget)});
  if (Status grown = held->charge.grow(width * height * 3); !grown) {
    return unexpected(std::move(grown).error());
  }
  held->rgb.resize(width * height * 3);
  if (!yuv) {
    std::copy_n(frame.pixels->data(), held->rgb.size(), held->rgb.begin());
  } else if (!convertToRgb24(frame, held->rgb.data())) {
    return unexpected("the video frame could not be converted to rgb");
  }
  scripting::VideoFrameResult result;
  result.presentation_ns = frame.pts;
  result.image.width = static_cast<uint32_t>(width);
  result.image.height = static_cast<uint32_t>(height);
  result.image.row_step = result.image.width * 3;
  result.image.encoding = "rgb8";
  result.image.timestamp_ns = frame.pts;
  result.image.data = Span<const uint8_t>(held->rgb.data(), held->rgb.size());
  result.image.anchor = std::move(held);
  return result;
}

}  // namespace
#endif

struct SceneVideoFrameSource::Impl {
  /// One topic's decoder. A call holds it, so retiring it never destroys it under the call.
  struct Topic {
    DatasetId dataset = 0;
    const void* parser = nullptr;  ///< the parser the extractor was made with: its generation
    std::atomic<bool> retired{false};
    std::mutex mutex;  ///< one call at a time: the decoder and its extractor are single-threaded
    StreamingVideoDecoder decoder;
  };

  Impl(ObjectStore& object_store, ExtractorFactory factory, ParserIdentity parser_identity)
      : store(object_store), extractors(std::move(factory)), parsers(std::move(parser_identity)) {}

  ObjectStore& store;
  ExtractorFactory extractors;
  ParserIdentity parsers;
  std::mutex topics_mutex;  ///< guards `topics`; never held while decoding
  std::unordered_map<uint32_t, std::shared_ptr<Topic>> topics;

  /// The topic's decoder for its parser now, made on first use and again when the parser changed (the old one retired,
  /// left to a call that holds it); null for an unknown topic.
  std::shared_ptr<Topic> topic(ObjectTopicId id) {
    const void* parser = parsers ? parsers(id) : nullptr;
    std::lock_guard lock(topics_mutex);
    std::shared_ptr<Topic>& state = topics[id.id];
    if (state != nullptr && state->parser != parser) {
      state->retired.store(true, std::memory_order_release);
      state = nullptr;
    }
    if (state == nullptr) {
      const ObjectTopicDescriptor descriptor = store.descriptor(id);
      if (descriptor.topic_name.empty()) {
        topics.erase(id.id);
        return nullptr;
      }
      state = std::make_shared<Topic>();
      state->dataset = descriptor.dataset_id;
      state->parser = parser;
#ifdef PJ_HAS_FFMPEG
      // `topics_mutex` is held across the factory: it only takes the session's parser-registry lock and builds a
      // closure, and nothing that holds that lock calls into this source, so no lock cycle; a slow factory would hold
      // every topic's lookup.
      state->decoder.attach(&store, id, extractors(id));
#endif
    }
    return state;
  }
};

SceneVideoFrameSource::SceneVideoFrameSource(ObjectStore& store, ExtractorFactory extractors, ParserIdentity parsers)
    : impl_(std::make_unique<Impl>(store, std::move(extractors), std::move(parsers))) {}

SceneVideoFrameSource::~SceneVideoFrameSource() = default;

StreamingVideoDecoder::NalExtractor SceneVideoFrameSource::canonicalExtractor() {
  // The span aliases the decoded object, kept until the next call: the decoder copies it into FFmpeg before then.
  auto last = std::make_shared<std::optional<sdk::BuiltinObject>>();
  return [last](const ResolvedObjectEntry& entry) -> Expected<StreamingVideoDecoder::ExtractedFrame> {
    auto object = deserializeBuiltinObject(
        sdk::BuiltinObjectType::kVideoFrame, entry.payload.bytes.data(), entry.payload.bytes.size());
    if (!object) {
      return unexpected(std::move(object).error());
    }
    *last = std::move(*object);
    const auto* frame = (*last)->get<sdk::VideoFrame>();
    return StreamingVideoDecoder::ExtractedFrame{frame->data, frame->format, frame->timestamp_ns};
  };
}

#ifdef PJ_HAS_FFMPEG
StreamingVideoDecoder::NalExtractor SceneVideoFrameSource::parserExtractor(
    MessageParserPluginBase* parser, std::shared_ptr<std::mutex> mutex, std::shared_ptr<void> keepalive) {
  // Members die in reverse order: the extractor, and the object it parsed last, before the library.
  struct Bound {
    std::shared_ptr<void> keepalive;
    StreamingVideoDecoder::NalExtractor extract;
    Expected<StreamingVideoDecoder::ExtractedFrame> operator()(const ResolvedObjectEntry& entry) const {
      return extract(entry);
    }
  };
  return Bound{std::move(keepalive), makeVideoFrameNalExtractor(parser, std::move(mutex))};
}
#endif

Expected<scripting::VideoFrameResult> SceneVideoFrameSource::frameAt(
    ObjectTopicId topic, Timestamp t, const std::shared_ptr<scripting::native::MemoryBudget>& budget,
    const std::function<bool()>& cancelled) {
#ifdef PJ_HAS_FFMPEG
  const std::shared_ptr<Impl::Topic> state = impl_->topic(topic);
  if (state == nullptr) {
    return unexpected("unknown video topic");
  }
  Expected<DecodedFrame> decoded = unexpected(std::string(kRetired));
  {
    std::lock_guard lock(state->mutex);
    if (state->retired.load(std::memory_order_acquire)) {
      return unexpected(kRetired);
    }
    decoded = state->decoder.decodeAt(t, cancelled ? std::make_shared<CancelToken>(cancelled) : CancelTokenPtr{});
    if (state->retired.load(std::memory_order_acquire)) {
      return unexpected(kRetired);
    }
  }
  if (!decoded) {
    return unexpected(std::move(decoded).error());
  }
  return toRgb(*decoded, budget);  // the frame's pixels are its own: converted outside the topic's lock
#else
  (void)topic;
  (void)t;
  (void)budget;
  (void)cancelled;
  return unexpected("video decoding is unavailable in this build");
#endif
}

void SceneVideoFrameSource::retireDataset(DatasetId dataset) {
  std::lock_guard lock(impl_->topics_mutex);
  std::erase_if(impl_->topics, [dataset](const auto& entry) {
    if (entry.second->dataset != dataset) {
      return false;
    }
    entry.second->retired.store(true, std::memory_order_release);
    return true;
  });
}

}  // namespace PJ
