// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pj_datastore/engine.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_marketplace/extension_manager.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/CurveColorRegistry.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
using namespace Qt::StringLiterals;

namespace {

void addScalarSamples(
    PJ::AppSession& session, PJ::DatasetId dataset_id, std::string_view topic, std::vector<PJ::Timestamp> timestamps) {
  PJ::DataWriter writer = session.sessionManager().dataEngine().createWriter();
  auto handle_or = writer.registerScalarSeries(dataset_id, topic, PJ::NumericType::kFloat64);
  ASSERT_TRUE(handle_or.has_value()) << handle_or.error();
  for (PJ::Timestamp timestamp : timestamps) {
    writer.appendScalar(*handle_or, timestamp, 1.0);
  }
  const auto changed_topics = session.sessionManager().commitChunks(writer.flushAll());
  ASSERT_FALSE(changed_topics.empty());
}

TEST(AppSessionTest, PluginDirOverrideDoesNotShiftMarketplaceManager) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());

  PJ::AppSession session(dir.path());

  // The catalog reports the --plugin-dir override as its extensions dir — that
  // is the top LOAD-priority tier in buildScanHierarchy.
  EXPECT_EQ(session.extensionCatalog().extensionsDir(), dir.path());
  // The ExtensionManager, in contrast, stays anchored on the MANAGED marketplace
  // dir regardless of any --plugin-dir override: the marketplace UI must keep
  // tracking what it installs and manages. The two paths differ whenever
  // --plugin-dir is provided.
  EXPECT_NE(session.extensionCatalog().extensionManager().extensionsDir(), dir.path());
}

TEST(AppSessionTest, BuiltinPluginFoldersOrderedWithPluginDirOverride) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());

  // dir.path() is the --plugin-dir override (the install dir), distinct from the
  // marketplace default location.
  PJ::AppSession session(dir.path());

  const QStringList builtins = session.extensionCatalog().builtinPluginFolders();
  // Built-in scanned folders: install dir (= override) first, then the
  // marketplace dir. The bundled (share) dir is not listed — it is a seed
  // source, not a scanned folder.
  ASSERT_EQ(builtins.size(), 2);
  EXPECT_EQ(builtins.at(0), dir.path());
  EXPECT_NE(builtins.at(1), dir.path());  // marketplace location, distinct from the override
}

TEST(AppSessionTest, InvalidExtensionDirectoryReportsDiagnostic) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString file_path = dir.filePath("not-a-directory");
  QFile file(file_path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  file.close();

  std::vector<PJ::Diagnostic> diagnostics;
  PJ::AppSession session(
      file_path, [&diagnostics](const PJ::Diagnostic& diagnostic) { diagnostics.push_back(diagnostic); });

  const auto is_error = [](const PJ::Diagnostic& diagnostic) {
    return diagnostic.level == PJ::DiagnosticLevel::kError;
  };
  EXPECT_TRUE(std::any_of(diagnostics.begin(), diagnostics.end(), is_error));
}

TEST(AppSessionTest, SeedPlaybackUsesScalarAndObjectTimeBounds) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  auto dataset =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  addScalarSamples(session, *dataset, "/imu/x", {100, 200});

  auto object_topic = session.sessionManager().objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/camera/image",
          .metadata_json = R"({"builtin_object_type":"kImage"})",
      });
  ASSERT_TRUE(object_topic.has_value()) << object_topic.error();
  ASSERT_TRUE(
      session.sessionManager().objectStore().pushOwned(*object_topic, 900, std::vector<uint8_t>{1}).has_value());

  // Seeding reads the VISIBLE catalog (production rebuilds before seeding).
  session.catalogModel().rebuildFromDatastore();
  EXPECT_TRUE(session.seedPlaybackFromSession());
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMin().value, 100.0e-9);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 900.0e-9);
  EXPECT_DOUBLE_EQ(session.playbackEngine().currentTime().value, 100.0e-9);
}

TEST(AppSessionTest, SeedPlaybackUsesDisplayRelativeSecondsForShiftedDataset) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  // A dataset on a time domain shifted by +2 s (display_time = raw - 2e9).
  auto domain = session.sessionManager().dataEngine().createTimeDomain("shifted");
  ASSERT_TRUE(domain.has_value()) << domain.error();
  session.sessionManager().dataEngine().setDisplayOffset(*domain, 2'000'000'000LL);
  auto dataset = session.sessionManager().dataEngine().createDataset(
      PJ::DatasetDescriptor{.source_name = "shifted.mcap", .time_domain_id = *domain});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  addScalarSamples(session, *dataset, "/imu/x", {5'000'000'000LL, 9'000'000'000LL});

  session.catalogModel().rebuildFromDatastore();
  EXPECT_TRUE(session.seedPlaybackFromSession());
  // Display seconds = (raw - 2e9)/1e9 -> [3, 7], NOT the absolute [5, 9] the old
  // offset-blind seeding produced.
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMin().value, 3.0);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 7.0);
  EXPECT_DOUBLE_EQ(session.playbackEngine().currentTime().value, 3.0);
}

TEST(AppSessionTest, SubsequentSeedPreservesCurrentTimeWhenNewRangeIsSubset) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  auto first_dataset =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "first.mcap"});
  ASSERT_TRUE(first_dataset.has_value()) << first_dataset.error();
  addScalarSamples(session, *first_dataset, "/imu/x", {100, 200});
  session.catalogModel().rebuildFromDatastore();
  ASSERT_TRUE(session.seedPlaybackFromSession());
  session.playbackEngine().setCurrentTime(PJ::DisplaySeconds{150.0e-9});

  auto second_dataset =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "second.mcap"});
  ASSERT_TRUE(second_dataset.has_value()) << second_dataset.error();
  addScalarSamples(session, *second_dataset, "/imu/x", {120, 180});

  session.catalogModel().rebuildFromDatastore();
  EXPECT_TRUE(session.seedPlaybackFromSession());
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMin().value, 100.0e-9);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 200.0e-9);
  EXPECT_DOUBLE_EQ(session.playbackEngine().currentTime().value, 150.0e-9);
}

// The DataEngine keeps a removed dataset's scalars (append-only tombstone),
// but the playback timeline must track the VISIBLE catalog: once dataset A is
// removed, loading dataset B must yield B's range, not A∪B.
TEST(AppSessionTest, RemovedDatasetStopsContributingToPlaybackRange) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  auto removed_dataset =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "old.mcap"});
  ASSERT_TRUE(removed_dataset.has_value()) << removed_dataset.error();
  addScalarSamples(session, *removed_dataset, "/imu/x", {100, 10'000});
  session.catalogModel().rebuildFromDatastore();
  ASSERT_TRUE(session.seedPlaybackFromSession());

  ASSERT_TRUE(session.catalogModel().removeDataset(*removed_dataset));

  auto loaded_dataset =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "new.mcap"});
  ASSERT_TRUE(loaded_dataset.has_value()) << loaded_dataset.error();
  addScalarSamples(session, *loaded_dataset, "/imu/x", {1'000, 2'000});
  session.catalogModel().rebuildFromDatastore();
  ASSERT_TRUE(session.seedPlaybackFromSession());

  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMin().value, 1'000.0e-9)
      << "removed dataset still stretches the timeline start";
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 2'000.0e-9)
      << "removed dataset still stretches the timeline end";
}

// Visibility is per-CURVE, not just per-dataset: trashing all of a topic's
// curves must drop that topic's bounds from the playback range even while
// sibling topics keep the dataset itself visible.
TEST(AppSessionTest, TrashedCurvesStopContributingToPlaybackRange) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  auto dataset =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  addScalarSamples(session, *dataset, "/imu/x", {100, 200});
  addScalarSamples(session, *dataset, "/gps/fix", {100, 1'000'000});
  session.catalogModel().rebuildFromDatastore();
  ASSERT_TRUE(session.seedPlaybackFromSession());
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 1'000'000.0e-9);

  // Trash the long topic's curves (the dataset stays visible through /imu/x).
  std::vector<QString> trashed_keys;
  for (const PJ::CatalogItem& item : session.catalogModel().items()) {
    if (item.topic_name == "/gps/fix"_L1) {
      trashed_keys.push_back(item.key);
    }
  }
  ASSERT_FALSE(trashed_keys.empty());
  session.catalogModel().removeItems(trashed_keys);

  ASSERT_TRUE(session.seedPlaybackFromSession());
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMin().value, 100.0e-9);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 200.0e-9)
      << "trashed topic's bounds must stop stretching the timeline";
}

// After a full catalog clear, the next seed behaves like a first load: range
// snaps to the new data only (no union with the cleared bounds) and the
// playhead snaps to the new start (no stale position carried over).
TEST(AppSessionTest, ClearAllThenSeedSnapsPlaybackToNewData) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  auto first_dataset =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "first.mcap"});
  ASSERT_TRUE(first_dataset.has_value()) << first_dataset.error();
  addScalarSamples(session, *first_dataset, "/imu/x", {100, 200});
  session.catalogModel().rebuildFromDatastore();
  ASSERT_TRUE(session.seedPlaybackFromSession());
  session.playbackEngine().setCurrentTime(PJ::DisplaySeconds{150.0e-9});

  session.catalogModel().clearAll();

  auto second_dataset =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "second.mcap"});
  ASSERT_TRUE(second_dataset.has_value()) << second_dataset.error();
  addScalarSamples(session, *second_dataset, "/imu/x", {10, 90});
  session.catalogModel().rebuildFromDatastore();
  ASSERT_TRUE(session.seedPlaybackFromSession());

  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMin().value, 10.0e-9);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 90.0e-9)
      << "cleared dataset's bounds must not survive into the new range";
  EXPECT_DOUBLE_EQ(session.playbackEngine().currentTime().value, 10.0e-9)
      << "playhead must snap to the new data's start after a full clear";
}

// Removing the LAST visible dataset must collapse the transport to the empty
// state: range reset to [0,0], cursor to 0, and playback stopped — not left
// pointing at (and "playing" over) the vanished data's stale range.
TEST(AppSessionTest, RemovingLastDatasetResetsPlaybackToEmptyAndStops) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  auto dataset = session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "only.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  addScalarSamples(session, *dataset, "/imu/x", {1'000, 2'000});
  session.catalogModel().rebuildFromDatastore();
  ASSERT_TRUE(session.seedPlaybackFromSession());
  session.playbackEngine().play();
  ASSERT_TRUE(session.playbackEngine().isPlaying());

  // Removing the sole dataset empties the catalog (cleared()), which must reset
  // the transport through the AppSession cleared() hook.
  ASSERT_TRUE(session.catalogModel().removeDataset(*dataset));

  EXPECT_FALSE(session.playbackEngine().isPlaying()) << "playback must stop when the last dataset is removed";
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMin().value, 0.0);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 0.0) << "empty timeline must reset to [0,0]";
  EXPECT_DOUBLE_EQ(session.playbackEngine().currentTime().value, 0.0) << "cursor must reset to 0 when empty";
}

// A full catalog clear (Remove-all / trash-all) must likewise stop playback and
// reset the range to empty, not merely re-arm the first-seed snap.
TEST(AppSessionTest, ClearAllStopsPlaybackAndResetsRangeToEmpty) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  auto dataset = session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "data.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  addScalarSamples(session, *dataset, "/imu/x", {100, 900});
  session.catalogModel().rebuildFromDatastore();
  ASSERT_TRUE(session.seedPlaybackFromSession());
  session.playbackEngine().play();
  ASSERT_TRUE(session.playbackEngine().isPlaying());

  session.catalogModel().clearAll();

  EXPECT_FALSE(session.playbackEngine().isPlaying()) << "playback must stop on a full clear";
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMin().value, 0.0);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 0.0);
  EXPECT_DOUBLE_EQ(session.playbackEngine().currentTime().value, 0.0);
}

// Removing the EARLIEST of several datasets while "Use time offset" is on must
// re-base the global time reference to the new earliest sample (the t0 cache
// cannot keep pointing at the vanished dataset's start). Uses the SessionManager
// removeDataset seam that invalidates the memoized origin.
TEST(AppSessionTest, RemovingEarliestDatasetRebasesGlobalTimeReference) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());
  session.sessionManager().setUseTimeOffset(true);

  auto early = session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "early.mcap"});
  ASSERT_TRUE(early.has_value()) << early.error();
  addScalarSamples(session, *early, "/imu/x", {100, 500});
  auto late = session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "late.mcap"});
  ASSERT_TRUE(late.has_value()) << late.error();
  addScalarSamples(session, *late, "/gps/x", {1'000, 2'000});
  session.catalogModel().rebuildFromDatastore();

  EXPECT_EQ(session.sessionManager().globalTimeReference(), 100) << "origin is the earliest sample across datasets";

  // The re-base must also signal a display reframe so surviving curve adapters
  // drop their now-stale cached offsets (QObject::connect counter — the test
  // links only Qt6::Core, not Qt6::Test/QSignalSpy).
  int reframe_count = 0;
  QObject::connect(
      &session.sessionManager(), qOverload<>(&PJ::SessionManager::displayOffsetChanged),
      [&reframe_count]() { ++reframe_count; });

  // Remove the earliest dataset through the session seam, then rebuild.
  ASSERT_TRUE(session.catalogModel().removeDataset(*early));
  session.sessionManager().removeDataset(*early);
  session.catalogModel().rebuildFromDatastore();

  EXPECT_EQ(session.sessionManager().globalTimeReference(), 1'000)
      << "removing the earliest dataset must re-base the global time reference";
  EXPECT_GE(reframe_count, 1) << "re-basing the origin must emit displayOffsetChanged() for a plot reframe";
}

TEST(AppSessionTest, ClearingCatalogForgetsRememberedCurveColors) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  // Load data so the catalog is non-empty (clearAll() is a no-op, and emits
  // nothing, on an already-empty catalog).
  auto dataset =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  addScalarSamples(session, *dataset, "/imu/x", {100, 200});
  session.catalogModel().rebuildFromDatastore();

  // A remembered curve color...
  session.curveColorRegistry().setColor(u"/imu/x"_s, u"#1f77b4"_s);
  ASSERT_TRUE(session.curveColorRegistry().color(u"/imu/x"_s).has_value());

  // ...is forgotten when the catalog is cleared (data replaced), via the
  // AppSession wiring of CatalogModel::cleared -> CurveColorRegistry::clear.
  session.catalogModel().clearAll();

  EXPECT_FALSE(session.curveColorRegistry().color(u"/imu/x"_s).has_value());
}

TEST(AppSessionTest, DatasetRawTimeRangeReturnsRawBounds) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  // One dataset spanning raw [1000, 9000] ns across two scalar topics.
  auto dataset =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  addScalarSamples(session, *dataset, "/imu/x", {1'000, 5'000});
  addScalarSamples(session, *dataset, "/gps/fix", {3'000, 9'000});
  session.catalogModel().rebuildFromDatastore();

  const auto range = session.datasetRawTimeRange(*dataset);
  ASSERT_TRUE(range.has_value());
  EXPECT_EQ(range->min, 1'000);  // RAW (pre-offset) union min
  EXPECT_EQ(range->max, 9'000);  // RAW (pre-offset) union max

  EXPECT_FALSE(session.datasetRawTimeRange(99999).has_value());  // unknown dataset
}

// datasetRawTimeRange is RAW (pre-offset): a display offset on the dataset's
// domain must not shift the reported bounds (the caller applies the offset).
TEST(AppSessionTest, DatasetRawTimeRangeIgnoresDisplayOffset) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  auto domain = session.sessionManager().dataEngine().createTimeDomain("shifted");
  ASSERT_TRUE(domain.has_value()) << domain.error();
  session.sessionManager().dataEngine().setDisplayOffset(*domain, 2'000'000'000LL);
  auto dataset = session.sessionManager().dataEngine().createDataset(
      PJ::DatasetDescriptor{.source_name = "shifted.mcap", .time_domain_id = *domain});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  addScalarSamples(session, *dataset, "/imu/x", {5'000'000'000LL, 9'000'000'000LL});
  session.catalogModel().rebuildFromDatastore();

  const auto range = session.datasetRawTimeRange(*dataset);
  ASSERT_TRUE(range.has_value());
  EXPECT_EQ(range->min, 5'000'000'000LL);  // raw, NOT display (raw - 2e9)
  EXPECT_EQ(range->max, 9'000'000'000LL);
}

TEST(AppSessionTest, CurveColorRegistryIsOwnedBySessionManager) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  // The registry is owned by SessionManager; AppSession's accessor delegates to
  // it. Plot widgets reach the same instance through their SessionManager
  // pointer, so it never has to be threaded through their constructors.
  EXPECT_EQ(&session.curveColorRegistry(), &session.sessionManager().curveColorRegistry());
}

namespace {

// Create a dataset on its own time domain shifted by `display_offset_ns`
// (display_time = raw - offset) and write one scalar topic into it.
PJ::DatasetId makeShiftedDataset(
    PJ::AppSession& session, const std::string& name, PJ::Timestamp display_offset_ns, const std::string& topic,
    std::vector<PJ::Timestamp> timestamps) {
  auto domain = session.sessionManager().dataEngine().createTimeDomain(name + "_domain");
  EXPECT_TRUE(domain.has_value());
  session.sessionManager().dataEngine().setDisplayOffset(*domain, display_offset_ns);
  auto dataset = session.sessionManager().dataEngine().createDataset(
      PJ::DatasetDescriptor{.source_name = name, .time_domain_id = *domain});
  EXPECT_TRUE(dataset.has_value());
  addScalarSamples(session, *dataset, topic, std::move(timestamps));
  return *dataset;
}

PJ::DatasetId makeEmptyShiftedDataset(
    PJ::AppSession& session, const std::string& name, PJ::Timestamp display_offset_ns) {
  auto domain = session.sessionManager().dataEngine().createTimeDomain(name + "_domain");
  EXPECT_TRUE(domain.has_value());
  session.sessionManager().dataEngine().setDisplayOffset(*domain, display_offset_ns);
  auto dataset = session.sessionManager().dataEngine().createDataset(
      PJ::DatasetDescriptor{.source_name = name, .time_domain_id = *domain});
  EXPECT_TRUE(dataset.has_value());
  return *dataset;
}

PJ::ObjectTopicId addObjectTopic(
    PJ::AppSession& session, PJ::DatasetId dataset_id, const std::string& topic_name,
    PJ::sdk::BuiltinObjectType object_type, PJ::Timestamp timestamp = 0) {
  const std::string metadata =
      std::string{R"({"builtin_object_type":")"} + std::string(PJ::sdk::name(object_type)) + R"("})";
  auto& object_store = session.sessionManager().objectStore();
  auto topic = object_store.registerTopic(
      PJ::ObjectTopicDescriptor{.dataset_id = dataset_id, .topic_name = topic_name, .metadata_json = metadata});
  EXPECT_TRUE(topic.has_value()) << topic.error();
  if (!topic.has_value()) {
    return {};
  }
  EXPECT_TRUE(object_store.pushOwned(*topic, timestamp, std::vector<uint8_t>{1}).has_value());
  return *topic;
}

std::size_t mergedSampleCount(PJ::AppSession& session, PJ::DatasetId dataset, const std::string& topic) {
  for (const PJ::TopicId tid : session.sessionManager().dataEngine().listTopics(dataset)) {
    const auto* st = session.sessionManager().dataEngine().getTopicStorage(tid);
    if (st != nullptr && st->descriptor().name == topic) {
      auto series = session.sessionManager().createReader().series(tid, 0);
      return series.has_value() ? series->size() : 0;
    }
  }
  return 0;
}

}  // namespace

// A bulk import (cloud fetch) FOCUSES playback: the range snaps to the new
// dataset's bounds even when an older dataset spans a much wider window — a
// 10s snippet must present a 10s timeline, not drown in the union.
TEST(AppSessionTest, FocusPlaybackSnapsRangeToTheGivenDatasets) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  auto old_dataset =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "old-wide.mcap"});
  ASSERT_TRUE(old_dataset.has_value()) << old_dataset.error();
  addScalarSamples(session, *old_dataset, "/imu/x", {1'000, 1'000'000'000});
  ASSERT_TRUE(session.seedPlaybackFromSession());

  auto snippet =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "snippet.mcap"});
  ASSERT_TRUE(snippet.has_value()) << snippet.error();
  addScalarSamples(session, *snippet, "/odom/x", {500'000, 600'000});

  EXPECT_TRUE(session.focusPlaybackOnDatasets({*snippet}));
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMin().value, 500'000.0e-9);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 600'000.0e-9);
  EXPECT_DOUBLE_EQ(session.playbackEngine().currentTime().value, 500'000.0e-9);
}

// Latched/static objects (tf_static, stale markers) carry payload-embedded
// stamps far OUTSIDE an import's window: scalar series alone bound the focused
// range; objects define it only when the import has no scalar data at all.
TEST(AppSessionTest, FocusPlaybackPrefersScalarBoundsOverObjectStamps) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  auto dataset =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "fetch.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  addScalarSamples(session, *dataset, "/odom/x", {500'000, 600'000});

  // A tf_static-shaped object entry stamped LONG before the window.
  auto object_topic = session.sessionManager().objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/tf_static",
          .metadata_json = R"({"builtin_object_type":"kFrameTransforms"})",
      });
  ASSERT_TRUE(object_topic.has_value()) << object_topic.error();
  ASSERT_TRUE(session.sessionManager().objectStore().pushOwned(*object_topic, 7, std::vector<uint8_t>{1}).has_value());

  EXPECT_TRUE(session.focusPlaybackOnDatasets({*dataset}));
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMin().value, 500'000.0e-9);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 600'000.0e-9);

  // Objects still seed when the import carries ONLY objects (a 3D-only fetch).
  auto objects_only =
      session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "cloud-3d.mcap"});
  ASSERT_TRUE(objects_only.has_value()) << objects_only.error();
  auto cloud_topic = session.sessionManager().objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *objects_only,
          .topic_name = "/points",
          .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
      });
  ASSERT_TRUE(cloud_topic.has_value()) << cloud_topic.error();
  ASSERT_TRUE(
      session.sessionManager().objectStore().pushOwned(*cloud_topic, 42'000, std::vector<uint8_t>{1}).has_value());

  EXPECT_TRUE(session.focusPlaybackOnDatasets({*objects_only}));
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMin().value, 42'000.0e-9);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 42'000.0e-9);
}

TEST(AppSessionMergeTest, CollapsesSelectionIntoMergedAnchor) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  // A at display [0, 1e9]; B dragged to display [2e9, 3e9] (raw 5e9..6e9, offset 3e9).
  const PJ::DatasetId a = makeShiftedDataset(session, "A", 0, "/s", {0, 1'000'000'000LL});
  const PJ::DatasetId b = makeShiftedDataset(session, "B", 3'000'000'000LL, "/s", {5'000'000'000LL, 6'000'000'000LL});
  session.catalogModel().rebuildFromDatastore();
  // Loader metadata on both contributors: the destructive merge must invalidate
  // every contributor's document, the anchor's included — no single artifact
  // describes the merged result.
  ASSERT_TRUE(session.sessionManager().setDatasetMetadata(a, uR"({"from": "a"})"_s, u"loader"_s).has_value());
  ASSERT_TRUE(session.sessionManager().setDatasetMetadata(b, uR"({"from": "b"})"_s, u"loader"_s).has_value());

  session.mergeDatasets({a, b});
  session.catalogModel().rebuildFromDatastore();
  EXPECT_EQ(session.sessionManager().datasetMetadata(a), nullptr);
  EXPECT_EQ(session.sessionManager().datasetMetadata(b), nullptr);

  // Only the merged anchor remains, relabelled, spanning the arranged union.
  const auto datasets = session.catalogModel().datasets();
  ASSERT_EQ(datasets.size(), 1U);
  EXPECT_EQ(datasets.front().first, a);
  EXPECT_TRUE(datasets.front().second.endsWith("_merged")) << datasets.front().second.toStdString();
  const auto range = session.datasetRawTimeRange(a);
  ASSERT_TRUE(range.has_value());
  EXPECT_EQ(range->min, 0);                // anchor's absolute start
  EXPECT_EQ(range->max, 3'000'000'000LL);  // B folded in at display [2e9,3e9] -> raw 2e9..3e9
  EXPECT_EQ(mergedSampleCount(session, a, "/s"), 4U);
}

TEST(AppSessionMergeTest, AnchorIsLeftmostInDisplayNotRaw) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  // Same raw [0,1e9], but B is dragged 5 s LEFT (offset 5e9 -> display [-5e9,-4e9]),
  // so B is the leftmost-in-display anchor even though A shares its raw start.
  const PJ::DatasetId a = makeShiftedDataset(session, "A", 0, "/s", {0, 1'000'000'000LL});
  const PJ::DatasetId b = makeShiftedDataset(session, "B", 5'000'000'000LL, "/s", {0, 1'000'000'000LL});
  session.catalogModel().rebuildFromDatastore();

  session.mergeDatasets({a, b});
  session.catalogModel().rebuildFromDatastore();

  const auto datasets = session.catalogModel().datasets();
  ASSERT_EQ(datasets.size(), 1U);
  EXPECT_EQ(datasets.front().first, b);  // B is the anchor (leftmost in display)
  const auto range = session.datasetRawTimeRange(b);
  ASSERT_TRUE(range.has_value());
  EXPECT_EQ(range->min, 0);  // B's absolute start (A folded in after, shifted +5e9)
  EXPECT_EQ(mergedSampleCount(session, b, "/s"), 4U);
}

TEST(AppSessionMergeTest, NoConflictWhenSharedObjectTopicsSameType) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  const PJ::DatasetId a = makeShiftedDataset(session, "A", 0, "/s", {0, 1'000'000'000LL});
  const PJ::DatasetId b = makeShiftedDataset(session, "B", 0, "/s", {2'000'000'000LL, 3'000'000'000LL});
  addObjectTopic(session, a, "/img", PJ::sdk::BuiltinObjectType::kImage);
  addObjectTopic(session, b, "/img", PJ::sdk::BuiltinObjectType::kImage, 2'000'000'000LL);
  session.catalogModel().rebuildFromDatastore();

  EXPECT_TRUE(session.objectMergeConflicts({a, b}).empty());
}

TEST(AppSessionMergeTest, DetectsConflictBetweenTwoSourceOnlyTopics) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  // Anchor Z is leftmost-in-display and has NO "/objects"; sources A and B both have "/objects" but
  // disagree on canonical type. The gate must catch this source-vs-source clash even though the anchor
  // lacks the name (else ObjectStore::mergeDatasets would silently fuse incompatible types).
  const PJ::DatasetId z = makeShiftedDataset(session, "Z", 0, "/s", {0, 1'000'000'000LL});
  const PJ::DatasetId a = makeShiftedDataset(session, "A", 0, "/s", {5'000'000'000LL, 6'000'000'000LL});
  const PJ::DatasetId b = makeShiftedDataset(session, "B", 0, "/s", {7'000'000'000LL, 8'000'000'000LL});
  addObjectTopic(session, a, "/objects", PJ::sdk::BuiltinObjectType::kImage, 5'000'000'000LL);
  addObjectTopic(session, b, "/objects", PJ::sdk::BuiltinObjectType::kPointCloud, 7'000'000'000LL);
  session.catalogModel().rebuildFromDatastore();

  const auto conflicts = session.objectMergeConflicts({z, a, b});
  ASSERT_EQ(conflicts.size(), 1U);
  EXPECT_EQ(conflicts[0].topic_name, "/objects");
  EXPECT_EQ(conflicts[0].source_dataset_id, b);
  EXPECT_EQ(conflicts[0].anchor_type, PJ::sdk::BuiltinObjectType::kImage);  // A (first source) set the type
  EXPECT_EQ(conflicts[0].source_type, PJ::sdk::BuiltinObjectType::kPointCloud);
}

TEST(AppSessionMergeTest, DirectMergeWithObjectTypeConflictIsRefused) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  const PJ::DatasetId a = makeShiftedDataset(session, "A", 0, "/s", {0, 1'000'000'000LL});
  const PJ::DatasetId b = makeShiftedDataset(session, "B", 0, "/s", {2'000'000'000LL, 3'000'000'000LL});
  const PJ::ObjectTopicId a_topic = addObjectTopic(session, a, "/x", PJ::sdk::BuiltinObjectType::kImage, 100);
  const PJ::ObjectTopicId b_topic =
      addObjectTopic(session, b, "/x", PJ::sdk::BuiltinObjectType::kPointCloud, 2'000'000'100LL);
  session.catalogModel().rebuildFromDatastore();

  EXPECT_EQ(session.mergeDatasets({a, b}), 0U);

  const auto datasets = session.catalogModel().datasets();
  ASSERT_EQ(datasets.size(), 2U);
  EXPECT_TRUE(std::any_of(datasets.begin(), datasets.end(), [a](const auto& dataset) { return dataset.first == a; }));
  EXPECT_TRUE(std::any_of(datasets.begin(), datasets.end(), [b](const auto& dataset) { return dataset.first == b; }));
  for (const auto& [id, label] : datasets) {
    (void)id;
    EXPECT_FALSE(label.endsWith("_merged")) << label.toStdString();
  }
  EXPECT_EQ(mergedSampleCount(session, a, "/s"), 2U);
  EXPECT_EQ(mergedSampleCount(session, b, "/s"), 2U);

  auto& store = session.sessionManager().objectStore();
  const auto a_after = store.findTopic(a, "/x");
  const auto b_after = store.findTopic(b, "/x");
  ASSERT_TRUE(a_after.has_value());
  ASSERT_TRUE(b_after.has_value());
  EXPECT_EQ(a_after->id, a_topic.id);
  EXPECT_EQ(b_after->id, b_topic.id);
  EXPECT_EQ(store.entryCount(*a_after), 1U);
  EXPECT_EQ(store.entryCount(*b_after), 1U);
  const auto a_entry = store.at(*a_after, std::size_t{0});
  const auto b_entry = store.at(*b_after, std::size_t{0});
  ASSERT_TRUE(a_entry.has_value());
  ASSERT_TRUE(b_entry.has_value());
  EXPECT_EQ(a_entry->timestamp, 100);
  EXPECT_EQ(b_entry->timestamp, 2'000'000'100LL);
}

TEST(AppSessionMergeTest, MergeFoldsObjectTopicsIntoAnchor) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  const PJ::DatasetId a = makeShiftedDataset(session, "A", 0, "/s", {0, 1'000'000'000LL});
  const PJ::DatasetId b = makeShiftedDataset(session, "B", 0, "/s", {2'000'000'000LL, 3'000'000'000LL});
  addObjectTopic(session, a, "/img", PJ::sdk::BuiltinObjectType::kImage, 100);
  addObjectTopic(session, b, "/img", PJ::sdk::BuiltinObjectType::kImage, 2'000'000'100LL);
  addObjectTopic(session, b, "/cloud", PJ::sdk::BuiltinObjectType::kPointCloud, 4'000'000'000LL);
  session.catalogModel().rebuildFromDatastore();

  PJ::DatasetId emitted_anchor = 0;
  std::vector<PJ::DatasetId> emitted_consumed;
  QObject::connect(
      &session, &PJ::AppSession::datasetsMerged, &session,
      [&](PJ::DatasetId anchor, const QList<PJ::DatasetId>& consumed) {
        emitted_anchor = anchor;
        emitted_consumed.assign(consumed.begin(), consumed.end());
      });

  const PJ::DatasetId anchor = session.mergeDatasets({a, b});
  ASSERT_EQ(anchor, a);

  auto& object_store = session.sessionManager().objectStore();
  const auto img = object_store.findTopic(anchor, "/img");
  ASSERT_TRUE(img.has_value());
  EXPECT_EQ(object_store.entryCount(*img), 2u);
  EXPECT_TRUE(object_store.findTopic(anchor, "/cloud").has_value());
  EXPECT_TRUE(object_store.listTopics(b).empty());

  const auto datasets = session.catalogModel().datasets();
  ASSERT_EQ(datasets.size(), 1U);
  EXPECT_EQ(datasets.front().first, a);
  EXPECT_TRUE(datasets.front().second.endsWith("_merged")) << datasets.front().second.toStdString();
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 4.0);
  EXPECT_EQ(emitted_anchor, a);
  EXPECT_EQ(emitted_consumed, (std::vector<PJ::DatasetId>{b}));
}

TEST(AppSessionMergeTest, MergeRefusedWhileSelectedDatasetStreaming) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  const PJ::DatasetId a = makeShiftedDataset(session, "A", 0, "/s", {0, 1'000'000'000LL});
  const PJ::DatasetId b = makeShiftedDataset(session, "B", 0, "/s", {2'000'000'000LL, 3'000'000'000LL});
  session.catalogModel().rebuildFromDatastore();
  session.setActiveStreamingDataset(b);

  EXPECT_EQ(session.activeStreamingDataset(), b);
  EXPECT_EQ(session.mergeDatasets({a, b}), 0U);

  const auto datasets = session.catalogModel().datasets();
  ASSERT_EQ(datasets.size(), 2U);
  EXPECT_TRUE(std::any_of(datasets.begin(), datasets.end(), [a](const auto& dataset) { return dataset.first == a; }));
  EXPECT_TRUE(std::any_of(datasets.begin(), datasets.end(), [b](const auto& dataset) { return dataset.first == b; }));
  for (const auto& [id, label] : datasets) {
    (void)id;
    EXPECT_FALSE(label.endsWith("_merged")) << label.toStdString();
  }
  EXPECT_EQ(mergedSampleCount(session, a, "/s"), 2U);
  EXPECT_EQ(mergedSampleCount(session, b, "/s"), 2U);
}

TEST(AppSessionMergeTest, ObjectOnlyMergeRefreshesRangeWithTimeOffset) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  constexpr PJ::Timestamp kSecond = 1'000'000'000LL;
  const PJ::DatasetId a = makeEmptyShiftedDataset(session, "A", 100 * kSecond);
  const PJ::DatasetId b = makeEmptyShiftedDataset(session, "B", -100 * kSecond);

  auto& store = session.sessionManager().objectStore();
  const PJ::ObjectTopicId a_topic =
      addObjectTopic(session, a, "/obj", PJ::sdk::BuiltinObjectType::kImage, 100 * kSecond);
  ASSERT_TRUE(store.pushOwned(a_topic, 200 * kSecond, std::vector<uint8_t>{2}).has_value());
  const PJ::ObjectTopicId b_topic = addObjectTopic(session, b, "/obj", PJ::sdk::BuiltinObjectType::kImage, 0);
  ASSERT_TRUE(store.pushOwned(b_topic, 10 * kSecond, std::vector<uint8_t>{3}).has_value());

  session.catalogModel().rebuildFromDatastore();
  session.sessionManager().setUseTimeOffset(true);
  ASSERT_TRUE(session.seedPlaybackFromSession());
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMin().value, 0.0);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 110.0);

  const PJ::DatasetId anchor = session.mergeDatasets({a, b});
  ASSERT_EQ(anchor, a);

  const auto raw_range = session.datasetRawTimeRange(anchor);
  ASSERT_TRUE(raw_range.has_value());
  EXPECT_EQ(raw_range->min, 100 * kSecond);
  EXPECT_EQ(raw_range->max, 210 * kSecond);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMin().value, -100.0);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 10.0);
}

// Byte-exact numerical check of the merged object data through the full runtime
// merge path: distinct payloads at known timestamps, with B display-shifted +2s
// so the merge rebases B's raw object timestamps by -2s. The merged topic must be
// the union of both sources' entries, ordered by (shifted) timestamp, with every
// payload value preserved.
TEST(AppSessionMergeTest, MergedObjectDataIsNumericallyCorrect) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  // A is the anchor (display start 0); B is shifted +2s in display, so the merge
  // applies raw_shift = anchor_offset - B_offset = -2s to B's object timestamps.
  const PJ::DatasetId a = makeShiftedDataset(session, "A", 0, "/s", {0, 10'000'000'000LL});
  const PJ::DatasetId b = makeShiftedDataset(session, "B", 2'000'000'000LL, "/s", {5'000'000'000LL, 6'000'000'000LL});

  auto& store = session.sessionManager().objectStore();
  const auto reg = [&store](PJ::DatasetId ds) {
    auto id = store.registerTopic(
        PJ::ObjectTopicDescriptor{
            .dataset_id = ds, .topic_name = "/obj", .metadata_json = R"({"builtin_object_type":"kImage"})"});
    EXPECT_TRUE(id.has_value()) << (id.has_value() ? "" : id.error());
    return *id;
  };
  const auto push = [&store](PJ::ObjectTopicId id, PJ::Timestamp ts, uint8_t byte) {
    EXPECT_TRUE(store.pushOwned(id, ts, std::vector<uint8_t>{byte}).has_value());
  };
  const auto a_obj = reg(a);
  push(a_obj, 4'000'000'000LL, 0xA1);
  push(a_obj, 8'000'000'000LL, 0xA2);
  const auto b_obj = reg(b);
  push(b_obj, 7'000'000'000LL, 0xB1);  // -2s shift -> 5s
  push(b_obj, 9'000'000'000LL, 0xB2);  // -2s shift -> 7s

  session.catalogModel().rebuildFromDatastore();
  const PJ::DatasetId anchor = session.mergeDatasets({a, b});
  ASSERT_EQ(anchor, a);

  const auto merged = store.findTopic(anchor, "/obj");
  ASSERT_TRUE(merged.has_value());
  ASSERT_EQ(store.entryCount(*merged), 4u);

  struct Expected {
    PJ::Timestamp ts;
    uint8_t byte;
  };
  const std::vector<Expected> expected = {
      {4'000'000'000LL, 0xA1},  // A @ 4s
      {5'000'000'000LL, 0xB1},  // B raw 7s, rebased -2s
      {7'000'000'000LL, 0xB2},  // B raw 9s, rebased -2s
      {8'000'000'000LL, 0xA2},  // A @ 8s
  };
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const auto entry = store.at(*merged, i);
    ASSERT_TRUE(entry.has_value()) << "missing merged entry " << i;
    EXPECT_EQ(entry->timestamp, expected[i].ts) << "timestamp mismatch at index " << i;
    ASSERT_EQ(entry->payload.bytes.size(), 1u);
    EXPECT_EQ(entry->payload.bytes[0], expected[i].byte) << "payload mismatch at index " << i;
  }
  // The non-anchor source's /obj is folded away after the fuse.
  EXPECT_FALSE(store.findTopic(b, "/obj").has_value());
}

TEST(AppSessionMergeTest, DetectsConflictOnSharedNameDifferentType) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  const PJ::DatasetId a = makeShiftedDataset(session, "A", 0, "/s", {0, 1'000'000'000LL});
  const PJ::DatasetId b = makeShiftedDataset(session, "B", 0, "/s", {2'000'000'000LL, 3'000'000'000LL});
  addObjectTopic(session, a, "/x", PJ::sdk::BuiltinObjectType::kImage);
  addObjectTopic(session, b, "/x", PJ::sdk::BuiltinObjectType::kPointCloud, 2'000'000'000LL);
  session.catalogModel().rebuildFromDatastore();

  const auto conflicts = session.objectMergeConflicts({b, a});
  ASSERT_EQ(conflicts.size(), 1U);
  EXPECT_EQ(conflicts[0].topic_name, "/x");
  EXPECT_EQ(conflicts[0].source_dataset_id, b);
  EXPECT_EQ(conflicts[0].anchor_type, PJ::sdk::BuiltinObjectType::kImage);
  EXPECT_EQ(conflicts[0].source_type, PJ::sdk::BuiltinObjectType::kPointCloud);
}

TEST(AppSessionMergeTest, SingleSelectionIsNoOp) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  const PJ::DatasetId a = makeShiftedDataset(session, "A", 0, "/s", {0, 1'000'000'000LL});
  session.catalogModel().rebuildFromDatastore();

  session.mergeDatasets({a});  // < 2 datasets -> nothing happens
  session.catalogModel().rebuildFromDatastore();

  const auto datasets = session.catalogModel().datasets();
  ASSERT_EQ(datasets.size(), 1U);
  EXPECT_FALSE(datasets.front().second.endsWith("_merged"));
}

TEST(AppSessionMergeTest, DataLessDatasetInSelectionIsIgnoredForAnchor) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  const PJ::DatasetId a = makeShiftedDataset(session, "A", 0, "/s", {0, 1'000'000'000LL});  // display [0,1]
  const PJ::DatasetId b = makeShiftedDataset(session, "B", 0, "/s", {2'000'000'000LL, 3'000'000'000LL});  // [2,3]
  // C is registered but carries NO samples -> no time-bearing data (exercises the
  // datasetRawTimeRange-returns-nullopt continue branch in the anchor pick).
  auto c_domain = session.sessionManager().dataEngine().createTimeDomain("C_domain");
  ASSERT_TRUE(c_domain.has_value());
  auto c = session.sessionManager().dataEngine().createDataset(
      PJ::DatasetDescriptor{.source_name = "C", .time_domain_id = *c_domain});
  ASSERT_TRUE(c.has_value());
  session.catalogModel().rebuildFromDatastore();

  const PJ::DatasetId anchor = session.mergeDatasets({a, *c, b});
  session.catalogModel().rebuildFromDatastore();

  EXPECT_EQ(anchor, a);                                // A is leftmost-in-display; data-less C can never anchor
  EXPECT_EQ(mergedSampleCount(session, a, "/s"), 4U);  // A's 2 + B's 2; C adds nothing
}

TEST(AppSessionMergeTest, RejectedMergeLeavesCatalogConsistentWithStore) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  const PJ::DatasetId a = makeShiftedDataset(session, "A", 0, "/s", {0, 1'000'000'000LL});
  const PJ::DatasetId b = makeShiftedDataset(session, "B", 0, "/s", {2'000'000'000LL, 3'000'000'000LL});
  session.catalogModel().rebuildFromDatastore();

  // A duplicated source makes the engine REJECT the merge (nothing mutated in the
  // store). The catalog must be left intact rather than removing B — otherwise B
  // would vanish from the UI while its data still lives in the store.
  const PJ::DatasetId anchor = session.mergeDatasets({a, b, b});
  session.catalogModel().rebuildFromDatastore();

  EXPECT_EQ(anchor, 0U);  // no-op
  const auto datasets = session.catalogModel().datasets();
  EXPECT_EQ(datasets.size(), 2U);  // both datasets still present
  for (const auto& [id, label] : datasets) {
    EXPECT_FALSE(label.endsWith("_merged")) << label.toStdString();
  }
  EXPECT_TRUE(session.datasetRawTimeRange(b).has_value());  // B's data still live
}

TEST(AppSessionTest, RecomputeRangeMovesRangeButNeverCurrentTime) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::AppSession session(dir.path());

  // Seed a dataset (display [0,10]) and park the playhead at an interior 4.0 s.
  const PJ::DatasetId ds = makeShiftedDataset(session, "A", 0, "/s", {0, 10'000'000'000LL});
  session.catalogModel().rebuildFromDatastore();
  ASSERT_TRUE(session.seedPlaybackFromSession());
  session.playbackEngine().setCurrentTime(PJ::DisplaySeconds{4.0});
  ASSERT_DOUBLE_EQ(session.playbackEngine().currentTime().value, 4.0);

  // Shift the offset so the union range moves to display [-2,8] (still around 4.0).
  // recomputeRange is the live-drag path: it must move the RANGE and return the
  // new min, but NEVER re-snap currentTime (the property the whole design guards).
  session.sessionManager().setDisplayOffset(ds, PJ::DisplayOffset{PJ::Duration{2'000'000'000LL}});
  const auto new_min = session.recomputeRange();
  ASSERT_TRUE(new_min.has_value());
  EXPECT_DOUBLE_EQ(new_min->value, -2.0);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMin().value, -2.0);
  EXPECT_DOUBLE_EQ(session.playbackEngine().rangeMax().value, 8.0);
  EXPECT_DOUBLE_EQ(session.playbackEngine().currentTime().value, 4.0);  // unchanged: no snap
}

}  // namespace
