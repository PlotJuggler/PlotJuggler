// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "pj_runtime/ObjectIngestTap.h"

namespace {

using PJ::ObjectIngestTapRegistry;

TEST(ObjectIngestTapRegistry, ExclusiveClaimReleasesAndCanBeReclaimed) {
  ObjectIngestTapRegistry registry;
  {
    auto first = registry.claimExclusive(PJ::sdk::BuiltinObjectType::kFrameTransforms, [](auto...) {});
    ASSERT_TRUE(first.has_value()) << first.error();
    EXPECT_TRUE(registry.claimed(PJ::sdk::BuiltinObjectType::kFrameTransforms));

    auto second = registry.claimExclusive(PJ::sdk::BuiltinObjectType::kFrameTransforms, [](auto...) {});
    EXPECT_FALSE(second.has_value());
  }

  EXPECT_FALSE(registry.claimed(PJ::sdk::BuiltinObjectType::kFrameTransforms));
  auto reclaimed = registry.claimExclusive(PJ::sdk::BuiltinObjectType::kFrameTransforms, [](auto...) {});
  EXPECT_TRUE(reclaimed.has_value()) << reclaimed.error();
}

TEST(ObjectIngestTapRegistry, MovedLeaseReleasesOnlyFromDestination) {
  ObjectIngestTapRegistry registry;
  std::optional<ObjectIngestTapRegistry::TapLease> destination;
  {
    auto source = registry.claimExclusive(PJ::sdk::BuiltinObjectType::kFrameTransforms, [](auto...) {});
    ASSERT_TRUE(source.has_value()) << source.error();
    destination.emplace(std::move(*source));
  }

  EXPECT_TRUE(registry.claimed(PJ::sdk::BuiltinObjectType::kFrameTransforms))
      << "destroying the moved-from lease must not release the claim";
  destination.reset();
  EXPECT_FALSE(registry.claimed(PJ::sdk::BuiltinObjectType::kFrameTransforms));
}

TEST(ObjectIngestTapRegistry, InvokeUnclaimedKindIsNoOp) {
  ObjectIngestTapRegistry registry;
  EXPECT_FALSE(registry.invoke(
      PJ::sdk::BuiltinObjectType::kFrameTransforms, PJ::ObjectTopicId{12}, /*dataset_id=*/34, /*store_ts=*/56,
      PJ::sdk::makePayloadView(std::vector<uint8_t>{1, 2, 3})));
}

TEST(ObjectIngestTapRegistry, InvokeForwardsIdentityTimestampAndBytes) {
  ObjectIngestTapRegistry registry;
  int calls = 0;
  PJ::ObjectTopicId received_topic{};
  PJ::DatasetId received_dataset = 0;
  PJ::Timestamp received_stamp = 0;
  std::vector<uint8_t> received_bytes;
  auto lease = registry.claimExclusive(
      PJ::sdk::BuiltinObjectType::kFrameTransforms,
      [&](PJ::ObjectTopicId topic, PJ::DatasetId dataset, PJ::Timestamp stamp, PJ::sdk::PayloadView payload) {
        ++calls;
        received_topic = topic;
        received_dataset = dataset;
        received_stamp = stamp;
        received_bytes.assign(payload.bytes.begin(), payload.bytes.end());
      });
  ASSERT_TRUE(lease.has_value()) << lease.error();

  const std::vector<uint8_t> expected{0x00, 0x7F, 0x80, 0xFF};
  EXPECT_TRUE(registry.invoke(
      PJ::sdk::BuiltinObjectType::kFrameTransforms, PJ::ObjectTopicId{42}, /*dataset_id=*/7, /*store_ts=*/123456,
      PJ::sdk::makePayloadView(expected)));

  EXPECT_EQ(calls, 1);
  EXPECT_EQ(received_topic.id, 42U);
  EXPECT_EQ(received_dataset, 7U);
  EXPECT_EQ(received_stamp, 123456);
  EXPECT_EQ(received_bytes, expected);
}

}  // namespace
