// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Marker topics are recognized and routed by their metadata: a name that merely
// looks like a marker topic is not one, and a marker topic needs no particular name.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "pj_base/builtin/plot_markers.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/MarkerTopics.h"

namespace {

constexpr PJ::DatasetId kDataset = 1;
const std::string kImageMetadata = R"({"builtin_object_type":"kImage"})";

PJ::ObjectTopicId add(
    PJ::ObjectStore& store, PJ::DatasetId dataset, const std::string& name, const std::string& metadata_json) {
  const auto id = store.registerTopic({.dataset_id = dataset, .topic_name = name, .metadata_json = metadata_json});
  EXPECT_TRUE(id.has_value()) << (id.has_value() ? std::string{} : id.error());
  return id.has_value() ? *id : PJ::ObjectTopicId{};
}

// A topic named like no marker topic, described as one.
PJ::ObjectTopicId addMarkers(
    PJ::ObjectStore& store, PJ::DatasetId dataset, const std::string& name, PJ::MarkerTopicInfo info) {
  return add(store, dataset, name, PJ::markerTopicMetadataJson(info));
}

TEST(MarkerTopicsTest, TheMetadataDecidesWhetherATopicIsAMarkerTopic) {
  PJ::ObjectStore store;
  const auto opaque = addMarkers(store, kDataset, "opaque-id-7", PJ::markerInfoForKey("imu/x"));
  const auto lookalike = add(store, kDataset, "__markers__/imu/x", kImageMetadata);
  const auto on_demand = add(store, kDataset, "__markers__/derived", R"({"pj_derived":"on_demand"})");
  const auto legacy = add(store, kDataset, "__markers__/imu/y", "{}");  // stamped on registration
  const auto media = add(store, kDataset, "/camera/image", kImageMetadata);

  EXPECT_TRUE(PJ::isMarkerTopic(store.descriptor(opaque)));
  EXPECT_FALSE(PJ::isMarkerTopic(store.descriptor(lookalike)));
  EXPECT_FALSE(PJ::isMarkerTopic(store.descriptor(on_demand)));
  EXPECT_TRUE(PJ::isMarkerTopic(store.descriptor(legacy)));
  EXPECT_FALSE(PJ::isMarkerTopic(store.descriptor(media)));
}

TEST(MarkerTopicsTest, MarkerInfoForKeyReadsTheReservedScopeKeys) {
  EXPECT_EQ(PJ::markerInfoForKey(PJ::sdk::kGlobalMarkerTopic).scope, PJ::MarkerScopeKind::kDataset);
  EXPECT_EQ(PJ::markerInfoForKey(PJ::kAllDatasetsMarkerTopic).scope, PJ::MarkerScopeKind::kAll);
  const PJ::MarkerTopicInfo series = PJ::markerInfoForKey("/imu/x");
  EXPECT_EQ(series.scope, PJ::MarkerScopeKind::kSeries);
  EXPECT_EQ(series.target, "/imu/x");
}

TEST(MarkerTopicsTest, MarkerInfoForScopeIsWhatTheScopesKeyMeans) {
  for (const PJ::MarkerScope scope : {PJ::MarkerScope::kDataset, PJ::MarkerScope::kAllDatasets}) {
    EXPECT_EQ(PJ::markerInfoForScope(scope), PJ::markerInfoForKey(PJ::markerScopeTopic(scope)));
  }
}

// The family of a series: its direct write first, then each generator's topic, found
// through the metadata whatever the topics are called, and on that dataset only.
TEST(MarkerTopicsTest, TopicsOfAFamilyAreFoundByScopeAndTarget) {
  PJ::ObjectStore store;
  PJ::MarkerTopicInfo owned = PJ::markerInfoForKey("imu/x");
  owned.owner = "plug/a";
  const auto first_owned = addMarkers(store, kDataset, "opaque-1", owned);
  owned.owner = "plug/b";
  const auto second_owned = addMarkers(store, kDataset, PJ::markerOwnerTopicName("imu/x", "plug/b"), owned);
  const auto bare = add(store, kDataset, "__markers__/imu/x", "{}");  // registered last, drawn first

  addMarkers(store, kDataset, "other-series", PJ::markerInfoForKey("imu/y"));
  addMarkers(store, kDataset, "dataset-scope", PJ::markerInfoForKey(PJ::sdk::kGlobalMarkerTopic));
  addMarkers(store, 2, "other-dataset", PJ::markerInfoForKey("imu/x"));
  add(store, kDataset, "__markers__/imu/x#ghost", kImageMetadata);

  const std::vector<PJ::ObjectTopicId> family =
      PJ::markerTopicsOf(PJ::scanMarkerTopics(store, kDataset), PJ::markerInfoForKey("imu/x"));
  ASSERT_EQ(family.size(), 3U);
  EXPECT_EQ(family[0], bare);
  EXPECT_EQ(family[1], first_owned);
  EXPECT_EQ(family[2], second_owned);
}

TEST(MarkerTopicsTest, PublishedScopesFollowTheMetadataOfEachScopesHome) {
  PJ::ObjectStore store;
  const std::vector<PJ::DatasetId> datasets{kDataset};
  EXPECT_TRUE(PJ::publishedMarkerScopes(store, datasets).empty());

  add(store, kDataset, "__markers__/__global__", kImageMetadata);  // not a marker topic
  EXPECT_TRUE(PJ::publishedMarkerScopes(store, datasets).empty());

  addMarkers(store, kDataset, "dataset-scope", PJ::markerInfoForKey(PJ::sdk::kGlobalMarkerTopic));
  EXPECT_EQ(
      PJ::publishedMarkerScopes(store, datasets),
      (std::vector<PJ::MarkerScopeKey>{{kDataset, PJ::MarkerScope::kDataset}}));

  addMarkers(store, PJ::kAllDatasetsMarkerDataset, "all-scope", PJ::markerInfoForKey(PJ::kAllDatasetsMarkerTopic));
  EXPECT_EQ(
      PJ::publishedMarkerScopes(store, datasets),
      (std::vector<PJ::MarkerScopeKey>{
          {kDataset, PJ::MarkerScope::kDataset}, {kDataset, PJ::MarkerScope::kAllDatasets}}));
}

// The shared home answers for every dataset; the dataset scope only for its own.
TEST(MarkerTopicsTest, PublishedScopesListEachDatasetInTurn) {
  PJ::ObjectStore store;
  addMarkers(store, 2, "dataset-scope", PJ::markerInfoForKey(PJ::sdk::kGlobalMarkerTopic));
  addMarkers(store, PJ::kAllDatasetsMarkerDataset, "all-scope", PJ::markerInfoForKey(PJ::kAllDatasetsMarkerTopic));

  const std::vector<PJ::DatasetId> datasets{kDataset, 2};
  const std::vector<PJ::MarkerScopeKey> expected{
      {kDataset, PJ::MarkerScope::kAllDatasets}, {2, PJ::MarkerScope::kDataset}, {2, PJ::MarkerScope::kAllDatasets}};
  EXPECT_EQ(PJ::publishedMarkerScopes(store, datasets), expected);
}

}  // namespace
