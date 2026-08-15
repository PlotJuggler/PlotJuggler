// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QFileInfo>
#include <QString>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "hermetic_catalog.h"
#include "pj_base/sdk/data_source_host_views.hpp"
#include "pj_base/sdk/service_registry.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/resident_payload_pool.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/DataSourceRuntimeHost.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/ObjectIngestTap.h"
using namespace Qt::StringLiterals;

#ifndef PJ_RUNTIME_HOST_OBJECT_PARSER_PATH
#error "PJ_RUNTIME_HOST_OBJECT_PARSER_PATH must be defined"
#endif

namespace {

class DataSourceRuntimeHostObjectIngestTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_NE(catalog_.findParserByEncoding(u"runtime_host_object"_s), nullptr);

    auto dataset_or = engine_.createDataset(PJ::DatasetDescriptor{.source_name = "test", .time_domain_id = 0});
    ASSERT_TRUE(dataset_or.has_value()) << dataset_or.error();

    dataset_id_ = static_cast<PJ::DatasetId>(*dataset_or);
    source_handle_ = PJ_data_source_handle_t{static_cast<uint32_t>(*dataset_or)};
    host_ = std::make_unique<PJ::DataSourceRuntimeHost>(
        engine_, catalog_, dataset_id_, source_handle_, object_store_, "runtime_host_test_source",
        /*parser_registrar=*/nullptr, /*secondary_object_store=*/nullptr, /*secondary_data_engine=*/nullptr,
        /*library_keepalive=*/nullptr, ingest_taps_);
    EXPECT_TRUE(host_->registerServices(registry_builder_).has_value());
  }

  [[nodiscard]] PJ::DataSourceRuntimeHostView runtime() {
    PJ::sdk::ServiceRegistry services(registry_builder_.view());
    auto runtime_or = services.require<PJ::sdk::DataSourceRuntimeHostService>();
    EXPECT_TRUE(runtime_or.has_value()) << runtime_or.error();
    return runtime_or.has_value() ? *runtime_or : PJ::DataSourceRuntimeHostView{};
  }

  [[nodiscard]] PJ::Expected<PJ::ParserBindingHandle> bindTopic(std::string topic_name, std::string type_name) {
    return runtime().ensureParserBinding(
        PJ::ParserBindingRequest{
            .topic_name = topic_name,
            .parser_encoding = "runtime_host_object",
            .type_name = type_name,
            .schema = PJ::Span<const uint8_t>{},
            .parser_config_json = "{}",
        });
  }

  [[nodiscard]] std::shared_ptr<std::atomic<int>> pushPayload(
      PJ::ParserBindingHandle binding, PJ::Timestamp timestamp, const std::vector<uint8_t>& payload) {
    auto fetch_calls = std::make_shared<std::atomic<int>>(0);
    auto status = runtime().pushMessage(binding, timestamp, [payload, fetch_calls]() -> std::vector<uint8_t> {
      fetch_calls->fetch_add(1);
      return payload;
    });
    EXPECT_TRUE(status.has_value()) << status.error();
    return fetch_calls;
  }

  [[nodiscard]] uint64_t totalRowCount() const {
    PJ::DataReader reader(engine_);
    const auto topics = reader.listTopics(dataset_id_);
    if (topics.empty()) {
      return 0;
    }
    const auto metadata = reader.getMetadata(topics.front());
    return metadata.has_value() ? metadata->total_row_count : 0;
  }

  QFileInfo plugin_file_{QString::fromUtf8(PJ_RUNTIME_HOST_OBJECT_PARSER_PATH)};
  PJ::test::HermeticCatalog catalog_box_{plugin_file_.absolutePath()};
  PJ::ExtensionCatalogService& catalog_{catalog_box_.service};
  PJ::DataEngine engine_;
  PJ::ObjectStore object_store_;
  PJ::DatasetId dataset_id_{0};
  PJ_data_source_handle_t source_handle_{};
  std::shared_ptr<PJ::ObjectIngestTapRegistry> ingest_taps_ = std::make_shared<PJ::ObjectIngestTapRegistry>();
  std::unique_ptr<PJ::DataSourceRuntimeHost> host_;
  PJ::ServiceRegistryBuilder registry_builder_;
};

TEST_F(DataSourceRuntimeHostObjectIngestTest, PushMessageEagerCommitsScalarsAndStoresOwnedObjectBytes) {
  host_->policyResolver().setDefault(PJ::sdk::ObjectIngestPolicy::kEager);

  auto binding_or = bindTopic("/camera/image", "mock/image");
  ASSERT_TRUE(binding_or.has_value()) << binding_or.error();

  const std::vector<uint8_t> payload{0x10, 0x20, 0x30, 0x40};
  auto fetch_calls = pushPayload(*binding_or, 123, payload);
  EXPECT_EQ(fetch_calls->load(), 1);

  host_->flushAll();
  EXPECT_EQ(totalRowCount(), 1U);

  auto object_topic = object_store_.findTopic(dataset_id_, "/camera/image");
  ASSERT_TRUE(object_topic.has_value());
  EXPECT_EQ(object_store_.descriptor(*object_topic).metadata_json, R"({"builtin_object_type":"kImage"})");
  EXPECT_EQ(object_store_.entryCount(*object_topic), 1U);
  // Under always-lazy ingest with captured anchor, object entries go to the
  // store via pushLazy. The lazy slot does not contribute to memoryUsage —
  // bytes are owned upstream via the captured anchor.
  EXPECT_EQ(object_store_.memoryUsage(*object_topic), 0U);
  auto entry = object_store_.latestAt(*object_topic, 123);
  ASSERT_TRUE(entry.has_value());
  ASSERT_NE(entry->payload.anchor, nullptr);
  EXPECT_EQ(std::vector<uint8_t>(entry->payload.bytes.begin(), entry->payload.bytes.end()), payload);
  EXPECT_EQ(fetch_calls->load(), 1);
}

TEST_F(DataSourceRuntimeHostObjectIngestTest, BindSchemaRegisteredObjectTypeCreatesObjectTopic) {
  auto binding_or = bindTopic("/camera/deferred_image", "mock/bind_schema_image");
  ASSERT_TRUE(binding_or.has_value()) << binding_or.error();

  auto object_topic = object_store_.findTopic(dataset_id_, "/camera/deferred_image");
  ASSERT_TRUE(object_topic.has_value());
  EXPECT_EQ(object_store_.descriptor(*object_topic).metadata_json, R"({"builtin_object_type":"kImage"})");
}

TEST_F(DataSourceRuntimeHostObjectIngestTest, PushMessageLazyObjectsEagerScalarsCommitsScalarsAndDefersObjectBytes) {
  host_->policyResolver().setDefault(PJ::sdk::ObjectIngestPolicy::kLazyObjectsEagerScalars);

  auto binding_or = bindTopic("/camera/image", "mock/image");
  ASSERT_TRUE(binding_or.has_value()) << binding_or.error();

  const std::vector<uint8_t> payload{0x10, 0x20, 0x30, 0x40};
  auto fetch_calls = pushPayload(*binding_or, 123, payload);
  // Exactly one ingest-time fetch feeds the scalar parse; the bytes are then
  // dropped, not pinned in the store entry.
  EXPECT_EQ(fetch_calls->load(), 1);

  host_->flushAll();
  EXPECT_EQ(totalRowCount(), 1U);

  auto object_topic = object_store_.findTopic(dataset_id_, "/camera/image");
  ASSERT_TRUE(object_topic.has_value());
  EXPECT_EQ(object_store_.entryCount(*object_topic), 1U);
  EXPECT_EQ(object_store_.memoryUsage(*object_topic), 0U);
  auto entry = object_store_.latestAt(*object_topic, 123);
  ASSERT_TRUE(entry.has_value());
  ASSERT_NE(entry->payload.anchor, nullptr);
  EXPECT_EQ(std::vector<uint8_t>(entry->payload.bytes.begin(), entry->payload.bytes.end()), payload);
  // Object entries hold the same deferred-fetch closure kPureLazy uses, so
  // each pull re-invokes the fetcher instead of replaying resident bytes.
  EXPECT_EQ(fetch_calls->load(), 2);
}

TEST_F(DataSourceRuntimeHostObjectIngestTest, LazyObjectsEagerScalarsSeedServesPullsWithoutRefetch) {
  // With a ResidentPayloadPool wired, the ingest-time fetch (the producer's hot
  // path) seeds the entry, so object pulls read those bytes with NO re-fetch —
  // until the pool evicts the seed, after which the re-fetch fallback serves.
  auto pool = std::make_shared<PJ::ResidentPayloadPool>(64ULL * 1024 * 1024);
  object_store_.setResidentPayloadPool(pool);
  host_->policyResolver().setDefault(PJ::sdk::ObjectIngestPolicy::kLazyObjectsEagerScalars);

  auto binding_or = bindTopic("/camera/image", "mock/image");
  ASSERT_TRUE(binding_or.has_value()) << binding_or.error();

  const std::vector<uint8_t> payload{0x10, 0x20, 0x30, 0x40};
  const uint64_t admitted_before = pool->stats().admitted;
  auto fetch_calls = pushPayload(*binding_or, 123, payload);
  EXPECT_EQ(fetch_calls->load(), 1);
  EXPECT_EQ(pool->stats().admitted - admitted_before, 1U);

  host_->flushAll();
  auto object_topic = object_store_.findTopic(dataset_id_, "/camera/image");
  ASSERT_TRUE(object_topic.has_value());

  auto entry = object_store_.latestAt(*object_topic, 123);
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(std::vector<uint8_t>(entry->payload.bytes.begin(), entry->payload.bytes.end()), payload);
  EXPECT_EQ(fetch_calls->load(), 1) << "resident seed must serve the pull without re-invoking the fetcher";
  EXPECT_GE(pool->stats().resident_hits, 1U);

  pool->trim();
  auto after_eviction = object_store_.latestAt(*object_topic, 123);
  ASSERT_TRUE(after_eviction.has_value());
  EXPECT_EQ(std::vector<uint8_t>(after_eviction->payload.bytes.begin(), after_eviction->payload.bytes.end()), payload);
  EXPECT_EQ(fetch_calls->load(), 2) << "an evicted seed degrades to the re-fetch fallback";
}

TEST_F(DataSourceRuntimeHostObjectIngestTest, TappedLazyObjectIsUnseededAndStillParsesScalars) {
  auto pool = std::make_shared<PJ::ResidentPayloadPool>(64ULL * 1024 * 1024);
  object_store_.setResidentPayloadPool(pool);
  host_->policyResolver().setDefault(PJ::sdk::ObjectIngestPolicy::kLazyObjectsEagerScalars);

  int tap_calls = 0;
  PJ::ObjectTopicId tapped_topic{};
  std::vector<uint8_t> tapped_bytes;
  auto lease = ingest_taps_->claimExclusive(
      PJ::sdk::BuiltinObjectType::kImage,
      [&](PJ::ObjectTopicId topic, PJ::DatasetId dataset, PJ::Timestamp stamp, PJ::sdk::PayloadView bytes) {
        ++tap_calls;
        tapped_topic = topic;
        EXPECT_EQ(dataset, dataset_id_);
        EXPECT_EQ(stamp, 123);
        tapped_bytes.assign(bytes.bytes.begin(), bytes.bytes.end());
      });
  ASSERT_TRUE(lease.has_value()) << lease.error();

  auto binding = bindTopic("/camera/tapped_image", "mock/image");
  ASSERT_TRUE(binding.has_value()) << binding.error();
  const std::vector<uint8_t> payload{0x10, 0x20, 0x30, 0x40};
  const uint64_t admitted_before = pool->stats().admitted;
  auto fetch_calls = pushPayload(*binding, 123, payload);

  EXPECT_EQ(tap_calls, 1);
  EXPECT_EQ(tapped_bytes, payload);
  EXPECT_EQ(pool->stats().admitted - admitted_before, 0U) << "tapped file entries must not retain pool seeds";
  host_->flushAll();
  EXPECT_EQ(totalRowCount(), 1U) << "the scalar parse must still commit";

  auto object_topic = object_store_.findTopic(dataset_id_, "/camera/tapped_image");
  ASSERT_TRUE(object_topic.has_value());
  EXPECT_EQ(tapped_topic.id, object_topic->id);
  auto entry = object_store_.latestAt(*object_topic, 123);
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(std::vector<uint8_t>(entry->payload.bytes.begin(), entry->payload.bytes.end()), payload);
  EXPECT_EQ(fetch_calls->load(), 2) << "the unseeded catalog closure must re-read on first resolve";
}

TEST_F(DataSourceRuntimeHostObjectIngestTest, TappedEagerObjectKeepsCapturedPayload) {
  host_->policyResolver().setDefault(PJ::sdk::ObjectIngestPolicy::kEager);
  int tap_calls = 0;
  auto lease = ingest_taps_->claimExclusive(
      PJ::sdk::BuiltinObjectType::kImage,
      [&](PJ::ObjectTopicId, PJ::DatasetId, PJ::Timestamp, PJ::sdk::PayloadView) { ++tap_calls; });
  ASSERT_TRUE(lease.has_value()) << lease.error();

  auto binding = bindTopic("/camera/stream_image", "mock/image");
  ASSERT_TRUE(binding.has_value()) << binding.error();
  const std::vector<uint8_t> payload{0xA0, 0xB0, 0xC0};
  auto fetch_calls = pushPayload(*binding, 456, payload);
  EXPECT_EQ(tap_calls, 1);

  auto object_topic = object_store_.findTopic(dataset_id_, "/camera/stream_image");
  ASSERT_TRUE(object_topic.has_value());
  auto entry = object_store_.latestAt(*object_topic, 456);
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(std::vector<uint8_t>(entry->payload.bytes.begin(), entry->payload.bytes.end()), payload);
  EXPECT_EQ(fetch_calls->load(), 1) << "streaming must resolve from its captured payload, not a dead fetcher";
}

TEST_F(DataSourceRuntimeHostObjectIngestTest, ThrowingTapIsContainedAndEntryStillLands) {
  host_->policyResolver().setDefault(PJ::sdk::ObjectIngestPolicy::kEager);
  auto lease = ingest_taps_->claimExclusive(
      PJ::sdk::BuiltinObjectType::kImage,
      [](PJ::ObjectTopicId, PJ::DatasetId, PJ::Timestamp, PJ::sdk::PayloadView) { throw std::runtime_error("boom"); });
  ASSERT_TRUE(lease.has_value()) << lease.error();

  auto binding = bindTopic("/camera/throwing_tap", "mock/image");
  ASSERT_TRUE(binding.has_value()) << binding.error();
  const std::vector<uint8_t> payload{0xDE, 0xAD, 0xBE, 0xEF};
  auto fetch_calls = pushPayload(*binding, 789, payload);

  EXPECT_EQ(host_->ingestTapFailures(), 1U);
  host_->flushAll();
  EXPECT_EQ(totalRowCount(), 1U);
  auto object_topic = object_store_.findTopic(dataset_id_, "/camera/throwing_tap");
  ASSERT_TRUE(object_topic.has_value());
  EXPECT_EQ(object_store_.entryCount(*object_topic), 1U);
  auto entry = object_store_.latestAt(*object_topic, 789);
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(std::vector<uint8_t>(entry->payload.bytes.begin(), entry->payload.bytes.end()), payload);
  EXPECT_EQ(fetch_calls->load(), 1);
}

TEST_F(DataSourceRuntimeHostObjectIngestTest, PushMessagePureLazyDefersFetchAndDoesNotCommitScalars) {
  host_->policyResolver().setDefault(PJ::sdk::ObjectIngestPolicy::kPureLazy);

  auto binding_or = bindTopic("/camera/image", "mock/image");
  ASSERT_TRUE(binding_or.has_value()) << binding_or.error();

  const std::vector<uint8_t> payload{0x10, 0x20, 0x30, 0x40};
  auto fetch_calls = pushPayload(*binding_or, 123, payload);
  EXPECT_EQ(fetch_calls->load(), 0);

  host_->flushAll();
  EXPECT_EQ(totalRowCount(), 0U);

  auto object_topic = object_store_.findTopic(dataset_id_, "/camera/image");
  ASSERT_TRUE(object_topic.has_value());
  EXPECT_EQ(object_store_.entryCount(*object_topic), 1U);
  EXPECT_EQ(object_store_.memoryUsage(*object_topic), 0U);
  auto entry = object_store_.latestAt(*object_topic, 123);
  ASSERT_TRUE(entry.has_value());
  ASSERT_NE(entry->payload.anchor, nullptr);
  EXPECT_EQ(std::vector<uint8_t>(entry->payload.bytes.begin(), entry->payload.bytes.end()), payload);
  EXPECT_EQ(fetch_calls->load(), 1);
}

// Regression for the kPureLazy capture-order use-after-dlclose: the FetcherOwner
// must OWN the DSO keepalive so that ~FetcherOwner's fetcher.release (plugin code)
// runs BEFORE the producing .so is dlclosed — even when the stored lazy closure is
// the LAST holder of the DSO token (post-evict / app close). Before the fix the
// lazy closure captured the keepalive as a sibling of the FetcherOwner, so it
// dropped first and release jumped into unmapped memory.
TEST_F(DataSourceRuntimeHostObjectIngestTest, PureLazyFetcherReleaseRunsWhileDsoStillMapped) {
  bool dso_mapped = true;
  bool released_while_mapped = false;
  // Stand-in plugin DSO token: dropping its last copy "unloads" the .so.
  auto library_keepalive =
      std::shared_ptr<void>(reinterpret_cast<void*>(0x1), [&dso_mapped](void*) { dso_mapped = false; });

  // ~ReleaseProbe runs when the fetcher callable is destroyed — i.e. exactly when
  // ~FetcherOwner calls fetcher.release — recording whether the DSO was still
  // mapped at that instant. An explicit ctor avoids a temporary whose destructor
  // would fire (and record) prematurely.
  struct ReleaseProbe {
    bool* dso_mapped;
    bool* released_while_mapped;
    ReleaseProbe(bool* mapped, bool* released) : dso_mapped(mapped), released_while_mapped(released) {}
    ~ReleaseProbe() {
      if (dso_mapped != nullptr && released_while_mapped != nullptr) {
        *released_while_mapped = *dso_mapped;
      }
    }
  };

  // Local host + store so their lifetimes are controlled independently. The store
  // outlives the host: it holds the lazy closure that pins the DSO.
  PJ::ObjectStore local_store;
  auto local_dataset_or = engine_.createDataset(PJ::DatasetDescriptor{.source_name = "probe", .time_domain_id = 0});
  ASSERT_TRUE(local_dataset_or.has_value()) << local_dataset_or.error();
  const auto local_dataset = static_cast<PJ::DatasetId>(*local_dataset_or);
  const PJ_data_source_handle_t local_handle{static_cast<uint32_t>(*local_dataset_or)};

  auto local_host = std::make_unique<PJ::DataSourceRuntimeHost>(
      engine_, catalog_, local_dataset, local_handle, local_store, "probe_source",
      /*parser_registrar=*/nullptr, /*secondary_object_store=*/nullptr, /*secondary_data_engine=*/nullptr,
      library_keepalive);
  local_host->policyResolver().setDefault(PJ::sdk::ObjectIngestPolicy::kPureLazy);

  PJ::ServiceRegistryBuilder local_registry;
  ASSERT_TRUE(local_host->registerServices(local_registry).has_value());
  PJ::sdk::ServiceRegistry services(local_registry.view());
  auto runtime_or = services.require<PJ::sdk::DataSourceRuntimeHostService>();
  ASSERT_TRUE(runtime_or.has_value()) << runtime_or.error();
  PJ::DataSourceRuntimeHostView runtime = *runtime_or;

  auto binding_or = runtime.ensureParserBinding(
      PJ::ParserBindingRequest{
          .topic_name = "/camera/image",
          .parser_encoding = "runtime_host_object",
          .type_name = "mock/image",
          .schema = PJ::Span<const uint8_t>{},
          .parser_config_json = "{}",
      });
  ASSERT_TRUE(binding_or.has_value()) << binding_or.error();

  // kPureLazy stores the fetcher closure without invoking it; the callable (and
  // its ReleaseProbe) lives inside the FetcherOwner until the entry is destroyed.
  auto probe = std::make_shared<ReleaseProbe>(&dso_mapped, &released_while_mapped);
  const std::vector<uint8_t> payload{0x01, 0x02, 0x03, 0x04};
  auto status = runtime.pushMessage(
      *binding_or, 123, [payload, probe = std::move(probe)]() -> std::vector<uint8_t> { return payload; });
  ASSERT_TRUE(status.has_value()) << status.error();

  // Catalog teardown + producing handle death: drop the test's token and the host
  // (its library_keepalive_ member). The stored lazy closure is now the ONLY DSO
  // token holder and must keep the .so mapped.
  library_keepalive.reset();
  local_host.reset();
  EXPECT_TRUE(dso_mapped) << "stored lazy closure must keep the producing DSO token mapped";
  EXPECT_FALSE(released_while_mapped);

  // Destroying the stored closure runs ~FetcherOwner -> fetcher.release; it MUST
  // run before the DSO unloads.
  local_store.clear();
  EXPECT_TRUE(released_while_mapped) << "fetcher.release ran after the DSO unloaded (capture-order UAF)";
  EXPECT_FALSE(dso_mapped) << "the keepalive drops once the stored closure is destroyed";
}

TEST_F(DataSourceRuntimeHostObjectIngestTest, PushMessageKeepsScalarOnlyTopicsEagerUnderLazyPolicy) {
  host_->policyResolver().setForTopic("/scalar/topic", PJ::sdk::ObjectIngestPolicy::kPureLazy);

  auto binding_or = bindTopic("/scalar/topic", "mock/scalar");
  ASSERT_TRUE(binding_or.has_value()) << binding_or.error();

  const std::vector<uint8_t> payload{0x10, 0x20};
  auto fetch_calls = pushPayload(*binding_or, 123, payload);
  EXPECT_EQ(fetch_calls->load(), 1);

  host_->flushAll();
  EXPECT_EQ(totalRowCount(), 1U);
  EXPECT_FALSE(object_store_.findTopic(dataset_id_, "/scalar/topic").has_value());
}

TEST_F(DataSourceRuntimeHostObjectIngestTest, SetObjectRetentionBudgetAppliesToEveryBoundObjectTopic) {
  auto binding_a = bindTopic("/camera/image", "mock/image");
  ASSERT_TRUE(binding_a.has_value()) << binding_a.error();
  auto binding_b = bindTopic("/camera/image_b", "mock/image");
  ASSERT_TRUE(binding_b.has_value()) << binding_b.error();

  constexpr int64_t kWindowNs = 5'000'000'000LL;
  constexpr size_t kMemCap = 16U * 1024U * 1024U;
  host_->setObjectRetentionBudget(kWindowNs, kMemCap);

  for (const auto* name : {"/camera/image", "/camera/image_b"}) {
    auto topic_id = object_store_.findTopic(dataset_id_, name);
    ASSERT_TRUE(topic_id.has_value());
    const auto budget = object_store_.retentionBudget(*topic_id);
    EXPECT_EQ(budget.time_window_ns, kWindowNs);
    EXPECT_EQ(budget.max_memory_bytes, kMemCap);
  }
}

// Regression for the streaming dual-store pause bug: object pushes must follow
// setObjectStoreTarget so the primary is frozen (never written, never evicted)
// while paused. Before the fix, cbPushMessage pushed straight to the primary
// regardless of the swap, so paused frames slid the primary's retention window
// and shrank the scrub-back range.
TEST_F(DataSourceRuntimeHostObjectIngestTest, ObjectPushFollowsStoreTargetSwap) {
  PJ::DataEngine secondary_engine;
  PJ::ObjectStore secondary_object_store;
  // Lockstep dataset on the secondary (same DatasetId), mirroring StreamingSourceManager.
  auto secondary_dataset =
      secondary_engine.createDataset(PJ::DatasetDescriptor{.source_name = "test", .time_domain_id = 0});
  ASSERT_TRUE(secondary_dataset.has_value()) << secondary_dataset.error();
  ASSERT_EQ(static_cast<PJ::DatasetId>(*secondary_dataset), dataset_id_);

  PJ::ServiceRegistryBuilder builder;
  PJ::DataSourceRuntimeHost host(
      engine_, catalog_, dataset_id_, source_handle_, object_store_, "dual_store_src", {}, &secondary_object_store,
      &secondary_engine, /*library_keepalive=*/nullptr);
  host.policyResolver().setDefault(PJ::sdk::ObjectIngestPolicy::kEager);
  ASSERT_TRUE(host.registerServices(builder).has_value());

  PJ::sdk::ServiceRegistry services(builder.view());
  auto runtime_or = services.require<PJ::sdk::DataSourceRuntimeHostService>();
  ASSERT_TRUE(runtime_or.has_value()) << runtime_or.error();
  PJ::DataSourceRuntimeHostView runtime = *runtime_or;

  auto binding = runtime.ensureParserBinding(
      PJ::ParserBindingRequest{
          .topic_name = "/camera/image",
          .parser_encoding = "runtime_host_object",
          .type_name = "mock/image",
          .schema = PJ::Span<const uint8_t>{},
          .parser_config_json = "{}",
      });
  ASSERT_TRUE(binding.has_value()) << binding.error();

  auto push = [&](PJ::Timestamp ts) {
    auto status = runtime.pushMessage(*binding, ts, []() -> std::vector<uint8_t> { return {1, 2, 3, 4}; });
    EXPECT_TRUE(status.has_value()) << status.error();
  };

  auto primary_topic = object_store_.findTopic(dataset_id_, "/camera/image");
  auto secondary_topic = secondary_object_store.findTopic(dataset_id_, "/camera/image");
  ASSERT_TRUE(primary_topic.has_value());
  ASSERT_TRUE(secondary_topic.has_value()) << "object topic must be lockstep-registered on both stores";

  // Live: target defaults to the primary.
  push(100);
  EXPECT_EQ(object_store_.entryCount(*primary_topic), 1U);
  EXPECT_EQ(secondary_object_store.entryCount(*secondary_topic), 0U);

  // Pause: swap to the secondary. New frames land on B; the primary is frozen.
  host.setObjectStoreTarget(&secondary_object_store);
  push(200);
  push(300);
  EXPECT_EQ(object_store_.entryCount(*primary_topic), 1U) << "primary must not grow while paused";
  EXPECT_EQ(secondary_object_store.entryCount(*secondary_topic), 2U);

  // Resume: swap back to the primary.
  host.setObjectStoreTarget(&object_store_);
  push(400);
  EXPECT_EQ(object_store_.entryCount(*primary_topic), 2U);
  EXPECT_EQ(secondary_object_store.entryCount(*secondary_topic), 2U);
}

// Mode-B regression (amcl_test_bag): a topic interleaving publishers whose
// embedded stamps disagree (amcl future-dates map->odom by seconds) must lose
// NOTHING. Pre-fix, the datastore's monotonic append rejected every regressed
// scalar row, which failed the whole pushMessage and silently dropped the
// object entry with it — the 3D view's TF buffer then froze.
TEST_F(DataSourceRuntimeHostObjectIngestTest, EmbeddedTimestampRegressionKeepsScalarsAndObjects) {
  host_->policyResolver().setDefault(PJ::sdk::ObjectIngestPolicy::kEager);

  auto binding_or = bindTopic("/tf_like", "mock/embedded_ts_image");
  ASSERT_TRUE(binding_or.has_value()) << binding_or.error();

  // Arrival order alternates the future-dated publisher (1000, 1100, ...)
  // with the lagging one (901, 1001, ...); host receive times stay monotonic.
  const std::vector<int64_t> embedded_stamps = {1000, 901, 1100, 1001, 1200, 1101};
  for (std::size_t arrival_index = 0; arrival_index < embedded_stamps.size(); ++arrival_index) {
    std::vector<uint8_t> payload(sizeof(int64_t));
    std::memcpy(payload.data(), &embedded_stamps[arrival_index], sizeof(int64_t));
    // pushPayload asserts the push succeeded — a regressed embedded stamp
    // must not fail the message.
    (void)pushPayload(*binding_or, static_cast<PJ::Timestamp>(10 + arrival_index), payload);
  }
  host_->flushAll();

  // Every scalar row landed, timestamped by its embedded stamp and readable
  // back in sorted order.
  EXPECT_EQ(totalRowCount(), embedded_stamps.size());
  PJ::DataReader reader(engine_);
  const auto topics = reader.listTopics(dataset_id_);
  ASSERT_FALSE(topics.empty());
  auto cursor_or = reader.rangeQuery(
      PJ::QueryRange{.topic_id = topics.front(), .t_min = 0, .t_max = std::numeric_limits<PJ::Timestamp>::max()});
  ASSERT_TRUE(cursor_or.has_value()) << cursor_or.error();
  std::vector<PJ::Timestamp> row_stamps;
  cursor_or->forEach([&row_stamps](const PJ::SampleRow& row) { row_stamps.push_back(row.timestamp); });
  EXPECT_EQ(row_stamps, (std::vector<PJ::Timestamp>{901, 1000, 1001, 1100, 1101, 1200}));

  // Every object entry landed too (the TF-buffer feed in the real pipeline).
  auto object_topic = object_store_.findTopic(dataset_id_, "/tf_like");
  ASSERT_TRUE(object_topic.has_value());
  EXPECT_EQ(object_store_.entryCount(*object_topic), embedded_stamps.size());
}

// NOTE: the two parser-binding TopicId-reuse pins that used to live here moved
// to StreamParserSwapTest in data_source_runtime_host_stream_swap_test.cpp,
// beside the sibling binding-reuse contract cases and that suite's ready-made
// two-engine lockstep fixture.

}  // namespace
