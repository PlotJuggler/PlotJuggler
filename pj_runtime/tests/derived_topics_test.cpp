// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A derived object topic is recognized by its metadata, never by its name, and is
// registered with the same bytes every time (the refill contract keys off them).

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/sdk/object_topic_metadata.hpp"
#include "pj_datastore/marker_topic_metadata.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/DerivedTopics.h"

namespace {

using PJ::sdk::BuiltinObjectType;

nlohmann::json parsed(BuiltinObjectType type) {
  return nlohmann::json::parse(PJ::derivedTopicMetadata(type));
}

bool isDerived(const std::string& name, const std::string& metadata_json) {
  return PJ::isDerivedObjectTopic({.dataset_id = 1, .topic_name = name, .metadata_json = metadata_json});
}

TEST(DerivedTopicsTest, ADerivedTopicIsRegisteredAsOnDemandOfItsType) {
  for (const BuiltinObjectType type :
       {BuiltinObjectType::kPointCloud, BuiltinObjectType::kSceneEntities, BuiltinObjectType::kImageAnnotations}) {
    const nlohmann::json metadata = parsed(type);
    EXPECT_EQ(metadata.value("pj_derived", ""), "on_demand");
    EXPECT_EQ(metadata.value("builtin_object_type", ""), std::string(PJ::sdk::name(type)));
  }
}

// D27: a live SceneEntities / ImageAnnotations node publishes a complete snapshot on
// every evaluation, so a stateless consumer renders each entry alone. A point cloud
// is not a field-table stream and stays unmarked.
TEST(DerivedTopicsTest, OnlyTheSnapshotTypesAreMarkedAsSnapshotStreams) {
  EXPECT_EQ(parsed(BuiltinObjectType::kSceneEntities).value("pj_snapshot", ""), "true");
  EXPECT_EQ(parsed(BuiltinObjectType::kImageAnnotations).value("pj_snapshot", ""), "true");
  EXPECT_FALSE(parsed(BuiltinObjectType::kPointCloud).contains("pj_snapshot"));
}

// findOrRegisterTopic reuses a topic only for byte-identical metadata, so a re-run of
// the same recipe must find its topic again.
// MED-05: a derived topic has no parser, so the tag is what makes Scene2D's image layer unwrap each serialized
// sdk::Image record instead of handing it whole to the JPEG decoder (a black view).
TEST(DerivedTopicsTest, DerivedImageTopicsDeclareTheCanonicalImageCodec) {
  EXPECT_EQ(parsed(BuiltinObjectType::kImage).value("image_codec", ""), "pj_image_v1");
  EXPECT_FALSE(parsed(BuiltinObjectType::kPointCloud).contains("image_codec"));
  EXPECT_FALSE(parsed(BuiltinObjectType::kDepthImage).contains("image_codec"));
}

TEST(DerivedTopicsTest, TheMetadataIsByteStableSoARefillFindsItsTopicAgain) {
  EXPECT_EQ(
      PJ::derivedTopicMetadata(BuiltinObjectType::kSceneEntities),
      PJ::derivedTopicMetadata(BuiltinObjectType::kSceneEntities));
  PJ::ObjectStore store;
  const PJ::ObjectTopicDescriptor descriptor{
      .dataset_id = 1,
      .topic_name = "derived/scene",
      .metadata_json = PJ::derivedTopicMetadata(BuiltinObjectType::kSceneEntities)};
  const auto first = store.findOrRegisterTopic(descriptor);
  const auto second = store.findOrRegisterTopic(descriptor);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*first, *second);
  EXPECT_TRUE(PJ::isDerivedObjectTopic(store.descriptor(*first)));
}

TEST(DerivedTopicsTest, TheMetadataDecidesWhetherATopicIsDerived) {
  EXPECT_TRUE(isDerived("opaque-id-7", PJ::derivedTopicMetadata(BuiltinObjectType::kPointCloud)));
  EXPECT_TRUE(isDerived("any", R"({"pj_derived":"on_demand"})"));

  EXPECT_FALSE(isDerived("pj_derived/looks_derived", "{}")) << "a name proves nothing";
  EXPECT_FALSE(isDerived("any", ""));
  EXPECT_FALSE(isDerived("any", "{}"));
  EXPECT_FALSE(isDerived("any", "not json pj_derived on_demand"));
  EXPECT_FALSE(isDerived("any", R"({"pj_derived":"markers"})")) << "another kind of derived topic";
  EXPECT_FALSE(isDerived("any", R"({"pj_derived":true})"));
  EXPECT_FALSE(isDerived("any", R"({"note":"pj_derived is on_demand"})"));
  EXPECT_FALSE(isDerived("any", PJ::markerTopicMetadataJson(PJ::markerInfoForKey("imu/x"))));
}

}  // namespace
