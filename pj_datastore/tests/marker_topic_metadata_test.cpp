// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/marker_topic_metadata.hpp"

#include <gtest/gtest.h>

#include <string>

#include "pj_datastore/object_store.hpp"

namespace PJ {
namespace {

constexpr DatasetId kDataset = 1;

ObjectTopicId registerTopic(ObjectStore& store, std::string name, std::string metadata_json) {
  const auto id = store.registerTopic(
      {.dataset_id = kDataset, .topic_name = std::move(name), .metadata_json = std::move(metadata_json)});
  EXPECT_TRUE(id.has_value()) << (id.has_value() ? std::string{} : id.error());
  return id.has_value() ? *id : ObjectTopicId{};
}

TEST(MarkerTopicMetadataTest, RoundTripsEveryScope) {
  for (const MarkerTopicInfo& info :
       {MarkerTopicInfo{MarkerScopeKind::kSeries, "/imu/x", "plug/rule"},
        MarkerTopicInfo{MarkerScopeKind::kSeries, "/imu/x"},
        MarkerTopicInfo{MarkerScopeKind::kDataset, "", "plug/rule"},
        MarkerTopicInfo{MarkerScopeKind::kAll, "", "plug/rule"}}) {
    const std::string json = markerTopicMetadataJson(info);
    EXPECT_NE(json.find(R"("builtin_object_type":"kPlotMarkers")"), std::string::npos) << json;
    EXPECT_NE(json.find(R"("pj_derived":"markers")"), std::string::npos) << json;
    const auto read = markerTopicInfo(json);
    ASSERT_TRUE(read.has_value()) << json;
    EXPECT_EQ(*read, info) << json;
  }
}

TEST(MarkerTopicMetadataTest, OnlyACompleteMarkerDescriptionIsReadBack) {
  EXPECT_FALSE(markerTopicInfo("").has_value());
  EXPECT_FALSE(markerTopicInfo("{}").has_value());
  EXPECT_FALSE(markerTopicInfo("not json pj_derived").has_value());
  EXPECT_FALSE(markerTopicInfo(R"({"builtin_object_type":"kImage"})").has_value());
  // Another kind of derived topic is not a marker topic: the key's presence is not enough.
  EXPECT_FALSE(markerTopicInfo(R"({"pj_derived":"on_demand","pj_marker_scope":"dataset"})").has_value());
  EXPECT_FALSE(markerTopicInfo(R"({"pj_derived":"markers"})").has_value()) << "no scope";
  EXPECT_FALSE(markerTopicInfo(R"({"pj_derived":"markers","pj_marker_scope":"galaxy"})").has_value());
  EXPECT_FALSE(markerTopicInfo(R"({"pj_derived":"markers","pj_marker_scope":"series"})").has_value()) << "no target";
  EXPECT_TRUE(markerTopicInfo(R"({"pj_derived":"markers","pj_marker_scope":"dataset"})").has_value());
}

// GAP-43: a plugin's direct write to a 0.35 marker name is described on registration,
// the one place the name is read.
TEST(MarkerTopicMetadataTest, LegacyMarkerNameIsStampedAtRegistration) {
  ObjectStore store;
  const ObjectTopicId series = registerTopic(store, "__markers__//imu/x", "{}");
  const ObjectTopicId dataset_scope = registerTopic(store, "__markers__/__global__", "");

  const auto series_info = markerTopicInfo(store.descriptor(series).metadata_json);
  ASSERT_TRUE(series_info.has_value()) << store.descriptor(series).metadata_json;
  EXPECT_EQ(*series_info, (MarkerTopicInfo{MarkerScopeKind::kSeries, "/imu/x"}));

  const auto dataset_info = markerTopicInfo(store.descriptor(dataset_scope).metadata_json);
  ASSERT_TRUE(dataset_info.has_value());
  EXPECT_EQ(*dataset_info, (MarkerTopicInfo{MarkerScopeKind::kDataset}));
}

TEST(MarkerTopicMetadataTest, LegacyStampKeepsTheWritersOwnFields) {
  ObjectStore store;
  const ObjectTopicId topic = registerTopic(store, "__markers__/a", R"({"object_type":"plot_markers"})");
  const std::string stamped = store.descriptor(topic).metadata_json;
  EXPECT_NE(stamped.find(R"("object_type":"plot_markers")"), std::string::npos) << stamped;
  EXPECT_TRUE(markerTopicInfo(stamped).has_value()) << stamped;
}

// The writer's key is arbitrary bytes but the stamp is JSON: a key that is not UTF-8
// registers unstamped, like any opaque topic, instead of throwing out of registration.
TEST(MarkerTopicMetadataTest, LegacyMarkerNameThatIsNotUtf8RegistersUnstamped) {
  ObjectStore store;
  ObjectTopicId topic;
  EXPECT_NO_THROW(topic = registerTopic(store, "__markers__/caf\xE9", "{}"));
  EXPECT_EQ(store.descriptor(topic).metadata_json, "{}");
}

TEST(MarkerTopicMetadataTest, StampingIsStableAcrossFindOrRegister) {
  ObjectStore store;
  const ObjectTopicId first = registerTopic(store, "__markers__/a", "{}");
  const auto again =
      store.findOrRegisterTopic({.dataset_id = kDataset, .topic_name = "__markers__/a", .metadata_json = "{}"});
  ASSERT_TRUE(again.has_value()) << again.error();
  EXPECT_EQ(*again, first);
}

// A name that looks like a marker topic but whose metadata says otherwise is left alone.
TEST(MarkerTopicMetadataTest, TopicsThatSayWhatTheyAreAreNotStamped) {
  ObjectStore store;
  const std::string image = R"({"builtin_object_type":"kImage"})";
  const std::string on_demand = R"({"pj_derived":"on_demand"})";
  const std::string other = R"({"k":1})";
  EXPECT_EQ(store.descriptor(registerTopic(store, "__markers__/cam", image)).metadata_json, image);
  EXPECT_EQ(store.descriptor(registerTopic(store, "__markers__/derived", on_demand)).metadata_json, on_demand);
  // Outside the marker namespace nothing is stamped, whatever the name resembles.
  EXPECT_EQ(store.descriptor(registerTopic(store, "/markers/x", other)).metadata_json, other);
  EXPECT_EQ(store.descriptor(registerTopic(store, "markers__/x", "{}")).metadata_json, "{}");
}

TEST(MarkerTopicMetadataTest, DescribedMarkerTopicIsKeptVerbatim) {
  ObjectStore store;
  const std::string json = markerTopicMetadataJson({MarkerScopeKind::kSeries, "k", "g"});
  EXPECT_EQ(store.descriptor(registerTopic(store, "__markers__/k#g", json)).metadata_json, json);
}

}  // namespace
}  // namespace PJ
