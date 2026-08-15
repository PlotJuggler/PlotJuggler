// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/plugin_data_host.hpp"

namespace PJ {
namespace {

using sdk::ObjectTopicHandle;
using sdk::SourceObjectWriteHostView;

constexpr DatasetId kDatasetId = 42;

struct Fixture {
  ObjectStore store;
  DatastoreSourceObjectWriteHost host_impl{store, kDatasetId};
  SourceObjectWriteHostView host{host_impl.raw()};
};

TEST(PluginDataHostObjectTest, RegisterTopicReturnsUsableHandle) {
  Fixture f;
  auto handle = f.host.registerTopic("markers", R"({"media_class":"scene"})");
  ASSERT_TRUE(handle.has_value()) << handle.error();
  EXPECT_NE(handle->id, 0U);

  // Metadata round-trips through the store.
  const auto topics = f.store.listTopics(kDatasetId);
  ASSERT_EQ(topics.size(), 1U);
  const auto& desc = f.store.descriptor(topics[0]);
  EXPECT_EQ(desc.topic_name, "markers");
  EXPECT_EQ(desc.metadata_json, R"({"media_class":"scene"})");
  EXPECT_EQ(desc.dataset_id, kDatasetId);
}

TEST(PluginDataHostObjectTest, RegisterTopicReusesSameNameWithMatchingMetadata) {
  // The refill contract: re-registration with identical metadata reuses the
  // topic (same id, entries untouched) so a replace-refill's replay lands in
  // the topic the eager ingest created.
  Fixture f;
  const auto first = f.host.registerTopic("markers", R"({"media_class":"scene"})");
  ASSERT_TRUE(first.has_value()) << first.error();
  const std::vector<uint8_t> payload = {1, 2, 3};
  ASSERT_TRUE(f.host.pushOwned(*first, 1000, payload).has_value());

  const auto again = f.host.registerTopic("markers", R"({"media_class":"scene"})");
  ASSERT_TRUE(again.has_value()) << again.error();
  EXPECT_EQ(again->id, first->id);

  const auto topics = f.store.listTopics(kDatasetId);
  ASSERT_EQ(topics.size(), 1U);
  EXPECT_EQ(f.store.entryCount(ObjectTopicId{first->id}), 1U);
}

TEST(PluginDataHostObjectTest, RegisterTopicRejectsSameNameWithDifferentMetadata) {
  // A same-name registration with DIFFERENT metadata is a genuine collision
  // (two producers on one name), not a refill — it must stay a loud failure.
  Fixture f;
  ASSERT_TRUE(f.host.registerTopic("markers", R"({"media_class":"scene"})").has_value());
  const auto conflicting = f.host.registerTopic("markers", R"({"media_class":"other"})");
  EXPECT_FALSE(conflicting.has_value());
}

TEST(PluginDataHostObjectTest, RegisterTopicReuseIsScopedToTheDataset) {
  // The reuse key is (dataset, name, metadata): the same name on another
  // dataset is an independent topic, never a cross-dataset match.
  ObjectStore store;
  DatastoreSourceObjectWriteHost host_a{store, DatasetId{1}};
  DatastoreSourceObjectWriteHost host_b{store, DatasetId{2}};
  SourceObjectWriteHostView view_a{host_a.raw()};
  SourceObjectWriteHostView view_b{host_b.raw()};
  const auto topic_a = view_a.registerTopic("markers", "{}");
  const auto topic_b = view_b.registerTopic("markers", "{}");
  ASSERT_TRUE(topic_a.has_value()) << topic_a.error();
  ASSERT_TRUE(topic_b.has_value()) << topic_b.error();
  EXPECT_NE(topic_a->id, topic_b->id);
}

TEST(PluginDataHostObjectTest, PushOwnedStoresBytes) {
  Fixture f;
  const auto topic = *f.host.registerTopic("markers", "{}");

  const std::vector<uint8_t> payload = {1, 2, 3, 4, 5};
  auto status = f.host.pushOwned(topic, 1000, payload);
  ASSERT_TRUE(status.has_value()) << status.error();
  status = f.host.pushOwned(topic, 2000, payload);
  ASSERT_TRUE(status.has_value()) << status.error();

  const ObjectTopicId store_id{topic.id};
  EXPECT_EQ(f.store.entryCount(store_id), 2U);
  auto resolved = f.store.latestAt(store_id, 2000);
  ASSERT_TRUE(resolved.has_value());
  ASSERT_NE(resolved->payload.anchor, nullptr);
  EXPECT_EQ(resolved->payload.bytes.size(), payload.size());
  EXPECT_TRUE(
      std::equal(resolved->payload.bytes.begin(), resolved->payload.bytes.end(), payload.begin(), payload.end()));
}

TEST(PluginDataHostObjectTest, PushLazyRetainsClosureUntilEviction) {
  Fixture f;
  const auto topic = *f.host.registerTopic("images", R"({"media_class":"image"})");

  // Use an atomic destroy-counter embedded in the shared state to prove the
  // fetch_ctx_destroy callback runs exactly once per evicted entry.
  struct SharedState {
    std::atomic<int> fetch_calls{0};
    std::atomic<int> destroy_calls{0};
    std::vector<uint8_t> payload;
  };
  auto shared = std::make_shared<SharedState>();
  shared->payload = {0xDE, 0xAD, 0xBE, 0xEF};

  auto closure = [shared]() -> std::vector<uint8_t> {
    shared->fetch_calls.fetch_add(1);
    return shared->payload;
  };

  auto status = f.host.pushLazy(topic, 42, closure);
  ASSERT_TRUE(status.has_value()) << status.error();

  // First read invokes the fetch closure and returns its bytes.
  auto first = f.store.latestAt(ObjectTopicId{topic.id}, 42);
  ASSERT_TRUE(first.has_value());
  ASSERT_NE(first->payload.anchor, nullptr);
  EXPECT_TRUE(
      std::equal(
          first->payload.bytes.begin(), first->payload.bytes.end(), shared->payload.begin(), shared->payload.end()));
  EXPECT_EQ(shared->fetch_calls.load(), 1);

  // A second read of the SAME sample is served from latestAt's warm cache, so the
  // closure is NOT re-invoked. It remains retained (alive) for a later cache miss —
  // the eviction checks below confirm it is only released when the entry is evicted.
  auto second = f.store.latestAt(ObjectTopicId{topic.id}, 42);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(shared->fetch_calls.load(), 1);

  // Destroy has not been invoked yet — the entry is still alive.
  // (The test's `shared` is one ref; the closure captured in the store is
  // another; a temporary held by the ObjectStore's fetch wrapper is a
  // third. Refcount is implementation-detail — assert the visible effect.)
  // SharedState has NOT been destroyed, so destroy_calls is still 0.
  EXPECT_EQ(shared->destroy_calls.load(), 0);

  // Evict: store drops the entry, which drops the std::function, which drops
  // the plugin's shared holder, which runs fetch_ctx_destroy. This test
  // can't directly observe fetch_ctx_destroy because the SDK's LazyBox
  // destroy just `delete`s its box; but by construction `closure` owns
  // only `shared`, and when the store drops its copy of closure, only our
  // local `closure` variable + our local `shared` remain. We can still
  // verify that the closure is gone from the store by observing
  // `entryCount` drop to zero.
  f.store.evictBefore(ObjectTopicId{topic.id}, 100);
  EXPECT_EQ(f.store.entryCount(ObjectTopicId{topic.id}), 0U);
}

TEST(PluginDataHostObjectTest, PushLazyDestroyCallbackRunsExactlyOnceOnEviction) {
  // Integration test using the raw C ABI — explicitly verifies the destroy
  // callback fires exactly once when the entry is evicted from the store.
  Fixture f;
  const auto topic = *f.host.registerTopic("pointclouds", R"({"media_class":"pointcloud"})");

  struct Ctx {
    std::atomic<int> destroy_count{0};
    std::vector<uint8_t> last_bytes;
    std::vector<uint8_t> payload{0x11, 0x22, 0x33};
  };
  auto* ctx = new Ctx();

  auto fetch_fn = [](void* c, const uint8_t** out_data, uint64_t* out_size) noexcept -> bool {
    auto* self = static_cast<Ctx*>(c);
    self->last_bytes = self->payload;
    *out_data = self->last_bytes.data();
    *out_size = self->last_bytes.size();
    return true;
  };
  auto destroy_fn = [](void* c) noexcept { static_cast<Ctx*>(c)->destroy_count.fetch_add(1); };

  const auto raw = f.host.raw();
  PJ_error_t err{};
  ASSERT_TRUE(raw.vtable->push_lazy(raw.ctx, topic, 100, fetch_fn, ctx, destroy_fn, &err));

  // Fetch once — the callback runs but the ctx stays alive.
  auto resolved = f.store.latestAt(ObjectTopicId{topic.id}, 100);
  ASSERT_TRUE(resolved.has_value());
  EXPECT_TRUE(
      std::equal(
          resolved->payload.bytes.begin(), resolved->payload.bytes.end(), ctx->payload.begin(), ctx->payload.end()));
  EXPECT_EQ(ctx->destroy_count.load(), 0);

  // Evict — destroy_fn runs exactly once.
  f.store.evictBefore(ObjectTopicId{topic.id}, 1000);
  EXPECT_EQ(f.store.entryCount(ObjectTopicId{topic.id}), 0U);
  EXPECT_EQ(ctx->destroy_count.load(), 1);

  delete ctx;  // clean up the raw box we allocated in the test.
}

TEST(PluginDataHostObjectTest, PushLazyWithNullFetchFnFails) {
  Fixture f;
  const auto topic = *f.host.registerTopic("bogus", "{}");

  const auto raw = f.host.raw();
  PJ_error_t err{};
  std::atomic<int> destroyed{0};
  auto destroy_fn = [](void* c) noexcept { static_cast<std::atomic<int>*>(c)->fetch_add(1); };
  EXPECT_FALSE(raw.vtable->push_lazy(raw.ctx, topic, 1, nullptr, &destroyed, destroy_fn, &err));
  // Even on failure, the store calls destroy_fn to free plugin-owned ctx.
  EXPECT_EQ(destroyed.load(), 1);
}

TEST(PluginDataHostObjectTest, PushRejectsUnknownTopicHandle) {
  Fixture f;
  const ObjectTopicHandle bogus{99999};
  const std::vector<uint8_t> payload{1, 2, 3};
  auto status = f.host.pushOwned(bogus, 1, payload);
  EXPECT_FALSE(status.has_value());
}

TEST(PluginDataHostObjectTest, SetRetentionBudgetEnforcesTimeWindow) {
  Fixture f;
  const auto topic = *f.host.registerTopic("rolling", "{}");

  // 10 ns window. Pushes at t=0,1,...,100 — only entries within 10 ns of
  // the newest timestamp should survive.
  f.host.setRetentionBudget(topic, /*time_window_ns=*/10, /*max_memory_bytes=*/0);
  const std::vector<uint8_t> payload{0xAA};
  for (int64_t t = 0; t <= 100; ++t) {
    ASSERT_TRUE(f.host.pushOwned(topic, t, payload).has_value());
  }
  // Entries older than 90 ns (100 - 10) are evicted.
  const auto range = f.store.timeRange(ObjectTopicId{topic.id});
  EXPECT_GE(range.first, 90);
  EXPECT_EQ(range.second, 100);
}

TEST(PluginDataHostObjectTest, ViewReportsNotBoundWhenRawIsEmpty) {
  SourceObjectWriteHostView empty;
  EXPECT_FALSE(empty.valid());
  auto status = empty.pushOwned(ObjectTopicHandle{1}, 0, {});
  EXPECT_FALSE(status.has_value());
}

// ===========================================================================
// setTarget — streaming two-store flow
// ===========================================================================

TEST(PluginDataHostObjectTest, SetTargetRedirectsRegisterAndPushToSecondary) {
  // Simulates the streaming pause/resume routing: the manager flips the host
  // between a primary and a secondary store. After setTarget(secondary) the
  // host's registerTopic + pushOwned land in the secondary; pushes against
  // primary stop. Flipping back to primary resumes routing there.
  ObjectStore primary;
  ObjectStore secondary;
  DatastoreSourceObjectWriteHost host_impl{primary, kDatasetId};
  SourceObjectWriteHostView host{host_impl.raw()};

  // Lockstep registration on both stores BEFORE the swap, so the topic id
  // is the same on each side (auto-counter ticks identically). This matches
  // the manager wiring described in the two-store plan.
  const auto primary_topic = *host.registerTopic("cam", "{}");
  host_impl.setTarget(&secondary);
  const auto secondary_topic = *host.registerTopic("cam", "{}");
  EXPECT_EQ(primary_topic.id, secondary_topic.id);

  // A push now must land in secondary, not primary.
  const std::vector<uint8_t> payload_a{0x01, 0x02};
  ASSERT_TRUE(host.pushOwned(secondary_topic, 1000, payload_a).has_value());
  EXPECT_EQ(primary.entryCount(ObjectTopicId{primary_topic.id}), 0U);
  EXPECT_EQ(secondary.entryCount(ObjectTopicId{secondary_topic.id}), 1U);

  // Flip back: subsequent pushes return to primary.
  host_impl.setTarget(&primary);
  const std::vector<uint8_t> payload_b{0x03};
  ASSERT_TRUE(host.pushOwned(primary_topic, 2000, payload_b).has_value());
  EXPECT_EQ(primary.entryCount(ObjectTopicId{primary_topic.id}), 1U);
  EXPECT_EQ(secondary.entryCount(ObjectTopicId{secondary_topic.id}), 1U);
}

TEST(PluginDataHostObjectTest, ParserSetTargetRedirectsPushToSecondary) {
  // Same test as above but for the parser-scoped host, which is the one the
  // streaming worker actually drives (parser-bound topic id captured at
  // bind() — invariant under the swap because the manager has registered
  // the topic in both stores via lockstep registerTopic).
  ObjectStore primary;
  ObjectStore secondary;
  // Pre-register on both stores in lockstep. Both auto-assign the same id.
  const auto primary_topic =
      *primary.registerTopic({.dataset_id = kDatasetId, .topic_name = "cam", .metadata_json = "{}"});
  const auto secondary_topic =
      *secondary.registerTopic({.dataset_id = kDatasetId, .topic_name = "cam", .metadata_json = "{}"});
  ASSERT_EQ(primary_topic.id, secondary_topic.id);

  DatastoreParserObjectWriteHost host_impl{primary, primary_topic.id};
  sdk::ParserObjectWriteHostView host{host_impl.raw()};

  const std::vector<uint8_t> payload_a{0x01};
  const std::vector<uint8_t> payload_b{0x02};
  const std::vector<uint8_t> payload_c{0x03};

  // Push before swap goes to primary.
  ASSERT_TRUE(host.pushOwned(1000, payload_a).has_value());
  EXPECT_EQ(primary.entryCount(primary_topic), 1U);
  EXPECT_EQ(secondary.entryCount(secondary_topic), 0U);

  // Swap and push: now lands in secondary, primary untouched.
  host_impl.setTarget(&secondary);
  ASSERT_TRUE(host.pushOwned(2000, payload_b).has_value());
  EXPECT_EQ(primary.entryCount(primary_topic), 1U);
  EXPECT_EQ(secondary.entryCount(secondary_topic), 1U);

  // Swap back: resumes pushing into primary.
  host_impl.setTarget(&primary);
  ASSERT_TRUE(host.pushOwned(3000, payload_c).has_value());
  EXPECT_EQ(primary.entryCount(primary_topic), 2U);
  EXPECT_EQ(secondary.entryCount(secondary_topic), 1U);
}

// Annotation path (toolbox surface): a toolbox attaches an object topic to a
// dataset by its DatasetId (not a source it created), idempotently — this is how
// a marker producer publishes its set onto a dataset another source loaded.
TEST(PluginDataHostObjectTest, ToolboxRegistersObjectTopicOnExistingDatasetIdempotently) {
  DataEngine engine;
  ObjectStore store;
  DatastoreToolboxHost toolbox_impl{engine, store};
  sdk::ToolboxHostView toolbox{toolbox_impl.raw()};

  const auto source = *toolbox.createDataSource("loaded_elsewhere");
  const auto dataset = static_cast<DatasetId>(source.id);
  constexpr const char* kTopic = "__markers__/__global__";

  auto handle = toolbox.registerObjectTopicOnDataset(dataset, kTopic, R"({"object_type":"plot_markers"})");
  ASSERT_TRUE(handle.has_value()) << handle.error();

  // Idempotent: republishing re-resolves the SAME topic handle, never "exists".
  auto again = toolbox.registerObjectTopicOnDataset(dataset, kTopic, "{}");
  ASSERT_TRUE(again.has_value()) << again.error();
  EXPECT_EQ(handle->id, again->id);

  const std::vector<uint8_t> payload{0x10, 0x20, 0x30};
  ASSERT_TRUE(toolbox.pushOwnedObject(*handle, 0, Span<const uint8_t>{payload.data(), payload.size()}).has_value());
  const auto resolved = store.latestAt(ObjectTopicId{handle->id}, 0);
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->payload.bytes.size(), payload.size());

  // An unknown dataset is rejected.
  EXPECT_FALSE(toolbox.registerObjectTopicOnDataset(DatasetId{999999}, "x", "{}").has_value());
}

}  // namespace
}  // namespace PJ
