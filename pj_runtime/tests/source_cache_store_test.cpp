// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// SourceCacheStore behavior, keyed to the #275 harvest rules the host cache
// must reproduce: pin-then-validate lookups whose contended case is a miss
// with a retry hint, eviction that never touches a pinned artifact and
// reports over-target instead of forcing, quarantine that turns a failing
// entry into a plain miss, and publish recovery that never hands back an
// unpinned path.

#include <gtest/gtest.h>

#include <QSettings>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>

#include "pj_runtime/SourceCacheStore.h"
#include "recording_test_utils.h"
using namespace Qt::StringLiterals;

namespace fs = std::filesystem;
using PJ::SourceCacheStore;

namespace {

constexpr std::array<unsigned char, 8> kMagic = {0x89, 'M', 'C', 'A', 'P', '0', '\r', '\n'};

void writeU64Le(std::ofstream& out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out.put(static_cast<char>((value >> (8 * i)) & 0xff));
  }
}

// A minimal file the store's validator accepts: magic, padding, a footer
// record (opcode 0x02, length 20, summary_start 0), magic.
void writeFakeMcap(const fs::path& file, std::size_t padding_bytes = 64) {
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(kMagic.data()), kMagic.size());
  const std::string pad(padding_bytes, 'x');
  out.write(pad.data(), static_cast<std::streamsize>(pad.size()));
  out.put('\x02');
  writeU64Le(out, 20);
  for (int i = 0; i < 20; ++i) {
    out.put('\0');  // summary_start = 0 (none), offsets, crc
  }
  out.write(reinterpret_cast<const char*>(kMagic.data()), kMagic.size());
}

// Magic at both ends around junk — the probes C-2 requires rejected.
void writeMagicSandwich(const fs::path& file, std::size_t junk_bytes) {
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(kMagic.data()), kMagic.size());
  const std::string junk(junk_bytes, 'j');
  out.write(junk.data(), static_cast<std::streamsize>(junk.size()));
  out.write(reinterpret_cast<const char*>(kMagic.data()), kMagic.size());
}

class SourceCacheStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // A per-process random token instead of getpid(): keeps parallel test
    // processes apart without POSIX-only calls (MSVC rejects ::getpid).
    static const unsigned process_token = std::random_device{}();
    root_ = fs::temp_directory_path() / ("source_cache_store_test-" + std::to_string(process_token) + "-" +
                                         ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(root_);
  }
  void TearDown() override {
    fs::remove_all(root_);
  }

  // Publish a validator-approved artifact for `identity` and drop the pin.
  fs::path publishArtifact(SourceCacheStore& store, const std::string& identity, std::size_t padding = 64) {
    auto txn = store.beginPublish(identity);
    EXPECT_TRUE(txn.has_value()) << txn.error().message;
    writeFakeMcap(txn->partialPath(), padding);
    auto pinned = store.publish(identity, std::move(*txn));
    EXPECT_TRUE(pinned.has_value()) << pinned.error().message;
    return pinned->path;  // pin released here — Pinned is discarded
  }

  fs::path root_;
};

TEST_F(SourceCacheStoreTest, AbsentIdentityIsAMissWithAReason) {
  SourceCacheStore store(root_);
  std::string reason;
  EXPECT_FALSE(store.lookup("mosaico:v1:sha256/128:00ff", &reason).has_value());
  EXPECT_FALSE(reason.empty());
}

TEST_F(SourceCacheStoreTest, PublishRoundTripsToAPinnedHit) {
  SourceCacheStore store(root_);
  const std::string identity = "mosaico:v1:sha256/128:00ff";
  const fs::path published = publishArtifact(store, identity);
  EXPECT_TRUE(fs::exists(published));

  auto hit = store.lookup(identity);
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(hit->path, published);
  EXPECT_TRUE(hit->pin.held());
  // Opaque provider identities must not collide across providers.
  EXPECT_FALSE(store.lookup("mcapcloud:v1:sha256/128:00ff").has_value());
}

TEST_F(SourceCacheStoreTest, MagicAloneDoesNotPassValidation) {
  SourceCacheStore store(root_);
  const std::string identity = "id-probe";
  const fs::path artifact = store.pathFor(identity);
  fs::create_directories(artifact.parent_path());

  {  // 8 bytes: a lone magic (head and tail reads overlap)
    std::ofstream out(artifact, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(kMagic.data()), kMagic.size());
  }
  EXPECT_FALSE(store.lookup(identity).has_value());

  writeMagicSandwich(artifact, 0);  // 16 bytes: magic+magic, no footer frame
  EXPECT_FALSE(store.lookup(identity).has_value());

  writeMagicSandwich(artifact, 64);  // magic + junk + magic
  EXPECT_FALSE(store.lookup(identity).has_value());

  writeFakeMcap(artifact);  // a real minimal artifact passes
  EXPECT_TRUE(store.lookup(identity).has_value());
}

TEST_F(SourceCacheStoreTest, CorruptFooterIsAMissAndTheFileIsKept) {
  SourceCacheStore store(root_);
  const std::string identity = "id-corrupt";
  const fs::path artifact = store.pathFor(identity);
  fs::create_directories(artifact.parent_path());
  std::ofstream(artifact, std::ios::binary) << "not an mcap at all";

  std::string reason;
  EXPECT_FALSE(store.lookup(identity, &reason).has_value());
  EXPECT_NE(reason.find("MCAP"), std::string::npos) << reason;
  EXPECT_TRUE(fs::exists(artifact)) << "a rejected artifact is left for the next publish to rename over";
}

// Harvest rule 2: a contended identity is a MISS with a retry hint — never an
// error, never an unpinned hit. MissKind classifies on EVIDENCE of an active
// publisher (a fresh partial), not on the hint.
TEST_F(SourceCacheStoreTest, ContendedLookupIsAMissWithARetryHint) {
  SourceCacheStore store(root_);
  const std::string identity = "id-contended";
  publishArtifact(store, identity);

  auto txn = store.beginPublish(identity);  // exclusive lock held
  ASSERT_TRUE(txn.has_value());
  std::ofstream(txn->partialPath(), std::ios::binary) << "in-flight bytes";  // the active publisher's evidence

  SourceCacheStore other(root_);
  std::string reason;
  SourceCacheStore::MissKind kind = SourceCacheStore::MissKind::kAbsent;
  EXPECT_FALSE(other.lookup(identity, &reason, &kind).has_value());
  EXPECT_NE(reason.find("retry"), std::string::npos) << reason;
  EXPECT_EQ(kind, SourceCacheStore::MissKind::kContended) << "a mid-publish identity must report a CONTENDED miss";
  txn->abort();

  EXPECT_TRUE(other.lookup(identity).has_value()) << "released lock makes the next lookup a hit";

  // Every other miss is kAbsent: the provider fallback is the right answer.
  EXPECT_FALSE(other.lookup("id-not-there", &reason, &kind).has_value());
  EXPECT_EQ(kind, SourceCacheStore::MissKind::kAbsent);
}

// F1: contention during a FIRST publication — no artifact exists yet, so the
// SDK reports a plain "no artifact" miss without any retry hint. The fresh
// partial is the evidence that must classify it kContended.
TEST_F(SourceCacheStoreTest, FirstPublicationContentionIsDetectedByItsFreshPartial) {
  SourceCacheStore store(root_);
  const std::string identity = "id-first-publish";
  auto txn = store.beginPublish(identity);
  ASSERT_TRUE(txn.has_value());
  std::ofstream(txn->partialPath(), std::ios::binary) << "first download in flight";

  SourceCacheStore other(root_);
  std::string reason;
  SourceCacheStore::MissKind kind = SourceCacheStore::MissKind::kAbsent;
  EXPECT_FALSE(other.lookup(identity, &reason, &kind).has_value());
  EXPECT_EQ(kind, SourceCacheStore::MissKind::kContended)
      << "a fresh partial with no artifact yet IS an active first publication";
  txn->abort();
}

// F2: a held/unopenable lock WITHOUT a fresh partial is NOT contention — the
// SDK decorates every shared-lock failure with the retry hint (a read-only
// lock file included), and refusing there would tell the user to wait
// forever. kAbsent keeps the safe provider fallback.
TEST_F(SourceCacheStoreTest, LockFailureWithoutAPartialIsAnAbsentMiss) {
  SourceCacheStore store(root_);
  const std::string identity = "id-locked-no-partial";
  publishArtifact(store, identity);

  auto txn = store.beginPublish(identity);  // lock held, but no partial written
  ASSERT_TRUE(txn.has_value());
  SourceCacheStore other(root_);
  std::string reason;
  SourceCacheStore::MissKind kind = SourceCacheStore::MissKind::kContended;
  EXPECT_FALSE(other.lookup(identity, &reason, &kind).has_value());
  EXPECT_NE(reason.find("retry"), std::string::npos) << reason;
  EXPECT_EQ(kind, SourceCacheStore::MissKind::kAbsent) << "a retry hint alone must not classify as contended";
  txn->abort();

  // The unreadable-lock-file variant (no publisher running at all).
  fs::path lock = store.pathFor(identity);
  lock += ".lock";
  ASSERT_TRUE(fs::exists(lock));
  fs::permissions(lock, fs::perms::none);
  EXPECT_FALSE(other.lookup(identity, &reason, &kind).has_value());
  EXPECT_EQ(kind, SourceCacheStore::MissKind::kAbsent) << reason;
  fs::permissions(lock, fs::perms::owner_read | fs::perms::owner_write);
}

// F1/F2: a crashed writer's STALE partial (mtime beyond the freshness
// window) must not become permanent false contention.
TEST_F(SourceCacheStoreTest, StalePartialDoesNotClassifyAsContended) {
  SourceCacheStore store(root_);
  const std::string identity = "id-stale-partial";
  auto txn = store.beginPublish(identity);
  ASSERT_TRUE(txn.has_value());
  std::ofstream(txn->partialPath(), std::ios::binary) << "crashed writer's leftovers";
  fs::last_write_time(txn->partialPath(), fs::file_time_type::clock::now() - std::chrono::minutes(5));

  SourceCacheStore other(root_);
  std::string reason;
  SourceCacheStore::MissKind kind = SourceCacheStore::MissKind::kContended;
  EXPECT_FALSE(other.lookup(identity, &reason, &kind).has_value());
  EXPECT_EQ(kind, SourceCacheStore::MissKind::kAbsent) << "an old partial is a crash leftover, not contention";
  txn->abort();
}

// Harvest rule 1: a pinned artifact survives eviction even when it is the
// oldest; rule 9: over-target is reported, never forced.
TEST_F(SourceCacheStoreTest, EvictionSkipsPinnedAndReportsOverTarget) {
  SourceCacheStore store(root_, /*budget_bytes=*/1);  // everything is over budget
  const std::string pinned_id = "id-pinned";
  const std::string loose_id = "id-loose";
  publishArtifact(store, pinned_id, 4096);
  // Ensure distinct LRU stamps so the pinned artifact is the OLDER one.
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  const fs::path loose_path = publishArtifact(store, loose_id, 4096);

  auto pin = store.lookup(pinned_id);
  ASSERT_TRUE(pin.has_value());

  const auto result = store.cleanup();
  EXPECT_FALSE(fs::exists(loose_path)) << "unpinned artifact over budget must be evicted";
  EXPECT_TRUE(fs::exists(pin->path)) << "pinned artifact must never be evicted";
  EXPECT_FALSE(result.target_met);
  EXPECT_GT(result.bytes_held_over_target, 0u);

  pin.reset();
  const auto after = store.cleanup();
  EXPECT_TRUE(after.target_met);
  EXPECT_FALSE(fs::exists(store.pathFor(pinned_id)));
}

// Harvest rule 4: quarantine evicts the entry (and its LRU stamp) so the
// next lookup is a plain miss; republish heals it.
TEST_F(SourceCacheStoreTest, QuarantineMakesTheNextLookupAMissAndRepublishHeals) {
  SourceCacheStore store(root_);
  const std::string identity = "id-heal";
  const fs::path artifact = publishArtifact(store, identity);

  ASSERT_TRUE(store.quarantine(identity));
  EXPECT_FALSE(fs::exists(artifact));
  fs::path touch = artifact;
  touch += ".touch";
  EXPECT_FALSE(fs::exists(touch));

  std::string reason;
  EXPECT_FALSE(store.lookup(identity, &reason).has_value());

  publishArtifact(store, identity);
  EXPECT_TRUE(store.lookup(identity).has_value());
}

TEST_F(SourceCacheStoreTest, QuarantineOfAPinnedArtifactIsRefusedWithAReason) {
  SourceCacheStore store(root_);
  const std::string identity = "id-pinned-quarantine";
  publishArtifact(store, identity);
  auto pin = store.lookup(identity);
  ASSERT_TRUE(pin.has_value());

  SourceCacheStore other(root_);
  std::string reason;
  EXPECT_FALSE(other.quarantine(identity, &reason));
  EXPECT_FALSE(reason.empty());
  EXPECT_TRUE(fs::exists(pin->path));
}

TEST_F(SourceCacheStoreTest, QuarantineOfAnAbsentArtifactIsIdempotent) {
  SourceCacheStore store(root_);
  EXPECT_TRUE(store.quarantine("id-never-published"));
}

// publish()'s retryable-commit branch is exactly "one lookup"; the race that
// triggers it needs an SDK seam to force deterministically, so the recovery
// STEP is pinned here on the state that branch sees: artifact published,
// caller unpinned.
TEST_F(SourceCacheStoreTest, RecoveryLookupAfterAPublishedArtifactReturnsAHeldPin) {
  SourceCacheStore store(root_);
  const std::string identity = "id-recovery";
  publishArtifact(store, identity);  // published, no pin held — the post-retryable state

  auto pinned = store.lookup(identity);
  ASSERT_TRUE(pinned.has_value());
  EXPECT_TRUE(pinned->pin.held());
  EXPECT_TRUE(fs::exists(pinned->path));
}

TEST_F(SourceCacheStoreTest, EmptyIdentityIsRejectedEverywhere) {
  SourceCacheStore store(root_);
  std::string reason;
  EXPECT_FALSE(store.lookup("", &reason).has_value());
  EXPECT_TRUE(store.pathFor("").empty());
  EXPECT_FALSE(store.beginPublish("").has_value());
  EXPECT_FALSE(store.quarantine("", &reason));
}

// A hostile identity cannot escape the cache root: the host digests the
// identity string, so the filename is always <hex>.mcap directly under root.
TEST_F(SourceCacheStoreTest, HostileIdentityStaysInsideTheRoot) {
  SourceCacheStore store(root_);
  const fs::path path = store.pathFor("../../etc/passwd");
  ASSERT_FALSE(path.empty());
  EXPECT_EQ(path.parent_path(), root_);
}

// T8 (acceptance row 9): the Preferences settings — capture_enabled included —
// round-trip through QSettings, and hand-edited values are clamped on load.
class SourceCacheStoreSettingsTest : public ::testing::Test {
 protected:
  PJ::test::IsolatedQtSettings settings_{u"PlotJugglerSourceCacheStoreTest"_s, u"SourceCacheStoreTest"_s};
};

TEST_F(SourceCacheStoreSettingsTest, SettingsRoundTripThroughQSettings) {
  SourceCacheStore::Settings settings;
  settings.directory = u"/tmp/some-cache"_s;
  settings.budget_gb = 42;
  settings.capture_enabled = false;
  SourceCacheStore::saveSettings(settings);

  const auto loaded = SourceCacheStore::loadSettings();
  EXPECT_EQ(loaded.directory, settings.directory);
  EXPECT_EQ(loaded.budget_gb, 42);
  EXPECT_FALSE(loaded.capture_enabled);

  QSettings raw;
  EXPECT_EQ(raw.value(u"Preferences::source_cache_directory"_s).toString(), settings.directory);
  EXPECT_EQ(raw.value(u"Preferences::source_cache_budget_gb"_s).toInt(), 42);
  EXPECT_FALSE(raw.value(u"Preferences::source_cache_capture_enabled"_s).toBool());
}

TEST_F(SourceCacheStoreSettingsTest, LoadedSettingsAreClampedAndDefaultToCaptureOn) {
  {
    QSettings raw;
    raw.setValue(u"Preferences::source_cache_budget_gb"_s, 1'000'000);
  }
  EXPECT_EQ(SourceCacheStore::loadSettings().budget_gb, 1000);
  {
    QSettings raw;
    raw.setValue(u"Preferences::source_cache_budget_gb"_s, 0);
  }
  EXPECT_EQ(SourceCacheStore::loadSettings().budget_gb, 1);
  // An absent capture key means "capture on" (the shipped default).
  {
    QSettings raw;
    raw.remove(u"Preferences::source_cache_capture_enabled"_s);
  }
  EXPECT_TRUE(SourceCacheStore::loadSettings().capture_enabled);
}

}  // namespace
