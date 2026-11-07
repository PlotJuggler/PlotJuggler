// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Coverage for TransformService's incremental TF ingest (H.20). The UID-keyed
// cursor in ingestNewerThanCursor() is the heart of PR #174 (live + scrubbable
// streaming TF) and the bulk file path both, yet shipped with zero tests — the
// amcl-bag frozen-TF and out-of-order-ingest history shows how easily this
// cursor/eviction logic drifts. These tests exercise it with only a
// SessionManager + a mock FrameTransforms parser: no GL, no Qt widgets.
//
// Test map (a-f from the review Fix block; g-j are UID/eviction/window extras):
//   (a) bulk ingest resolves transforms across the whole stamp range
//   (b) N one-at-a-time incremental ingests == one bulk ingest (same buffer)
//   (c) a redundant ingest with nothing new returns false and changes nothing
//   (d) a non-TF topic is classified once and never re-decoded (parse counter)
//   (e) a reparent-conflict edge drops; the rest resolve; ingest continues
//   (f) invalidateDataset clears the SAME shared_ptr in place
//   (g) equal-timestamp late append: a same-stamp arrival is NOT lost (UID cursor)
//   (h) eviction continuity: ingest continues past a front-evicted gap
//   (i) transient probe failure (M.34): a topic that fails parse once is not
//       permanently blacklisted — it classifies as TF on a later tick
//   (j) live window (H.11): an over-window edge drops old samples but the newest
//       resolves; a single-sample (static-like) edge still resolves at late stamps

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QString>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/frame_transforms.hpp"
#include "pj_base/builtin/frame_transforms_codec.hpp"
#include "pj_base/builtin/occupancy_grid.hpp"
#include "pj_base/dataset.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/merge_result.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/TransformService.h"
#include "pj_tf/tf_buffer.h"

namespace {

using PJ::TransformService;
using pj::tf::Clock;
using pj::tf::TimePoint;
using pj::tf::TransformBuffer;

constexpr std::string_view kTfSchema = "mock/frame_transforms";
constexpr std::string_view kGridSchema = "mock/occupancy_grid";

// -----------------------------------------------------------------------------
// Trivial self-describing payload format for the mock parsers.
//
// A FrameTransforms payload is a flat byte buffer: each edge is a 6-byte record
// {parent_index, child_index, stamp_offset_ns(int32 LE)}. Frame name for index N
// is the literal "f<N>". The stamp offset is added to the entry's store
// timestamp so a single payload can carry several edges at distinct sensor
// stamps. An empty payload is a no-op (zero edges). This keeps the mock
// capture-free so makeBoundHandle's create lambda has no captures (see below).
// -----------------------------------------------------------------------------

constexpr std::size_t kEdgeRecordBytes = 6;

std::string frameName(uint8_t index) {
  return "f" + std::to_string(static_cast<int>(index));
}

void appendEdge(std::vector<uint8_t>& payload, uint8_t parent_index, uint8_t child_index, int32_t stamp_offset_ns) {
  payload.push_back(parent_index);
  payload.push_back(child_index);
  uint32_t bits = 0;
  std::memcpy(&bits, &stamp_offset_ns, sizeof(bits));
  payload.push_back(static_cast<uint8_t>(bits & 0xFFU));
  payload.push_back(static_cast<uint8_t>((bits >> 8) & 0xFFU));
  payload.push_back(static_cast<uint8_t>((bits >> 16) & 0xFFU));
  payload.push_back(static_cast<uint8_t>((bits >> 24) & 0xFFU));
}

// One edge, parent f<parent>-child f<child>, at the entry's own store timestamp.
std::vector<uint8_t> edgePayload(uint8_t parent_index, uint8_t child_index) {
  std::vector<uint8_t> payload;
  appendEdge(payload, parent_index, child_index, /*stamp_offset_ns=*/0);
  return payload;
}

// `vary_translation` makes the translation depend on the entry timestamp, so
// consecutive entries on one edge differ (the buffer does not store a repeat).
PJ::sdk::FrameTransforms decodeFrameTransforms(
    PJ::Timestamp entry_ts, const PJ::sdk::PayloadView& payload, bool vary_translation = false) {
  PJ::sdk::FrameTransforms result;
  const auto& bytes = payload.bytes;
  for (std::size_t offset = 0; offset + kEdgeRecordBytes <= bytes.size(); offset += kEdgeRecordBytes) {
    const uint8_t parent_index = bytes[offset];
    const uint8_t child_index = bytes[offset + 1];
    uint32_t bits = static_cast<uint32_t>(bytes[offset + 2]) | (static_cast<uint32_t>(bytes[offset + 3]) << 8) |
                    (static_cast<uint32_t>(bytes[offset + 4]) << 16) | (static_cast<uint32_t>(bytes[offset + 5]) << 24);
    int32_t stamp_offset_ns = 0;
    std::memcpy(&stamp_offset_ns, &bits, sizeof(stamp_offset_ns));

    PJ::sdk::FrameTransform edge;
    edge.timestamp = entry_ts + stamp_offset_ns;
    edge.parent_frame_id = frameName(parent_index);
    edge.child_frame_id = frameName(child_index);
    edge.translation = {1.0 + (vary_translation ? static_cast<double>(entry_ts) : 0.0), 2.0, 3.0};
    edge.rotation = {0.0, 0.0, 0.0, 1.0};  // identity (w=1) — a valid unit quaternion
    result.transforms.push_back(std::move(edge));
  }
  return result;
}

// FrameTransforms parser that decodes the format above and bumps `counter` once
// per parseObject call (so a test can assert how many times the topic was
// decoded). The first call may be forced to fail when `fail_first` is engaged —
// covers the transient-probe-failure path (M.34).
class CountingTfParser : public PJ::MessageParserPluginBase {
 public:
  CountingTfParser(std::atomic<int>* counter, std::atomic<bool>* fail_first, bool vary_translation = false) {
    registerSchemaHandler(
        kTfSchema,
        PJ::sdk::SchemaHandler{
            .object_type = PJ::sdk::BuiltinObjectType::kFrameTransforms,
            .parse_scalars = {},
            .parse_object = [counter, fail_first, vary_translation](
                                PJ::Timestamp ts, PJ::sdk::PayloadView payload) -> PJ::Expected<PJ::sdk::ObjectRecord> {
              if (counter != nullptr) {
                counter->fetch_add(1);
              }
              if (fail_first != nullptr && fail_first->exchange(false)) {
                return PJ::unexpected(std::string("forced transient parse failure"));
              }
              return PJ::sdk::ObjectRecord{.ts = ts, .object = decodeFrameTransforms(ts, payload, vary_translation)};
            },
        });
  }
};

class MalformedTfParser : public PJ::MessageParserPluginBase {
 public:
  MalformedTfParser() {
    registerSchemaHandler(
        kTfSchema,
        PJ::sdk::SchemaHandler{
            .object_type = PJ::sdk::BuiltinObjectType::kFrameTransforms,
            .parse_scalars = {},
            .parse_object = [](PJ::Timestamp ts, PJ::sdk::PayloadView) -> PJ::Expected<PJ::sdk::ObjectRecord> {
              auto validEdge = [ts](std::string parent, std::string child) {
                PJ::sdk::FrameTransform edge;
                edge.timestamp = ts;
                edge.parent_frame_id = std::move(parent);
                edge.child_frame_id = std::move(child);
                edge.translation = {1.0, 2.0, 3.0};
                edge.rotation = {0.0, 0.0, 0.0, 1.0};
                return edge;
              };

              PJ::sdk::FrameTransforms transforms;
              transforms.transforms.push_back(validEdge("f0", "f1"));
              transforms.transforms.push_back(validEdge("f0", "f2"));
              transforms.transforms.push_back(validEdge("f3", "f2"));  // reparent conflict
              transforms.transforms.push_back(validEdge("f4", "f4"));  // self-loop
              auto invalid_rotation = validEdge("f0", "f5");
              invalid_rotation.rotation.x = 0.0;
              invalid_rotation.rotation.y = 0.0;
              invalid_rotation.rotation.z = 0.0;
              invalid_rotation.rotation.w = 0.0;
              transforms.transforms.push_back(std::move(invalid_rotation));
              auto invalid_translation = validEdge("f0", "f6");
              invalid_translation.translation.x = std::numeric_limits<double>::infinity();
              transforms.transforms.push_back(std::move(invalid_translation));
              transforms.transforms.push_back(validEdge("f0", ""));
              return PJ::sdk::ObjectRecord{.ts = ts, .object = std::move(transforms)};
            },
        });
  }
};

// Non-TF parser: always emits a 1x1 OccupancyGrid and bumps `counter` per call,
// so a test can prove the classification probe never re-decodes a non-TF topic.
class CountingGridParser : public PJ::MessageParserPluginBase {
 public:
  explicit CountingGridParser(std::atomic<int>* counter) {
    registerSchemaHandler(
        kGridSchema,
        PJ::sdk::SchemaHandler{
            .object_type = PJ::sdk::BuiltinObjectType::kOccupancyGrid,
            .parse_scalars = {},
            .parse_object =
                [counter](PJ::Timestamp ts, PJ::sdk::PayloadView /*payload*/) -> PJ::Expected<PJ::sdk::ObjectRecord> {
              if (counter != nullptr) {
                counter->fetch_add(1);
              }
              static const uint8_t k_cell[1] = {0};
              PJ::sdk::OccupancyGrid grid;
              grid.timestamp_ns = ts;
              grid.frame_id = "map";
              grid.resolution = 0.05;
              grid.width = 1;
              grid.height = 1;
              grid.data = PJ::Span<const uint8_t>(k_cell, 1);
              return PJ::sdk::ObjectRecord{.ts = ts, .object = grid};
            },
        });
  }
};

// Each call site must pass a distinct lambda type: vtableWithCreate() holds one
// `static` vtable per CreateFn instantiation, so a shared plain function-pointer
// type would latch the first create function for every handle (see the rebind
// test's identical note). The bound schema is selected per call.
template <typename CreateFn>
std::unique_ptr<PJ::MessageParserHandle> makeBoundHandle(std::string_view schema, CreateFn create_fn) {
  static constexpr const char* kManifest =
      R"({"id":"counting-tf-parser","name":"Counting TF Parser","version":"1.0.0","encoding":["mock"]})";
  auto handle =
      std::make_unique<PJ::MessageParserHandle>(PJ::MessageParserPluginBase::vtableWithCreate(create_fn, kManifest));
  EXPECT_TRUE(handle->valid());
  const auto bound = handle->bindSchema(schema, {});
  EXPECT_TRUE(bound.has_value());
  return handle;
}

PJ::ObjectTopicId registerTopic(PJ::ObjectStore& store, PJ::DatasetId dataset_id, const std::string& name) {
  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = dataset_id;
  desc.topic_name = name;
  const auto topic_id = store.registerTopic(desc);
  EXPECT_TRUE(topic_id.has_value());
  return *topic_id;
}

// `vary_translation`: an entry's translation follows its store timestamp, so successive
// entries on one edge are distinct samples rather than skipped repeats.
PJ::ObjectTopicId registerMockTfTopic(
    PJ::SessionManager& session, PJ::DatasetId dataset_id, const std::string& name, bool vary_translation = false) {
  const PJ::ObjectTopicId topic = registerTopic(session.objectStore(), dataset_id, name);
  // One lambda per branch: each lambda type gets its own vtable (see makeBoundHandle).
  if (vary_translation) {
    session.registerObjectTopicParser(topic, makeBoundHandle(kTfSchema, []() noexcept -> void* {
                                        return new CountingTfParser(nullptr, nullptr, /*vary_translation=*/true);
                                      }));
  } else {
    session.registerObjectTopicParser(
        topic, makeBoundHandle(kTfSchema, []() noexcept -> void* { return new CountingTfParser(nullptr, nullptr); }));
  }
  return topic;
}

void invokeTfTap(
    PJ::SessionManager& session, PJ::ObjectTopicId topic, PJ::DatasetId dataset_id, PJ::Timestamp stamp,
    const std::vector<uint8_t>& payload) {
  ASSERT_TRUE(session.ingestTaps().invoke(
      PJ::sdk::BuiltinObjectType::kFrameTransforms, topic, dataset_id, stamp, PJ::sdk::makePayloadView(payload)));
}

std::vector<std::string> sortedFrames(const TransformBuffer& buffer) {
  std::vector<std::string> frames = buffer.getAllFrames();
  std::sort(frames.begin(), frames.end());
  return frames;
}

// True iff a lookup at `stamp` for child<-its-known-parent succeeds. Looks up the
// child relative to the world root (frame "f0" in every fixture below), so the
// whole composed chain must be resolvable. Fixtures stamp their edges at the entry
// time unless a test says otherwise, so the clock matters only where it is passed.
bool resolves(
    const TransformBuffer& buffer, const std::string& target, const std::string& source, int64_t stamp_ns,
    Clock clock = Clock::kHeader) {
  return buffer.tryLookupTransform(target, source, TimePoint{std::chrono::nanoseconds(stamp_ns)}, clock).has_value();
}

// -----------------------------------------------------------------------------
// Canonical (parser-less) TF helpers. Unlike the mock CountingTfParser above —
// which derives each edge's inner stamp from the entry's STORE timestamp and so
// would silently absorb a merge time-shift — the canonical path carries the
// per-transform stamp INSIDE the serialized payload. That is the field a dataset
// merge with a non-zero shift must move; the bytes are otherwise untouched. A
// topic with builtin_object_type=kFrameTransforms metadata and no bound parser
// classifies and decodes through resolveObject()'s canonical codec branch.
// -----------------------------------------------------------------------------

constexpr std::string_view kCanonicalTfMetadata = R"({"builtin_object_type":"kFrameTransforms"})";

PJ::ObjectTopicId registerCanonicalTfTopic(PJ::ObjectStore& store, PJ::DatasetId dataset_id, const std::string& name) {
  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = dataset_id;
  desc.topic_name = name;
  desc.metadata_json = std::string(kCanonicalTfMetadata);
  const auto topic_id = store.registerTopic(desc);
  EXPECT_TRUE(topic_id.has_value());
  return *topic_id;
}

// One edge parent->child carrying its own absolute inner stamp in the payload.
std::vector<uint8_t> canonicalEdgePayload(
    const std::string& parent, const std::string& child, PJ::Timestamp inner_stamp_ns) {
  PJ::sdk::FrameTransforms ft;
  PJ::sdk::FrameTransform edge;
  edge.timestamp = inner_stamp_ns;
  edge.parent_frame_id = parent;
  edge.child_frame_id = child;
  edge.translation = {1.0, 2.0, 3.0};
  edge.rotation = {0.0, 0.0, 0.0, 1.0};  // identity (w=1)
  ft.transforms.push_back(std::move(edge));
  return PJ::serializeFrameTransforms(ft);
}

// -----------------------------------------------------------------------------
// Dataset merge time-shift: a source folded into the anchor with a non-zero raw
// shift (the Source-Timeline arrangement case) must have its TF moved in time,
// just like its scalar samples and object-entry timestamps. The merge shifts the
// object-ENTRY timestamp but leaves the serialized payload (and its inner
// per-transform stamp) untouched; canonical TF is keyed by that inner stamp, so
// today the source's frames land at their ORIGINAL time on the anchor's clock.
//
// This drives the exact post-merge rebuild the MainWindow datasetsMerged handler
// runs: invalidate the consumed source, invalidate the anchor, bulk re-ingest the
// anchor. The discriminator is the EXPECT_FALSE — a TF lookup strictly before an
// edge's first sample returns nothing, so a correctly-shifted source edge no
// longer resolves at its pre-shift time. It FAILS today (the edge is still at the
// original stamp) and must pass once the shift reaches the inner stamps.
// -----------------------------------------------------------------------------
TEST(TransformService, MergeShiftMovesCanonicalTfInTime) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();

  const auto anchor_tf = registerCanonicalTfTopic(store, /*dataset_id=*/1, "/tf");
  const auto source_tf = registerCanonicalTfTopic(store, /*dataset_id=*/2, "/tf");

  constexpr PJ::Timestamp kT0 = 1'000'000'000;     // 1 s, the shared absolute base
  constexpr PJ::Timestamp kShift = 5'000'000'000;  // +5 s applied to the source on merge

  // Anchor edge f0->f1 at T0 (the anchor is never shifted).
  ASSERT_TRUE(store.pushOwned(anchor_tf, kT0, canonicalEdgePayload("f0", "f1", kT0)).has_value());
  // Source edge f0->f2 at the SAME absolute T0; the merge shifts it by +kShift.
  ASSERT_TRUE(store.pushOwned(source_tf, kT0, canonicalEdgePayload("f0", "f2", kT0)).has_value());

  ASSERT_TRUE(store.mergeDatasets(/*anchor_id=*/1, {PJ::DatasetMergeSource{.dataset_id = 2, .raw_shift_ns = kShift}})
                  .has_value());

  // Rebuild TF exactly as MainWindow's datasetsMerged handler does.
  TransformService service(session);
  service.invalidateDataset(/*consumed=*/2);
  service.invalidateDataset(/*anchor=*/1);
  service.ingestFrameTransformsForDataset(/*anchor=*/1);

  auto buffer = service.transformBuffer(/*dataset_id=*/1);
  ASSERT_NE(buffer, nullptr);

  // The anchor's own edge is untouched.
  EXPECT_TRUE(resolves(*buffer, "f0", "f1", kT0)) << "anchor edge must stay at its original time";

  // The merged source edge must move WITH the shift.
  EXPECT_TRUE(resolves(*buffer, "f0", "f2", kT0 + kShift)) << "shifted source edge must resolve at the shifted time";
  EXPECT_FALSE(resolves(*buffer, "f0", "f2", kT0))
      << "shifted source edge must NOT resolve at its pre-shift time (the merge shift must reach the inner stamp)";
  // The entry clock is the store's, which the merge shifts too.
  EXPECT_TRUE(resolves(*buffer, "f0", "f2", kT0 + kShift, Clock::kEntry));
  EXPECT_FALSE(resolves(*buffer, "f0", "f2", kT0, Clock::kEntry));
}

// -----------------------------------------------------------------------------
// (a) Bulk ingest resolves transforms across the whole stamp range.
// -----------------------------------------------------------------------------
TEST(TransformService, BulkIngestResolvesAcrossStampRange) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const auto topic = registerTopic(store, /*dataset_id=*/1, "/tf");

  // A 3-deep chain f0 -> f1 -> f2 published at three increasing stamps.
  const std::vector<int64_t> stamps = {100, 200, 300};
  for (const int64_t stamp : stamps) {
    std::vector<uint8_t> payload;
    appendEdge(payload, /*parent=*/0, /*child=*/1, /*offset=*/0);
    appendEdge(payload, /*parent=*/1, /*child=*/2, /*offset=*/0);
    ASSERT_TRUE(store.pushOwned(topic, stamp, std::move(payload)).has_value());
  }

  session.registerObjectTopicParser(
      topic, makeBoundHandle(kTfSchema, []() noexcept -> void* { return new CountingTfParser(nullptr, nullptr); }));

  TransformService service(session);
  service.ingestFrameTransformsForDataset(/*dataset_id=*/1);

  auto buffer = service.transformBuffer(/*dataset_id=*/1);
  ASSERT_NE(buffer, nullptr);
  for (const int64_t stamp : stamps) {
    EXPECT_TRUE(resolves(*buffer, "f0", "f2", stamp)) << "f0<-f2 must resolve at stamp " << stamp;
  }
  // The dynamic edge resolves over the whole range, not just the last sample.
  EXPECT_TRUE(resolves(*buffer, "f0", "f2", 100));
  EXPECT_TRUE(resolves(*buffer, "f0", "f2", 300));

  std::vector<std::string> frames = buffer->getAllFrames();
  std::sort(frames.begin(), frames.end());
  EXPECT_EQ(frames, (std::vector<std::string>{"f0", "f1", "f2"}));
}

// -----------------------------------------------------------------------------
// Out-of-order ingest guard (sibling of the scene_entities OOO regression).
// transform_service's UID-cursor walk is UNBOUNDED by time: it folds every newly
// arrived entry into the TIME-indexed TransformBuffer, which orders by stamp
// internally. So a late (out-of-order) edge lands at its own earlier time and
// stays resolvable — there is no `uid <= latestAt(t)` proxy to break (unlike the
// scene_entities replay). This pins that safety-by-construction.
// -----------------------------------------------------------------------------
TEST(TransformService, OutOfOrderEdgeIsIngestedAtItsOwnTime) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const auto topic = registerTopic(store, /*dataset_id=*/1, "/tf");

  // Arrival (== UID) order: f0->f1@100, f0->f2@300, then f0->f3@200 OUT OF ORDER
  // (newest UID, middle stamp). Distinct child frames make each edge's presence a
  // binary yes/no rather than something TF interpolation could paper over.
  ASSERT_TRUE(store.pushOwned(topic, 100, edgePayload(/*parent=*/0, /*child=*/1)).has_value());
  ASSERT_TRUE(store.pushOwned(topic, 300, edgePayload(/*parent=*/0, /*child=*/2)).has_value());
  ASSERT_TRUE(store.pushOwned(topic, 200, edgePayload(/*parent=*/0, /*child=*/3)).has_value());

  session.registerObjectTopicParser(
      topic, makeBoundHandle(kTfSchema, []() noexcept -> void* { return new CountingTfParser(nullptr, nullptr); }));

  TransformService service(session);
  service.ingestFrameTransformsForDataset(/*dataset_id=*/1);

  auto buffer = service.transformBuffer(/*dataset_id=*/1);
  ASSERT_NE(buffer, nullptr);

  // Every edge — including the out-of-order f0->f3@200 — was ingested and resolves
  // at its own stamp.
  EXPECT_TRUE(resolves(*buffer, "f0", "f1", 100));
  EXPECT_TRUE(resolves(*buffer, "f0", "f3", 200)) << "out-of-order TF edge @200 was dropped or mis-placed";
  EXPECT_TRUE(resolves(*buffer, "f0", "f2", 300));

  std::vector<std::string> frames = buffer->getAllFrames();
  std::sort(frames.begin(), frames.end());
  EXPECT_EQ(frames, (std::vector<std::string>{"f0", "f1", "f2", "f3"}));
}

TEST(TransformService, TapVsDrainEquivalence) {
  PJ::SessionManager drain_session;
  PJ::ObjectStore& drain_store = drain_session.objectStore();
  const auto drain_topic = registerMockTfTopic(drain_session, /*dataset_id=*/1, "/tf");

  PJ::SessionManager tap_session;
  const auto tap_topic = registerMockTfTopic(tap_session, /*dataset_id=*/1, "/tf");

  struct Message {
    PJ::Timestamp stamp;
    std::vector<uint8_t> payload;
  };
  std::vector<Message> messages;
  messages.push_back({100, edgePayload(/*parent=*/0, /*child=*/1)});
  std::vector<uint8_t> multi_edge;
  appendEdge(multi_edge, /*parent=*/0, /*child=*/2, /*offset=*/0);
  appendEdge(multi_edge, /*parent=*/2, /*child=*/4, /*offset=*/0);
  messages.push_back({300, std::move(multi_edge)});
  messages.push_back({200, edgePayload(/*parent=*/0, /*child=*/3)});  // late, out of order

  TransformService drain_service(drain_session);
  TransformService tap_service(tap_session);
  tap_service.activateIngestTap();
  ASSERT_TRUE(tap_session.ingestTaps().claimed(PJ::sdk::BuiltinObjectType::kFrameTransforms));

  for (const Message& message : messages) {
    ASSERT_TRUE(drain_store.pushOwned(drain_topic, message.stamp, message.payload).has_value());
    ASSERT_TRUE(tap_session.ingestTaps().invoke(
        PJ::sdk::BuiltinObjectType::kFrameTransforms, tap_topic, /*dataset_id=*/1, message.stamp,
        PJ::sdk::makePayloadView(message.payload)));
  }
  drain_service.ingestFrameTransformsForDataset(/*dataset_id=*/1);

  const auto drain_buffer = drain_service.transformBuffer(/*dataset_id=*/1);
  const auto tap_buffer = tap_service.transformBuffer(/*dataset_id=*/1);
  EXPECT_EQ(sortedFrames(*drain_buffer), sortedFrames(*tap_buffer));

  const std::vector<std::pair<std::string, std::string>> pairs = {{"f0", "f1"}, {"f0", "f3"}, {"f0", "f4"}};
  for (const Message& message : messages) {
    for (const auto& [target, source] : pairs) {
      const TimePoint stamp{std::chrono::nanoseconds(message.stamp)};
      const auto drained = drain_buffer->tryLookupTransform(target, source, stamp, Clock::kHeader);
      const auto tapped = tap_buffer->tryLookupTransform(target, source, stamp, Clock::kHeader);
      ASSERT_EQ(drained.has_value(), tapped.has_value()) << target << "<-" << source << " at " << message.stamp;
      if (drained.has_value()) {
        EXPECT_EQ(drained->t, tapped->t);
        EXPECT_EQ(drained->q, tapped->q);
      } else {
        EXPECT_EQ(drained.error(), tapped.error());
      }
    }
  }
}

// Captures Qt log output for its scope so tests can assert on the loss
// warnings — the only observable surface of fetch-failure reporting. Swallows
// the captured messages (tests stay quiet); restores the prior handler on exit.
class ScopedMessageCapture {
 public:
  ScopedMessageCapture() {
    instance_ = this;
    previous_ = qInstallMessageHandler(&ScopedMessageCapture::handler);
  }
  ~ScopedMessageCapture() {
    qInstallMessageHandler(previous_);
    instance_ = nullptr;
  }
  ScopedMessageCapture(const ScopedMessageCapture&) = delete;
  ScopedMessageCapture& operator=(const ScopedMessageCapture&) = delete;

  [[nodiscard]] int countContaining(const char* needle) const {
    int count = 0;
    for (const auto& message : messages_) {
      if (message.contains(QLatin1String(needle))) {
        ++count;
      }
    }
    return count;
  }

 private:
  static void handler(QtMsgType /*type*/, const QMessageLogContext& /*context*/, const QString& message) {
    if (instance_ != nullptr) {
      instance_->messages_.push_back(message);
    }
  }
  static ScopedMessageCapture* instance_;
  QList<QString> messages_;
  QtMessageHandler previous_ = nullptr;
};
ScopedMessageCapture* ScopedMessageCapture::instance_ = nullptr;

TEST(TransformService, BareServiceIncrementalIngestRemainsActive) {
  PJ::SessionManager session;
  const auto topic = registerMockTfTopic(session, /*dataset_id=*/1, "/tf");
  ASSERT_TRUE(session.objectStore().pushOwned(topic, 100, edgePayload(/*parent=*/0, /*child=*/1)).has_value());

  TransformService service(session);
  EXPECT_TRUE(service.ingestNewTransforms(/*dataset_id=*/1));
  EXPECT_TRUE(resolves(*service.transformBuffer(1), "f0", "f1", 100));
}

TEST(TransformService, ActiveTapMakesIncrementalDrainAZeroResolveNoOp) {
  PJ::SessionManager session;
  const auto topic = registerMockTfTopic(session, /*dataset_id=*/1, "/tf");
  int fetch_calls = 0;
  const std::vector<uint8_t> payload = edgePayload(/*parent=*/0, /*child=*/1);
  ASSERT_TRUE(session.objectStore()
                  .pushLazy(
                      topic, 100,
                      [&]() -> std::optional<PJ::sdk::PayloadView> {
                        ++fetch_calls;
                        return PJ::sdk::makePayloadView(payload);
                      })
                  .has_value());

  TransformService service(session);
  service.activateIngestTap();
  EXPECT_FALSE(service.ingestNewTransforms(/*dataset_id=*/1));
  EXPECT_FALSE(service.ingestNewTransforms(/*dataset_id=*/1));
  EXPECT_EQ(fetch_calls, 0) << "an active tap must not advance or resolve the ObjectStore cursor";

  service.ingestFrameTransformsForDataset(/*dataset_id=*/1);
  EXPECT_EQ(fetch_calls, 2) << "the explicit rebuild must classify and ingest the entry skipped by per-tick ingest";
  EXPECT_TRUE(resolves(*service.transformBuffer(1), "f0", "f1", 100));
}

TEST(TransformService, ExplicitRebuildWithTapActiveMatchesTapBuiltBuffer) {
  PJ::SessionManager session;
  const auto topic = registerMockTfTopic(session, /*dataset_id=*/1, "/tf");
  const std::vector<uint8_t> payload = edgePayload(/*parent=*/0, /*child=*/1);
  int fetch_calls = 0;
  ASSERT_TRUE(session.objectStore()
                  .pushLazy(
                      topic, 100,
                      [&]() -> std::optional<PJ::sdk::PayloadView> {
                        ++fetch_calls;
                        return PJ::sdk::makePayloadView(payload);
                      })
                  .has_value());

  TransformService service(session);
  service.activateIngestTap();
  invokeTfTap(session, topic, /*dataset_id=*/1, 100, payload);
  const auto buffer = service.transformBuffer(1);
  const auto before = buffer->tryLookupTransform("f0", "f1", TimePoint{std::chrono::nanoseconds(100)}, Clock::kHeader);
  ASSERT_TRUE(before.has_value());
  const std::vector<std::string> frames_before = sortedFrames(*buffer);

  service.ingestFrameTransformsForDataset(/*dataset_id=*/1);

  EXPECT_EQ(fetch_calls, 2);
  EXPECT_EQ(service.transformBuffer(1), buffer) << "explicit rebuilds preserve the active shared buffer identity";
  EXPECT_EQ(sortedFrames(*buffer), frames_before);
  const auto after = buffer->tryLookupTransform("f0", "f1", TimePoint{std::chrono::nanoseconds(100)}, Clock::kHeader);
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->t, before->t);
  EXPECT_EQ(after->q, before->q);
}

TEST(TransformService, TapAndDrainAccountForMalformedEdgesIdentically) {
  PJ::SessionManager drain_session;
  const auto drain_topic = registerTopic(drain_session.objectStore(), /*dataset_id=*/1, "/tf");
  drain_session.registerObjectTopicParser(
      drain_topic, makeBoundHandle(kTfSchema, []() noexcept -> void* { return new MalformedTfParser(); }));
  ASSERT_TRUE(drain_session.objectStore().pushOwned(drain_topic, 100, std::vector<uint8_t>{1}).has_value());

  PJ::SessionManager tap_session;
  const auto tap_topic = registerTopic(tap_session.objectStore(), /*dataset_id=*/1, "/tf");
  tap_session.registerObjectTopicParser(
      tap_topic, makeBoundHandle(kTfSchema, []() noexcept -> void* { return new MalformedTfParser(); }));

  TransformService drain_service(drain_session);
  TransformService tap_service(tap_session);
  tap_service.activateIngestTap();
  ScopedMessageCapture capture;
  drain_service.ingestFrameTransformsForDataset(/*dataset_id=*/1);
  invokeTfTap(tap_session, tap_topic, /*dataset_id=*/1, 100, std::vector<uint8_t>{1});

  EXPECT_EQ(capture.countContaining("1 reparent-conflict, 1 self-loop, and 3 invalid edge(s)"), 2)
      << "the drain and tap must report the same edge-fold outcomes";
  const auto drain_buffer = drain_service.transformBuffer(1);
  const auto tap_buffer = tap_service.transformBuffer(1);
  EXPECT_EQ(sortedFrames(*drain_buffer), (std::vector<std::string>{"f0", "f1", "f2"}));
  EXPECT_EQ(sortedFrames(*tap_buffer), sortedFrames(*drain_buffer));
  EXPECT_TRUE(resolves(*drain_buffer, "f0", "f1", 100));
  EXPECT_TRUE(resolves(*drain_buffer, "f0", "f2", 100));
  EXPECT_TRUE(resolves(*tap_buffer, "f0", "f1", 100));
  EXPECT_TRUE(resolves(*tap_buffer, "f0", "f2", 100));
}

TEST(TransformService, TapOutOfOrderEdgeLandsAtItsOwnStamp) {
  PJ::SessionManager session;
  const auto topic = registerMockTfTopic(session, /*dataset_id=*/1, "/tf");
  TransformService service(session);
  service.activateIngestTap();

  invokeTfTap(session, topic, 1, 100, edgePayload(/*parent=*/0, /*child=*/1));
  invokeTfTap(session, topic, 1, 300, edgePayload(/*parent=*/0, /*child=*/2));
  invokeTfTap(session, topic, 1, 200, edgePayload(/*parent=*/0, /*child=*/3));

  const auto buffer = service.transformBuffer(1);
  EXPECT_TRUE(resolves(*buffer, "f0", "f1", 100));
  EXPECT_TRUE(resolves(*buffer, "f0", "f3", 200));
  EXPECT_TRUE(resolves(*buffer, "f0", "f2", 300));
  EXPECT_EQ(sortedFrames(*buffer), (std::vector<std::string>{"f0", "f1", "f2", "f3"}));
}

// TF R6: the ingest stores each transform's own header stamp AND the entry time of
// the message that carried it, on the drain and the tap paths alike. The edge is
// stamped 40 ms before its store (log) time.
TEST(TransformService, IngestStoresHeaderStampAndEntryTime) {
  constexpr PJ::Timestamp kLogged = 1'000'000'000;
  constexpr int32_t kHeaderLead = -40'000'000;
  constexpr PJ::Timestamp kStamp = kLogged + kHeaderLead;
  std::vector<uint8_t> payload;
  appendEdge(payload, /*parent=*/0, /*child=*/1, kHeaderLead);

  PJ::SessionManager drain_session;
  const auto drain_topic = registerMockTfTopic(drain_session, /*dataset_id=*/1, "/tf");
  ASSERT_TRUE(drain_session.objectStore().pushOwned(drain_topic, kLogged, std::vector<uint8_t>(payload)).has_value());
  TransformService drain_service(drain_session);
  drain_service.ingestFrameTransformsForDataset(/*dataset_id=*/1);

  PJ::SessionManager tap_session;
  const auto tap_topic = registerMockTfTopic(tap_session, /*dataset_id=*/1, "/tf");
  TransformService tap_service(tap_session);
  tap_service.activateIngestTap();
  invokeTfTap(tap_session, tap_topic, /*dataset_id=*/1, kLogged, payload);

  for (const auto& buffer : {drain_service.transformBuffer(1), tap_service.transformBuffer(1)}) {
    EXPECT_TRUE(resolves(*buffer, "f0", "f1", kStamp, Clock::kHeader));
    EXPECT_FALSE(resolves(*buffer, "f0", "f1", kStamp - 1, Clock::kHeader));
    EXPECT_FALSE(resolves(*buffer, "f0", "f1", kLogged - 1, Clock::kEntry)) << "not logged yet";
    EXPECT_TRUE(resolves(*buffer, "f0", "f1", kLogged, Clock::kEntry));
  }
}

TEST(TransformService, ConcurrentTapProducersKeepDatasetsIsolated) {
  PJ::SessionManager session;
  const auto topic_a = registerMockTfTopic(session, /*dataset_id=*/1, "/tf_a");
  const auto topic_b = registerMockTfTopic(session, /*dataset_id=*/2, "/tf_b");
  TransformService service(session);
  service.activateIngestTap();

  std::thread producer_a([&]() {
    for (PJ::Timestamp stamp = 1; stamp <= 200; ++stamp) {
      invokeTfTap(session, topic_a, 1, stamp, edgePayload(/*parent=*/0, /*child=*/1));
    }
  });
  std::thread producer_b([&]() {
    for (PJ::Timestamp stamp = 1; stamp <= 200; ++stamp) {
      invokeTfTap(session, topic_b, 2, stamp, edgePayload(/*parent=*/10, /*child=*/11));
    }
  });
  producer_a.join();
  producer_b.join();

  const auto buffer_a = service.transformBuffer(1);
  const auto buffer_b = service.transformBuffer(2);
  EXPECT_EQ(sortedFrames(*buffer_a), (std::vector<std::string>{"f0", "f1"}));
  EXPECT_EQ(sortedFrames(*buffer_b), (std::vector<std::string>{"f10", "f11"}));
  EXPECT_FALSE(resolves(*buffer_a, "f10", "f11", 200));
  EXPECT_FALSE(resolves(*buffer_b, "f0", "f1", 200));
}

TEST(TransformService, CommitReplacingLoadPublishesOnlyStagingGeneration) {
  PJ::SessionManager session;
  const auto topic = registerMockTfTopic(session, /*dataset_id=*/1, "/tf");
  TransformService service(session);
  service.activateIngestTap();
  invokeTfTap(session, topic, 1, 100, edgePayload(/*parent=*/0, /*child=*/1));
  const auto old_buffer = service.transformBuffer(1);
  int ready_count = 0;
  QObject::connect(
      &service, &TransformService::datasetTransformsReady, [&ready_count](PJ::DatasetId) { ++ready_count; });

  service.beginReplacingLoad(1);
  invokeTfTap(session, topic, 1, 200, edgePayload(/*parent=*/0, /*child=*/2));
  EXPECT_FALSE(resolves(*old_buffer, "f0", "f2", 200));
  service.commitReplacingLoad(1);

  const auto committed = service.transformBuffer(1);
  EXPECT_EQ(committed, old_buffer) << "the commit keeps the buffer docks and layers hold";
  EXPECT_EQ(ready_count, 1);
  EXPECT_FALSE(resolves(*committed, "f0", "f1", 100));
  EXPECT_TRUE(resolves(*committed, "f0", "f2", 200));
}

TEST(TransformService, AbortReplacingLoadPreservesTapBufferAndRebuildsInactiveBuffer) {
  {
    PJ::SessionManager session;
    const auto topic = registerMockTfTopic(session, /*dataset_id=*/1, "/tf");
    TransformService service(session);
    service.activateIngestTap();
    invokeTfTap(session, topic, 1, 100, edgePayload(/*parent=*/0, /*child=*/1));
    const auto active = service.transformBuffer(1);
    const auto before =
        active->tryLookupTransform("f0", "f1", TimePoint{std::chrono::nanoseconds(100)}, Clock::kHeader);
    ASSERT_TRUE(before.has_value());

    service.beginReplacingLoad(1);
    invokeTfTap(session, topic, 1, 200, edgePayload(/*parent=*/0, /*child=*/2));
    service.abortReplacingLoad(1);

    EXPECT_EQ(service.transformBuffer(1), active);
    const auto after = active->tryLookupTransform("f0", "f1", TimePoint{std::chrono::nanoseconds(100)}, Clock::kHeader);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->t, before->t);
    EXPECT_EQ(after->q, before->q);
    EXPECT_FALSE(resolves(*active, "f0", "f2", 200));
  }

  {
    PJ::SessionManager session;
    const auto dataset = session.dataEngine().createDataset(
        PJ::DatasetDescriptor{.source_name = "inactive_reload", .time_domain_id = 0});
    ASSERT_TRUE(dataset.has_value()) << dataset.error();
    const auto topic = registerMockTfTopic(session, *dataset, "/tf");
    ASSERT_TRUE(session.objectStore().pushOwned(topic, 100, edgePayload(/*parent=*/0, /*child=*/1)).has_value());
    TransformService service(session);
    service.ingestFrameTransformsForDataset(*dataset);
    const auto active = service.transformBuffer(*dataset);
    ASSERT_TRUE(resolves(*active, "f0", "f1", 100));

    service.beginReplacingLoad(*dataset);
    {
      auto refill = session.beginRefill(*dataset);
      ASSERT_TRUE(session.objectStore().pushOwned(topic, 200, edgePayload(/*parent=*/0, /*child=*/2)).has_value());
      EXPECT_TRUE(service.ingestNewTransforms(*dataset));
      EXPECT_TRUE(resolves(*active, "f0", "f2", 200)) << "the old buffer must first contain aborted generation data";
    }
    service.abortReplacingLoad(*dataset);

    EXPECT_EQ(service.transformBuffer(*dataset), active);
    EXPECT_TRUE(resolves(*active, "f0", "f1", 100));
    EXPECT_FALSE(resolves(*active, "f0", "f2", 200))
        << "inactive rollback must rebuild from the restored store, removing aborted edges";
  }
}

TEST(TransformService, TapRoutingChangesOnlyAtGenerationBoundaries) {
  PJ::SessionManager session;
  const auto topic = registerMockTfTopic(session, /*dataset_id=*/1, "/tf");
  TransformService service(session);
  service.activateIngestTap();

  invokeTfTap(session, topic, 1, 100, edgePayload(/*parent=*/0, /*child=*/1));
  const auto original = service.transformBuffer(1);
  service.beginReplacingLoad(1);
  invokeTfTap(session, topic, 1, 200, edgePayload(/*parent=*/0, /*child=*/2));
  EXPECT_TRUE(resolves(*original, "f0", "f1", 100));
  EXPECT_FALSE(resolves(*original, "f0", "f2", 200));

  service.commitReplacingLoad(1);
  const auto replacement = service.transformBuffer(1);
  invokeTfTap(session, topic, 1, 300, edgePayload(/*parent=*/0, /*child=*/3));
  EXPECT_FALSE(resolves(*replacement, "f0", "f1", 100));
  EXPECT_TRUE(resolves(*replacement, "f0", "f2", 200));
  EXPECT_TRUE(resolves(*replacement, "f0", "f3", 300));
  EXPECT_EQ(replacement, original) << "the commit replaces the state, not the buffer";
}

// A reload commit reaches every holder of the dataset's buffer, however early it
// took the handle: the active buffer adopts the staged state instead of being
// swapped out, so no holder keeps resolving the old TF.
TEST(TransformService, ReloadCommitReachesBuffersHeldBeforeIt) {
  PJ::SessionManager session;
  const auto topic = registerMockTfTopic(session, /*dataset_id=*/1, "/tf");
  TransformService service(session);
  service.activateIngestTap();
  invokeTfTap(session, topic, 1, 100, edgePayload(/*parent=*/0, /*child=*/1));
  const auto held = service.transformBuffer(1);

  service.beginReplacingLoad(1);
  invokeTfTap(session, topic, 1, 100, edgePayload(/*parent=*/0, /*child=*/2));
  service.commitReplacingLoad(1);

  EXPECT_TRUE(resolves(*held, "f0", "f2", 100)) << "the held buffer must resolve the reloaded TF";
  EXPECT_FALSE(resolves(*held, "f0", "f1", 100));
  EXPECT_EQ(service.snapshot(1).use_count(), 2) << "the staging buffer is gone: the state is the active one's alone";
}

TEST(TransformService, RepeatedBeginReplacesUnfinishedStagingGeneration) {
  PJ::SessionManager session;
  const auto topic = registerMockTfTopic(session, /*dataset_id=*/1, "/tf");
  TransformService service(session);
  service.activateIngestTap();

  service.beginReplacingLoad(1);
  invokeTfTap(session, topic, 1, 100, edgePayload(/*parent=*/0, /*child=*/1));
  service.beginReplacingLoad(1);
  invokeTfTap(session, topic, 1, 200, edgePayload(/*parent=*/0, /*child=*/2));
  service.commitReplacingLoad(1);

  const auto active = service.transformBuffer(1);
  EXPECT_FALSE(resolves(*active, "f0", "f1", 100));
  EXPECT_TRUE(resolves(*active, "f0", "f2", 200));
}

TEST(TransformService, AbortWithoutBeginIsNoOp) {
  {
    PJ::SessionManager session;
    const auto topic = registerMockTfTopic(session, /*dataset_id=*/1, "/tf");
    TransformService service(session);
    service.activateIngestTap();
    invokeTfTap(session, topic, 1, 100, edgePayload(/*parent=*/0, /*child=*/1));
    const auto active = service.transformBuffer(1);
    const uint64_t revision_before = active->revision();

    service.abortReplacingLoad(1);

    EXPECT_EQ(service.transformBuffer(1), active);
    EXPECT_EQ(active->revision(), revision_before);
    EXPECT_TRUE(resolves(*active, "f0", "f1", 100));
  }

  {
    PJ::SessionManager session;
    const auto topic = registerMockTfTopic(session, /*dataset_id=*/1, "/tf");
    ASSERT_TRUE(session.objectStore().pushOwned(topic, 100, edgePayload(/*parent=*/0, /*child=*/1)).has_value());
    TransformService service(session);
    service.ingestFrameTransformsForDataset(1);
    const auto active = service.transformBuffer(1);
    const uint64_t revision_before = active->revision();

    service.abortReplacingLoad(1);

    EXPECT_EQ(service.transformBuffer(1), active);
    EXPECT_EQ(active->revision(), revision_before);
    EXPECT_TRUE(resolves(*active, "f0", "f1", 100));
  }
}

TEST(TransformService, ConcurrentTapWriterAndBufferReaderStayCoherent) {
  PJ::SessionManager session;
  const auto topic = registerMockTfTopic(session, /*dataset_id=*/1, "/tf");
  TransformService service(session);
  service.activateIngestTap();
  const auto buffer = service.transformBuffer(1);
  std::atomic<bool> finished{false};
  std::atomic<bool> incoherent{false};

  std::thread producer([&]() {
    for (PJ::Timestamp stamp = 1; stamp <= 5000; ++stamp) {
      invokeTfTap(session, topic, 1, stamp, edgePayload(/*parent=*/0, /*child=*/1));
    }
    finished.store(true);
  });
  std::thread reader([&]() {
    while (!finished.load()) {
      const std::vector<std::string> frames = sortedFrames(*buffer);
      if (!frames.empty() && frames != std::vector<std::string>{"f0", "f1"}) {
        incoherent.store(true);
      }
      (void)buffer->tryLookupTransform("f0", "f1", TimePoint{std::chrono::nanoseconds(2500)}, Clock::kHeader);
    }
  });
  producer.join();
  reader.join();

  EXPECT_FALSE(incoherent.load());
  EXPECT_EQ(sortedFrames(*buffer), (std::vector<std::string>{"f0", "f1"}));
  EXPECT_TRUE(resolves(*buffer, "f0", "f1", 5000));
}

TEST(TransformService, WorkerTapCanOverlapGuiExplicitRebuild) {
  PJ::SessionManager session;
  const auto topic = registerMockTfTopic(session, /*dataset_id=*/1, "/tf");
  std::atomic<bool> fetch_entered{false};
  std::atomic<bool> tap_finished{false};
  ASSERT_TRUE(session.objectStore()
                  .pushLazy(
                      topic, 200,
                      [&]() -> std::optional<PJ::sdk::PayloadView> {
                        fetch_entered.store(true);
                        while (!tap_finished.load()) {
                          std::this_thread::yield();
                        }
                        return PJ::sdk::makePayloadView(edgePayload(/*parent=*/0, /*child=*/2));
                      })
                  .has_value());
  TransformService service(session);
  service.activateIngestTap();
  const auto buffer = service.transformBuffer(1);
  std::thread worker([&]() {
    while (!fetch_entered.load()) {
      std::this_thread::yield();
    }
    invokeTfTap(session, topic, 1, 100, edgePayload(/*parent=*/0, /*child=*/1));
    tap_finished.store(true);
  });

  service.ingestFrameTransformsForDataset(1);
  worker.join();

  EXPECT_TRUE(resolves(*buffer, "f0", "f1", 100));
  EXPECT_TRUE(resolves(*buffer, "f0", "f2", 200));
}

TEST(TransformService, ExplicitRebuildRejectsWorkerThreadInDebug) {
#if (!defined(QT_NO_DEBUG) || defined(QT_FORCE_ASSERTS)) && GTEST_HAS_DEATH_TEST
  PJ::SessionManager session;
  TransformService service(session);
  EXPECT_DEATH(
      {
        std::thread worker([&]() { service.ingestFrameTransformsForDataset(1); });
        worker.join();
      },
      "QThread::currentThread");
#else
  GTEST_SKIP() << "Q_ASSERT thread-affinity checks are compiled out in this build";
#endif
}

// A lazy entry whose re-read FAILS (fetch returns nullopt: file truncated,
// replaced, or corrupt) is counted and skipped — its edges are permanently
// lost — while every healthy neighbour still ingests. The cursor advances past
// the failure: a later ingest must NOT retry it (re-reading a corrupt region
// cannot succeed).
TEST(TransformService, FailedLazyFetchIsSkippedAndNotRetried) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const auto topic = registerTopic(store, /*dataset_id=*/1, "/tf");

  int fetch_calls = 0;
  ASSERT_TRUE(store.pushOwned(topic, 100, edgePayload(/*parent=*/0, /*child=*/1)).has_value());
  ASSERT_TRUE(store
                  .pushLazy(
                      topic, 200,
                      [&fetch_calls]() -> std::optional<PJ::sdk::PayloadView> {
                        ++fetch_calls;
                        return std::nullopt;  // source cannot re-produce the bytes
                      })
                  .has_value());
  ASSERT_TRUE(store.pushOwned(topic, 300, edgePayload(/*parent=*/0, /*child=*/2)).has_value());

  session.registerObjectTopicParser(
      topic, makeBoundHandle(kTfSchema, []() noexcept -> void* { return new CountingTfParser(nullptr, nullptr); }));

  TransformService service(session);
  ScopedMessageCapture capture;
  service.ingestFrameTransformsForDataset(/*dataset_id=*/1);

  auto buffer = service.transformBuffer(/*dataset_id=*/1);
  ASSERT_NE(buffer, nullptr);
  EXPECT_TRUE(resolves(*buffer, "f0", "f1", 100));
  EXPECT_TRUE(resolves(*buffer, "f0", "f2", 300)) << "an edge after the failed entry must still ingest";
  EXPECT_EQ(fetch_calls, 1);
  // Pins the REPORTING, not just the skip: exactly one loss warning fired.
  EXPECT_EQ(capture.countContaining("LOST"), 1);

  // The drain cursor advanced past the failed entry: a later ingest neither
  // re-fetches, re-ingests, nor re-warns.
  EXPECT_FALSE(service.ingestNewTransforms(/*dataset_id=*/1));
  EXPECT_EQ(fetch_calls, 1) << "a permanently failed fetch must not be retried by the drain";
  EXPECT_EQ(capture.countContaining("LOST"), 1) << "the loss must be reported exactly once";
}

// A FAILED newest entry must not block classification: the probe walks back to
// an older resolvable entry, the topic classifies, healthy edges ingest, and
// the failed entry is fetched once by the probe and once by the drain — never
// again on later ingests (no per-tick retry storm).
TEST(TransformService, FailedNewestEntryDoesNotBlockClassification) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const auto topic = registerTopic(store, /*dataset_id=*/1, "/tf");

  int fetch_calls = 0;
  ASSERT_TRUE(store.pushOwned(topic, 100, edgePayload(/*parent=*/0, /*child=*/1)).has_value());
  ASSERT_TRUE(store
                  .pushLazy(
                      topic, 200,
                      [&fetch_calls]() -> std::optional<PJ::sdk::PayloadView> {
                        ++fetch_calls;
                        return std::nullopt;  // permanently unresolvable NEWEST entry
                      })
                  .has_value());

  session.registerObjectTopicParser(
      topic, makeBoundHandle(kTfSchema, []() noexcept -> void* { return new CountingTfParser(nullptr, nullptr); }));

  TransformService service(session);
  ScopedMessageCapture capture;
  service.ingestFrameTransformsForDataset(/*dataset_id=*/1);

  auto buffer = service.transformBuffer(/*dataset_id=*/1);
  ASSERT_NE(buffer, nullptr);
  EXPECT_TRUE(resolves(*buffer, "f0", "f1", 100)) << "a failed newest entry must not block classification";
  EXPECT_EQ(capture.countContaining("LOST"), 1);
  const int calls_after_first = fetch_calls;

  // Classified and drained: later ingests must not re-probe or re-fetch the
  // failed entry, and must not re-warn.
  EXPECT_FALSE(service.ingestNewTransforms(/*dataset_id=*/1));
  EXPECT_EQ(fetch_calls, calls_after_first) << "no per-tick retry of the failed entry";
  EXPECT_EQ(capture.countContaining("LOST"), 1);
}

// -----------------------------------------------------------------------------
// (b) Equivalence: N one-at-a-time incremental ingests == one bulk ingest.
// -----------------------------------------------------------------------------
TEST(TransformService, IncrementalEqualsBulk) {
  // Bulk: dataset 1, all N entries present before a single ingest.
  PJ::SessionManager bulk_session;
  PJ::ObjectStore& bulk_store = bulk_session.objectStore();
  const auto bulk_topic = registerTopic(bulk_store, /*dataset_id=*/1, "/tf");

  // Incremental: dataset 2, entries pushed one at a time with an ingest after each.
  PJ::SessionManager inc_session;
  PJ::ObjectStore& inc_store = inc_session.objectStore();
  const auto inc_topic = registerTopic(inc_store, /*dataset_id=*/2, "/tf");

  bulk_session.registerObjectTopicParser(bulk_topic, makeBoundHandle(kTfSchema, []() noexcept -> void* {
                                           return new CountingTfParser(nullptr, nullptr);
                                         }));
  inc_session.registerObjectTopicParser(
      inc_topic, makeBoundHandle(kTfSchema, []() noexcept -> void* { return new CountingTfParser(nullptr, nullptr); }));

  TransformService bulk_service(bulk_session);
  TransformService inc_service(inc_session);

  const std::vector<int64_t> stamps = {100, 150, 200, 250, 300};
  // Feed the incremental side one entry per ingest tick.
  for (const int64_t stamp : stamps) {
    ASSERT_TRUE(inc_store.pushOwned(inc_topic, stamp, edgePayload(/*parent=*/0, /*child=*/1)).has_value());
    inc_service.ingestNewTransforms(/*dataset_id=*/2);
  }
  // Feed the bulk side everything up front, then ingest once.
  for (const int64_t stamp : stamps) {
    ASSERT_TRUE(bulk_store.pushOwned(bulk_topic, stamp, edgePayload(/*parent=*/0, /*child=*/1)).has_value());
  }
  bulk_service.ingestFrameTransformsForDataset(/*dataset_id=*/1);

  auto bulk_buffer = bulk_service.transformBuffer(/*dataset_id=*/1);
  auto inc_buffer = inc_service.transformBuffer(/*dataset_id=*/2);

  std::vector<std::string> bulk_frames = bulk_buffer->getAllFrames();
  std::vector<std::string> inc_frames = inc_buffer->getAllFrames();
  std::sort(bulk_frames.begin(), bulk_frames.end());
  std::sort(inc_frames.begin(), inc_frames.end());
  EXPECT_EQ(bulk_frames, inc_frames);

  // Lookups must agree at every published stamp (the dynamic edge has a distinct
  // sample at each), proving the incremental path reconstructed the same history.
  for (const int64_t stamp : stamps) {
    const auto bulk_tf =
        bulk_buffer->tryLookupTransform("f0", "f1", TimePoint{std::chrono::nanoseconds(stamp)}, Clock::kHeader);
    const auto inc_tf =
        inc_buffer->tryLookupTransform("f0", "f1", TimePoint{std::chrono::nanoseconds(stamp)}, Clock::kHeader);
    ASSERT_TRUE(bulk_tf.has_value()) << "bulk lookup at " << stamp;
    ASSERT_TRUE(inc_tf.has_value()) << "incremental lookup at " << stamp;
    EXPECT_EQ(bulk_tf->t, inc_tf->t);
  }
}

// -----------------------------------------------------------------------------
// (c) Idempotency: a redundant ingest with nothing new returns false and the
//     buffer is unchanged (revision stable, lookups stable).
// -----------------------------------------------------------------------------
TEST(TransformService, RedundantIngestIsNoOp) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const auto topic = registerTopic(store, /*dataset_id=*/1, "/tf");
  ASSERT_TRUE(store.pushOwned(topic, 100, edgePayload(/*parent=*/0, /*child=*/1)).has_value());

  session.registerObjectTopicParser(
      topic, makeBoundHandle(kTfSchema, []() noexcept -> void* { return new CountingTfParser(nullptr, nullptr); }));

  TransformService service(session);
  EXPECT_TRUE(service.ingestNewTransforms(/*dataset_id=*/1)) << "first ingest applies the one edge";

  auto buffer = service.transformBuffer(/*dataset_id=*/1);
  const uint64_t revision_after_first = buffer->revision();

  // No new entries pushed: a second ingest must be a pure no-op.
  EXPECT_FALSE(service.ingestNewTransforms(/*dataset_id=*/1)) << "redundant ingest must report no change";
  EXPECT_EQ(buffer->revision(), revision_after_first) << "redundant ingest must not touch the buffer";
  EXPECT_TRUE(resolves(*buffer, "f0", "f1", 100));
}

// -----------------------------------------------------------------------------
// (d) Non-TF memo: a topic that parses as a non-FrameTransforms object is
//     classified exactly once and never re-decoded across ticks.
// -----------------------------------------------------------------------------
TEST(TransformService, NonTfTopicClassifiedOnce) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const auto grid_topic = registerTopic(store, /*dataset_id=*/1, "/map");
  ASSERT_TRUE(store.pushOwned(grid_topic, 100, std::vector<uint8_t>{0x00}).has_value());

  static std::atomic<int> grid_parse_calls{0};
  grid_parse_calls.store(0);
  session.registerObjectTopicParser(grid_topic, makeBoundHandle(kGridSchema, []() noexcept -> void* {
                                      return new CountingGridParser(&grid_parse_calls);
                                    }));

  TransformService service(session);
  service.ingestNewTransforms(/*dataset_id=*/1);
  // Push another entry and ingest again: a re-probe here would re-decode it.
  ASSERT_TRUE(store.pushOwned(grid_topic, 200, std::vector<uint8_t>{0x00}).has_value());
  service.ingestNewTransforms(/*dataset_id=*/1);

  EXPECT_EQ(grid_parse_calls.load(), 1) << "a non-TF topic must be classified by a single probe, never re-decoded";
  // The grid topic must contribute no frames (it was never ingested as TF).
  EXPECT_TRUE(service.transformBuffer(/*dataset_id=*/1)->getAllFrames().empty());
}

// -----------------------------------------------------------------------------
// (e) Reparent conflict: one entry carrying edges [f1<-f0, f1<-f2, f3<-f0] — the
//     conflicting edge (f1 claimed by two parents) drops; the rest resolve.
// -----------------------------------------------------------------------------
TEST(TransformService, ReparentConflictDropsOneEdgeContinues) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const auto topic = registerTopic(store, /*dataset_id=*/1, "/tf");

  // child f1 first parented to f0, then conflictingly to f2; child f3 to f0.
  std::vector<uint8_t> payload;
  appendEdge(payload, /*parent=*/0, /*child=*/1, /*offset=*/0);  // f0 -> f1   (kept)
  appendEdge(payload, /*parent=*/2, /*child=*/1, /*offset=*/0);  // f2 -> f1   (reparent conflict, dropped)
  appendEdge(payload, /*parent=*/0, /*child=*/3, /*offset=*/0);  // f0 -> f3   (kept)
  ASSERT_TRUE(store.pushOwned(topic, 100, std::move(payload)).has_value());

  session.registerObjectTopicParser(
      topic, makeBoundHandle(kTfSchema, []() noexcept -> void* { return new CountingTfParser(nullptr, nullptr); }));

  TransformService service(session);
  service.ingestFrameTransformsForDataset(/*dataset_id=*/1);
  auto buffer = service.transformBuffer(/*dataset_id=*/1);

  // f1 stays parented to f0 (the first edge won); f3 resolves too.
  EXPECT_EQ(buffer->getParent("f1"), std::optional<std::string>("f0"));
  EXPECT_TRUE(resolves(*buffer, "f0", "f1", 100));
  EXPECT_TRUE(resolves(*buffer, "f0", "f3", 100));
  // f2 is still a known frame (it appeared as a parent) but never became f1's parent.
  EXPECT_NE(buffer->getParent("f1"), std::optional<std::string>("f2"));
}

// -----------------------------------------------------------------------------
// (f) invalidateDataset clears the SAME shared_ptr in place (docks hold it by
//     pointer, so a swap would orphan them).
// -----------------------------------------------------------------------------
TEST(TransformService, InvalidateClearsBufferInPlace) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const auto topic = registerTopic(store, /*dataset_id=*/1, "/tf");
  ASSERT_TRUE(store.pushOwned(topic, 100, edgePayload(/*parent=*/0, /*child=*/1)).has_value());

  session.registerObjectTopicParser(
      topic, makeBoundHandle(kTfSchema, []() noexcept -> void* { return new CountingTfParser(nullptr, nullptr); }));

  TransformService service(session);
  service.ingestFrameTransformsForDataset(/*dataset_id=*/1);

  auto buffer_before = service.transformBuffer(/*dataset_id=*/1);
  ASSERT_FALSE(buffer_before->getAllFrames().empty());

  service.invalidateDataset(/*dataset_id=*/1);

  // SAME pointer, now empty — not a freshly swapped-in buffer.
  auto buffer_after = service.transformBuffer(/*dataset_id=*/1);
  EXPECT_EQ(buffer_before.get(), buffer_after.get()) << "invalidate must clear in place, not swap the shared_ptr";
  EXPECT_TRUE(buffer_before->getAllFrames().empty()) << "the held pointer must observe the cleared state";
}

// -----------------------------------------------------------------------------
// (g) Equal-timestamp late append: e1@T (edge f1<-f0), ingest, then e2@T (edge
//     f2<-f0) at the SAME store timestamp, ingest. The old timestamp-only cursor
//     lost the second same-stamp entry forever; the UID cursor must reach it.
// -----------------------------------------------------------------------------
TEST(TransformService, EqualTimestampLateAppendNotLost) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const auto topic = registerTopic(store, /*dataset_id=*/1, "/tf");

  ASSERT_TRUE(store.pushOwned(topic, 100, edgePayload(/*parent=*/0, /*child=*/1)).has_value());

  session.registerObjectTopicParser(
      topic, makeBoundHandle(kTfSchema, []() noexcept -> void* { return new CountingTfParser(nullptr, nullptr); }));

  TransformService service(session);
  service.ingestNewTransforms(/*dataset_id=*/1);
  auto buffer = service.transformBuffer(/*dataset_id=*/1);
  EXPECT_TRUE(resolves(*buffer, "f0", "f1", 100));

  // Second entry at the SAME timestamp 100, carrying a different edge.
  ASSERT_TRUE(store.pushOwned(topic, 100, edgePayload(/*parent=*/0, /*child=*/2)).has_value());
  EXPECT_TRUE(service.ingestNewTransforms(/*dataset_id=*/1)) << "a same-stamp late entry must still be ingested";

  EXPECT_TRUE(resolves(*buffer, "f0", "f1", 100)) << "first same-stamp edge stays";
  EXPECT_TRUE(resolves(*buffer, "f0", "f2", 100)) << "second same-stamp edge must NOT be lost (UID cursor)";
}

// -----------------------------------------------------------------------------
// (h) Eviction continuity: a memory budget forces front-eviction between two
//     ingest calls. Ingest must continue past the gap (UIDs are stable across
//     eviction) and the newest edges must resolve. A pre-eviction sample that the
//     store no longer holds is unrecoverable by design — we assert only the
//     newest survives, never claiming the evicted one is still present.
// -----------------------------------------------------------------------------
TEST(TransformService, EvictionContinuity) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const auto topic = registerTopic(store, /*dataset_id=*/1, "/tf");

  session.registerObjectTopicParser(
      topic, makeBoundHandle(kTfSchema, []() noexcept -> void* { return new CountingTfParser(nullptr, nullptr); }));

  TransformService service(session);
  // Use a live cache window so the buffer keeps the late samples (a kKeepAll bulk
  // buffer would too, but this matches the streaming code path under test).
  service.setLiveCacheWindow(/*dataset_id=*/1, std::chrono::nanoseconds(std::chrono::seconds(3600)));

  // First batch of dynamic edges f0->f1, then ingest.
  for (int64_t stamp = 100; stamp <= 130; stamp += 10) {
    ASSERT_TRUE(store.pushOwned(topic, stamp, edgePayload(/*parent=*/0, /*child=*/1)).has_value());
  }
  service.ingestNewTransforms(/*dataset_id=*/1);

  // Tighten the memory budget so the NEXT pushes front-evict earlier entries:
  // each edge payload is kEdgeRecordBytes bytes, so a 2-entry budget keeps only
  // the two newest entries of the topic.
  PJ::RetentionBudget budget;
  budget.max_memory_bytes = kEdgeRecordBytes * 2;
  store.setRetentionBudget(topic, budget);

  // Push enough late entries to drive front-eviction below the cursor's position.
  for (int64_t stamp = 200; stamp <= 260; stamp += 10) {
    ASSERT_TRUE(store.pushOwned(topic, stamp, edgePayload(/*parent=*/0, /*child=*/1)).has_value());
  }
  ASSERT_LT(store.entryCount(topic), 7u) << "budget should have evicted the front entries";

  // Ingest must continue past the evicted gap — no infinite loop, no hang — and
  // the newest edge must resolve.
  EXPECT_TRUE(service.ingestNewTransforms(/*dataset_id=*/1)) << "ingest must continue past the eviction gap";
  auto buffer = service.transformBuffer(/*dataset_id=*/1);
  EXPECT_TRUE(resolves(*buffer, "f0", "f1", 260)) << "the newest post-eviction edge must resolve";
}

// -----------------------------------------------------------------------------
// (i) Transient probe failure (M.34): a parser whose parse_object fails the first
//     call then succeeds. After two ticks the topic IS classified TF and its
//     transforms land — a single bad newest message must not blacklist it.
// -----------------------------------------------------------------------------
TEST(TransformService, TransientProbeFailureNotBlacklisted) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const auto topic = registerTopic(store, /*dataset_id=*/1, "/tf");
  ASSERT_TRUE(store.pushOwned(topic, 100, edgePayload(/*parent=*/0, /*child=*/1)).has_value());

  static std::atomic<bool> fail_first{true};
  fail_first.store(true);
  session.registerObjectTopicParser(
      topic, makeBoundHandle(kTfSchema, []() noexcept -> void* { return new CountingTfParser(nullptr, &fail_first); }));

  TransformService service(session);
  // First tick: the classification probe parse FAILS — the topic must NOT be
  // blacklisted; the buffer stays empty for now.
  service.ingestNewTransforms(/*dataset_id=*/1);
  auto buffer = service.transformBuffer(/*dataset_id=*/1);
  EXPECT_TRUE(buffer->getAllFrames().empty()) << "the failed probe must not have ingested anything";

  // Second tick: the parser now succeeds — the topic classifies as TF and ingests.
  EXPECT_TRUE(service.ingestNewTransforms(/*dataset_id=*/1)) << "a transient failure must not permanently suppress TF";
  EXPECT_TRUE(resolves(*buffer, "f0", "f1", 100)) << "the topic must classify as TF on a later tick";
}

// -----------------------------------------------------------------------------
// (j) Live window (H.11): a small cache window drops over-window samples on a busy
//     edge, but the newest still resolves; a single-sample (static-like) edge
//     resolves at late stamps thanks to the always-keep-last invariant.
// -----------------------------------------------------------------------------
TEST(TransformService, LiveWindowDropsOldKeepsNewAndStatic) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const auto dynamic_topic = registerTopic(store, /*dataset_id=*/1, "/tf");
  const auto static_topic = registerTopic(store, /*dataset_id=*/1, "/tf_static");

  // The dynamic edge's translation changes with every stamp: an identical repeat
  // would not be stored, and the window must have distinct samples to drop.
  session.registerObjectTopicParser(dynamic_topic, makeBoundHandle(kTfSchema, []() noexcept -> void* {
                                      return new CountingTfParser(nullptr, nullptr, /*vary_translation=*/true);
                                    }));
  session.registerObjectTopicParser(static_topic, makeBoundHandle(kTfSchema, []() noexcept -> void* {
                                      return new CountingTfParser(nullptr, nullptr);
                                    }));

  TransformService service(session);
  // 50 ns rolling window. Stamps are in ns directly.
  service.setLiveCacheWindow(/*dataset_id=*/1, std::chrono::nanoseconds(50));

  // Static-like edge f0->f1: a SINGLE sample published once, early.
  ASSERT_TRUE(store.pushOwned(static_topic, 100, edgePayload(/*parent=*/0, /*child=*/1)).has_value());

  // Busy dynamic edge f1->f2 spanning well past the 50 ns window.
  for (int64_t stamp = 100; stamp <= 300; stamp += 25) {
    ASSERT_TRUE(store.pushOwned(dynamic_topic, stamp, edgePayload(/*parent=*/1, /*child=*/2)).has_value());
  }
  service.ingestFrameTransformsForDataset(/*dataset_id=*/1);
  auto buffer = service.transformBuffer(/*dataset_id=*/1);

  // The dynamic edge dropped its old samples: a stamp far below (newest - window)
  // no longer resolves on that edge, but the newest does.
  EXPECT_FALSE(resolves(*buffer, "f1", "f2", 100)) << "an over-window-old dynamic sample must have been evicted";
  EXPECT_TRUE(resolves(*buffer, "f1", "f2", 300)) << "the newest dynamic sample must resolve";
  // The sample spanning the cutoff (250) is kept: it holds the value until the next one.
  EXPECT_TRUE(resolves(*buffer, "f1", "f2", 260)) << "the sample valid at the cutoff must be retained";

  // The single-sample static edge resolves at a late stamp (always-keep-last).
  EXPECT_TRUE(resolves(*buffer, "f0", "f1", 300)) << "a single-sample (static) edge must resolve at late stamps";
  // And the full composed chain f0<-f2 resolves at the newest stamp.
  EXPECT_TRUE(resolves(*buffer, "f0", "f2", 300)) << "static+dynamic compose at the live edge";
}

// -----------------------------------------------------------------------------
// Snapshots (docs/derived_objects/ARCHITECTURE.md §12, R1/R4): what scripts read.
// -----------------------------------------------------------------------------

// The f0<-f1 translation x a snapshot resolves at `stamp_ns`, or nullopt.
std::optional<double> snapshotX(const pj::tf::TransformSnapshot& snapshot, int64_t stamp_ns) {
  const auto found =
      snapshot.tryLookupTransform("f0", "f1", TimePoint{std::chrono::nanoseconds(stamp_ns)}, Clock::kHeader);
  return found ? std::optional<double>(found->t.x) : std::nullopt;
}

bool snapshotResolves(const pj::tf::TransformSnapshot& snapshot, const std::string& source, int64_t stamp_ns) {
  return snapshot.tryLookupTransform("f0", source, TimePoint{std::chrono::nanoseconds(stamp_ns)}, Clock::kHeader)
      .has_value();
}

// Live ingest through the tap, then a retention change: the snapshot keeps the
// state it captured (TFL-07, TFL-11); a snapshot taken afterwards sees the new one.
TEST(TransformServiceSnapshot, SnapshotIsImmutableWhileTheBufferMutates) {
  PJ::SessionManager session;
  const auto topic = registerMockTfTopic(session, /*dataset_id=*/1, "/tf", /*vary_translation=*/true);
  TransformService service(session);
  service.activateIngestTap();
  service.setLiveCacheWindow(1, std::chrono::seconds(1));
  for (const PJ::Timestamp stamp : {100, 150, 200}) {
    invokeTfTap(session, topic, 1, stamp, edgePayload(/*parent=*/0, /*child=*/1));
  }
  const auto snapshot = service.snapshot(1);
  ASSERT_NE(snapshot, nullptr);
  const uint64_t revision = snapshot->revision();

  invokeTfTap(session, topic, 1, 250, edgePayload(/*parent=*/0, /*child=*/1));
  EXPECT_EQ(snapshot->revision(), revision);
  EXPECT_EQ(snapshotX(*snapshot, 250), 201.0) << "the append after the snapshot is not in it";

  // Retention alone, on a state a snapshot shares: 10 ns past the newest sample
  // keeps the one at 200 and drops 100 and 150.
  const auto before_retention = service.snapshot(1);
  service.setLiveCacheWindow(1, std::chrono::nanoseconds(10));
  ASSERT_FALSE(resolves(*service.transformBuffer(1), "f0", "f1", 150));
  EXPECT_NE(service.transformBuffer(1)->revision(), before_retention->revision()) << "eviction bumps the revision";
  EXPECT_EQ(snapshotX(*before_retention, 150), 151.0) << "retention evicted from a shared state";
  EXPECT_EQ(snapshotX(*snapshot, 100), 101.0);

  const auto after = service.snapshot(1);
  EXPECT_NE(after->revision(), before_retention->revision());
  EXPECT_EQ(snapshotX(*after, 150), std::nullopt) << "retention reaches the next snapshot";
  EXPECT_EQ(snapshotX(*after, 250), 251.0);
}

// Repeated calls at one revision return one object, even across a write that
// changed nothing; a new revision returns a new object.
TEST(TransformServiceSnapshot, SnapshotIsSharedPerRevision) {
  PJ::SessionManager session;
  const auto topic = registerMockTfTopic(session, /*dataset_id=*/1, "/tf", /*vary_translation=*/true);
  TransformService service(session);
  service.activateIngestTap();
  invokeTfTap(session, topic, 1, 100, edgePayload(/*parent=*/0, /*child=*/1));

  const auto first = service.snapshot(1);
  EXPECT_EQ(service.snapshot(1), first);
  // The same sample again: no revision bump, although the write copied the state.
  invokeTfTap(session, topic, 1, 100, edgePayload(/*parent=*/0, /*child=*/1));
  EXPECT_EQ(service.snapshot(1), first);

  invokeTfTap(session, topic, 1, 200, edgePayload(/*parent=*/0, /*child=*/1));
  const auto second = service.snapshot(1);
  EXPECT_NE(second, first);
  EXPECT_NE(second->revision(), first->revision());
}

// Every replacement (reload commit and rollback, invalidation, merge rebuild,
// removal of all datasets) leaves earlier snapshots readable and unchanged, and
// the next snapshot sees the replacement (TFL-07, TFL-10).
TEST(TransformServiceSnapshot, ReplacementLeavesOldSnapshotsValidAndNewOnesSeeTheReplacement) {
  PJ::SessionManager session;
  const auto topic = registerMockTfTopic(session, /*dataset_id=*/1, "/tf");
  TransformService service(session);
  service.activateIngestTap();
  invokeTfTap(session, topic, 1, 100, edgePayload(/*parent=*/0, /*child=*/1));
  const auto original = service.snapshot(1);

  service.beginReplacingLoad(1);
  invokeTfTap(session, topic, 1, 100, edgePayload(/*parent=*/0, /*child=*/2));
  EXPECT_EQ(service.snapshot(1), original) << "the staging generation stays invisible until it commits";
  service.commitReplacingLoad(1);
  const auto committed = service.snapshot(1);
  // The committed buffer took exactly as many writes as the original: a
  // buffer-local revision counter would collide here and serve a stale entry.
  EXPECT_NE(committed->revision(), original->revision());
  EXPECT_TRUE(snapshotResolves(*committed, "f2", 100));
  EXPECT_FALSE(snapshotResolves(*committed, "f1", 100));
  EXPECT_TRUE(snapshotResolves(*original, "f1", 100));
  EXPECT_FALSE(snapshotResolves(*original, "f2", 100));

  service.beginReplacingLoad(1);
  invokeTfTap(session, topic, 1, 100, edgePayload(/*parent=*/0, /*child=*/3));
  service.abortReplacingLoad(1);
  EXPECT_EQ(service.snapshot(1), committed) << "a rolled-back reload leaves the committed state";

  service.invalidateDataset(1);
  const auto invalidated = service.snapshot(1);
  EXPECT_TRUE(invalidated->getAllFrames().empty());
  EXPECT_TRUE(snapshotResolves(*committed, "f2", 100)) << "invalidation must not empty a snapshot";

  ASSERT_TRUE(session.objectStore().pushOwned(topic, 300, edgePayload(/*parent=*/0, /*child=*/4)).has_value());
  service.ingestFrameTransformsForDataset(1);
  const auto rebuilt = service.snapshot(1);
  EXPECT_TRUE(snapshotResolves(*rebuilt, "f4", 300));
  EXPECT_TRUE(invalidated->getAllFrames().empty()) << "the merge rebuild must not fill an earlier snapshot";

  service.invalidateAll();
  EXPECT_TRUE(service.snapshot(1)->getAllFrames().empty());
  EXPECT_TRUE(snapshotResolves(*rebuilt, "f4", 300)) << "removing every dataset must not empty a snapshot";
  EXPECT_TRUE(snapshotResolves(*original, "f1", 100));
}

// A complete file buffer is shared, not copied: the snapshot is the buffer's own
// state object (storage identity), held by the buffer and this test only.
TEST(TransformServiceSnapshot, SnapshotOfACompleteFileBufferDoesNotCopy) {
  PJ::SessionManager session;
  const auto topic = registerMockTfTopic(session, /*dataset_id=*/1, "/tf", /*vary_translation=*/true);
  for (const PJ::Timestamp stamp : {100, 200, 300}) {
    ASSERT_TRUE(session.objectStore().pushOwned(topic, stamp, edgePayload(/*parent=*/0, /*child=*/1)).has_value());
  }
  TransformService service(session);
  service.ingestFrameTransformsForDataset(1);

  const auto snapshot = service.snapshot(1);
  EXPECT_EQ(snapshot.use_count(), 2) << "shared with the buffer, not a copy";
  EXPECT_EQ(service.snapshot(1), snapshot);
  EXPECT_EQ(snapshotX(*snapshot, 100), 101.0);
  EXPECT_EQ(snapshotX(*snapshot, 250), 201.0);
  EXPECT_EQ(snapshotX(*snapshot, 300), 301.0);
}

// A snapshot is read from a worker thread with no lock while the tap keeps writing
// into its buffer from another: every read sees the captured state. The producer
// starts once the reader has read and waits for another read halfway, so the reads
// overlap the writes.
TEST(TransformServiceSnapshot, SnapshotReadsNeedNoLockWhileTheTapWrites) {
  PJ::SessionManager session;
  const auto topic = registerMockTfTopic(session, /*dataset_id=*/1, "/tf", /*vary_translation=*/true);
  TransformService service(session);
  service.activateIngestTap();
  service.setLiveCacheWindow(1, std::chrono::nanoseconds(100));
  invokeTfTap(session, topic, 1, 1, edgePayload(/*parent=*/0, /*child=*/1));
  invokeTfTap(session, topic, 1, 2, edgePayload(/*parent=*/0, /*child=*/1));
  const auto snapshot = service.snapshot(1);
  std::atomic<bool> finished{false};
  std::atomic<bool> changed{false};
  std::atomic<int> reads{0};
  const auto await_read_after = [&reads](int seen) {
    while (reads.load() <= seen) {
      std::this_thread::yield();
    }
  };

  std::thread producer([&]() {
    await_read_after(0);
    for (PJ::Timestamp stamp = 3; stamp <= 5000; ++stamp) {
      if (stamp == 2500) {
        await_read_after(reads.load());
      }
      invokeTfTap(session, topic, 1, stamp, edgePayload(/*parent=*/0, /*child=*/1));
    }
    finished.store(true);
  });
  std::thread reader([&]() {
    while (!finished.load()) {
      if (snapshotX(*snapshot, 1) != 2.0 || snapshotX(*snapshot, 5000) != 3.0 || snapshot->getAllFrames().size() != 2) {
        changed.store(true);
      }
      reads.fetch_add(1);
    }
  });
  producer.join();
  reader.join();

  EXPECT_GE(reads.load(), 2) << "the reads must overlap the writes";
  EXPECT_FALSE(changed.load());
  EXPECT_EQ(snapshotX(*service.snapshot(1), 5000), 5001.0);
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
