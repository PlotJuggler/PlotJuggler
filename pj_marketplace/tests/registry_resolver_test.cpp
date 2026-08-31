// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_marketplace/registry_resolver.hpp"

#include <gtest/gtest.h>

#include <utility>

namespace PJ {
namespace {

Extension candidate(
    QString id, QString version, QString platform = "linux-x86_64", QString app_floor = {}, QString sdk_floor = {}) {
  Extension extension;
  extension.name = id;
  extension.id = std::move(id);
  extension.version = std::move(version);
  extension.min_sdk_required = std::move(sdk_floor);
  extension.min_plotjuggler_version = std::move(app_floor);
  if (!platform.isEmpty()) {
    extension.platforms.insert(std::move(platform), {"https://example.invalid/plugin.zip", "sha256:test"});
  }
  return extension;
}

const RegistryResolutionContext kHost{"linux-x86_64", "0.21.0", "4.0.0"};

TEST(RegistryResolver, SelectsHighestCompatibleReleaseForPlatform) {
  const QList<Extension> candidates = {
      candidate("plugin", "4.0.0", "linux-x86_64", "5.0.0"),
      candidate("plugin", "3.0.0", "linux-x86_64", {}, "99.0.0"),
      candidate("plugin", "2.0.0", "linux-x86_64", "4.0.0", "0.21.0"),
      candidate("plugin", "9.0.0", "windows-x86_64"),
  };

  const auto resolved = resolveRegistryCandidates(candidates, kHost);

  ASSERT_TRUE(resolved) << resolved.error().toStdString();
  ASSERT_EQ(resolved->size(), 1);
  EXPECT_EQ(resolved->front().version, "2.0.0");
}

TEST(RegistryResolver, KeepsHighestPlatformReleaseWhenAllAreIncompatible) {
  const QList<Extension> candidates = {
      candidate("plugin", "2.0.0", "linux-x86_64", "5.0.0"),
      candidate("plugin", "3.0.0", "linux-x86_64", "6.0.0"),
  };

  const auto resolved = resolveRegistryCandidates(candidates, kHost);

  ASSERT_TRUE(resolved) << resolved.error().toStdString();
  ASSERT_EQ(resolved->size(), 1);
  EXPECT_EQ(resolved->front().version, "3.0.0");
}

TEST(RegistryResolver, KeepsHighestOverallWhenNoPlatformReleaseExists) {
  const QList<Extension> candidates = {
      candidate("plugin", "2.0.0", "windows-x86_64"),
      candidate("plugin", "3.0.0", "macos-arm64", "99.0.0"),
  };

  const auto resolved = resolveRegistryCandidates(candidates, kHost);

  ASSERT_TRUE(resolved) << resolved.error().toStdString();
  ASSERT_EQ(resolved->size(), 1);
  EXPECT_EQ(resolved->front().version, "3.0.0");
}

TEST(RegistryResolver, RejectsInvalidCandidateEvenWhenItCouldNotWin) {
  const QList<Extension> candidates = {
      candidate("plugin", "2.0.0"),
      candidate("plugin", "4.1", "windows-x86_64"),
  };

  const auto resolved = resolveRegistryCandidates(candidates, kHost);

  ASSERT_FALSE(resolved);
  EXPECT_TRUE(resolved.error().contains("invalid", Qt::CaseInsensitive));
  EXPECT_TRUE(resolved.error().contains("4.1"));
}

TEST(RegistryResolver, RejectsVersionSlotReuseAcrossBuildMetadata) {
  const QList<Extension> candidates = {
      candidate("plugin", "1.0.0+first", "linux-x86_64"),
      candidate("plugin", "1.0.0+replacement", "windows-x86_64"),
  };

  const auto resolved = resolveRegistryCandidates(candidates, kHost);

  ASSERT_FALSE(resolved);
  EXPECT_TRUE(resolved.error().contains("reuses", Qt::CaseInsensitive));
}

TEST(RegistryResolver, PreservesFirstIdOrderAfterGrouping) {
  const QList<Extension> candidates = {
      candidate("zeta", "1.0.0"),
      candidate("alpha", "1.0.0"),
      candidate("zeta", "2.0.0"),
  };

  const auto resolved = resolveRegistryCandidates(candidates, kHost);

  ASSERT_TRUE(resolved) << resolved.error().toStdString();
  ASSERT_EQ(resolved->size(), 2);
  EXPECT_EQ(resolved->at(0).id, "zeta");
  EXPECT_EQ(resolved->at(0).version, "2.0.0");
  EXPECT_EQ(resolved->at(1).id, "alpha");
}

}  // namespace
}  // namespace PJ
