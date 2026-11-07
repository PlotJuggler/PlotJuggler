// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// SceneVideoFrameSource, what an on-demand script's `video:frame_at(t)` decodes through
// (docs/archive/derived_objects/SCENARIOS.md, rows MED-13, MED-14, CON-13): the frame presented at a time over
// Scene2D's streaming decoder, a topic id reused after a clear, and a dataset retired while a call is in flight.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <string>
#include <vector>

#include "SceneVideoFrameSource.h"
#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_base/builtin/video_frame.hpp"
#include "pj_scripting/memory_budget.h"
#include "test_mp4_demux.h"

namespace PJ {
namespace {

constexpr Timestamp kSec = 1'000'000'000;
constexpr DatasetId kDataset = 1;

const std::string kFixture = "pj_scene2D/testdata/test_h264_bframes.mp4";

/// The B-frame fixture as canonical VideoFrame entries of `/video`: stored at their DTS from 0, presented at their PTS
/// plus `pts_offset`. Returns the presentation times, ascending.
std::vector<Timestamp> putVideo(ObjectStore& store, ObjectTopicId& topic, Timestamp pts_offset) {
  const auto packets = test::extractAnnexBPackets(kFixture);
  std::vector<Timestamp> presentation;
  const auto registered = store.registerTopic({kDataset, "/video", R"({"builtin_object_type":"kVideoFrame"})"});
  if (!registered || packets.empty()) {
    return presentation;
  }
  topic = *registered;
  const Timestamp origin = packets.front().dts;
  for (const auto& packet : packets) {
    sdk::VideoFrame frame;
    frame.format = "h264";
    frame.timestamp_ns = packet.timestamp - origin + pts_offset;
    frame.data = {packet.data.data(), packet.data.size()};
    if (!store.pushOwned(topic, packet.dts - origin, serializeBuiltinObject(sdk::BuiltinObject(frame)).value())) {
      return {};
    }
    presentation.push_back(frame.timestamp_ns);
  }
  std::sort(presentation.begin(), presentation.end());
  return presentation;
}

SceneVideoFrameSource canonicalSource(ObjectStore& store) {
  return SceneVideoFrameSource(store, [](ObjectTopicId) { return SceneVideoFrameSource::canonicalExtractor(); });
}

TEST(SceneVideoFrameSourceTest, RepeatedAndBackwardSeeksReturnFullFramesAtTheirPresentationTime) {
  ObjectStore store;
  ObjectTopicId topic;
  const auto presentation = putVideo(store, topic, 0);
  ASSERT_GT(presentation.size(), 5u) << "tracked B-frame fixture missing";
  auto source = canonicalSource(store);
  const Timestamp early = presentation[2];
  const Timestamp late = presentation[presentation.size() / 2];
  const auto budget = std::make_shared<scripting::native::MemoryBudget>(std::size_t{64} << 20);

  const auto a = source.frameAt(topic, late, budget, {});
  ASSERT_TRUE(a) << a.error();
  const auto b = source.frameAt(topic, early, budget, {});
  ASSERT_TRUE(b) << b.error();
  const auto c = source.frameAt(topic, late + 1, budget, {});  // between frames: the one on screen
  ASSERT_TRUE(c) << c.error();
  EXPECT_EQ(a->presentation_ns, late);
  EXPECT_EQ(b->presentation_ns, early);
  EXPECT_EQ(c->presentation_ns, late);
  EXPECT_EQ(a->image.timestamp_ns, late);
  EXPECT_EQ(a->image.encoding, "rgb8");
  EXPECT_GT(a->image.width, 0u);
  EXPECT_EQ(a->image.row_step, a->image.width * 3);
  EXPECT_EQ(a->image.data.size(), static_cast<std::size_t>(a->image.row_step) * a->image.height);
  ASSERT_EQ(a->image.data.size(), c->image.data.size());
  EXPECT_TRUE(std::equal(a->image.data.begin(), a->image.data.end(), c->image.data.begin()));
  EXPECT_GE(budget->used(), 3 * a->image.data.size()) << "each frame's pixels are charged while it lives";

  EXPECT_FALSE(source.frameAt(topic, -kSec, budget, {})) << "nothing is presented before the stream starts";
  EXPECT_FALSE(source.frameAt(ObjectTopicId{999}, late, budget, {}));
  const auto tiny = std::make_shared<scripting::native::MemoryBudget>(16);
  const auto refused = source.frameAt(topic, late, tiny, {});
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().find("native memory limit"), std::string::npos) << refused.error();
  EXPECT_EQ(tiny->used(), 0u);
}

// The store's revisions are process-unique: a topic id reused after a clear is a new series, never the indexed one.
TEST(SceneVideoFrameSourceTest, ClearForgetsIndexesAndDecodedFramesWhenTopicIdsAreReused) {
  ObjectStore store;
  ObjectTopicId first;
  const auto before = putVideo(store, first, 0);
  ASSERT_GT(before.size(), 5u);
  auto source = canonicalSource(store);
  const Timestamp target = before[before.size() / 2];
  const auto old_frame = source.frameAt(first, target, nullptr, {});
  ASSERT_TRUE(old_frame) << old_frame.error();
  EXPECT_EQ(old_frame->presentation_ns, target);

  store.clear();
  ObjectTopicId second;
  const auto after = putVideo(store, second, 10 * kSec);
  ASSERT_EQ(first, second);
  const auto frame = source.frameAt(second, target + 10 * kSec, nullptr, {});
  ASSERT_TRUE(frame) << frame.error();
  EXPECT_EQ(frame->presentation_ns, target + 10 * kSec);
  // A query in the reorder gap before the first presented frame shows that frame.
  const auto clamped = source.frameAt(second, 0, nullptr, {});
  ASSERT_TRUE(clamped) << clamped.error();
  EXPECT_EQ(clamped->presentation_ns, after.front());
}

// Retiring a dataset neither waits for a call in flight on its topic nor destroys the decoder under it: the call fails
// when it completes, and the next one decodes on a fresh decoder.
TEST(SceneVideoFrameSourceTest, InvalidationRetiresInFlightIndexWithoutBlockingOrDestroyingIt) {
  ObjectStore store;
  ObjectTopicId topic;
  const auto presentation = putVideo(store, topic, 0);
  ASSERT_GT(presentation.size(), 5u);
  std::promise<void> entered;
  std::promise<void> resume;
  auto release = resume.get_future().share();
  std::atomic<bool> first{false};
  int decoders_made = 0;
  SceneVideoFrameSource source(store, [&](ObjectTopicId) -> StreamingVideoDecoder::NalExtractor {
    ++decoders_made;
    return [&, canonical = SceneVideoFrameSource::canonicalExtractor()](const ResolvedObjectEntry& entry) {
      if (first.exchange(false)) {
        entered.set_value();
        release.wait();
      }
      return canonical(entry);
    };
  });
  const Timestamp target = presentation[presentation.size() / 2];
  ASSERT_TRUE(source.frameAt(topic, target, nullptr, {}));
  source.retireDataset(kDataset + 1);  // another dataset's: the decoder stays
  ASSERT_TRUE(source.frameAt(topic, presentation[1], nullptr, {}));
  EXPECT_EQ(decoders_made, 1);

  first = true;  // the seek to `target` feeds packets, so the next call parks inside the decoder
  auto pending = std::async(std::launch::async, [&] { return source.frameAt(topic, target, nullptr, {}); });
  ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(10)), std::future_status::ready);
  source.retireDataset(kDataset);  // returns while the call is parked inside the decoder
  resume.set_value();
  const auto stale = pending.get();
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error(), "video source generation changed");
  const auto fresh = source.frameAt(topic, target, nullptr, {});
  ASSERT_TRUE(fresh) << fresh.error();
  EXPECT_EQ(fresh->presentation_ns, target);
  EXPECT_EQ(decoders_made, 2);
}

// A topic's parser registered again (a stream retyped) is a new generation: the next call decodes through a decoder
// made with the new parser's extractor, while the old one is left to the call that holds it.
TEST(SceneVideoFrameSourceTest, AReRegisteredParserIsUsedByTheNextFrame) {
  ObjectStore store;
  ObjectTopicId topic;
  const auto presentation = putVideo(store, topic, 0);
  ASSERT_GT(presentation.size(), 5u);
  int parser_a = 0;
  int parser_b = 0;
  const void* current = &parser_a;
  std::map<const void*, int> extracted;
  SceneVideoFrameSource source(
      store,
      [&](ObjectTopicId) -> StreamingVideoDecoder::NalExtractor {
        return [&, made_with = current,
                canonical = SceneVideoFrameSource::canonicalExtractor()](const ResolvedObjectEntry& entry) {
          ++extracted[made_with];
          return canonical(entry);
        };
      },
      [&](ObjectTopicId) { return current; });
  const Timestamp target = presentation[presentation.size() / 2];
  ASSERT_TRUE(source.frameAt(topic, target, nullptr, {}));
  ASSERT_GT(extracted[&parser_a], 0);
  current = &parser_b;
  const int through_a = extracted[&parser_a];
  const auto frame = source.frameAt(topic, target, nullptr, {});
  ASSERT_TRUE(frame) << frame.error();
  EXPECT_EQ(frame->presentation_ns, target);
  EXPECT_GT(extracted[&parser_b], 0);
  EXPECT_EQ(extracted[&parser_a], through_a) << "the old parser's decoder is not used again";
}

// A run cancelled while its frame's GOP is fed stops at the next packet, so the topic's next call does not wait for the
// whole walk.
TEST(SceneVideoFrameSourceTest, ACancelledCallStopsInsideItsGopWalk) {
  ObjectStore store;
  ObjectTopicId topic;
  const auto presentation = putVideo(store, topic, 0);
  ASSERT_GT(presentation.size(), 10u);
  int extracted = 0;
  SceneVideoFrameSource source(store, [&](ObjectTopicId) -> StreamingVideoDecoder::NalExtractor {
    return [&, canonical = SceneVideoFrameSource::canonicalExtractor()](const ResolvedObjectEntry& entry) {
      ++extracted;
      return canonical(entry);
    };
  });
  ASSERT_TRUE(source.frameAt(topic, presentation[0], nullptr, {}));  // the index is built
  const int armed_at = extracted;
  const Timestamp last = presentation.back();
  const auto cancelled = source.frameAt(topic, last, nullptr, [&] { return extracted >= armed_at + 2; });
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error(), "cancelled");
  EXPECT_LE(extracted, armed_at + 3) << "the walk stops at the packet after the cancel";
  const auto next = source.frameAt(topic, last, nullptr, {});
  ASSERT_TRUE(next) << next.error();
  EXPECT_EQ(next->presentation_ns, last);
}

}  // namespace
}  // namespace PJ
