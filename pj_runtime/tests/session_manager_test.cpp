// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>
#include <algorithm>
#include <any>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "on_demand_test_utils.h"
#include "pj_base/buffer_anchor.hpp"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_base/builtin/log.hpp"
#include "pj_base/builtin/plot_markers.hpp"
#include "pj_datastore/data_processor.hpp"
#include "pj_datastore/saturating_time.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/BatchEngine.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/DerivedTopics.h"
#include "pj_runtime/EnginePool.h"
#include "pj_runtime/OnDemandService.h"
#include "pj_runtime/ResultHistory.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/TransformService.h"
#include "pj_tf/tf_buffer.h"
using namespace Qt::StringLiterals;

namespace {

class PassThroughProcessor : public PJ::proc::DataProcessor {
 public:
  const char* id() const override {
    return "test-pass";
  }
  const char* bracketLabel() const override {
    return "Pass";
  }
  PJ::proc::TraitMask traits() const override {
    return PJ::proc::kStatelessOneToOne | PJ::proc::kPreservesOrder;
  }
  bool isStreamSafe() const override {
    return true;
  }
  void reset() override {}
  std::optional<PJ::proc::Sample> calculateNextPoint(const PJ::proc::Sample& input) override {
    return input;
  }
};

TEST(SessionManagerSourceTest, LastLoadedSourceStartsEmpty) {
  PJ::SessionManager session;
  EXPECT_FALSE(session.lastLoadedSource().has_value());
}

// recordLoadedSource stores normalizedSourcePath(path) — compare through the
// same contract, not against the raw literal: on Windows even an absolute
// Unix-style input gains a drive prefix from the absolute-path fallback.
TEST(SessionManagerSourceTest, RecordLoadedSourceStoresPathAndPrefix) {
  PJ::SessionManager session;
  session.recordLoadedSource(u"/tmp/run42.csv"_s, u"robot"_s);
  const auto src = session.lastLoadedSource();
  ASSERT_TRUE(src.has_value());
  EXPECT_EQ(src->path, PJ::SessionManager::normalizedSourcePath(u"/tmp/run42.csv"_s));
  EXPECT_EQ(src->prefix, u"robot"_s);
}

TEST(SessionManagerSourceIdentityTest, FullPathRejectsRemintedSameBasenameCollision) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  ASSERT_TRUE(QDir().mkpath(dir.filePath(u"left"_s)));
  ASSERT_TRUE(QDir().mkpath(dir.filePath(u"right"_s)));
  const QString intended_path = dir.filePath(u"left/run.mcap"_s);
  const QString collision_path = dir.filePath(u"right/run.mcap"_s);
  ASSERT_TRUE(QFile(intended_path).open(QIODevice::WriteOnly));
  ASSERT_TRUE(QFile(collision_path).open(QIODevice::WriteOnly));

  PJ::SessionManager session;
  const auto reminted_collision = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "run.mcap"});
  const auto intended = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "run.mcap"});
  ASSERT_TRUE(reminted_collision.has_value());
  ASSERT_TRUE(intended.has_value());
  session.setDatasetSourcePath(*reminted_collision, collision_path);
  session.setDatasetSourcePath(*intended, intended_path);

  // The saved id points at the wrong same-basename file: the path qualifier must
  // veto the exact-id match and fall back to the unique full-path match instead.
  const PJ::DatasetIdentityResolution resolved =
      session.resolveDatasetIdentity(*reminted_collision, u"run.mcap"_s, intended_path);
  ASSERT_TRUE(resolved.id.has_value());
  EXPECT_EQ(*resolved.id, intended.value());
  EXPECT_FALSE(resolved.ambiguous);
}

// FIX D: a dataset loaded from a file that is then deleted from disk must still
// resolve by its full path. normalizedSourcePath returns the same cleaned-absolute
// form at store and resolve time for a plain (non-symlinked) path, so the string
// comparison in path_matches holds even after the file is gone — whereas a
// canonicalFilePath-only comparison (empty for a missing file) would drop it.
TEST(SessionManagerSourceIdentityTest, DeletedSourceFileStillResolvesByPath) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString saved_path = dir.filePath(u"run.mcap"_s);
  ASSERT_TRUE(QFile(saved_path).open(QIODevice::WriteOnly));

  PJ::SessionManager session;
  const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "run.mcap"});
  ASSERT_TRUE(dataset.has_value());
  // Register the path WHILE the file exists, then delete it from disk.
  session.setDatasetSourcePath(*dataset, saved_path);
  ASSERT_TRUE(QFile::remove(saved_path));
  ASSERT_FALSE(QFileInfo::exists(saved_path));

  const PJ::DatasetIdentityResolution resolved = session.resolveDatasetIdentity(0, u"run.mcap"_s, saved_path);
  ASSERT_TRUE(resolved.id.has_value()) << "a deleted-on-disk source must still resolve by path";
  EXPECT_EQ(*resolved.id, dataset.value());
  EXPECT_FALSE(resolved.ambiguous);
}

TEST(SessionManagerSourceIdentityTest, DuplicateSourceWithoutPathIsAmbiguousButExactLiveIdStillWins) {
  PJ::SessionManager session;
  const auto first = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "same.mcap"});
  const auto second = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "same.mcap"});
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());

  const PJ::DatasetIdentityResolution exact = session.resolveDatasetIdentity(*second, u"same.mcap"_s);
  ASSERT_TRUE(exact.id.has_value());
  EXPECT_EQ(*exact.id, second.value()) << "same-session undo keeps its exact DatasetId";

  const PJ::DatasetIdentityResolution portable = session.resolveDatasetIdentity(999, u"same.mcap"_s);
  EXPECT_FALSE(portable.id.has_value());
  EXPECT_TRUE(portable.ambiguous) << "persisted source-only identity must never choose by load order";
}

TEST(SessionManagerSourceIdentityTest, NumericOnlyIdentityIsSameSessionOnly) {
  PJ::SessionManager session;
  const auto only = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "solo.mcap"});
  ASSERT_TRUE(only.has_value());

  // A bare id with no portable qualifiers resolves only while that exact id lives.
  const PJ::DatasetIdentityResolution live = session.resolveDatasetIdentity(*only, QString());
  ASSERT_TRUE(live.id.has_value());
  EXPECT_EQ(*live.id, only.value());

  const PJ::DatasetIdentityResolution stale = session.resolveDatasetIdentity(*only + 100, QString());
  EXPECT_FALSE(stale.id.has_value());
  EXPECT_FALSE(stale.ambiguous) << "no qualifier = not-found, never a remint fallback";
}

TEST(SessionManagerSourceIdentityTest, RemoveDatasetDropsItsSourcePath) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = dir.filePath(u"gone.mcap"_s);
  ASSERT_TRUE(QFile(path).open(QIODevice::WriteOnly));

  PJ::SessionManager session;
  const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "gone.mcap"});
  ASSERT_TRUE(dataset.has_value());
  session.setDatasetSourcePath(*dataset, path);
  ASSERT_FALSE(session.datasetSourcePath(*dataset).isEmpty());

  session.removeDataset(*dataset);
  EXPECT_TRUE(session.datasetSourcePath(*dataset).isEmpty());
}

TEST(SessionManagerSourceIdentityTest, ObjectTopicDisambiguatesFanOutSiblingsOfOneFile) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = dir.filePath(u"fanout.mcap"_s);
  ASSERT_TRUE(QFile(path).open(QIODevice::WriteOnly));

  PJ::SessionManager session;
  const auto left = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "fanout/left"});
  const auto right = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "fanout/right"});
  ASSERT_TRUE(left.has_value());
  ASSERT_TRUE(right.has_value());
  session.setDatasetSourcePath(*left, path);
  session.setDatasetSourcePath(*right, path);
  const auto left_topic = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{.dataset_id = *left, .topic_name = "/camera", .metadata_json = "{}"});
  ASSERT_TRUE(left_topic.has_value());

  // Path-only identity is ambiguous across the two siblings...
  const PJ::DatasetIdentityResolution by_path = session.resolveDatasetIdentity(0, QString(), path);
  EXPECT_FALSE(by_path.id.has_value());
  EXPECT_TRUE(by_path.ambiguous);

  // ...but exactly one sibling owns the object topic, so the topic-aware
  // resolver can pick it; a topic neither sibling owns stays ambiguous.
  const PJ::DatasetIdentityResolution by_topic = session.resolveObjectDatasetIdentity(0, QString(), path, u"/camera"_s);
  ASSERT_TRUE(by_topic.id.has_value());
  EXPECT_EQ(*by_topic.id, left.value());
  EXPECT_FALSE(by_topic.ambiguous);

  const PJ::DatasetIdentityResolution unknown_topic =
      session.resolveObjectDatasetIdentity(0, QString(), path, u"/lidar"_s);
  EXPECT_FALSE(unknown_topic.id.has_value());
  EXPECT_TRUE(unknown_topic.ambiguous);
}

TEST(SessionManagerSourceTest, RecordLoadedSourceAppendsDistinctPaths) {
  PJ::SessionManager session;
  session.recordLoadedSource(u"/tmp/a.csv"_s, QString());
  session.recordLoadedSource(u"/tmp/b.csv"_s, u"p"_s);
  // Both distinct files are tracked, in load order.
  const auto& sources = session.loadedSources();
  ASSERT_EQ(sources.size(), 2u);
  EXPECT_EQ(sources[0].path, PJ::SessionManager::normalizedSourcePath(u"/tmp/a.csv"_s));
  EXPECT_EQ(sources[1].path, PJ::SessionManager::normalizedSourcePath(u"/tmp/b.csv"_s));
  // lastLoadedSource() is the most recent.
  const auto src = session.lastLoadedSource();
  ASSERT_TRUE(src.has_value());
  EXPECT_EQ(src->path, PJ::SessionManager::normalizedSourcePath(u"/tmp/b.csv"_s));
  EXPECT_EQ(src->prefix, u"p"_s);
}

TEST(SessionManagerSourceTest, RecordLoadedSourceReplacesSamePathInPlace) {
  PJ::SessionManager session;
  session.recordLoadedSource(u"/tmp/a.csv"_s, QString());
  session.recordLoadedSource(u"/tmp/b.csv"_s, u"p"_s);
  // Re-recording an existing path (a reload) updates it in place, keeping its
  // position and not growing the list.
  session.recordLoadedSource(u"/tmp/a.csv"_s, u"robot"_s, u"CSV"_s, uR"({"x":1})"_s);
  const auto& sources = session.loadedSources();
  ASSERT_EQ(sources.size(), 2u);
  EXPECT_EQ(sources[0].path, PJ::SessionManager::normalizedSourcePath(u"/tmp/a.csv"_s));
  EXPECT_EQ(sources[0].prefix, u"robot"_s);
  EXPECT_EQ(sources[0].plugin_id, u"CSV"_s);
  EXPECT_EQ(sources[0].plugin_config_json, uR"({"x":1})"_s);
  EXPECT_EQ(sources[1].path, PJ::SessionManager::normalizedSourcePath(u"/tmp/b.csv"_s));
}

TEST(SessionManagerSourceTest, ClearLoadedSourceResetsToEmpty) {
  PJ::SessionManager session;
  session.recordLoadedSource(u"/tmp/a.csv"_s, QString());
  session.recordLoadedSource(u"/tmp/b.csv"_s, QString());
  session.clearLoadedSource();
  EXPECT_FALSE(session.lastLoadedSource().has_value());
  EXPECT_TRUE(session.loadedSources().empty());
}

TEST(SessionManagerSourceTest, RecordLoadedSourceStoresPluginIdAndConfig) {
  PJ::SessionManager session;
  session.recordLoadedSource(u"/tmp/run42.mcap"_s, u""_s, u"DataLoad MCAP"_s, uR"({"topics":["/imu"]})"_s);
  const auto src = session.lastLoadedSource();
  ASSERT_TRUE(src.has_value());
  EXPECT_EQ(src->path, PJ::SessionManager::normalizedSourcePath(u"/tmp/run42.mcap"_s));
  EXPECT_EQ(src->prefix, QString());
  EXPECT_EQ(src->plugin_id, u"DataLoad MCAP"_s);
  EXPECT_EQ(src->plugin_config_json, uR"({"topics":["/imu"]})"_s);
}

TEST(SessionManagerSourceTest, RecordLoadedSourceDefaultsPluginFieldsToEmpty) {
  PJ::SessionManager session;
  // Old 2-arg shape — plugin fields default-empty.
  session.recordLoadedSource(u"/tmp/a.csv"_s, u"p"_s);
  const auto src = session.lastLoadedSource();
  ASSERT_TRUE(src.has_value());
  EXPECT_EQ(src->path, PJ::SessionManager::normalizedSourcePath(u"/tmp/a.csv"_s));
  EXPECT_EQ(src->prefix, u"p"_s);
  EXPECT_TRUE(src->plugin_id.isEmpty());
  EXPECT_TRUE(src->plugin_config_json.isEmpty());
}

TEST(SessionManagerSourceTest, ClearLoadedSourceResetsPluginFieldsToo) {
  PJ::SessionManager session;
  session.recordLoadedSource(u"/tmp/a.mcap"_s, QString(), u"DataLoad MCAP"_s, uR"({"x":1})"_s);
  session.clearLoadedSource();
  EXPECT_FALSE(session.lastLoadedSource().has_value());
}

TEST(SessionManagerObjectsTest, EvictDatasetObjectsRemovesOnlyThatDatasetsTopics) {
  PJ::SessionManager session;
  auto dataset_a = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "a.mcap"});
  ASSERT_TRUE(dataset_a.has_value());
  ASSERT_TRUE(session.objectStore()
                  .registerTopic(
                      PJ::ObjectTopicDescriptor{
                          .dataset_id = *dataset_a,
                          .topic_name = "/camera/a",
                          .metadata_json = R"({"builtin_object_type":"kImage"})",
                      })
                  .has_value());
  auto dataset_b = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "b.mcap"});
  ASSERT_TRUE(dataset_b.has_value());
  ASSERT_TRUE(session.objectStore()
                  .registerTopic(
                      PJ::ObjectTopicDescriptor{
                          .dataset_id = *dataset_b,
                          .topic_name = "/camera/b",
                          .metadata_json = R"({"builtin_object_type":"kImage"})",
                      })
                  .has_value());

  session.evictDatasetObjects(*dataset_a);

  EXPECT_TRUE(session.objectStore().listTopics(*dataset_a).empty()) << "dataset a's objects must be evicted";
  EXPECT_EQ(session.objectStore().listTopics(*dataset_b).size(), 1U) << "unrelated dataset b must be untouched";
}

TEST(SessionManagerObjectsTest, ClearAllObjectsEvictsEveryTopic) {
  PJ::SessionManager session;
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value());
  ASSERT_TRUE(session.objectStore()
                  .registerTopic(
                      PJ::ObjectTopicDescriptor{
                          .dataset_id = *dataset,
                          .topic_name = "/camera/image",
                          .metadata_json = R"({"builtin_object_type":"kImage"})",
                      })
                  .has_value());
  ASSERT_FALSE(session.objectStore().listTopics().empty());

  session.clearAllObjects();

  EXPECT_TRUE(session.objectStore().listTopics().empty());
}

TEST(SessionManagerObjectsTest, EvictObjectTopicsRemovesOnlySpecifiedTopics) {
  PJ::SessionManager session;
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value());
  auto topic_keep = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/camera/keep",
          .metadata_json = R"({"builtin_object_type":"kImage"})",
      });
  ASSERT_TRUE(topic_keep.has_value());
  auto topic_drop = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/camera/drop",
          .metadata_json = R"({"builtin_object_type":"kImage"})",
      });
  ASSERT_TRUE(topic_drop.has_value());
  ASSERT_EQ(session.objectStore().listTopics(*dataset).size(), 2U);

  // Trashing one object-topic entry evicts only that topic, sibling topics in the
  // same dataset survive (drives onCatalogTrashRequested).
  session.evictObjectTopics({*topic_drop});

  const auto remaining = session.objectStore().listTopics(*dataset);
  ASSERT_EQ(remaining.size(), 1U) << "sibling object topic in the same dataset must survive";
  EXPECT_EQ(remaining.front().id, topic_keep->id) << "the surviving topic must be the one not evicted";
}

TEST(SessionManagerTimeTest, DisplayOffsetReadsLiveTimeDomainShift) {
  PJ::SessionManager session;
  auto domain = session.dataEngine().createTimeDomain("shifted");
  ASSERT_TRUE(domain.has_value());
  auto dataset = session.dataEngine().createDataset(
      PJ::DatasetDescriptor{.source_name = "shifted.mcap", .time_domain_id = *domain});
  ASSERT_TRUE(dataset.has_value());

  // Offset set AFTER createDataset: it must be read live, since the dataset's
  // snapshot of the domain would still report zero.
  session.dataEngine().setDisplayOffset(*domain, 2'000'000'000LL);
  EXPECT_EQ(session.displayOffset(*dataset).value, std::chrono::nanoseconds{2'000'000'000LL});
}

TEST(SessionManagerTimeTest, DisplayOffsetIsZeroForDefaultDomainAndUnknownDataset) {
  PJ::SessionManager session;
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "plain.mcap"});
  ASSERT_TRUE(dataset.has_value());
  EXPECT_EQ(session.displayOffset(*dataset).value, std::chrono::nanoseconds{0});  // default (id 0) domain
  EXPECT_EQ(session.displayOffset(9999).value, std::chrono::nanoseconds{0});      // unknown dataset
}

TEST(SessionManagerTimeTest, SetDisplayOffsetWritesDomainAndEmits) {
  PJ::SessionManager session;
  auto& engine = session.dataEngine();
  const auto domain = engine.createTimeDomain("src");
  ASSERT_TRUE(domain.has_value());
  const auto dataset = engine.createDataset(PJ::DatasetDescriptor{.source_name = "s.mcap", .time_domain_id = *domain});
  ASSERT_TRUE(dataset.has_value());

  QSignalSpy spy(&session, qOverload<PJ::DatasetId>(&PJ::SessionManager::displayOffsetChanged));
  session.setDisplayOffset(*dataset, PJ::DisplayOffset{std::chrono::nanoseconds{500}});

  EXPECT_EQ(session.displayOffset(*dataset).value.count(), 500);
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(spy.front().front().value<PJ::DatasetId>(), *dataset);
}

TEST(SessionManagerTimeTest, SetDisplayOffsetIgnoresUnknownDatasetAndDefaultDomain) {
  PJ::SessionManager session;
  auto& engine = session.dataEngine();
  // Dataset bound to the default (id 0) domain: no-op + warning, no emit.
  const auto default_domain_dataset = engine.createDataset(PJ::DatasetDescriptor{.source_name = "plain.mcap"});
  ASSERT_TRUE(default_domain_dataset.has_value());

  QSignalSpy spy(&session, qOverload<PJ::DatasetId>(&PJ::SessionManager::displayOffsetChanged));
  session.setDisplayOffset(*default_domain_dataset, PJ::DisplayOffset{std::chrono::nanoseconds{500}});
  session.setDisplayOffset(9999, PJ::DisplayOffset{std::chrono::nanoseconds{500}});  // unknown dataset

  EXPECT_EQ(session.displayOffset(*default_domain_dataset).value.count(), 0);
  EXPECT_EQ(spy.count(), 0);
}

// Proves the mechanism the per-source TimeDomain loader change enables: each
// source gets its own domain, so an offset on one source cannot leak into
// another's displayOffset.
TEST(SessionManagerTimeTest, PerSourceDomainsIsolateOffsets) {
  PJ::SessionManager session;
  auto& engine = session.dataEngine();
  const auto td_a = engine.createTimeDomain("a");
  const auto td_b = engine.createTimeDomain("b");
  ASSERT_TRUE(td_a.has_value() && td_b.has_value());
  EXPECT_NE(*td_a, *td_b);  // distinct domains
  const auto ds_a = engine.createDataset(PJ::DatasetDescriptor{.source_name = "a", .time_domain_id = *td_a});
  const auto ds_b = engine.createDataset(PJ::DatasetDescriptor{.source_name = "b", .time_domain_id = *td_b});
  ASSERT_TRUE(ds_a.has_value() && ds_b.has_value());

  session.setDisplayOffset(static_cast<PJ::DatasetId>(*ds_a), PJ::DisplayOffset{std::chrono::nanoseconds{777}});
  EXPECT_EQ(session.displayOffset(static_cast<PJ::DatasetId>(*ds_a)).value.count(), 777);
  EXPECT_EQ(session.displayOffset(static_cast<PJ::DatasetId>(*ds_b)).value.count(), 0);  // unaffected
}

// Trivial parser whose only job is to exist (vt_/ctx_ non-null) so the slot is
// reported valid by SessionManager. The concurrency canary never calls parse;
// it only races (re-)registration against per-tick binding reads.
class NoopParser : public PJ::MessageParserPluginBase {};

std::unique_ptr<PJ::MessageParserHandle> makeNoopHandle() {
  static constexpr const char* kManifest =
      R"({"id":"noop-parser","name":"Noop Parser","version":"1.0.0","encoding":["mock"]})";
  // One static vtable per CreateFn instantiation; the create fn allocates a fresh
  // NoopParser each call, so every handle owns a distinct instance.
  auto handle = std::make_unique<PJ::MessageParserHandle>(
      PJ::MessageParserPluginBase::vtableWithCreate([]() noexcept -> void* { return new NoopParser; }, kManifest));
  EXPECT_TRUE(handle->valid());
  return handle;
}

// Crash / TSan canary for the parser-slot race (H.12): the streaming worker
// re-registers fresh handles for the same ObjectTopicId in a tight loop while
// the GUI thread resolves the per-use binding and touches the parser through the
// keepalive. Deterministic (fixed iteration count, no sleeps) so it reproduces
// the unsynchronized-map UB and the replace-frees-live-parser hazard reliably
// under a sanitizer; a plain run just exercises the lock discipline.
TEST(SessionManagerParserRaceTest, ConcurrentRegisterAndBindIsSafe) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic{42};

  // Seed one parser so the reader sees a binding immediately.
  session.registerObjectTopicParser(topic, makeNoopHandle());

  constexpr int kIterations = 5000;
  std::atomic<bool> writer_done{false};
  std::atomic<bool> writer_at_handoff{false};
  std::atomic<bool> reader_has_snapshot{false};
  std::atomic<bool> handoff_replacement_done{false};

  std::thread writer([&] {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
      // Coordinate one replacement so the reader deterministically holds the
      // prior binding across it. Without this handoff, a fast runner can finish
      // all 5,000 writes before the reader is scheduled, which tests scheduler
      // luck instead of the keepalive contract.
      const bool handoff = iteration == kIterations / 2;
      if (handoff) {
        writer_at_handoff.store(true, std::memory_order_release);
        while (!reader_has_snapshot.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
      }

      // Each registration overwrites the slot, dropping the previous handle —
      // exactly the cross-thread replacement that used to free a parser out from
      // under a reader holding only a raw pointer.
      session.registerObjectTopicParser(topic, makeNoopHandle());
      if (handoff) {
        handoff_replacement_done.store(true, std::memory_order_release);
      }
    }
    writer_done.store(true, std::memory_order_release);
  });

  while (!writer_at_handoff.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  // Keep this snapshot alive while the writer replaces its slot, then
  // dereference the old handle. This is the lifetime edge the test protects.
  PJ::SessionManager::ParserBinding handoff_binding = session.parserBindingForObjectTopic(topic);
  const bool handoff_binding_valid = static_cast<bool>(handoff_binding);
  reader_has_snapshot.store(true, std::memory_order_release);
  while (!handoff_replacement_done.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  // Reader loop: take a per-use snapshot, and when truthy, dereference the parser
  // through the binding's keepalive — the keepalive is what must keep a replaced
  // parser alive for the duration of this access.
  std::size_t valid_bindings = 0;
  std::size_t manifest_bytes = 0;  // sink so the manifest read is not optimized away
  if (handoff_binding_valid) {
    const auto* handle = static_cast<const PJ::MessageParserHandle*>(handoff_binding.keepalive.get());
    manifest_bytes += handle->manifest().size();
    ++valid_bindings;
  } else {
    ADD_FAILURE() << "seeded parser binding disappeared before the coordinated replacement";
  }

  while (!writer_done.load(std::memory_order_acquire)) {
    PJ::SessionManager::ParserBinding binding = session.parserBindingForObjectTopic(topic);
    if (binding) {
      // keepalive holds the handle (and DSO) mapped; reading manifest() exercises
      // the parser instance the snapshot named, even if the writer just replaced
      // the slot.
      const auto* handle = static_cast<const PJ::MessageParserHandle*>(binding.keepalive.get());
      manifest_bytes += handle->manifest().size();
      ++valid_bindings;
    }
  }
  writer.join();
  EXPECT_GT(manifest_bytes, 0U);  // keeps the keepalive-deref in the binary

  // Drain any in-flight replacement, then assert a final binding is valid.
  PJ::SessionManager::ParserBinding final_binding = session.parserBindingForObjectTopic(topic);
  EXPECT_TRUE(static_cast<bool>(final_binding)) << "topic must still have a valid parser after the race";
  EXPECT_GT(valid_bindings, 0U) << "reader never observed a valid binding";
}

// --- datasetDisplayRange: the offset-aware range primitive shared by the
// streaming-playback seed (so its origin matches AppSession's file-load seed) ---

void writeScalarSamples(
    PJ::SessionManager& session, PJ::DatasetId dataset_id, std::string_view topic,
    std::vector<PJ::Timestamp> timestamps) {
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(dataset_id, topic, PJ::NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value()) << handle.error();
  for (const PJ::Timestamp timestamp : timestamps) {
    writer.appendScalar(*handle, timestamp, 1.0);
  }
  ASSERT_FALSE(session.commitChunks(writer.flushAll()).empty());
}

TEST(SessionManagerRangeTest, DatasetDisplayRangeUnionsScalarAndObjectBoundsInDisplaySeconds) {
  PJ::SessionManager session;
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  writeScalarSamples(session, *dataset, "/imu/x", {100, 200});

  auto object_topic = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/camera/image",
          .metadata_json = R"({"builtin_object_type":"kImage"})",
      });
  ASSERT_TRUE(object_topic.has_value()) << object_topic.error();
  ASSERT_TRUE(session.objectStore().pushOwned(*object_topic, 900, std::vector<uint8_t>{1}).has_value());

  // Min from the scalar topic (100 ns), max from the object topic (900 ns); no
  // offset, so display seconds == raw / 1e9.
  const auto range = session.datasetDisplayRange(*dataset);
  ASSERT_TRUE(range.has_value());
  EXPECT_DOUBLE_EQ(range->min.value, 100.0e-9);
  EXPECT_DOUBLE_EQ(range->max.value, 900.0e-9);
}

TEST(SessionManagerRangeTest, DatasetDisplayRangeAppliesDatasetDisplayOffset) {
  PJ::SessionManager session;
  // A dataset on a time domain shifted by +2 s (display_time = raw - 2e9).
  auto domain = session.dataEngine().createTimeDomain("shifted");
  ASSERT_TRUE(domain.has_value()) << domain.error();
  session.dataEngine().setDisplayOffset(*domain, 2'000'000'000LL);
  auto dataset = session.dataEngine().createDataset(
      PJ::DatasetDescriptor{.source_name = "shifted.mcap", .time_domain_id = *domain});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  writeScalarSamples(session, *dataset, "/imu/x", {5'000'000'000LL, 9'000'000'000LL});

  // Display seconds = (raw - 2e9)/1e9 -> [3, 7], matching the file-load seed
  // (AppSessionTest.SeedPlaybackUsesDisplayRelativeSecondsForShiftedDataset) and
  // NOT the offset-blind absolute [5, 9] the old streaming path produced.
  const auto range = session.datasetDisplayRange(*dataset);
  ASSERT_TRUE(range.has_value());
  EXPECT_DOUBLE_EQ(range->min.value, 3.0);
  EXPECT_DOUBLE_EQ(range->max.value, 7.0);
}

// A finding sits at kFindingTimestamp, a sentinel off the data clock: folding it into the dataset's
// bounds would stretch the range back to the year 1677.
TEST(SessionManagerRangeTest, DerivedFindingDoesNotWidenTheDatasetRange) {
  PJ::SessionManager session;
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  auto cloud = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/points",
          .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
      });
  auto finding = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "owner/pin/cropped",
          .metadata_json = PJ::derivedTopicMetadata(PJ::sdk::BuiltinObjectType::kPointCloud),
      });
  ASSERT_TRUE(cloud.has_value() && finding.has_value());
  ASSERT_TRUE(session.objectStore().pushOwned(*cloud, 100, std::vector<uint8_t>{1}).has_value());
  ASSERT_TRUE(session.objectStore().pushOwned(*cloud, 900, std::vector<uint8_t>{1}).has_value());
  ASSERT_TRUE(session.objectStore().pushOwned(*finding, PJ::kFindingTimestamp, std::vector<uint8_t>{2}).has_value());

  const auto range = session.datasetDisplayRange(*dataset);
  ASSERT_TRUE(range.has_value());
  EXPECT_DOUBLE_EQ(range->min.value, 100.0e-9);
  EXPECT_DOUBLE_EQ(range->max.value, 900.0e-9);
}

TEST(SessionManagerRangeTest, DatasetDisplayRangeIsNulloptForEmptyDataset) {
  PJ::SessionManager session;
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "empty.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  EXPECT_FALSE(session.datasetDisplayRange(*dataset).has_value());
}

// --- "Use time offset": the per-dataset relative-time frame ---

// Creates a dataset on its OWN TimeDomain (mirroring FileLoader's one-domain-per-
// source) and writes scalar samples. The per-source domain is required because
// "align starts" / Timeline offsets are written to the domain — a default (id 0)
// domain carries no shiftable offset.
PJ::DatasetId addDataset(
    PJ::SessionManager& session, std::string_view source, std::string_view topic,
    std::vector<PJ::Timestamp> timestamps) {
  auto domain = session.dataEngine().createTimeDomain(std::string(source));
  EXPECT_TRUE(domain.has_value()) << domain.error();
  auto dataset = session.dataEngine().createDataset(
      PJ::DatasetDescriptor{.source_name = std::string(source), .time_domain_id = *domain});
  EXPECT_TRUE(dataset.has_value()) << dataset.error();
  writeScalarSamples(session, *dataset, topic, std::move(timestamps));
  return *dataset;
}

TEST(SessionManagerTimeOffsetTest, DefaultsOffWithZeroOffset) {
  PJ::SessionManager session;
  const PJ::DatasetId a = addDataset(session, "a.mcap", "/a", {5'000'000'000LL, 9'000'000'000LL});
  EXPECT_FALSE(session.useTimeOffset());
  // Default: no auto-align. A data-bearing dataset shows at its natural absolute
  // time (offset 0) — NOT rebased to its own start. Regression guard for the bug
  // where "Use time offset" defaulted on and collapsed every dataset to zero.
  EXPECT_EQ(session.displayOffset(a).value.count(), 0);
}

// The Source Timeline edits the PER-SOURCE alignment offset (sourceDisplayOffset),
// which round-trips EXACTLY. The global "Use time offset" reference is layered on
// ONLY in displayOffset(): the two compose (alignment + global) instead of the old
// conflated model where a drag overwrote the global frame. So a drag while the
// frame is on no longer collapses onto the global term.
TEST(SessionManagerTimeOffsetTest, AlignmentOffsetIsSeparateFromGlobalFrame) {
  PJ::SessionManager session;
  const PJ::DatasetId a = addDataset(session, "a.mcap", "/a", {5'000'000'000LL, 9'000'000'000LL});

  // Frame on: displayOffset subtracts the global earliest (5 s here); the per-source
  // alignment is untouched (still 0) so the Source Timeline bar stays put.
  session.setUseTimeOffset(true);
  EXPECT_EQ(session.sourceDisplayOffset(a).value.count(), 0);
  EXPECT_EQ(session.globalTimeReference(), 5'000'000'000LL);
  EXPECT_EQ(session.displayOffset(a).value.count(), 5'000'000'000LL);

  // A Timeline drag writes the per-source alignment; it round-trips verbatim, and
  // displayOffset COMPOSES it with the global term (7e9 + 5e9), never overwrites.
  session.setDisplayOffset(a, PJ::DisplayOffset{std::chrono::nanoseconds{7'000'000'000LL}});
  EXPECT_EQ(session.sourceDisplayOffset(a).value.count(), 7'000'000'000LL);
  EXPECT_EQ(session.displayOffset(a).value.count(), 12'000'000'000LL);
}

// "Use time offset" subtracts ONE uniform global origin (the earliest sample across
// all datasets), so cross-dataset time gaps are PRESERVED — not a per-source rebase
// that collapses every dataset to zero. The per-source alignment offsets stay zero,
// so the Source Timeline's bars never move when the global frame is toggled.
TEST(SessionManagerTimeOffsetTest, UsesUniformGlobalOriginPreservingGaps) {
  PJ::SessionManager session;
  // Two datasets recorded 2 s apart.
  const PJ::DatasetId a = addDataset(session, "a.mcap", "/a", {5'000'000'000LL, 9'000'000'000LL});
  const PJ::DatasetId b = addDataset(session, "b.mcap", "/b", {3'000'000'000LL, 4'000'000'000LL});

  session.setUseTimeOffset(true);
  EXPECT_TRUE(session.useTimeOffset());
  // Both datasets share the SAME global origin (b's 3 s, the global earliest); the
  // per-source alignment offsets are untouched (bars invariant to the toggle).
  EXPECT_EQ(session.globalTimeReference(), 3'000'000'000LL);
  EXPECT_EQ(session.displayOffset(a).value.count(), 3'000'000'000LL);
  EXPECT_EQ(session.displayOffset(b).value.count(), 3'000'000'000LL);
  EXPECT_EQ(session.sourceDisplayOffset(a).value.count(), 0);
  EXPECT_EQ(session.sourceDisplayOffset(b).value.count(), 0);
  const auto range_a = session.datasetDisplayRange(a);
  const auto range_b = session.datasetDisplayRange(b);
  ASSERT_TRUE(range_a.has_value());
  ASSERT_TRUE(range_b.has_value());
  // b starts the axis at 0; a starts 2 s later — the real gap is PRESERVED (the old
  // per-source rebase would have put both at 0).
  EXPECT_DOUBLE_EQ(range_b->min.value, 0.0);
  EXPECT_DOUBLE_EQ(range_a->min.value, 2.0);  // (5e9 - 3e9)/1e9
  EXPECT_DOUBLE_EQ(range_a->max.value, 6.0);  // (9e9 - 3e9)/1e9
  EXPECT_DOUBLE_EQ(range_b->max.value, 1.0);  // (4e9 - 3e9)/1e9

  session.setUseTimeOffset(false);
  EXPECT_FALSE(session.useTimeOffset());
  EXPECT_EQ(session.displayOffset(a).value.count(), 0);
  EXPECT_EQ(session.globalTimeReference(), 0);
  const auto range_a_off = session.datasetDisplayRange(a);
  ASSERT_TRUE(range_a_off.has_value());
  EXPECT_DOUBLE_EQ(range_a_off->min.value, 5.0);  // back to absolute epoch seconds
}

TEST(SessionManagerTimeOffsetTest, EarlierDatasetIngestNotifiesGlobalReframe) {
  PJ::SessionManager session;
  static_cast<void>(addDataset(session, "later.mcap", "/later", {5'000'000'000LL, 6'000'000'000LL}));
  session.setUseTimeOffset(true);
  ASSERT_EQ(session.globalTimeReference(), 5'000'000'000LL);

  QSignalSpy global_changes(&session, qOverload<>(&PJ::SessionManager::displayOffsetChanged));
  static_cast<void>(addDataset(session, "earlier.mcap", "/earlier", {2'000'000'000LL, 3'000'000'000LL}));

  EXPECT_EQ(session.globalTimeReference(), 2'000'000'000LL);
  EXPECT_EQ(global_changes.count(), 1) << "existing adapters must be told that the shared absolute/display frame moved";
}

// FileLoader's completion seam (FIX 3): plugin ingest commits straight to DataEngine
// via the C-ABI write host, BYPASSING SessionManager::commitChunks — so committing an
// earlier dataset that way leaves the memoized origin stale (no notify fires). The
// public refreshDatasetTimeReference seam re-scans the dataset and emits the reframe.
TEST(SessionManagerTimeOffsetTest, RefreshDatasetTimeReferenceReframesAfterDirectEngineCommit) {
  PJ::SessionManager session;
  static_cast<void>(addDataset(session, "later.mcap", "/later", {5'000'000'000LL, 6'000'000'000LL}));
  session.setUseTimeOffset(true);
  ASSERT_EQ(session.globalTimeReference(), 5'000'000'000LL);

  // Create an EARLIER dataset and commit its rows straight to DataEngine (mirrors the
  // plugin write host) — SessionManager never sees a commitChunks/notifyIngest, so its
  // origin memo stays at 5 s despite the earlier data being live.
  auto domain = session.dataEngine().createTimeDomain("earlier.mcap");
  ASSERT_TRUE(domain.has_value()) << domain.error();
  auto dataset = session.dataEngine().createDataset(
      PJ::DatasetDescriptor{.source_name = "earlier.mcap", .time_domain_id = *domain});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(*dataset, "/earlier", PJ::NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value()) << handle.error();
  writer.appendScalar(*handle, 2'000'000'000LL, 1.0);
  writer.appendScalar(*handle, 3'000'000'000LL, 1.0);
  ASSERT_FALSE(session.dataEngine().commitChunks(writer.flushAll()).empty());  // NOT session.commitChunks

  // The origin is still stale (the direct commit bypassed the notify path).
  QSignalSpy global_changes(&session, qOverload<>(&PJ::SessionManager::displayOffsetChanged));

  // The completion seam re-scans and reframes exactly once.
  session.refreshDatasetTimeReference(*dataset);
  EXPECT_EQ(session.globalTimeReference(), 2'000'000'000LL);
  EXPECT_EQ(global_changes.count(), 1) << "a plugin-direct-committed earlier file must reframe on the completion seam";

  // Idempotent: a second refresh with the origin unchanged emits nothing.
  session.refreshDatasetTimeReference(*dataset);
  EXPECT_EQ(global_changes.count(), 1) << "no spurious reframe when the origin did not move";
}

// FIX 3 removal face: a dataset with committed data removed via SessionManager::
// removeDataset must reframe when it was the origin owner (FileLoader routes its
// discard/error cleanups through this instead of dataEngine().removeDataset directly).
TEST(SessionManagerTimeOffsetTest, RemoveDatasetReframesWhenItOwnedTheOrigin) {
  PJ::SessionManager session;
  static_cast<void>(addDataset(session, "later.mcap", "/later", {5'000'000'000LL, 6'000'000'000LL}));
  const PJ::DatasetId earliest = addDataset(session, "earlier.mcap", "/earlier", {2'000'000'000LL, 3'000'000'000LL});
  session.setUseTimeOffset(true);
  ASSERT_EQ(session.globalTimeReference(), 2'000'000'000LL);

  QSignalSpy global_changes(&session, qOverload<>(&PJ::SessionManager::displayOffsetChanged));
  session.removeDataset(earliest);
  EXPECT_EQ(session.globalTimeReference(), 5'000'000'000LL) << "origin re-bases to the surviving later dataset";
  EXPECT_EQ(global_changes.count(), 1) << "dropping the origin owner must reframe surviving plots exactly once";
}

TEST(SessionManagerTimeOffsetTest, ObjectTopicEvictionRebasesOnlyWhenTheNumericOriginMoves) {
  PJ::SessionManager session;
  const auto make_object_dataset = [&session](std::string source, PJ::Timestamp stamp) {
    const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = std::move(source)});
    EXPECT_TRUE(dataset.has_value()) << dataset.error();
    if (!dataset.has_value()) {
      return PJ::ObjectTopicId{};
    }
    const auto topic = session.objectStore().registerTopic(
        PJ::ObjectTopicDescriptor{.dataset_id = *dataset, .topic_name = "/object", .metadata_json = "{}"});
    EXPECT_TRUE(topic.has_value()) << topic.error();
    if (!topic.has_value()) {
      return PJ::ObjectTopicId{};
    }
    EXPECT_TRUE(session.objectStore().pushOwned(*topic, stamp, std::vector<uint8_t>{1}).has_value());
    return *topic;
  };

  const PJ::ObjectTopicId earliest = make_object_dataset("early", 2'000'000'000LL);
  const PJ::ObjectTopicId survivor = make_object_dataset("middle", 5'000'000'000LL);
  const PJ::ObjectTopicId non_origin = make_object_dataset("late", 8'000'000'000LL);
  ASSERT_NE(earliest.id, 0U);
  ASSERT_NE(survivor.id, 0U);
  ASSERT_NE(non_origin.id, 0U);
  session.notifyIngest({}, /*live=*/false);
  session.setUseTimeOffset(true);
  ASSERT_EQ(session.globalTimeReference(), 2'000'000'000LL);

  QSignalSpy global_changes(&session, qOverload<>(&PJ::SessionManager::displayOffsetChanged));
  session.evictObjectTopics({non_origin});
  EXPECT_EQ(session.globalTimeReference(), 2'000'000'000LL);
  EXPECT_EQ(global_changes.count(), 0) << "removing a later object must not emit a spurious frame change";

  session.evictObjectTopics({earliest});
  EXPECT_EQ(session.globalTimeReference(), 5'000'000'000LL);
  EXPECT_EQ(global_changes.count(), 1) << "removing the object-only t0 owner must reframe surviving data exactly once";
}

TEST(SessionManagerTimeOffsetTest, ClearAllObjectsRebasesToSurvivingScalarData) {
  PJ::SessionManager session;
  const PJ::DatasetId scalar_dataset = addDataset(session, "scalar", "/scalar", {8'000'000'000LL, 9'000'000'000LL});
  const auto object_dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "object"});
  ASSERT_TRUE(object_dataset.has_value()) << object_dataset.error();
  const auto object_topic = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{.dataset_id = *object_dataset, .topic_name = "/object", .metadata_json = "{}"});
  ASSERT_TRUE(object_topic.has_value()) << object_topic.error();
  ASSERT_TRUE(session.objectStore().pushOwned(*object_topic, 3'000'000'000LL, std::vector<uint8_t>{1}).has_value());
  session.notifyIngest({}, /*live=*/false);

  session.setUseTimeOffset(true);
  ASSERT_EQ(session.globalTimeReference(), 3'000'000'000LL);
  QSignalSpy global_changes(&session, qOverload<>(&PJ::SessionManager::displayOffsetChanged));

  session.clearAllObjects();

  EXPECT_TRUE(session.objectStore().listTopics().empty());
  EXPECT_EQ(session.globalTimeReference(), 8'000'000'000LL);
  const auto scalar_range = session.datasetDisplayRange(scalar_dataset);
  ASSERT_TRUE(scalar_range.has_value());
  EXPECT_DOUBLE_EQ(scalar_range->min.value, 0.0);
  EXPECT_EQ(global_changes.count(), 1) << "the scalar survivor must become t0 after object data is cleared";
}

TEST(SessionManagerTimeOffsetTest, RetentionDoesNotMoveRelativeOffsetForward) {
  PJ::SessionManager session;
  auto dataset_or = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "stream"});
  ASSERT_TRUE(dataset_or.has_value()) << dataset_or.error();
  const PJ::DatasetId dataset = *dataset_or;

  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(dataset, "/stream/x", PJ::NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value()) << handle.error();
  const PJ::ScalarSeriesHandle stream_series = *handle;

  const auto append_stream_samples = [&](std::vector<PJ::Timestamp> timestamps) {
    for (const PJ::Timestamp timestamp : timestamps) {
      writer.appendScalar(stream_series, timestamp, 1.0);
    }
    ASSERT_FALSE(session.commitChunks(writer.flushAll()).empty());
  };

  append_stream_samples({0, 5'000'000'000LL, 10'000'000'000LL});

  session.setUseTimeOffset(true);
  EXPECT_EQ(session.displayOffset(dataset).value.count(), 0);

  const auto topics = session.dataEngine().listTopics(dataset);
  session.dataEngine().enforceRetention(5'000'000'000LL, dataset);
  session.notifyIngest(QVector<PJ::TopicId>(topics.begin(), topics.end()), /*live=*/true);

  EXPECT_EQ(session.displayOffset(dataset).value.count(), 0)
      << "streaming retention must not slide the relative-time origin forward";
  const auto retained_range = session.datasetDisplayRange(dataset);
  ASSERT_TRUE(retained_range.has_value());
  EXPECT_DOUBLE_EQ(retained_range->min.value, 5.0);
  EXPECT_DOUBLE_EQ(retained_range->max.value, 10.0);

  append_stream_samples({20'000'000'000LL});
  session.dataEngine().enforceRetention(5'000'000'000LL, dataset);
  session.notifyIngest(QVector<PJ::TopicId>(topics.begin(), topics.end()), /*live=*/true);

  EXPECT_EQ(session.displayOffset(dataset).value.count(), 0);
  const auto advanced_range = session.datasetDisplayRange(dataset);
  ASSERT_TRUE(advanced_range.has_value());
  EXPECT_DOUBLE_EQ(advanced_range->min.value, 20.0);
  EXPECT_DOUBLE_EQ(advanced_range->max.value, 20.0);
}

TEST(SessionManagerTimeOffsetTest, RefillInvalidatesPinnedRelativeOffset) {
  PJ::SessionManager session;
  const PJ::DatasetId dataset = addDataset(session, "reload", "/a", {5'000'000'000LL, 9'000'000'000LL});

  session.setUseTimeOffset(true);
  EXPECT_EQ(session.displayOffset(dataset).value.count(), 5'000'000'000LL);

  {
    PJ::RefillGuard guard = session.beginRefill(dataset);
    writeScalarSamples(session, dataset, "/a", {20'000'000'000LL, 25'000'000'000LL});
    guard.commit();
  }

  EXPECT_EQ(session.displayOffset(dataset).value.count(), 20'000'000'000LL);
  const auto range = session.datasetDisplayRange(dataset);
  ASSERT_TRUE(range.has_value());
  EXPECT_DOUBLE_EQ(range->min.value, 0.0);
  EXPECT_DOUBLE_EQ(range->max.value, 5.0);
}

TEST(SessionManagerTimeOffsetTest, DisplayOffsetChangedEmittedOnFrameFlip) {
  PJ::SessionManager session;
  int changes = 0;
  QObject::connect(
      &session, qOverload<>(&PJ::SessionManager::displayOffsetChanged), &session, [&changes]() { ++changes; });

  session.setUseTimeOffset(true);  // off -> on : one change
  EXPECT_EQ(changes, 1);
  session.setUseTimeOffset(true);  // already on : no emit
  EXPECT_EQ(changes, 1);
  session.setUseTimeOffset(false);  // on -> off : one change
  EXPECT_EQ(changes, 2);
}

TEST(SessionManagerTimeOffsetTest, EmptyDatasetHasZeroOffsetWhenEnabled) {
  PJ::SessionManager session;
  auto empty = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "empty.mcap"});
  ASSERT_TRUE(empty.has_value()) << empty.error();
  session.setUseTimeOffset(true);
  EXPECT_EQ(session.displayOffset(*empty).value.count(), 0);
}

TEST(SessionManagerSignalTest, LiveNotifyEmitsEvenWithoutScalarTopicIds) {
  PJ::SessionManager session;
  int emissions = 0;
  bool saw_live = false;
  QObject::connect(
      &session, &PJ::SessionManager::samplesIngested, &session, [&](const QVector<PJ::TopicId>& ids, bool live) {
        ++emissions;
        EXPECT_TRUE(ids.isEmpty());
        saw_live = live;
      });

  session.notifyIngest({}, /*live=*/false);
  EXPECT_EQ(emissions, 0);

  session.notifyIngest({}, /*live=*/true);
  EXPECT_EQ(emissions, 1);
  EXPECT_TRUE(saw_live);
}

// --- Scaffolding for the refill parser-slot cases ---------------------------
// A replacing reload deliberately re-binds same-named topics into the SAME
// ObjectTopicId, so the replacement's parser lands in the prior parser's slot.
// These helpers make "which parser is bound" observable: each parser stamps its
// own tag onto every object it decodes, so a test can assert that restored BYTES
// are read back by the parser that wrote them — not merely that some parser is
// registered.

constexpr std::string_view kTaggedSchema = "tagged";

class TaggedObjectParser : public PJ::MessageParserPluginBase {
 public:
  explicit TaggedObjectParser(std::string tag) {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kLog;
    handler.parse_object = [tag = std::move(tag)](
                               PJ::Timestamp ts, PJ::sdk::PayloadView payload) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      PJ::sdk::Log decoded;
      decoded.timestamp_ns = ts;
      decoded.name = tag;  // the decoding parser's identity, readable by the test
      decoded.message.assign(reinterpret_cast<const char*>(payload.bytes.data()), payload.bytes.size());
      return PJ::sdk::ObjectRecord{ts, PJ::sdk::BuiltinObject{decoded}};
    };
    registerSchemaHandler(kTaggedSchema, std::move(handler));
  }
};

// vtableWithCreate caches one `static` vtable per CreateFn instantiation, so the
// A and B factories MUST use distinct captureless lambdas — a shared one would
// latch a single create function (and thus a single tag) for both handles.
std::unique_ptr<PJ::MessageParserHandle> makeTaggedHandleA() {
  static constexpr const char* kManifest =
      R"({"id":"tagged-parser-a","name":"Tagged Parser A","version":"1.0.0","encoding":["mock"]})";
  auto handle = std::make_unique<PJ::MessageParserHandle>(PJ::MessageParserPluginBase::vtableWithCreate(
      []() noexcept -> void* { return new TaggedObjectParser("A"); }, kManifest));
  EXPECT_TRUE(handle->valid());
  EXPECT_TRUE(handle->bindSchema(kTaggedSchema, {}).has_value());
  return handle;
}

std::unique_ptr<PJ::MessageParserHandle> makeTaggedHandleB() {
  static constexpr const char* kManifest =
      R"({"id":"tagged-parser-b","name":"Tagged Parser B","version":"1.0.0","encoding":["mock"]})";
  auto handle = std::make_unique<PJ::MessageParserHandle>(PJ::MessageParserPluginBase::vtableWithCreate(
      []() noexcept -> void* { return new TaggedObjectParser("B"); }, kManifest));
  EXPECT_TRUE(handle->valid());
  EXPECT_TRUE(handle->bindSchema(kTaggedSchema, {}).has_value());
  return handle;
}

// Decode entry `index` of `topic` through whatever parser is CURRENTLY bound to
// it, exactly as a lazy reader would (per-use binding, parse under its mutex).
// Returns the decoding parser's tag and the bytes it saw; an empty tag means no
// parser is bound or the decode failed.
struct DecodedTag {
  std::string tag;
  std::string bytes;
};

DecodedTag decodeThroughBoundParser(PJ::SessionManager& session, PJ::ObjectTopicId topic, size_t index) {
  const PJ::SessionManager::ParserBinding binding = session.parserBindingForObjectTopic(topic);
  const auto entry = session.objectStore().at(topic, index);
  if (!binding || !entry.has_value()) {
    return {};
  }
  std::lock_guard<std::mutex> lock(*binding.mutex);
  auto decoded = binding.parser->parseObject(entry->timestamp, entry->payload);
  if (!decoded.has_value()) {
    return {};
  }
  const auto* log = decoded->object.get<PJ::sdk::Log>();
  return log != nullptr ? DecodedTag{log->name, log->message} : DecodedTag{};
}

// The parser instance a topic is bound to right now, as an identity (not merely
// "some parser exists"): the binding's keepalive IS the registered handle.
const void* boundParserHandle(PJ::SessionManager& session, PJ::ObjectTopicId topic) {
  return session.parserBindingForObjectTopic(topic).keepalive.get();
}

// A dataset with one object topic carrying `payload`, served by parser A.
struct TaggedObjectFixture {
  PJ::DatasetId dataset = 0;
  PJ::ObjectTopicId topic;
  const PJ::MessageParserHandle* parser_a = nullptr;
};

TaggedObjectFixture makeTaggedObjectDataset(PJ::SessionManager& session, std::vector<uint8_t> payload) {
  TaggedObjectFixture fixture;
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "reload.mcap"});
  EXPECT_TRUE(dataset.has_value()) << dataset.error();
  if (!dataset.has_value()) {
    return fixture;
  }
  fixture.dataset = *dataset;
  auto topic = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{.dataset_id = fixture.dataset, .topic_name = "/cam", .metadata_json = "{}"});
  EXPECT_TRUE(topic.has_value()) << topic.error();
  if (!topic.has_value()) {
    return fixture;
  }
  fixture.topic = *topic;
  EXPECT_TRUE(session.objectStore().pushOwned(fixture.topic, 250, std::move(payload)).has_value());
  auto handle_a = makeTaggedHandleA();
  fixture.parser_a = handle_a.get();
  session.registerObjectTopicParser(fixture.topic, std::move(handle_a));
  return fixture;
}

// --- beginRefill / RefillGuard: in-place transactional reload prep ---
// (Migrated from the former SessionManagerClearRefillTest, which covered the deleted
// clearDatasetForRefill. The "writes back into the same topic id" case it also had is
// now covered by CommitKeepsRefilledDataAndFreesSnapshot below.)

TEST(SessionManagerRefillGuardTest, DetachEmitsAboutToBeReplacedBeforeEmptyIngest) {
  PJ::SessionManager session;
  auto ds = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "reload.mcap"});
  ASSERT_TRUE(ds.has_value()) << ds.error();
  writeScalarSamples(session, *ds, "/a", {100, 200, 300});
  writeScalarSamples(session, *ds, "/b", {150, 250});
  const auto topics_before = session.dataEngine().listTopics(*ds);
  ASSERT_EQ(topics_before.size(), 2u);
  ASSERT_TRUE(session.datasetDisplayRange(*ds).has_value());

  // Record the emission order of the two signals (direct, same-thread connections).
  std::vector<QString> order;
  QObject::connect(&session, &PJ::SessionManager::datasetAboutToBeReplaced, &session, [&order](PJ::DatasetId) {
    order.emplace_back(u"about"_s);
  });
  QObject::connect(
      &session, &PJ::SessionManager::samplesIngested, &session,
      [&order](const QVector<PJ::TopicId>&, bool live) { order.emplace_back(live ? u"ingest_live"_s : u"ingest"_s); });

  {
    PJ::RefillGuard guard = session.beginRefill(*ds);
    // beginRefill's detach MUST emit datasetAboutToBeReplaced before the empty-state
    // (non-live) ingest notify — adapters drop cached TopicChunk* before the data moves.
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], u"about"_s);
    EXPECT_EQ(order[1], u"ingest"_s);

    // Dataset is now empty, but its topic ids stay registered (a refill reuses them).
    EXPECT_FALSE(session.datasetDisplayRange(*ds).has_value());
    EXPECT_EQ(session.dataEngine().listTopics(*ds), topics_before);
    guard.commit();  // keep the empty state (no refill here); avoid a rollback that would restore data
  }
}

// --- RefillGuard: the transactional in-place reload. Default outcome is ROLLBACK
//     (restore prior data); commit() keeps the refilled data. ---

TEST(SessionManagerRefillGuardTest, RollbackRestoresPriorScalarAndObjectData) {
  PJ::SessionManager session;
  auto ds = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "reload.mcap"});
  ASSERT_TRUE(ds.has_value()) << ds.error();
  writeScalarSamples(session, *ds, "/a", {100, 200, 300});
  auto object_topic = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{.dataset_id = *ds, .topic_name = "/cam", .metadata_json = "{}"});
  ASSERT_TRUE(object_topic.has_value()) << object_topic.error();
  ASSERT_TRUE(session.objectStore().pushOwned(*object_topic, 250, std::vector<uint8_t>{1, 2, 3}).has_value());
  const auto scalar_topics_before = session.dataEngine().listTopics(*ds);
  const auto object_topics_before = session.objectStore().listTopics(*ds);

  {
    PJ::RefillGuard guard = session.beginRefill(*ds);
    // In scope the dataset is detached (empty) but every id stays registered.
    EXPECT_FALSE(session.datasetDisplayRange(*ds).has_value());
    EXPECT_EQ(session.objectStore().entryCount(*object_topic), 0u);
    EXPECT_EQ(session.dataEngine().listTopics(*ds), scalar_topics_before);
    // guard dtor (no commit) rolls back here.
  }

  EXPECT_TRUE(session.datasetDisplayRange(*ds).has_value()) << "scalar data restored";
  EXPECT_EQ(session.objectStore().entryCount(*object_topic), 1u) << "object data restored";
  EXPECT_EQ(session.dataEngine().listTopics(*ds), scalar_topics_before) << "scalar ids stable";
  EXPECT_EQ(session.objectStore().listTopics(*ds), object_topics_before) << "object ids stable";
}

// Restoring the bytes is only half of "as if the reload never happened": the
// replacement registers ITS parser under the same stable ObjectTopicId, so a
// rollback that leaves that parser bound serves prior-era bytes through the
// replacement's schema (misdecoded images, rejected clouds).
TEST(SessionManagerRefillGuardTest, RollbackRestoresPriorObjectParserBinding) {
  PJ::SessionManager session;
  const TaggedObjectFixture fixture = makeTaggedObjectDataset(session, {'a', 'b', 'c'});
  ASSERT_EQ(decodeThroughBoundParser(session, fixture.topic, 0).tag, "A");
  // A consumer that bound before the reload; its mutex must stay THE serialization
  // point for parser A after rollback (a restore with a fresh mutex would let old
  // and new bindings enter the same non-thread-safe parser concurrently).
  const PJ::SessionManager::ParserBinding binding_before = session.parserBindingForObjectTopic(fixture.topic);

  {
    PJ::RefillGuard guard = session.beginRefill(fixture.dataset);
    // The replacement brings a different schema's parser for the same topic name,
    // which the refill re-binds into the SAME id.
    session.registerObjectTopicParser(fixture.topic, makeTaggedHandleB());
    ASSERT_TRUE(session.objectStore().pushOwned(fixture.topic, 900, std::vector<uint8_t>{'x', 'y'}).has_value());
    ASSERT_EQ(decodeThroughBoundParser(session, fixture.topic, 0).tag, "B");
    // Guard destroyed uncommitted: the refill failed, so it rolls back here.
  }

  EXPECT_EQ(boundParserHandle(session, fixture.topic), static_cast<const void*>(fixture.parser_a))
      << "the ACTIVE binding must be parser A again, the instance that wrote the restored bytes";
  const DecodedTag restored = decodeThroughBoundParser(session, fixture.topic, 0);
  EXPECT_EQ(restored.tag, "A") << "restored payloads must decode through the parser that wrote them";
  EXPECT_EQ(restored.bytes, "abc");
  const PJ::SessionManager::ParserBinding binding_after = session.parserBindingForObjectTopic(fixture.topic);
  EXPECT_EQ(binding_after.mutex.get(), binding_before.mutex.get())
      << "restore must bring back the ORIGINAL parse mutex, not a fresh one";
}

// The discard route FileLoader actually takes: the guard lives in a
// std::optional and is reset, so the snapshot must survive the move into it.
TEST(SessionManagerRefillGuardTest, DiscardedRefillRestoresPriorObjectParserBinding) {
  PJ::SessionManager session;
  const TaggedObjectFixture fixture = makeTaggedObjectDataset(session, {'a', 'b', 'c'});

  std::optional<PJ::RefillGuard> guard = session.beginRefill(fixture.dataset);
  session.registerObjectTopicParser(fixture.topic, makeTaggedHandleB());
  ASSERT_TRUE(session.objectStore().pushOwned(fixture.topic, 900, std::vector<uint8_t>{'x', 'y'}).has_value());
  guard.reset();  // user discarded the reload

  EXPECT_EQ(boundParserHandle(session, fixture.topic), static_cast<const void*>(fixture.parser_a));
  const DecodedTag restored = decodeThroughBoundParser(session, fixture.topic, 0);
  EXPECT_EQ(restored.tag, "A");
  EXPECT_EQ(restored.bytes, "abc") << "the moved guard must carry the object snapshot, not just the parser snapshot";
}

// The success path is the mirror image: a committed refill keeps ITS parser, so
// the snapshot must not be reinstalled (and must be released) at commit.
TEST(SessionManagerRefillGuardTest, CommitKeepsRefilledObjectParserBinding) {
  PJ::SessionManager session;
  const TaggedObjectFixture fixture = makeTaggedObjectDataset(session, {'a', 'b', 'c'});
  const void* parser_b = nullptr;

  {
    PJ::RefillGuard guard = session.beginRefill(fixture.dataset);
    auto handle_b = makeTaggedHandleB();
    parser_b = handle_b.get();
    session.registerObjectTopicParser(fixture.topic, std::move(handle_b));
    ASSERT_TRUE(session.objectStore().pushOwned(fixture.topic, 900, std::vector<uint8_t>{'x', 'y'}).has_value());
    guard.commit();
  }

  EXPECT_EQ(boundParserHandle(session, fixture.topic), parser_b) << "commit keeps the replacement's binding";
  const DecodedTag refilled = decodeThroughBoundParser(session, fixture.topic, 0);
  EXPECT_EQ(refilled.tag, "B");
  EXPECT_EQ(refilled.bytes, "xy");
}

// "Prior state" includes the ABSENCE of a parser: a topic served by a builtin
// codec must not come back bound to the replacement's plugin parser.
TEST(SessionManagerRefillGuardTest, RollbackErasesAParserTheRefillAddedToAnUnboundPriorTopic) {
  PJ::SessionManager session;
  auto ds = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "reload.mcap"});
  ASSERT_TRUE(ds.has_value()) << ds.error();
  auto topic = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{.dataset_id = *ds, .topic_name = "/cam", .metadata_json = "{}"});
  ASSERT_TRUE(topic.has_value()) << topic.error();
  ASSERT_TRUE(session.objectStore().pushOwned(*topic, 250, std::vector<uint8_t>{'a'}).has_value());
  ASSERT_EQ(boundParserHandle(session, *topic), nullptr) << "prior state: no parser bound";

  {
    PJ::RefillGuard guard = session.beginRefill(*ds);
    session.registerObjectTopicParser(*topic, makeTaggedHandleB());
    ASSERT_NE(boundParserHandle(session, *topic), nullptr);
  }

  EXPECT_EQ(boundParserHandle(session, *topic), nullptr);
}

// A topic destructively removed while the guard is active (e.g. "Remove all
// Datasets" mid-reload) must NOT get its parser resurrected by the rollback:
// ObjectStore::clear() resets id allocation, so a later unrelated topic can
// reuse the raw id — and would silently inherit the stale parser.
TEST(SessionManagerRefillGuardTest, RollbackSkipsParserRestoreForTopicsRemovedDuringRefill) {
  PJ::SessionManager session;
  const TaggedObjectFixture fixture = makeTaggedObjectDataset(session, {'a', 'b', 'c'});
  ASSERT_NE(boundParserHandle(session, fixture.topic), nullptr);

  {
    PJ::RefillGuard guard = session.beginRefill(fixture.dataset);
    session.clearAllObjects();  // destructive removal while the reload is in flight
    // Guard destroyed uncommitted: the refill failed, so it rolls back here.
  }

  EXPECT_EQ(boundParserHandle(session, fixture.topic), nullptr)
      << "no parser may be restored for a topic whose series is gone";

  // The store's id allocation restarted, so a fresh registration reuses the raw id.
  auto reused = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{.dataset_id = fixture.dataset, .topic_name = "/other", .metadata_json = "{}"});
  ASSERT_TRUE(reused.has_value()) << reused.error();
  ASSERT_EQ(reused->id, fixture.topic.id) << "premise: the raw id is reused";
  EXPECT_EQ(boundParserHandle(session, *reused), nullptr) << "the reused id must not inherit the stale parser";
}

TEST(SessionManagerRefillGuardTest, CommitKeepsRefilledDataAndFreesSnapshot) {
  PJ::SessionManager session;
  auto ds = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "reload.mcap"});
  ASSERT_TRUE(ds.has_value()) << ds.error();
  writeScalarSamples(session, *ds, "/a", {100, 200, 300});
  const auto topics_before = session.dataEngine().listTopics(*ds);
  ASSERT_EQ(topics_before.size(), 1u);
  const PJ::TopicId topic = topics_before.front();

  {
    PJ::RefillGuard guard = session.beginRefill(*ds);
    EXPECT_GT(guard.snapshotBytes(), 0u) << "prior data held aside";
    // Refill writes NEW samples into the SAME topic id.
    PJ::DataWriter writer = session.dataEngine().createWriter();
    const PJ::ScalarSeriesHandle handle{topic, 0};
    writer.appendScalar(handle, 1000, 1.0);
    writer.appendScalar(handle, 2000, 1.0);
    ASSERT_FALSE(session.commitChunks(writer.flushAll()).empty());
    guard.commit();  // keep the refilled data
    EXPECT_EQ(guard.snapshotBytes(), 0u) << "commit frees the held-aside prior data";
  }

  EXPECT_EQ(session.dataEngine().listTopics(*ds), topics_before) << "id stable across commit";
  {
    // Only the refilled data remains (1000..2000) — commit froze the prior snapshot
    // rather than restoring it, and the refill did not double-count.
    auto lock = session.dataEngine().lockEngine();
    const PJ::TopicStorage* storage = session.dataEngine().getTopicStorage(topic);
    ASSERT_NE(storage, nullptr);
    EXPECT_EQ(storage->timeMin(), 1000);
    EXPECT_EQ(storage->timeMax(), 2000);
  }
}

// Provenance shares the refill transaction's fate: COMMIT rewrote the
// dataset's content from a new source, so the provider record attached to the
// OLD content is detached structurally at the commit point; ROLLBACK restores
// the old content and keeps its record.
TEST(SessionManagerRefillGuardTest, CommitDetachesSourceRecordRollbackKeepsIt) {
  PJ::SessionManager session;
  auto ds = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "reload.mcap"});
  ASSERT_TRUE(ds.has_value()) << ds.error();
  writeScalarSamples(session, *ds, "/a", {100, 200});
  session.attachSourceRecord(
      *ds, PJ::SourceRecord{
               .provider_id = u"cloud-provider"_s,
               .source_identity = u"digest"_s,
               .descriptor_json = uR"({"d":1})"_s,
           });

  {
    PJ::RefillGuard guard = session.beginRefill(*ds);
    // Rollback (guard destroyed uncommitted).
  }
  ASSERT_NE(session.sourceRecord(*ds), nullptr) << "rollback restores the old content — its record stays";
  EXPECT_EQ(session.sourceRecord(*ds)->provider_id, u"cloud-provider"_s);

  {
    PJ::RefillGuard guard = session.beginRefill(*ds);
    guard.commit();
  }
  EXPECT_EQ(session.sourceRecord(*ds), nullptr) << "commit rewrote the content — the old record is stale provenance";
}

TEST(SessionManagerRefillGuardTest, ProcessorOutputsReplayBeforePruneAndKeepStableTopicIds) {
  PJ::SessionManager session;
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "reload.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  writeScalarSamples(session, *dataset, "/a", {100, 200});
  const PJ::TopicId input = session.dataEngine().listTopics(*dataset).front();
  auto filter =
      session.dataProcessorService().applyFilter(input, *dataset, std::make_unique<PassThroughProcessor>(), "/a[Pass]");
  ASSERT_TRUE(filter.has_value()) << filter.error();
  const PJ::TopicId output = filter->output_topic_id;
  ASSERT_EQ(session.dataEngine().getTopicStorage(output)->metadata().total_row_count, 2U);

  {
    PJ::RefillGuard guard = session.beginRefill(*dataset);
    EXPECT_TRUE(session.dataEngine().getTopicStorage(output)->empty());
    PJ::DataWriter writer = session.dataEngine().createWriter();
    writer.appendScalar(PJ::ScalarSeriesHandle{input, 0}, 1000, 10.0);
    writer.appendScalar(PJ::ScalarSeriesHandle{input, 0}, 2000, 20.0);
    ASSERT_FALSE(session.dataEngine().commitChunks(writer.flushAll()).empty());
    ASSERT_TRUE(guard.recomputeProcessors().has_value());
    guard.pruneVanishedTopics();
    guard.commit();
  }

  const auto topics = session.dataEngine().listTopics(*dataset);
  EXPECT_NE(std::find(topics.begin(), topics.end(), output), topics.end());
  const PJ::TopicStorage* output_storage = session.dataEngine().getTopicStorage(output);
  ASSERT_NE(output_storage, nullptr);
  EXPECT_EQ(output_storage->metadata().total_row_count, 2U);
  EXPECT_EQ(output_storage->timeMin(), 1000);
  EXPECT_EQ(output_storage->timeMax(), 2000);
}

TEST(SessionManagerRefillGuardTest, RollbackRetiresTopicsAddedByRefill) {
  PJ::SessionManager session;
  auto ds = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "reload.mcap"});
  ASSERT_TRUE(ds.has_value()) << ds.error();
  writeScalarSamples(session, *ds, "/a", {100, 200});
  auto object_before = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{.dataset_id = *ds, .topic_name = "/cam", .metadata_json = "{}"});
  ASSERT_TRUE(object_before.has_value()) << object_before.error();
  ASSERT_TRUE(session.objectStore().pushOwned(*object_before, 150, std::vector<uint8_t>{1}).has_value());
  const auto scalar_before = session.dataEngine().listTopics(*ds);
  const auto object_before_ids = session.objectStore().listTopics(*ds);

  {
    PJ::RefillGuard guard = session.beginRefill(*ds);
    // The failed refill adds a NEW scalar topic AND a NEW object topic.
    writeScalarSamples(session, *ds, "/b", {500});
    auto added = session.objectStore().registerTopic(
        PJ::ObjectTopicDescriptor{.dataset_id = *ds, .topic_name = "/cam2", .metadata_json = "{}"});
    ASSERT_TRUE(added.has_value()) << added.error();
    ASSERT_TRUE(session.objectStore().pushOwned(*added, 600, std::vector<uint8_t>{9}).has_value());
    // no commit -> rollback
  }

  EXPECT_EQ(session.dataEngine().listTopics(*ds), scalar_before) << "added scalar topic retired";
  EXPECT_EQ(session.objectStore().listTopics(*ds), object_before_ids) << "added object topic removed";
  EXPECT_EQ(session.objectStore().entryCount(*object_before), 1u) << "original object restored";
  EXPECT_TRUE(session.datasetDisplayRange(*ds).has_value()) << "original scalar restored";
}

TEST(SessionManagerRefillGuardTest, AboutToBeReplacedFiresOnConstructionAndRollback) {
  PJ::SessionManager session;
  auto ds = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "reload.mcap"});
  ASSERT_TRUE(ds.has_value()) << ds.error();
  writeScalarSamples(session, *ds, "/a", {100, 200});
  int about_count = 0;
  QObject::connect(&session, &PJ::SessionManager::datasetAboutToBeReplaced, &session, [&about_count](PJ::DatasetId) {
    ++about_count;
  });
  {
    PJ::RefillGuard guard = session.beginRefill(*ds);  // fires once (detach)
    // no commit -> rollback fires once more (drop partial chunk pointers)
  }
  EXPECT_EQ(about_count, 2) << "fires on construction (detach) and on rollback";
}

TEST(SessionManagerRefillGuardTest, BeginRefillOnEmptyDatasetIsSafeNoop) {
  PJ::SessionManager session;
  auto ds = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "empty.mcap"});
  ASSERT_TRUE(ds.has_value()) << ds.error();
  EXPECT_NO_THROW({
    PJ::RefillGuard guard = session.beginRefill(*ds);
    EXPECT_EQ(guard.snapshotBytes(), 0u);
  });  // dtor rolls back an empty snapshot -> no-op
  EXPECT_NO_THROW({
    PJ::RefillGuard guard = session.beginRefill(*ds);
    guard.commit();
  });
}

TEST(SessionManagerRefillGuardTest, PruneVanishedTopicsRetiresEmptyPriorScalarTopics) {
  PJ::SessionManager session;
  auto ds = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "reload.mcap"});
  ASSERT_TRUE(ds.has_value()) << ds.error();
  writeScalarSamples(session, *ds, "/a", {100, 200});
  writeScalarSamples(session, *ds, "/b", {150});
  writeScalarSamples(session, *ds, "/c", {300});
  const auto before = session.dataEngine().listTopics(*ds);
  ASSERT_EQ(before.size(), 3u);
  const PJ::TopicId topic_a = before[0];
  const PJ::TopicId topic_b = before[1];
  const PJ::TopicId topic_c = before[2];

  {
    PJ::RefillGuard guard = session.beginRefill(*ds);
    // The reloaded file has only /a and /b; the refill writes back into those two,
    // never /c, so /c stays empty (vanished).
    PJ::DataWriter writer = session.dataEngine().createWriter();
    writer.appendScalar(PJ::ScalarSeriesHandle{topic_a, 0}, 1000, 1.0);
    writer.appendScalar(PJ::ScalarSeriesHandle{topic_b, 0}, 1500, 1.0);
    ASSERT_FALSE(session.commitChunks(writer.flushAll()).empty());

    guard.pruneVanishedTopics();
    guard.commit();
  }

  const auto after = session.dataEngine().listTopics(*ds);
  EXPECT_EQ(after.size(), 2u) << "the vanished topic /c is retired";
  EXPECT_NE(std::find(after.begin(), after.end(), topic_a), after.end()) << "/a kept (refilled)";
  EXPECT_NE(std::find(after.begin(), after.end(), topic_b), after.end()) << "/b kept (refilled)";
  EXPECT_EQ(std::find(after.begin(), after.end(), topic_c), after.end()) << "/c retired (vanished)";
  EXPECT_TRUE(session.datasetDisplayRange(*ds).has_value());
}

TEST(SessionManagerRefillGuardTest, PruneVanishedTopicsRemovesEmptyPriorObjectTopics) {
  PJ::SessionManager session;
  auto ds = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "reload.mcap"});
  ASSERT_TRUE(ds.has_value()) << ds.error();
  auto cam_a = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{.dataset_id = *ds, .topic_name = "/cam/a", .metadata_json = "{}"});
  auto cam_b = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{.dataset_id = *ds, .topic_name = "/cam/b", .metadata_json = "{}"});
  ASSERT_TRUE(cam_a.has_value()) << cam_a.error();
  ASSERT_TRUE(cam_b.has_value()) << cam_b.error();
  ASSERT_TRUE(session.objectStore().pushOwned(*cam_a, 100, std::vector<uint8_t>{1}).has_value());
  ASSERT_TRUE(session.objectStore().pushOwned(*cam_b, 100, std::vector<uint8_t>{2}).has_value());
  ASSERT_EQ(session.objectStore().listTopics(*ds).size(), 2u);

  {
    PJ::RefillGuard guard = session.beginRefill(*ds);
    // The reloaded file has only /cam/a; the refill pushes to it, never /cam/b.
    ASSERT_TRUE(session.objectStore().pushOwned(*cam_a, 1000, std::vector<uint8_t>{3}).has_value());
    guard.pruneVanishedTopics();
    guard.commit();
  }

  const auto after = session.objectStore().listTopics(*ds);
  ASSERT_EQ(after.size(), 1u) << "the vanished object topic /cam/b is removed";
  EXPECT_EQ(after.front(), *cam_a) << "/cam/a kept (refilled)";
}

// beginRefill empties every object topic of the dataset, and prune then retires the
// ones the reloaded file did not rewrite. Marker topics are host-published, never
// file-backed, so a refill NEVER rewrites them — without the exemption a per-dataset
// Reload silently deletes every committed marker (the regression #464 introduced).
TEST(SessionManagerRefillGuardTest, PruneVanishedTopicsKeepsMarkerTopics) {
  PJ::SessionManager session;
  auto ds = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "reload.mcap"});
  ASSERT_TRUE(ds.has_value()) << ds.error();
  const std::string marker_topic = PJ::sdk::markerObjectTopicName("/sensor/value");
  auto cam = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{.dataset_id = *ds, .topic_name = "/cam/a", .metadata_json = "{}"});
  auto markers = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{.dataset_id = *ds, .topic_name = marker_topic, .metadata_json = "{}"});
  ASSERT_TRUE(cam.has_value()) << cam.error();
  ASSERT_TRUE(markers.has_value()) << markers.error();
  ASSERT_TRUE(session.objectStore().pushOwned(*cam, 100, std::vector<uint8_t>{1}).has_value());
  ASSERT_TRUE(session.objectStore().pushOwned(*markers, 0, std::vector<uint8_t>{2}).has_value());

  {
    PJ::RefillGuard guard = session.beginRefill(*ds);
    // The reloaded file rewrites its own object topic; nothing rewrites the markers.
    ASSERT_TRUE(session.objectStore().pushOwned(*cam, 1000, std::vector<uint8_t>{3}).has_value());
    guard.pruneVanishedTopics();
    guard.commit();
  }

  const auto after = session.objectStore().listTopics(*ds);
  EXPECT_NE(std::find(after.begin(), after.end(), *cam), after.end()) << "/cam/a kept (refilled)";
  EXPECT_NE(std::find(after.begin(), after.end(), *markers), after.end())
      << "the marker topic survives an empty refill — the producer is the host, not the file";
}

// An on-demand node's derived topic is host-published too: the refill never rewrites it, and its id
// is what the node's ResultHistory publishes into.
TEST(SessionManagerRefillGuardTest, PruneVanishedTopicsKeepsDerivedObjectTopics) {
  PJ::SessionManager session;
  auto ds = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "reload.mcap"});
  ASSERT_TRUE(ds.has_value()) << ds.error();
  auto cam = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{.dataset_id = *ds, .topic_name = "/cam/a", .metadata_json = "{}"});
  auto derived = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *ds,
          .topic_name = "owner/crop/cropped",
          .metadata_json = PJ::derivedTopicMetadata(PJ::sdk::BuiltinObjectType::kPointCloud)});
  ASSERT_TRUE(cam.has_value() && derived.has_value());
  ASSERT_TRUE(session.objectStore().pushOwned(*cam, 100, std::vector<uint8_t>{1}).has_value());
  ASSERT_TRUE(session.objectStore().pushOwned(*derived, 100, std::vector<uint8_t>{2}).has_value());

  {
    PJ::RefillGuard guard = session.beginRefill(*ds);
    ASSERT_TRUE(session.objectStore().pushOwned(*cam, 1000, std::vector<uint8_t>{3}).has_value());
    guard.pruneVanishedTopics();
    guard.commit();
  }

  const auto after = session.objectStore().listTopics(*ds);
  EXPECT_NE(std::find(after.begin(), after.end(), *derived), after.end()) << "the derived topic survives";
}

TEST(SessionManagerRefillGuardTest, MovedGuardRollsBackExactlyOnce) {
  // The guard is move-only and lives in std::optional<RefillGuard> in FileLoader;
  // the move must neutralize the source (null session_ + mark committed) so the
  // moved-from dtor no-ops. A bug there would roll back TWICE.
  PJ::SessionManager session;
  auto ds = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "reload.mcap"});
  ASSERT_TRUE(ds.has_value()) << ds.error();
  writeScalarSamples(session, *ds, "/a", {100, 200, 300});
  const auto topics_before = session.dataEngine().listTopics(*ds);

  int about_count = 0;
  QObject::connect(&session, &PJ::SessionManager::datasetAboutToBeReplaced, &session, [&about_count](PJ::DatasetId) {
    ++about_count;
  });
  {
    PJ::RefillGuard original = session.beginRefill(*ds);  // detach fires datasetAboutToBeReplaced (1)
    PJ::RefillGuard moved = std::move(original);          // moved-from `original` must become inert
    // Scope exit destroys `moved` first (rollback -> fires (2)), then the moved-from
    // `original` (must no-op). A double rollback would push about_count to 3.
  }
  EXPECT_EQ(about_count, 2) << "exactly one rollback despite the move (moved-from guard is inert)";
  EXPECT_TRUE(session.datasetDisplayRange(*ds).has_value()) << "data restored once";
  EXPECT_EQ(session.dataEngine().listTopics(*ds), topics_before) << "ids stable";
}

// --- SourceRecord: dataset-keyed provenance from provider plugins, and the
// record tier of resolveDatasetIdentity ---

TEST(SessionManagerSourceRecordTest, AttachQueryDetachRoundTrip) {
  PJ::SessionManager session;
  const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "cloud.mcap"});
  ASSERT_TRUE(dataset.has_value());

  EXPECT_EQ(session.sourceRecord(*dataset), nullptr) << "no record attached yet";

  session.attachSourceRecord(
      *dataset, PJ::SourceRecord{
                    .provider_id = u"mcap-cloud"_s,
                    .source_identity = u"sha256:abc"_s,
                    .descriptor_json = u"{\"key\":\"run1.mcap\"}"_s,
                });
  const PJ::SourceRecord* record = session.sourceRecord(*dataset);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->provider_id, u"mcap-cloud"_s);
  EXPECT_EQ(record->source_identity, u"sha256:abc"_s);
  EXPECT_EQ(record->descriptor_json, u"{\"key\":\"run1.mcap\"}"_s);

  session.detachSourceRecord(*dataset);
  EXPECT_EQ(session.sourceRecord(*dataset), nullptr);
  session.detachSourceRecord(*dataset);  // idempotent
  EXPECT_EQ(session.sourceRecord(9999), nullptr) << "unknown dataset has no record";
}

TEST(SessionManagerSourceRecordTest, AttachOverwritesExistingRecord) {
  PJ::SessionManager session;
  const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "cloud.mcap"});
  ASSERT_TRUE(dataset.has_value());

  session.attachSourceRecord(*dataset, PJ::SourceRecord{u"provider-a"_s, u"identity-a"_s, u"{\"a\":1}"_s});
  session.attachSourceRecord(*dataset, PJ::SourceRecord{u"provider-b"_s, u"identity-b"_s, u"{\"b\":2}"_s});

  const PJ::SourceRecord* record = session.sourceRecord(*dataset);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->provider_id, u"provider-b"_s);
  EXPECT_EQ(record->source_identity, u"identity-b"_s);
  EXPECT_EQ(record->descriptor_json, u"{\"b\":2}"_s);
}

TEST(SessionManagerSourceRecordTest, RemoveDatasetDropsItsSourceRecord) {
  PJ::SessionManager session;
  const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "gone.mcap"});
  ASSERT_TRUE(dataset.has_value());
  session.attachSourceRecord(*dataset, PJ::SourceRecord{u"p"_s, u"i"_s, u"{}"_s});
  ASSERT_NE(session.sourceRecord(*dataset), nullptr);

  session.removeDataset(*dataset);
  EXPECT_EQ(session.sourceRecord(*dataset), nullptr)
      << "dataset removal must invalidate its provenance record (same lifecycle as the source path)";
}

TEST(SessionManagerSourceRecordTest, RecordTierBeatsPathAndSourceName) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path_a = dir.filePath(u"a/run.mcap"_s);
  const QString path_b = dir.filePath(u"b/run.mcap"_s);
  ASSERT_TRUE(QDir().mkpath(dir.filePath(u"a"_s)));
  ASSERT_TRUE(QDir().mkpath(dir.filePath(u"b"_s)));
  ASSERT_TRUE(QFile(path_a).open(QIODevice::WriteOnly));
  ASSERT_TRUE(QFile(path_b).open(QIODevice::WriteOnly));

  PJ::SessionManager session;
  const auto a = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "run.mcap"});
  const auto b = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "run.mcap"});
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  session.setDatasetSourcePath(*a, path_a);
  session.setDatasetSourcePath(*b, path_b);
  const PJ::SourceRecord record{u"mcap-cloud"_s, u"sha256:abc"_s, u"{\"key\":\"run.mcap\"}"_s};
  session.attachSourceRecord(*a, record);

  // The saved source/path qualifiers point at B, but the record names A: the
  // record tier runs BEFORE the path/name tiers, so A wins.
  const PJ::DatasetIdentityResolution resolved = session.resolveDatasetIdentity(999, u"run.mcap"_s, path_b, record);
  ASSERT_TRUE(resolved.id.has_value());
  EXPECT_EQ(*resolved.id, a.value());
  EXPECT_FALSE(resolved.ambiguous);
}

TEST(SessionManagerSourceRecordTest, ExactIdVetoedWhenSuppliedRecordDisagrees) {
  PJ::SessionManager session;
  const auto a = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "a.mcap"});
  const auto b = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "b.mcap"});
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  const PJ::SourceRecord record_a{u"p"_s, u"identity-a"_s, u"{\"a\":1}"_s};
  const PJ::SourceRecord record_b{u"p"_s, u"identity-b"_s, u"{\"b\":2}"_s};
  session.attachSourceRecord(*a, record_a);
  session.attachSourceRecord(*b, record_b);

  // saved_id names A (and A's source label matches), but the supplied record is
  // B's: every supplied qualifier must agree for exact-id trust, so tier 1 is
  // vetoed and the unique record match resolves to B instead.
  const PJ::DatasetIdentityResolution resolved = session.resolveDatasetIdentity(*a, u"a.mcap"_s, {}, record_b);
  ASSERT_TRUE(resolved.id.has_value());
  EXPECT_EQ(*resolved.id, b.value());
  EXPECT_FALSE(resolved.ambiguous);
}

TEST(SessionManagerSourceRecordTest, RecordMatchRequiresByteEqualDescriptor) {
  PJ::SessionManager session;
  const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "cloud.mcap"});
  ASSERT_TRUE(dataset.has_value());
  session.attachSourceRecord(*dataset, PJ::SourceRecord{u"p"_s, u"same-identity"_s, u"{\"topics\":[\"/imu\"]}"_s});

  // Same (provider, identity) fast path, but the full canonical descriptor
  // differs: the identity digest alone must NOT be trusted.
  const PJ::DatasetIdentityResolution resolved = session.resolveDatasetIdentity(
      0, {}, {}, PJ::SourceRecord{u"p"_s, u"same-identity"_s, u"{\"topics\":[\"/gps\"]}"_s});
  EXPECT_FALSE(resolved.id.has_value());
  EXPECT_FALSE(resolved.ambiguous) << "a descriptor mismatch is a non-match, not ambiguity";
}

TEST(SessionManagerSourceRecordTest, MultipleConfirmedRecordMatchesAreAmbiguous) {
  PJ::SessionManager session;
  const auto first = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "one.mcap"});
  const auto second = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "two.mcap"});
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  const PJ::SourceRecord record{u"p"_s, u"same"_s, u"{\"same\":true}"_s};
  session.attachSourceRecord(*first, record);
  session.attachSourceRecord(*second, record);

  const PJ::DatasetIdentityResolution resolved = session.resolveDatasetIdentity(0, {}, {}, record);
  EXPECT_FALSE(resolved.id.has_value());
  EXPECT_TRUE(resolved.ambiguous) << "multiple confirmed record matches must never be guessed between";
}

// Regression pin: existing (record-less) callers must resolve identically —
// both through the 3-argument form and through the overload with an empty
// record — and an ATTACHED record must not perturb a record-less query.
TEST(SessionManagerSourceRecordTest, EmptyRecordQueryKeepsExistingResolution) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = dir.filePath(u"run.mcap"_s);
  ASSERT_TRUE(QFile(path).open(QIODevice::WriteOnly));

  PJ::SessionManager session;
  const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "run.mcap"});
  ASSERT_TRUE(dataset.has_value());
  session.setDatasetSourcePath(*dataset, path);
  session.attachSourceRecord(*dataset, PJ::SourceRecord{u"p"_s, u"i"_s, u"{}"_s});

  const PJ::DatasetIdentityResolution three_arg = session.resolveDatasetIdentity(999, u"run.mcap"_s, path);
  const PJ::DatasetIdentityResolution four_arg =
      session.resolveDatasetIdentity(999, u"run.mcap"_s, path, PJ::SourceRecord{});
  ASSERT_TRUE(three_arg.id.has_value());
  ASSERT_TRUE(four_arg.id.has_value());
  EXPECT_EQ(*three_arg.id, dataset.value());
  EXPECT_EQ(*four_arg.id, dataset.value());
  EXPECT_FALSE(three_arg.ambiguous);
  EXPECT_FALSE(four_arg.ambiguous);
}

TEST(SessionManagerMergeTest, MergeInvalidatesContributorRecordsIncludingAnchor) {
  PJ::SessionManager session;
  const PJ::DatasetId anchor = addDataset(session, "anchor.mcap", "/a", {100, 200});
  const PJ::DatasetId source1 = addDataset(session, "src1.mcap", "/b", {150, 250});
  const PJ::DatasetId source2 = addDataset(session, "src2.mcap", "/c", {160, 260});
  session.attachSourceRecord(anchor, PJ::SourceRecord{u"p"_s, u"anchor"_s, u"{\"a\":1}"_s});
  session.attachSourceRecord(source1, PJ::SourceRecord{u"p"_s, u"src1"_s, u"{\"s\":1}"_s});
  session.attachSourceRecord(source2, PJ::SourceRecord{u"p"_s, u"src2"_s, u"{\"s\":2}"_s});

  const auto report = session.mergeDatasets(
      anchor, {PJ::DatasetMergeSource{.dataset_id = source1, .raw_shift_ns = 0},
               PJ::DatasetMergeSource{.dataset_id = source2, .raw_shift_ns = 0}});
  ASSERT_TRUE(report.has_value()) << "the merge itself must succeed for this test to mean anything";

  // Replaying any ONE contributor's descriptor cannot recreate the merged
  // result, so every contributor's provenance — the anchor's included — is gone.
  EXPECT_EQ(session.sourceRecord(anchor), nullptr);
  EXPECT_EQ(session.sourceRecord(source1), nullptr);
  EXPECT_EQ(session.sourceRecord(source2), nullptr);
}

TEST(SessionManagerMergeTest, RejectedMergeKeepsSourceRecords) {
  PJ::SessionManager session;
  const PJ::DatasetId anchor = addDataset(session, "anchor.mcap", "/a", {100, 200});
  session.attachSourceRecord(anchor, PJ::SourceRecord{u"p"_s, u"anchor"_s, u"{\"a\":1}"_s});

  // A self-source merge is rejected by the engine up front: nothing mutated, so
  // provenance must survive.
  const auto report = session.mergeDatasets(anchor, {PJ::DatasetMergeSource{.dataset_id = anchor, .raw_shift_ns = 0}});
  ASSERT_FALSE(report.has_value());
  EXPECT_NE(session.sourceRecord(anchor), nullptr);
}

// --- Toolbox bulk-import lifecycle (hoisted from MainWindow, #470) ---

TEST(SessionManagerIngestTest, BeginUpdateEndTracksActiveSetAndEmits) {
  PJ::SessionManager session;
  EXPECT_FALSE(session.hasActiveIngests());

  int began = 0;
  int progressed = 0;
  int ended = 0;
  QObject::connect(
      &session, &PJ::SessionManager::ingestBegan, &session,
      [&](PJ::IngestToken token, const QString& label, quint64 total) {
        ++began;
        EXPECT_EQ(token.dataset_id, 7U);
        EXPECT_NE(token.id, 0U);
        EXPECT_EQ(label, u"Cloud import"_s);
        EXPECT_EQ(total, 100U);
      });
  QObject::connect(
      &session, &PJ::SessionManager::ingestProgressed, &session,
      [&](PJ::IngestToken token, quint64 current, quint64 total) {
        ++progressed;
        EXPECT_EQ(token.dataset_id, 7U);
        EXPECT_EQ(current, 50U);
        EXPECT_EQ(total, 200U);
      });
  QObject::connect(
      &session, &PJ::SessionManager::ingestEnded, &session, [&](PJ::IngestToken token, PJ::IngestOutcome outcome) {
        ++ended;
        EXPECT_EQ(token.dataset_id, 7U);
        EXPECT_EQ(outcome, PJ::IngestOutcome::kCompleted);
      });

  const PJ::IngestToken token = session.beginIngest(7, u"Cloud import"_s, 100, /*cancellable=*/false, nullptr);
  EXPECT_EQ(began, 1);
  EXPECT_TRUE(session.hasActiveIngests());
  EXPECT_TRUE(session.ingestActive(7));
  EXPECT_FALSE(session.ingestActive(8));
  ASSERT_EQ(session.activeIngests().size(), 1U);
  const auto entry = session.activeIngests().at(7);
  EXPECT_EQ(entry.label, u"Cloud import"_s);
  EXPECT_EQ(entry.current, 0U);
  EXPECT_EQ(entry.total, 100U);
  EXPECT_EQ(entry.token.id, token.id);

  session.updateIngest(token, 50, 200);  // total may be refined mid-flight
  EXPECT_EQ(progressed, 1);
  EXPECT_EQ(session.activeIngests().at(7).current, 50U);
  EXPECT_EQ(session.activeIngests().at(7).total, 200U);

  session.endIngest(token, PJ::IngestOutcome::kCompleted);
  EXPECT_EQ(ended, 1);
  EXPECT_FALSE(session.hasActiveIngests());
  EXPECT_FALSE(session.ingestActive(7));
  session.endIngest(token, PJ::IngestOutcome::kCompleted);  // idempotent: an already-ended token is silent
  EXPECT_EQ(ended, 1);
}

TEST(SessionManagerIngestTest, UpdateIngestPublishesDatasetTopicsViaNotifyIngest) {
  PJ::SessionManager session;
  const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "cloud.mcap"});
  ASSERT_TRUE(dataset.has_value());
  writeScalarSamples(session, *dataset, "/imu/x", {100, 200});
  const auto expected_topics = session.dataEngine().listTopics(*dataset);
  ASSERT_FALSE(expected_topics.empty());

  int publications = 0;
  QObject::connect(
      &session, &PJ::SessionManager::samplesIngested, &session, [&](const QVector<PJ::TopicId>& ids, bool live) {
        ++publications;
        EXPECT_FALSE(live) << "bulk-import progress publishes non-live";
        EXPECT_EQ(std::vector<PJ::TopicId>(ids.begin(), ids.end()), expected_topics);
      });

  const PJ::IngestToken token = session.beginIngest(*dataset, u"import"_s, 0, /*cancellable=*/false, nullptr);
  session.updateIngest(token, 1, 2);
  EXPECT_EQ(publications, 1) << "updateIngest must publish the dataset's topics through notifyIngest";
}

TEST(SessionManagerIngestTest, UpdateIngestForUntrackedDatasetStillPublishesButStaysSilent) {
  PJ::SessionManager session;
  const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "cloud.mcap"});
  ASSERT_TRUE(dataset.has_value());
  writeScalarSamples(session, *dataset, "/imu/x", {100, 200});

  int publications = 0;
  int progressed = 0;
  QObject::connect(&session, &PJ::SessionManager::samplesIngested, &session, [&](const QVector<PJ::TopicId>&, bool) {
    ++publications;
  });
  QObject::connect(&session, &PJ::SessionManager::ingestProgressed, &session, [&](PJ::IngestToken, quint64, quint64) {
    ++progressed;
  });

  // No beginIngest, so the token names an ingest the session never tracked: the
  // data publication must STILL happen (a lifecycle ordering slip must never
  // drop flushed rows), but no lifecycle signal fires.
  session.updateIngest(PJ::IngestToken{.id = 999, .dataset_id = *dataset}, 1, 2);
  EXPECT_EQ(publications, 1);
  EXPECT_EQ(progressed, 0);
  EXPECT_FALSE(session.hasActiveIngests());
}

TEST(SessionManagerMetadataTest, SetReplaceAndClear) {
  PJ::SessionManager session;
  const PJ::DatasetId dataset = 7;

  EXPECT_EQ(session.datasetMetadata(dataset), nullptr);
  ASSERT_TRUE(session.setDatasetMetadata(dataset, uR"({"messages": 12})"_s, u"loader_a"_s).has_value());
  const PJ::DatasetMetadata* stored = session.datasetMetadata(dataset);
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(stored->plugin_id, u"loader_a"_s);
  EXPECT_TRUE(stored->json.contains(u"messages"_s));

  // Replace-whole-document, then clear via the two spellings of "nothing".
  ASSERT_TRUE(session.setDatasetMetadata(dataset, uR"({"messages": 13})"_s, u"loader_b"_s).has_value());
  EXPECT_EQ(session.datasetMetadata(dataset)->plugin_id, u"loader_b"_s);
  ASSERT_TRUE(session.setDatasetMetadata(dataset, u"{}"_s, u"loader_b"_s).has_value());
  EXPECT_EQ(session.datasetMetadata(dataset), nullptr);
  ASSERT_TRUE(session.setDatasetMetadata(dataset, uR"({"again": 1})"_s, u"loader_b"_s).has_value());
  ASSERT_TRUE(session.setDatasetMetadata(dataset, QString{}, u"loader_b"_s).has_value());
  EXPECT_EQ(session.datasetMetadata(dataset), nullptr);
}

TEST(SessionManagerMetadataTest, RejectsOutOfBoundsDocumentsAndKeepsPrevious) {
  PJ::SessionManager session;
  const PJ::DatasetId dataset = 7;
  ASSERT_TRUE(session.setDatasetMetadata(dataset, uR"({"keep": true})"_s, u"loader"_s).has_value());

  EXPECT_FALSE(session.setDatasetMetadata(dataset, u"not json"_s, u"loader"_s).has_value());
  EXPECT_FALSE(session.setDatasetMetadata(dataset, u"[1, 2]"_s, u"loader"_s).has_value());

  QString deep = u"1"_s;
  for (int level = 0; level < 33; ++level) {
    deep = uR"({"n": %1})"_s.arg(deep);
  }
  EXPECT_FALSE(session.setDatasetMetadata(dataset, deep, u"loader"_s).has_value());

  QStringList many;
  for (int index = 0; index < 16400; ++index) {
    many << QString::number(index);
  }
  const QString wide = uR"({"values": [%1]})"_s.arg(many.join(u","_s));
  EXPECT_FALSE(session.setDatasetMetadata(dataset, wide, u"loader"_s).has_value());

  const QString huge = uR"({"blob": "%1"})"_s.arg(QString(1024 * 1024, u'x'));
  EXPECT_FALSE(session.setDatasetMetadata(dataset, huge, u"loader"_s).has_value());

  // Every rejection kept the previous document.
  ASSERT_NE(session.datasetMetadata(dataset), nullptr);
  EXPECT_TRUE(session.datasetMetadata(dataset)->json.contains(u"keep"_s));
}

TEST(SessionManagerMetadataTest, RemoveDatasetErasesMetadata) {
  PJ::SessionManager session;
  const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "meta.mcap"});
  ASSERT_TRUE(dataset.has_value());
  ASSERT_TRUE(session.setDatasetMetadata(*dataset, uR"({"a": 1})"_s, u"loader"_s).has_value());
  session.removeDataset(*dataset);
  EXPECT_EQ(session.datasetMetadata(*dataset), nullptr);
}

TEST(SessionManagerMetadataTest, RefillCommitClearsMetadataRollbackKeepsIt) {
  PJ::SessionManager session;
  const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "refill.mcap"});
  ASSERT_TRUE(dataset.has_value());
  ASSERT_TRUE(session.setDatasetMetadata(*dataset, uR"({"old": true})"_s, u"loader"_s).has_value());

  {
    PJ::RefillGuard rollback_guard = session.beginRefill(*dataset);
    // Guard destroyed uncommitted: the reload failed/discarded — the prior
    // content came back, and its metadata must still describe it.
  }
  EXPECT_NE(session.datasetMetadata(*dataset), nullptr);

  PJ::RefillGuard commit_guard = session.beginRefill(*dataset);
  commit_guard.commit();
  // Committed refill: new content, stale document cleared (the new load's
  // document, if any, is published at its own commit seam).
  EXPECT_EQ(session.datasetMetadata(*dataset), nullptr);
}

}  // namespace

TEST(SessionManagerTimeTest, DisplayTimeForSourceHonoursCurrentOffsetAndRejectsUnknownDataset) {
  PJ::SessionManager session;
  auto domain = session.dataEngine().createTimeDomain("shifted");
  ASSERT_TRUE(domain.has_value());
  auto dataset = session.dataEngine().createDataset(
      PJ::DatasetDescriptor{.source_name = "shifted.mcap", .time_domain_id = *domain});
  ASSERT_TRUE(dataset.has_value());

  EXPECT_FALSE(session.displayTimeForSource(9999, 5'000'000'000LL).has_value());
  EXPECT_FALSE(session.displayTimeForSource(0, 5'000'000'000LL).has_value());

  session.dataEngine().setDisplayOffset(*domain, 2'000'000'000LL);
  const auto display = session.displayTimeForSource(*dataset, 5'000'000'000LL);
  ASSERT_TRUE(display.has_value());
  EXPECT_DOUBLE_EQ(display->value, 3.0);
}

// A preview belongs to the plugin instance that made it (OWN-08): ending the previews
// an instance lists removes those ephemeral transforms and marker rules in both
// engines and repaints once. A persistent or unknown id in the list is skipped, and an
// unlisted preview stays.
TEST(SessionManagerTest, RemoveEphemeralsEndsOnlyTheListedPreviews) {
  PJ::SessionManager session;
  const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "run.mcap"});
  ASSERT_TRUE(dataset.has_value());
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(*dataset, "/sensor", PJ::NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value());
  writer.appendScalar(*handle, 100, 1.0);
  ASSERT_FALSE(session.commitChunks(writer.flushAll()).empty());

  struct Recipe {
    std::string owner;
    std::string id;
    bool ephemeral;
  };
  const std::vector<Recipe> recipes{{"a", "preview", true}, {"a", "kept", false}, {"b", "preview", true}};
  for (const Recipe& recipe : recipes) {
    const std::string script = "-- pj-script: luau\nreturn { id=\"" + recipe.id + "\", name=\"" + recipe.id +
                               "\", output=\"same\",\n"
                               "  create = function(p) return { calculate = function(t, v) return v end } end }\n";
    ASSERT_TRUE(
        session.dataProcessorService()
            .upsertTransform(
                recipe.owner, recipe.id, {"/sensor"}, {recipe.owner + "_" + recipe.id}, script, "{}", recipe.ephemeral)
            .has_value());
    PJ::BatchEngine::GeneratorRecipe rule;
    rule.id = recipe.owner + "/" + recipe.id;
    rule.dataset_id = *dataset;
    if (!recipe.ephemeral) {
      rule.outputs = {std::string(PJ::sdk::kGlobalMarkerTopic)};
    }
    rule.script = "createMarker(100)";
    rule.ephemeral = recipe.ephemeral;
    ASSERT_TRUE(session.batchEngine().upsertGenerator(rule).has_value());
  }
  QSignalSpy markers_changed(&session, &PJ::SessionManager::markersChanged);

  session.removeEphemerals({"a/preview", "a/kept", "a/gone"}, {"a/preview", "a/kept", "a/gone"}, {});

  EXPECT_EQ(session.dataProcessorService().transformIdsForPlugin("a"), std::vector<std::string>{"kept"});
  EXPECT_EQ(session.dataProcessorService().transformIdsForPlugin("b"), std::vector<std::string>{"preview"});
  EXPECT_EQ(session.batchEngine().firstBinding("a/preview"), nullptr);
  EXPECT_NE(session.batchEngine().firstBinding("a/kept"), nullptr);
  EXPECT_NE(session.batchEngine().firstBinding("b/preview"), nullptr);
  EXPECT_EQ(markers_changed.count(), 1) << "the removed preview's markers are repainted away once";
}

// ---- The on-demand engine through the session's dataset life cycle (ARCHITECTURE §8, §13) -------------------

namespace PJ {
namespace {

using OnDemandStatus = NodeScheduler::NodeState::Status;

constexpr Timestamp kOnDemandSec = 1'000'000'000;
constexpr const char* kCloudMetadata = R"({"builtin_object_type":"kPointCloud"})";

RecipeGraph::Recipe cropRecipe(std::string id = "crop", std::string input = "cloud") {
  return test::onDemandRecipe(
      std::move(id), {std::move(input)},
      {{"cropped", scripting::OutputType::kObject, sdk::BuiltinObjectType::kPointCloud}});
}

/// A session with the dataset `log` holding the cloud topic `cloud`, its on-demand engine running the fake.
struct OnDemandSession {
  OnDemandSession() {
    OnDemandService::setRunForTest(fake.function());
    session = std::make_unique<SessionManager>();
    OnDemandService::setRunForTest({});
    ds = addDataset("log");
    cloud = addCloud(ds, "cloud");
    QObject::connect(session.get(), &SessionManager::objectTopicsChanged, [this] { ++topics_changed; });
    QObject::connect(session.get(), &SessionManager::derivedEntriesPublished, [this] { ++published; });
    QObject::connect(
        session.get(), &SessionManager::derivedStateChanged,
        [this](const QString& key, const std::vector<ObjectTopicId>& topics) {
          state_changes.emplace_back(key.toStdString(), topics);
        });
  }

  DatasetId addDataset(const std::string& name) {
    return session->dataEngine().createDataset(DatasetDescriptor{.source_name = name}).value();
  }
  ObjectTopicId addCloud(DatasetId dataset, const std::string& name) {
    return session->objectStore().registerTopic({dataset, name, kCloudMetadata}).value();
  }
  void push(ObjectTopicId topic, Timestamp t, size_t points = 4) {
    const auto bytes = serializeBuiltinObject(sdk::BuiltinObject(test::makeCloud(points))).value();
    ASSERT_TRUE(session->objectStore().pushOwned(topic, t, bytes).has_value());
  }
  OnDemandService& service() {
    return session->onDemandService();
  }
  void install(RecipeGraph::Recipe recipe) {
    const auto installed = service().install(std::move(recipe));
    ASSERT_TRUE(installed.has_value()) << installed.error();
  }
  /// Runs what is posted (a resume), then publishes everything submitted.
  void settle() {
    QCoreApplication::processEvents();
    session->enginePool().waitIdle();
  }
  /// Waits, without running the event loop, until a computed run waits for its publication.
  void waitComputedUndrained() {
    for (int spin = 0; spin < 5000 && session->enginePool().pendingPublishes() == 0; ++spin) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(session->enginePool().pendingPublishes(), 1u);
  }
  [[nodiscard]] std::optional<NodeScheduler::NodeState> state(const std::string& key = "owner/crop") {
    return service().state(key);
  }
  [[nodiscard]] std::optional<ObjectTopicId> topic(const std::string& name, DatasetId dataset = 0) const {
    return session->objectStore().findTopic(dataset != 0 ? dataset : ds, name);
  }
  [[nodiscard]] std::vector<Timestamp> ring(const std::string& name, DatasetId dataset = 0) const {
    const auto found = topic(name, dataset);
    return found ? test::timestampsOf(session->objectStore(), *found) : std::vector<Timestamp>{};
  }
  [[nodiscard]] std::optional<SequentialUID> shownUid(const std::string& name, Timestamp t) const {
    const auto found = topic(name);
    const auto entry = found ? session->objectStore().latestEntryIdAt(*found, t) : std::nullopt;
    return entry ? std::optional<SequentialUID>(entry->uid) : std::nullopt;
  }
  [[nodiscard]] uint32_t shownWidth(const std::string& name, Timestamp t) const {
    return test::decodeAt<sdk::PointCloud>(session->objectStore(), topic(name).value(), t).width;
  }

  test::FakeRun fake;  ///< outlives the session, whose pool joins the runs
  std::unique_ptr<SessionManager> session;
  DatasetId ds = 0;
  ObjectTopicId cloud;
  int topics_changed = 0;
  int published = 0;
  std::vector<std::pair<std::string, std::vector<ObjectTopicId>>> state_changes;
};

// ASY-04, CACHE-01: a reload drops the results at its start, serves nothing while the dataset refills, and
// recomputes at the parked cursor from the refilled data once it committed.
TEST(SessionManagerTest, ReloadBumpsTheEpochAndRecomputesAtTheParkedCursor) {
  OnDemandSession fx;
  fx.push(fx.cloud, 20 * kOnDemandSec, 4);
  fx.install(cropRecipe());
  fx.service().requestAt("owner/crop", 20 * kOnDemandSec);
  fx.settle();
  ASSERT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{20 * kOnDemandSec}));

  {
    RefillGuard guard = fx.session->beginRefill(fx.ds);
    EXPECT_TRUE(fx.ring("owner/crop/cropped").empty()) << "the old meaning's result is gone";
    EXPECT_EQ(fx.state()->status, OnDemandStatus::kPending);
    fx.push(fx.cloud, 20 * kOnDemandSec, 6);
    fx.service().requestAt("owner/crop", 20 * kOnDemandSec);
    fx.settle();
    EXPECT_EQ(fx.fake.started(), 1) << "nothing runs on the partly refilled data";
    EXPECT_EQ(fx.state()->status, OnDemandStatus::kPending);
    guard.commit();
  }
  EXPECT_EQ(fx.fake.started(), 1) << "the commit resumes from the event loop";
  fx.settle();
  EXPECT_EQ(fx.fake.finished(), 2);
  EXPECT_EQ(fx.state()->status, OnDemandStatus::kReady);
  EXPECT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{20 * kOnDemandSec}));
  EXPECT_EQ(fx.shownWidth("owner/crop/cropped", 20 * kOnDemandSec), 6u);
}

// A result computed from the old data but not yet published when the reload begins is never published.
TEST(SessionManagerTest, ReloadBeginningInvalidatesAComputedUndrainedResult) {
  OnDemandSession fx;
  fx.push(fx.cloud, 20 * kOnDemandSec, 4);
  fx.install(cropRecipe());
  fx.service().requestAt("owner/crop", 20 * kOnDemandSec);
  fx.waitComputedUndrained();
  {
    RefillGuard guard = fx.session->beginRefill(fx.ds);
    fx.push(fx.cloud, 20 * kOnDemandSec, 6);
    guard.commit();
  }
  fx.settle();
  EXPECT_EQ(fx.fake.finished(), 2);
  EXPECT_EQ(fx.published, 1) << "only the refilled data's result";
  EXPECT_EQ(fx.shownWidth("owner/crop/cropped", 20 * kOnDemandSec), 6u);
}

// A rollback restores the dataset's bytes but not a result of the old epoch: it is computed again.
TEST(SessionManagerTest, RollbackDoesNotRestoreAnOldEpochResult) {
  OnDemandSession fx;
  fx.push(fx.cloud, 20 * kOnDemandSec, 4);
  fx.install(cropRecipe());
  fx.service().requestAt("owner/crop", 20 * kOnDemandSec);
  fx.settle();
  const std::optional<SequentialUID> before = fx.shownUid("owner/crop/cropped", 20 * kOnDemandSec);
  ASSERT_TRUE(before.has_value());
  {
    RefillGuard guard = fx.session->beginRefill(fx.ds);
    fx.push(fx.cloud, 20 * kOnDemandSec, 6);
  }  // rolled back
  EXPECT_TRUE(fx.ring("owner/crop/cropped").empty()) << "the rollback restored no result";
  fx.settle();
  EXPECT_EQ(fx.fake.finished(), 2) << "the cache of the old epoch serves nothing";
  const std::optional<SequentialUID> after = fx.shownUid("owner/crop/cropped", 20 * kOnDemandSec);
  ASSERT_TRUE(after.has_value());
  EXPECT_NE(after->value, before->value);
  EXPECT_EQ(fx.shownWidth("owner/crop/cropped", 20 * kOnDemandSec), 4u) << "computed from the restored bytes";
}

// GAP-32: removing the dataset while its node computes leaves no node, no state, no topic and no publication.
TEST(SessionManagerTest, DatasetRemovalMidFlightLeavesNoState) {
  OnDemandSession fx;
  fx.fake.gated = true;
  fx.push(fx.cloud, 20 * kOnDemandSec);
  fx.install(cropRecipe());
  fx.service().requestAt("owner/crop", 20 * kOnDemandSec);
  ASSERT_TRUE(fx.fake.waitStarted(1));
  const ObjectTopicId output = fx.topic("owner/crop/cropped").value();
  EXPECT_EQ(fx.service().keyOfTopic(output), "owner/crop");
  EXPECT_EQ(fx.service().stateOfTopic(output)->status, OnDemandStatus::kPending);
  ASSERT_FALSE(fx.state_changes.empty());
  EXPECT_EQ(fx.state_changes.back().first, "owner/crop");
  EXPECT_EQ(fx.state_changes.back().second, std::vector<ObjectTopicId>{output});

  fx.session->evictDatasetObjects(fx.ds);  // the shell's order: objects, catalog, then the dataset
  fx.session->removeDataset(fx.ds);
  fx.fake.allow(1);
  fx.settle();
  EXPECT_EQ(fx.service().graph().find("owner/crop"), nullptr);
  EXPECT_FALSE(fx.state().has_value());
  EXPECT_FALSE(fx.service().keyOfTopic(output).has_value());
  EXPECT_TRUE(fx.service().history().topicsOf("owner/crop").empty());
  EXPECT_EQ(fx.published, 0);
  EXPECT_EQ(fx.fake.started(), 1);
}

// GAP-32: a merge re-binds a consumed dataset's recipes onto the anchor, and they run there at their cursor, which
// moves by the dataset's raw shift like its data.
TEST(SessionManagerTest, MergeReinstallsOnDemandRecipesOnTheAnchor) {
  OnDemandSession fx;
  const DatasetId source = fx.addDataset("other");
  const ObjectTopicId cloud_b = fx.addCloud(source, "cloud_b");
  fx.push(cloud_b, 20 * kOnDemandSec);
  fx.install(cropRecipe("crop", "cloud_b"));
  fx.service().requestAt("owner/crop", 20 * kOnDemandSec);
  fx.settle();
  ASSERT_EQ(fx.ring("owner/crop/cropped", source), (std::vector<Timestamp>{20 * kOnDemandSec}));

  ASSERT_TRUE(fx.session->mergeDatasets(fx.ds, {{source, 5 * kOnDemandSec}}).has_value());
  fx.settle();
  const RecipeGraph::Node* node = fx.service().graph().find("owner/crop");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->dataset, fx.ds);
  EXPECT_EQ(fx.state()->status, OnDemandStatus::kReady) << fx.state()->detail;
  EXPECT_EQ(fx.state()->requested_t, 25 * kOnDemandSec) << "its cursor moved with its data";
  EXPECT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{25 * kOnDemandSec}));
  EXPECT_EQ(fx.fake.finished(), 2) << "the anchor's data is new to it";
  EXPECT_GE(fx.topics_changed, 1) << "the consumed dataset's output topic is retired";
}

// A consumed dataset's finding moves with its data: its pinned time is shifted by the merge's raw shift, and it is
// evaluated again there.
TEST(SessionManagerTest, MergeShiftsPinnedTimeAndReevaluatesTheFinding) {
  OnDemandSession fx;
  const DatasetId source = fx.addDataset("other");
  const ObjectTopicId cloud_b = fx.addCloud(source, "cloud_b");
  fx.push(cloud_b, 10 * kOnDemandSec);
  fx.push(cloud_b, 20 * kOnDemandSec);
  RecipeGraph::Recipe pinned = cropRecipe("pin", "cloud_b");
  pinned.pinned_t = 15 * kOnDemandSec;
  fx.install(pinned);
  fx.service().requestAt("owner/pin", 0);
  fx.settle();
  ASSERT_EQ(fx.fake.finished(), 1);

  ASSERT_TRUE(fx.session->mergeDatasets(fx.ds, {{source, 5 * kOnDemandSec}}).has_value());
  fx.settle();
  const RecipeGraph::Node* node = fx.service().graph().find("owner/pin");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->recipe.pinned_t, 20 * kOnDemandSec);
  const auto records = fx.fake.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[1].query_time, 20 * kOnDemandSec);
  EXPECT_EQ(fx.state("owner/pin")->requested_t, 20 * kOnDemandSec);
  EXPECT_EQ(fx.ring("owner/pin/cropped"), (std::vector<Timestamp>{kFindingTimestamp}));
}

// An input qualified with the consumed dataset's source (`other:cloud_b`, `other:owner/crop/cropped`) keeps working
// after the merge: it binds on the anchor, and the script still reads it under the name it declared.
TEST(SessionManagerTest, MergeRebindsAQualifiedInputOnTheAnchor) {
  OnDemandSession fx;
  const DatasetId source = fx.addDataset("other");
  const ObjectTopicId cloud_b = fx.addCloud(source, "cloud_b");
  fx.push(cloud_b, 20 * kOnDemandSec);
  fx.install(cropRecipe("crop", "other:cloud_b"));
  fx.install(cropRecipe("reader", "other:owner/crop/cropped"));
  fx.service().requestAt("owner/crop", 20 * kOnDemandSec);
  fx.settle();
  ASSERT_EQ(fx.state()->status, OnDemandStatus::kReady);

  ASSERT_TRUE(fx.session->mergeDatasets(fx.ds, {{source, 0}}).has_value());
  fx.settle();
  const RecipeGraph::Node* node = fx.service().graph().find("owner/crop");
  ASSERT_NE(node, nullptr) << "the recipe still resolves";
  EXPECT_EQ(node->dataset, fx.ds);
  EXPECT_EQ(node->recipe.inputs, std::vector<std::string>{"other:cloud_b"}) << "the script's name is kept";
  EXPECT_EQ(node->bindings[0].name, "cloud_b");
  EXPECT_EQ(fx.state()->status, OnDemandStatus::kReady) << fx.state()->detail;
  const RecipeGraph::Node* reader = fx.service().graph().find("owner/reader");
  ASSERT_NE(reader, nullptr) << "the chained reader still resolves";
  EXPECT_EQ(reader->dataset, fx.ds);
  EXPECT_EQ(reader->bindings[0].upstream, "owner/crop");
  EXPECT_EQ(reader->bindings[0].name, "cropped");
  EXPECT_EQ(reader->recipe.inputs, std::vector<std::string>{"other:owner/crop/cropped"}) << "the script's name is kept";
  EXPECT_FALSE(fx.service().graph().missingInput("owner/reader").has_value());
}

// A consumed dataset stays registered, so its source keeps qualifying inputs: they bind on the anchor from then on.
TEST(SessionManagerTest, AQualifiedInputKeepsBindingOnTheAnchorAfterItsMerge) {
  OnDemandSession fx;
  const DatasetId source = fx.addDataset("other");
  fx.push(fx.addCloud(source, "cloud_b"), 20 * kOnDemandSec);
  fx.install(cropRecipe("crop", "other:cloud_b"));
  fx.install(cropRecipe("reader", "other:owner/crop/cropped"));
  ASSERT_TRUE(fx.session->mergeDatasets(fx.ds, {{source, 0}}).has_value());
  fx.settle();

  const auto reinstalled = fx.service().install(cropRecipe("reader", "other:owner/crop/cropped"));
  EXPECT_TRUE(reinstalled.has_value()) << (reinstalled ? "" : reinstalled.error());

  const DatasetId third = fx.addDataset("third");
  fx.push(fx.addCloud(third, "cloud_c"), 20 * kOnDemandSec);
  ASSERT_TRUE(fx.session->mergeDatasets(fx.ds, {{third, 0}}).has_value());
  fx.settle();
  EXPECT_NE(fx.service().graph().find("owner/crop"), nullptr) << "a second merge keeps the qualified input";
  EXPECT_NE(fx.service().graph().find("owner/reader"), nullptr) << "and its chained reader";
}

// An anchor merged away in turn takes the datasets it consumed with it.
TEST(SessionManagerTest, AQualifiedInputFollowsItsAnchorIntoALaterMerge) {
  OnDemandSession fx;
  const DatasetId source = fx.addDataset("other");
  fx.push(fx.addCloud(source, "cloud_b"), 20 * kOnDemandSec);
  fx.install(cropRecipe("crop", "other:cloud_b"));
  ASSERT_TRUE(fx.session->mergeDatasets(fx.ds, {{source, 0}}).has_value());
  fx.settle();

  const DatasetId final_anchor = fx.addDataset("final");
  fx.push(fx.addCloud(final_anchor, "cloud_c"), 20 * kOnDemandSec);
  ASSERT_TRUE(fx.session->mergeDatasets(final_anchor, {{fx.ds, 0}}).has_value());
  fx.settle();
  const RecipeGraph::Node* node = fx.service().graph().find("owner/crop");
  ASSERT_NE(node, nullptr) << "the recipe still resolves";
  EXPECT_EQ(node->dataset, final_anchor);
}

// A dataset's suspension cancels its nodes' runs: their epoch moved, so their results could never be published.
TEST(SessionManagerTest, SuspendingADatasetCancelsItsRunningRun) {
  OnDemandSession fx;
  fx.fake.delay = std::chrono::milliseconds(5000);  // polls its cancellation
  fx.push(fx.cloud, 20 * kOnDemandSec);
  fx.install(cropRecipe());
  fx.service().requestAt("owner/crop", 20 * kOnDemandSec);
  ASSERT_TRUE(fx.fake.waitStarted(1));
  RefillGuard guard = fx.session->beginRefill(fx.ds);
  for (int spin = 0; spin < 3000 && fx.fake.finished() == 0; ++spin) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(fx.fake.finished(), 1);
  EXPECT_TRUE(fx.fake.records()[0].cancelled);
  guard.commit();
}

// A rollback restores the same bytes and identities, but a new epoch: a failure remembered before the reload does not
// fail the request again without a run.
TEST(SessionManagerTest, ARollbackRunsAFailedRecipeAgain) {
  OnDemandSession fx;
  fx.fake.fail = "boom";
  fx.push(fx.cloud, 20 * kOnDemandSec);
  fx.install(cropRecipe());
  fx.service().requestAt("owner/crop", 20 * kOnDemandSec);
  fx.settle();
  ASSERT_EQ(fx.state()->status, OnDemandStatus::kError);
  fx.fake.fail.clear();
  { RefillGuard guard = fx.session->beginRefill(fx.ds); }  // rolled back
  fx.settle();
  EXPECT_EQ(fx.fake.finished(), 2);
  EXPECT_EQ(fx.state()->status, OnDemandStatus::kReady);
}

// The finding's store timestamp is a sentinel, never a time: a merge never shifts it, also not for a moment.
TEST(SessionManagerTest, MergeNeverShiftsTheFindingSentinel) {
  OnDemandSession fx;
  const DatasetId source = fx.addDataset("other");
  const ObjectTopicId cloud_b = fx.addCloud(source, "cloud_b");
  fx.push(cloud_b, 10 * kOnDemandSec);
  RecipeGraph::Recipe pinned = cropRecipe("pin", "cloud_b");
  pinned.pinned_t = 15 * kOnDemandSec;
  fx.install(pinned);
  fx.service().requestAt("owner/pin", 0);
  fx.settle();
  ASSERT_EQ(fx.ring("owner/pin/cropped", source), (std::vector<Timestamp>{kFindingTimestamp}));

  ASSERT_TRUE(fx.session->mergeDatasets(fx.ds, {{source, 5 * kOnDemandSec}}).has_value());
  for (const ObjectTopicId topic : fx.session->objectStore().listTopics(fx.ds)) {
    for (const Timestamp t : test::timestampsOf(fx.session->objectStore(), topic)) {
      EXPECT_NE(t, saturatingAdd(kFindingTimestamp, 5 * kOnDemandSec)) << "a derived entry merged as raw data";
    }
  }
  fx.settle();
  EXPECT_EQ(fx.ring("owner/pin/cropped"), (std::vector<Timestamp>{kFindingTimestamp}));
}

// A raw input evicted from under a recipe leaves it installed, missing that input by name, and a tombstone clears
// the dropped result; the input coming back relinks it.
TEST(SessionManagerTest, RemovingARawInputLeavesItsReadersMissingAndAllowsRelinking) {
  OnDemandSession fx;
  fx.push(fx.cloud, 20 * kOnDemandSec);
  fx.install(cropRecipe());
  fx.service().requestAt("owner/crop", 20 * kOnDemandSec);
  fx.settle();
  ASSERT_EQ(fx.state()->status, OnDemandStatus::kReady);

  fx.session->evictObjectTopics({fx.cloud});
  fx.settle();
  ASSERT_NE(fx.service().graph().find("owner/crop"), nullptr);
  EXPECT_EQ(fx.state()->status, OnDemandStatus::kMissingInput);
  EXPECT_NE(fx.state()->detail.find("'cloud'"), std::string::npos) << fx.state()->detail;
  EXPECT_EQ(fx.ring("owner/crop/cropped"), std::vector<Timestamp>{20 * kOnDemandSec})
      << "only the tombstone that clears the dropped result a layer may still draw";
  EXPECT_EQ(fx.shownWidth("owner/crop/cropped", 20 * kOnDemandSec), 0u);
  fx.service().requestAt("owner/crop", 10 * kOnDemandSec);
  fx.settle();
  EXPECT_EQ(fx.ring("owner/crop/cropped"), std::vector<Timestamp>{20 * kOnDemandSec})
      << "nothing drawn at 10 s once the tombstone is written: no second one";

  const ObjectTopicId again = fx.addCloud(fx.ds, "cloud");
  fx.push(again, 20 * kOnDemandSec, 6);
  fx.service().requestAt("owner/crop", 20 * kOnDemandSec);
  fx.settle();
  EXPECT_EQ(fx.state()->status, OnDemandStatus::kReady);
  EXPECT_EQ(fx.shownWidth("owner/crop/cropped", 20 * kOnDemandSec), 6u);
}

// PUB-02: a dataset removal retires every output topic of its recipes and announces it once; an edit keeps its
// topics and announces nothing; an edit that renames an output retires the old one.
TEST(SessionManagerTest, RemovalRetiresTopicsAndAnnouncesOnce) {
  OnDemandSession fx;
  fx.install(cropRecipe("crop"));
  fx.install(cropRecipe("crop2"));
  const ObjectTopicId kept = fx.topic("owner/crop/cropped").value();
  EXPECT_EQ(fx.topics_changed, 0);
  RecipeGraph::Recipe edited = cropRecipe("crop");
  edited.params_json = R"({"zmin": 1})";
  fx.install(edited);
  EXPECT_EQ(fx.topic("owner/crop/cropped"), kept);
  EXPECT_EQ(fx.topics_changed, 0);
  RecipeGraph::Recipe renamed = cropRecipe("crop2");
  renamed.outputs[0].name = "trimmed";
  fx.install(renamed);
  EXPECT_EQ(fx.topics_changed, 1);

  fx.session->removeDataset(fx.ds);
  EXPECT_EQ(fx.topics_changed, 2);
  EXPECT_FALSE(fx.topic("owner/crop/cropped").has_value());
  EXPECT_FALSE(fx.topic("owner/crop2/trimmed").has_value());
}

// The session's resolver: a dataset-qualified input resolves only in that dataset, the qualifier stripped; an object
// topic with no builtin type is no object input; an on-demand output, qualified or not, never resolves as a topic (the
// graph binds those itself, a qualified one only on its node's dataset).
TEST(SessionManagerTest, TheResolverBindsQualifiedTopicsAndNeverAnOnDemandOutput) {
  OnDemandSession fx;
  const DatasetId other = fx.addDataset("other");
  fx.addCloud(other, "cloud");
  ASSERT_TRUE(fx.session->objectStore().registerTopic({fx.ds, "untyped", "{}"}).has_value());
  const auto ambiguous = fx.service().install(cropRecipe("crop", "cloud"));
  ASSERT_FALSE(ambiguous.has_value());
  EXPECT_NE(ambiguous.error().find("more than one dataset"), std::string::npos) << ambiguous.error();

  fx.install(cropRecipe("crop", "other:cloud"));
  EXPECT_EQ(fx.service().graph().find("owner/crop")->dataset, other);
  EXPECT_EQ(fx.service().graph().find("owner/crop")->bindings[0].name, "cloud");
  const auto untyped = fx.service().install(cropRecipe("raw", "untyped"));
  ASSERT_FALSE(untyped.has_value());
  EXPECT_NE(untyped.error().find("unknown input 'untyped'"), std::string::npos) << untyped.error();
  const auto elsewhere = fx.service().install(cropRecipe("reader", "log:owner/crop/cropped"));
  ASSERT_FALSE(elsewhere.has_value());
  EXPECT_NE(elsewhere.error().find("names another dataset"), std::string::npos) << elsewhere.error();
  fx.install(cropRecipe("reader", "other:owner/crop/cropped"));
  EXPECT_EQ(fx.service().graph().find("owner/reader")->dataset, other);
  EXPECT_EQ(fx.service().graph().find("owner/reader")->bindings[0].upstream, "owner/crop");
}

// CON-11/14: destroying the session cancels the on-demand run before the pool joins it; nothing is published.
TEST(SessionManagerTest, SessionDestructionCancelsOnDemandRunsBeforeThePool) {
  OnDemandSession fx;
  fx.fake.delay = std::chrono::milliseconds(5000);  // polls its cancellation
  fx.push(fx.cloud, 20 * kOnDemandSec);
  fx.install(cropRecipe());
  fx.service().requestAt("owner/crop", 20 * kOnDemandSec);
  ASSERT_TRUE(fx.fake.waitStarted(1));
  fx.session.reset();
  const auto records = fx.fake.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_TRUE(records[0].cancelled);
  EXPECT_EQ(fx.published, 0);
}

// The refill's caller commits the reload's TF right after the refill (FileLoader does, before the event loop turns):
// the requests resume from the event loop, so the recompute runs once, on the reloaded TF.
TEST(SessionManagerTest, AReloadResumesAfterItsCallerCommitsTheReloadedTf) {
  OnDemandSession fx;
  TransformService transforms(*fx.session);
  fx.session->setTransformService(&transforms);
  pj::tf::StampedTransform tf;
  tf.stamp = fromRaw(1 * kOnDemandSec);
  tf.parent_frame = "map";
  tf.child_frame = "base_link";
  ASSERT_TRUE(transforms.transformBuffer(fx.ds)->setTransform(tf).has_value());
  fx.fake.observe_tf = true;
  fx.push(fx.cloud, 20 * kOnDemandSec);
  fx.install(cropRecipe());
  fx.service().requestAt("owner/crop", 20 * kOnDemandSec);
  fx.settle();
  ASSERT_EQ(fx.fake.finished(), 1);

  transforms.beginReplacingLoad(fx.ds);
  {
    RefillGuard guard = fx.session->beginRefill(fx.ds);
    fx.push(fx.cloud, 20 * kOnDemandSec, 6);
    guard.commit();
  }
  transforms.commitReplacingLoad(fx.ds);
  const uint64_t reloaded = transforms.snapshot(fx.ds)->revision();
  ASSERT_NE(reloaded, fx.fake.records()[0].tf->revision());
  fx.settle();
  const auto records = fx.fake.records();
  ASSERT_EQ(records.size(), 2u) << "one recompute, not one on the old TF and another on the new";
  EXPECT_EQ(records[1].tf->revision(), reloaded);
  EXPECT_EQ(fx.state()->status, OnDemandStatus::kReady);
  fx.session->setTransformService(nullptr);
}

// ARCHITECTURE §19: while a dataset ingests, its runs share one TF snapshot for the brake period, and a result that
// read it serves its request (the revision a request compares is that snapshot's), so nothing runs again until the
// ingest ends.
TEST(SessionManagerTest, OnDemandRunsShareOneTfSnapshotDuringALiveIngest) {
  OnDemandSession fx;
  TransformService transforms(*fx.session);
  fx.session->setTransformService(&transforms);
  const auto write = [&](Timestamp t) {
    pj::tf::StampedTransform tf;
    tf.stamp = fromRaw(t);
    tf.parent_frame = "map";
    tf.child_frame = "base_link";
    tf.transform.t.x = static_cast<double>(t);
    ASSERT_TRUE(transforms.transformBuffer(fx.ds)->setTransform(tf).has_value());
  };
  write(1 * kOnDemandSec);
  fx.fake.observe_tf = true;
  fx.fake.gated = true;
  fx.push(fx.cloud, 10 * kOnDemandSec);
  fx.push(fx.cloud, 20 * kOnDemandSec);
  fx.install(cropRecipe());
  const IngestToken ingest = fx.session->beginIngest(fx.ds, u"load"_s, 0, false, {});

  fx.service().requestAt("owner/crop", 10 * kOnDemandSec);
  ASSERT_TRUE(fx.fake.waitStarted(1));
  write(2 * kOnDemandSec);  // the ingest moves the TF on
  fx.fake.allow(1);
  fx.settle();
  EXPECT_EQ(fx.fake.started(), 1) << "the result read the snapshot its request compares";
  EXPECT_EQ(fx.state()->status, OnDemandStatus::kReady);

  fx.service().requestAt("owner/crop", 20 * kOnDemandSec);
  fx.fake.allow(1);
  fx.settle();
  const std::shared_ptr<const pj::tf::TransformSnapshot> held = fx.fake.records()[1].tf;
  const long holders = held.use_count();
  fx.session->endIngest(ingest, IngestOutcome::kCompleted);
  EXPECT_EQ(held.use_count(), holders - 1) << "the session lets the snapshot go when the ingest ends";
  fx.service().requestAt("owner/crop", 10 * kOnDemandSec);
  fx.fake.allow(1);
  fx.settle();
  const auto records = fx.fake.records();
  ASSERT_EQ(records.size(), 3u);
  EXPECT_EQ(records[1].tf, records[0].tf) << "one snapshot during the ingest";
  ASSERT_NE(records[2].tf, nullptr);
  EXPECT_NE(records[2].tf->revision(), records[0].tf->revision()) << "a fresh one after it";
  fx.session->setTransformService(nullptr);
}

// ARCHITECTURE §19 for a stream, which never begins an ingest: a dataset with a live announcement within the brake
// period shares one TF snapshot between its runs, as during a file ingest.
TEST(SessionManagerTest, TfSnapshotsAreThrottledWhileAStreamingDatasetIngests) {
  auto now = std::chrono::steady_clock::now();  // outlives the fixture, whose session reads it through the clock
  OnDemandSession fx;
  fx.session->setTfClock([&now] { return now; });
  TransformService transforms(*fx.session);
  fx.session->setTransformService(&transforms);
  const auto write = [&](Timestamp t) {
    pj::tf::StampedTransform tf;
    tf.stamp = fromRaw(t);
    tf.parent_frame = "map";
    tf.child_frame = "base_link";
    tf.transform.t.x = static_cast<double>(t);
    ASSERT_TRUE(transforms.transformBuffer(fx.ds)->setTransform(tf).has_value());
  };
  write(1 * kOnDemandSec);
  fx.fake.observe_tf = true;
  fx.fake.gated = true;
  fx.push(fx.cloud, 10 * kOnDemandSec);
  fx.push(fx.cloud, 20 * kOnDemandSec);
  fx.install(cropRecipe());
  ASSERT_FALSE(fx.session->ingestActive(fx.ds));

  fx.session->notifyIngest({}, /*live=*/true, fx.ds);
  fx.service().requestAt("owner/crop", 10 * kOnDemandSec);
  ASSERT_TRUE(fx.fake.waitStarted(1));
  write(2 * kOnDemandSec);  // the stream moves the TF on
  fx.session->notifyIngest({}, /*live=*/true, fx.ds);
  fx.fake.allow(1);
  fx.settle();
  EXPECT_EQ(fx.fake.started(), 1) << "the result read the snapshot its request compares";

  fx.service().requestAt("owner/crop", 20 * kOnDemandSec);
  fx.fake.allow(1);
  fx.settle();
  const auto records = fx.fake.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[1].tf, records[0].tf) << "one snapshot while the stream ingests";

  now += std::chrono::milliseconds(600);  // past kTfSnapshotPeriod (500 ms) with no announcement: the stream stopped
  fx.service().requestAt("owner/crop", 10 * kOnDemandSec);
  fx.fake.allow(1);
  fx.settle();
  const auto after = fx.fake.records();
  ASSERT_EQ(after.size(), 3u);
  EXPECT_NE(after[2].tf->revision(), after[0].tf->revision()) << "a fresh snapshot once the stream is quiet";
  fx.session->setTransformService(nullptr);
}

}  // namespace
}  // namespace PJ
