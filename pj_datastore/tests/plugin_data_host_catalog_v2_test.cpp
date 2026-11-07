// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Catalog snapshot v2 (acquire_catalog_snapshot_v2): the same scalar catalog as v1
// plus every object topic (dataset, canonical builtin type, whole metadata, retained
// entry count and raw time range), in one deep copy taken without ever holding the
// engine lock and the ObjectStore lock at the same time.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pj_base/sdk/platform.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/plugin_data_host.hpp"

namespace PJ {
namespace {

using namespace PJ::sdk;

struct Fixture {
  DataEngine engine;
  ObjectStore object_store;
  DatastoreToolboxHost toolbox_impl{engine, object_store};
  ToolboxHostView toolbox{toolbox_impl.raw()};
};

const std::vector<uint8_t> kPayload{0x01, 0x02};

TEST(PluginDataHostCatalogV2Test, ObjectTopicsCarryDatasetTypeCountAndRange) {
  Fixture f;
  ASSERT_TRUE(f.toolbox.hasCatalogSnapshotV2());
  const auto source = *f.toolbox.createDataSource("s");
  const auto scalar_topic = *f.toolbox.ensureTopic(source, "scalars");
  ASSERT_TRUE(f.toolbox.ensureField(scalar_topic, "x", PrimitiveType::kFloat64).has_value());

  const std::string metadata = R"({"builtin_object_type":"kPointCloud","frame":"lidar"})";
  const auto cloud_topic = *f.toolbox.registerObjectTopic(source, "cloud", metadata);
  ASSERT_TRUE(f.toolbox.pushOwnedObject(cloud_topic, 5'000'000'000LL, kPayload).has_value());
  ASSERT_TRUE(f.toolbox.pushOwnedObject(cloud_topic, 20'000'000'000LL, kPayload).has_value());

  auto snapshot = f.toolbox.catalogSnapshotV2();
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error();

  ASSERT_EQ(snapshot->objectTopics().size(), 1u);
  const PJ_object_topic_info_t& info = snapshot->objectTopics()[0];
  EXPECT_EQ(info.handle.id, cloud_topic.id);
  EXPECT_EQ(toStringView(info.name), "cloud");
  EXPECT_EQ(toStringView(info.builtin_object_type), name(BuiltinObjectType::kPointCloud));
  EXPECT_EQ(toStringView(info.metadata_json), metadata) << "the whole document, not only the type";
  EXPECT_EQ(info.source.id, source.id);
  EXPECT_EQ(info.entry_count, 2u);
  EXPECT_EQ(info.time_min_ns, 5'000'000'000LL);
  EXPECT_EQ(info.time_max_ns, 20'000'000'000LL);
  EXPECT_EQ(info.reserved[0], 0u);
  EXPECT_EQ(info.reserved[1], 0u);

  // The scalar half is unchanged vs. v1.
  auto v1 = f.toolbox.catalogSnapshot();
  ASSERT_TRUE(v1.has_value()) << v1.error();
  ASSERT_EQ(v1->topics().size(), snapshot->topics().size());
  ASSERT_EQ(v1->topics().size(), 1u);
  EXPECT_EQ(toStringView(v1->topics()[0].name), toStringView(snapshot->topics()[0].name));
  ASSERT_EQ(v1->fields().size(), snapshot->fields().size());
  ASSERT_EQ(snapshot->dataSources().size(), 1u);
  EXPECT_EQ(toStringView(snapshot->dataSources()[0].name), "s");
}

TEST(PluginDataHostCatalogV2Test, EmptyTopicHasZeroRange) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("s");
  (void)*f.toolbox.registerObjectTopic(source, "empty", R"({"builtin_object_type":"kPointCloud"})");

  auto snapshot = f.toolbox.catalogSnapshotV2();
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error();
  ASSERT_EQ(snapshot->objectTopics().size(), 1u);
  const PJ_object_topic_info_t& info = snapshot->objectTopics()[0];
  EXPECT_EQ(info.entry_count, 0u);
  EXPECT_EQ(info.time_min_ns, 0);
  EXPECT_EQ(info.time_max_ns, 0);
}

TEST(PluginDataHostCatalogV2Test, ReleaseFreesState) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("s");
  (void)*f.toolbox.registerObjectTopic(source, "t", "{}");

  // The RAII view calls release() on destruction; ASan flags a leak here if the
  // snapshot's release/release_ctx were not wired.
  {
    auto snapshot = f.toolbox.catalogSnapshotV2();
    ASSERT_TRUE(snapshot.has_value()) << snapshot.error();
    EXPECT_EQ(snapshot->objectTopics().size(), 1u);
  }
}

// The snapshot is a deep copy: it outlives the host and the stores it was read from.
TEST(PluginDataHostCatalogV2Test, SnapshotOutlivesTheHost) {
  Expected<CatalogSnapshotV2> snapshot = unexpected("not taken");
  {
    Fixture f;
    const auto source = *f.toolbox.createDataSource("s");
    (void)*f.toolbox.registerObjectTopic(source, "cloud", R"({"builtin_object_type":"kPointCloud"})");
    snapshot = f.toolbox.catalogSnapshotV2();
    ASSERT_TRUE(snapshot.has_value()) << snapshot.error();
  }
  ASSERT_EQ(snapshot->objectTopics().size(), 1u);
  EXPECT_EQ(toStringView(snapshot->objectTopics()[0].name), "cloud");
  EXPECT_EQ(toStringView(snapshot->objectTopics()[0].metadata_json), R"({"builtin_object_type":"kPointCloud"})");
}

TEST(PluginDataHostCatalogV2Test, V1SnapshotIsUnchanged) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("s");
  const auto scalar_topic = *f.toolbox.ensureTopic(source, "scalars");
  ASSERT_TRUE(f.toolbox.ensureField(scalar_topic, "x", PrimitiveType::kFloat64).has_value());
  (void)*f.toolbox.registerObjectTopic(source, "cloud", R"({"builtin_object_type":"kPointCloud"})");

  auto v1 = f.toolbox.catalogSnapshot();
  ASSERT_TRUE(v1.has_value()) << v1.error();
  ASSERT_EQ(v1->dataSources().size(), 1u);
  ASSERT_EQ(v1->topics().size(), 1u);
  EXPECT_EQ(toStringView(v1->topics()[0].name), "scalars");
  ASSERT_EQ(v1->fields().size(), 1u);
  EXPECT_EQ(toStringView(v1->fields()[0].name), "x");
}

// A derived topic is listed with its metadata (pj_derived included) and the count and
// range of what its ring retains now: an evicted publication no longer counts.
TEST(PluginDataHostCatalogV2Test, DerivedTopicReportsItsRingRange) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("s");
  const std::string metadata = R"({"builtin_object_type":"kPointCloud","pj_derived":"on_demand"})";
  const ObjectTopicId topic =
      *f.object_store.registerTopic({.dataset_id = source.id, .topic_name = "crop", .metadata_json = metadata});
  for (const Timestamp stamp : {Timestamp{-3'000}, Timestamp{1'000}, Timestamp{2'000}}) {
    ASSERT_TRUE(f.object_store.pushOwned(topic, stamp, kPayload).has_value());
  }
  // The ring's eviction: the oldest publication goes, as ResultHistory's bundle drop does.
  ASSERT_TRUE(f.object_store
                  .pushOwnedBundle({OwnedObjectWrite{
                      .topic = topic,
                      .drop = [](Timestamp stamp, SequentialUID) { return stamp < 0; },
                      .append = false,
                  }})
                  .has_value());

  auto snapshot = f.toolbox.catalogSnapshotV2();
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error();
  ASSERT_EQ(snapshot->objectTopics().size(), 1u);
  const PJ_object_topic_info_t& info = snapshot->objectTopics()[0];
  EXPECT_EQ(toStringView(info.metadata_json), metadata);
  EXPECT_EQ(info.entry_count, 2u);
  EXPECT_EQ(info.time_min_ns, 1'000);
  EXPECT_EQ(info.time_max_ns, 2'000);
}

// `source` is the dataset a topic lives on, and the list is sorted by (dataset, name),
// so one name on two datasets gives two rows.
TEST(PluginDataHostCatalogV2Test, SameNameOnTwoDatasetsIsTwoRowsSortedByDataset) {
  Fixture f;
  const auto first = *f.toolbox.createDataSource("a");
  const auto second = *f.toolbox.createDataSource("b");
  (void)*f.toolbox.registerObjectTopic(second, "points", "{}");
  (void)*f.toolbox.registerObjectTopic(second, "image", "{}");
  (void)*f.toolbox.registerObjectTopic(first, "points", "{}");
  ASSERT_LT(first.id, second.id);

  auto snapshot = f.toolbox.catalogSnapshotV2();
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error();
  const auto topics = snapshot->objectTopics();
  ASSERT_EQ(topics.size(), 3u);
  EXPECT_EQ(topics[0].source.id, first.id);
  EXPECT_EQ(toStringView(topics[0].name), "points");
  EXPECT_EQ(topics[1].source.id, second.id);
  EXPECT_EQ(toStringView(topics[1].name), "image");
  EXPECT_EQ(topics[2].source.id, second.id);
  EXPECT_EQ(toStringView(topics[2].name), "points");
}

// builtin_object_type is a PJ::sdk::name() value or "": a type this SDK does not know,
// kNone, a non-string type and metadata that is not a JSON object all give "", and the
// metadata itself is passed through as it is.
TEST(PluginDataHostCatalogV2Test, AMergedAwayDatasetCarriesNoTopics) {
  Fixture f;
  const auto anchor = *f.toolbox.createDataSource("log_0.mcap");
  const auto consumed = *f.toolbox.createDataSource("log_1.mcap");
  ASSERT_TRUE(f.toolbox.ensureField(*f.toolbox.ensureTopic(anchor, "imu"), "x", PrimitiveType::kFloat64).has_value());
  ASSERT_TRUE(f.toolbox.ensureField(*f.toolbox.ensureTopic(consumed, "imu"), "x", PrimitiveType::kFloat64).has_value());

  ASSERT_TRUE(f.engine.mergeDatasets(anchor.id, {{consumed.id, 0}}).has_value());

  auto snapshot = f.toolbox.catalogSnapshot();
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error();
  ASSERT_EQ(snapshot->topics().size(), 1u);
  EXPECT_EQ(snapshot->topics()[0].source.id, anchor.id);
}

TEST(PluginDataHostCatalogV2Test, TypeIsACanonicalNameOrEmpty) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("s");
  const std::vector<std::string> metadata{
      R"({"builtin_object_type":"kBogus"})",
      R"({"builtin_object_type":"kNone"})",
      R"({"builtin_object_type":7})",
      "not json",
      R"({"frame":"base"})",
  };
  for (size_t index = 0; index < metadata.size(); ++index) {
    (void)*f.toolbox.registerObjectTopic(source, "t" + std::to_string(index), metadata[index]);
  }

  auto snapshot = f.toolbox.catalogSnapshotV2();
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error();
  ASSERT_EQ(snapshot->objectTopics().size(), metadata.size());
  for (size_t index = 0; index < metadata.size(); ++index) {
    const auto& info = snapshot->objectTopics()[index];
    EXPECT_EQ(toStringView(info.builtin_object_type), "") << metadata[index];
    EXPECT_EQ(toStringView(info.metadata_json), metadata[index]);
  }
}

// The host half of the caller-size rule, called through the raw slot: a caller struct
// below the v2 minimum is refused without a byte written to it, and so is a NULL one.
TEST(PluginDataHostCatalogV2Test, StructSizeBelowV2MinimumIsRejected) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("s");
  (void)*f.toolbox.registerObjectTopic(source, "cloud", "{}");
  const PJ_toolbox_host_t raw = f.toolbox_impl.raw();

  PJ_catalog_snapshot_v2_t out;
  std::memset(&out, 0xAB, sizeof(out));
  out.struct_size = PJ_CATALOG_SNAPSHOT_V2_MIN_SIZE - 1;
  PJ_catalog_snapshot_v2_t before;
  std::memcpy(&before, &out, sizeof(out));
  PJ_error_t error{};
  EXPECT_FALSE(raw.vtable->acquire_catalog_snapshot_v2(raw.ctx, &out, &error));
  EXPECT_NE(std::string(error.message).find("struct_size"), std::string::npos) << error.message;
  EXPECT_EQ(std::memcmp(&out, &before, sizeof(out)), 0) << "a refused caller struct is left as it was";

  PJ_error_t null_error{};
  EXPECT_FALSE(raw.vtable->acquire_catalog_snapshot_v2(raw.ctx, nullptr, &null_error));
  EXPECT_NE(std::string(null_error.message).find("null"), std::string::npos) << null_error.message;
}

// A newer client's larger struct is accepted: the host writes only its own sizeof,
// reports that size, and leaves the client's tail untouched.
TEST(PluginDataHostCatalogV2Test, LargerStructSizeIsAcceptedAndFilledSizeWrittenBack) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("s");
  (void)*f.toolbox.registerObjectTopic(source, "cloud", "{}");
  const PJ_toolbox_host_t raw = f.toolbox_impl.raw();

  struct Padded {
    PJ_catalog_snapshot_v2_t snapshot;
    unsigned char tail[16];
  };
  Padded buffer{};
  std::memset(buffer.tail, 0xAB, sizeof(buffer.tail));
  buffer.snapshot.struct_size = sizeof(Padded);
  PJ_error_t error{};
  ASSERT_TRUE(raw.vtable->acquire_catalog_snapshot_v2(raw.ctx, &buffer.snapshot, &error)) << error.message;
  EXPECT_EQ(buffer.snapshot.struct_size, sizeof(PJ_catalog_snapshot_v2_t));
  EXPECT_EQ(buffer.snapshot.reserved, 0u);
  EXPECT_EQ(buffer.snapshot.object_topic_count, 1u);
  for (const unsigned char byte : buffer.tail) {
    EXPECT_EQ(byte, 0xAB);
  }
  ASSERT_NE(buffer.snapshot.release, nullptr);
  buffer.snapshot.release(buffer.snapshot.release_ctx);
}

// PJ_BENCH=1 prints the snapshot's cost per object topic: 100, 10k and 100k topics on
// one dataset, one entry each, metadata the size of a derived cloud's; median of 21.
// The scalar half is empty, so this is the object half alone (the ObjectStore read
// under its shared lock, the metadata parse after it, the sort).
TEST(PluginDataHostCatalogV2Bench, SnapshotNsPerObjectTopic) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  for (const size_t topic_count : {size_t{100}, size_t{10'000}, size_t{100'000}}) {
    Fixture f;
    const auto source = *f.toolbox.createDataSource("s");
    for (size_t index = 0; index < topic_count; ++index) {
      const auto topic = *f.toolbox.registerObjectTopic(
          source, "derived/topic_" + std::to_string(index),
          R"({"builtin_object_type":"kPointCloud","pj_derived":"on_demand"})");
      ASSERT_TRUE(f.toolbox.pushOwnedObject(topic, static_cast<int64_t>(index), kPayload).has_value());
    }
    std::vector<double> ns_per_topic;
    for (int run = 0; run < 21; ++run) {
      const auto start = std::chrono::steady_clock::now();
      auto snapshot = f.toolbox.catalogSnapshotV2();
      const auto elapsed = std::chrono::steady_clock::now() - start;
      ASSERT_TRUE(snapshot.has_value()) << snapshot.error();
      ASSERT_EQ(snapshot->objectTopics().size(), topic_count);
      ns_per_topic.push_back(
          static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) /
          static_cast<double>(topic_count));
    }
    std::nth_element(ns_per_topic.begin(), ns_per_topic.begin() + 10, ns_per_topic.end());
    std::printf("catalog v2, %zu object topics: %.1f ns per topic (median of 21)\n", topic_count, ns_per_topic[10]);
  }
}

}  // namespace
}  // namespace PJ
