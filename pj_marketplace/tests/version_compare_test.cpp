// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_marketplace/version_compare.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace PJ {
namespace {

TEST(PluginVersionOrder, UsesSdkSemverPrecedence) {
  const std::vector<std::string> chain = {
      "1.0.0-alpha",  "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
      "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1",       "1.0.0",
  };
  for (size_t lower = 0; lower + 1 < chain.size(); ++lower) {
    for (size_t higher = lower + 1; higher < chain.size(); ++higher) {
      EXPECT_LT(comparePluginVersions(chain[lower], chain[higher]), 0);
      EXPECT_GT(comparePluginVersions(chain[higher], chain[lower]), 0);
    }
  }
}

TEST(PluginVersionOrder, IgnoresBuildMetadataForPrecedence) {
  EXPECT_EQ(comparePluginVersions("1.0.0+build.7", "1.0.0"), 0);
  EXPECT_EQ(comparePluginVersions("1.0.0+a", "1.0.0+b"), 0);
  EXPECT_LT(comparePluginVersions("1.0.0-rc.1+build.7", "1.0.0"), 0);
}

TEST(PluginVersionOrder, HandlesArbitrarilyLargeNumericIdentifiers) {
  EXPECT_GT(comparePluginVersions("999999999999999999999.0.0", "4.0.0"), 0);
  EXPECT_LT(comparePluginVersions("4.0.0", "1000000000000000000000.0.0"), 0);
  EXPECT_LT(comparePluginVersions("1.0.0-rc.99999999999999999999", "1.0.0-rc.100000000000000000000"), 0);
}

TEST(PluginVersionOrder, ValidVersionOutranksInvalidRecoveryData) {
  for (const std::string invalid : {"", "4", "4.1", "4.01.0", "v4.1.0", "garbage"}) {
    EXPECT_FALSE(isPluginVersionValid(invalid));
    EXPECT_GT(comparePluginVersions("0.0.0", invalid), 0) << invalid;
    EXPECT_LT(comparePluginVersions(invalid, "0.0.0"), 0) << invalid;
  }
}

TEST(PluginVersionOrder, InvalidRecoveryDataHasDeterministicLexicalFallback) {
  EXPECT_EQ(comparePluginVersions("garbage", "garbage"), 0);
  EXPECT_LT(comparePluginVersions("garbage-a", "garbage-b"), 0);
  EXPECT_GT(comparePluginVersions("v2", "v1"), 0);
}

}  // namespace
}  // namespace PJ
