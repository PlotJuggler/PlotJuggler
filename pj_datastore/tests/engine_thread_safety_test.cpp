// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Targeted DataEngine race tests beyond the headline commit/read repro
// (engine_concurrency_test.cpp). Each exercises one hazard the engine lock must
// close; before the lock they race/segfault (TSan flags them), after they pass.
// Run under ctest (the CMake registration sets a per-test TIMEOUT) so a deadlock
// fails loudly. The tests at the bottom drive the REAL worker write path (the
// C-ABI write host), not the locked commitChunks() the older tests use — the path
// C1 had to lock.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <latch>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "pj_base/dataset.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/builtin_transforms.hpp"
#include "pj_datastore/derived_engine.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/plugin_data_host.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/writer.hpp"

// ThreadSanitizer-active detection: GCC defines __SANITIZE_THREAD__; Clang
// reports it through __has_feature(thread_sanitizer). The pause/resume
// WriteCore-swap race guard below is only meaningful under TSan, so it skips
// itself in ordinary builds (see its GTEST_SKIP).
#if defined(__SANITIZE_THREAD__)
#define PJ_TSAN_BUILD 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define PJ_TSAN_BUILD 1
#endif
#endif

namespace PJ {
namespace {

constexpr Timestamp kMinTs = std::numeric_limits<Timestamp>::min();
constexpr Timestamp kMaxTs = std::numeric_limits<Timestamp>::max();

// Register a scalar topic and commit `rows` samples (ts == val == i). Returns id.
TopicId makeTopic(DataEngine& engine, DatasetId dataset_id, std::string_view name, int rows) {
  DataWriter w = engine.createWriter();
  auto handle = w.registerScalarSeries(dataset_id, name, NumericType::kFloat64);
  EXPECT_TRUE(handle.has_value()) << (handle.has_value() ? "" : handle.error());
  for (int i = 0; i < rows; ++i) {
    w.appendScalar(*handle, static_cast<Timestamp>(i), static_cast<double>(i));
  }
  engine.commitChunks(w.flushAll());
  return handle->topic_id;
}

// Fully iterate a full-range read of `topic`; returns rows seen (0 if missing).
std::size_t readAll(DataReader& reader, TopicId topic) {
  auto cursor = reader.rangeQuery(QueryRange{.topic_id = topic, .t_min = kMinTs, .t_max = kMaxTs});
  if (!cursor.has_value()) {
    return 0;
  }
  std::size_t rows = 0;
  cursor->forEach([&rows](const SampleRow&) { ++rows; });
  return rows;
}

// A worker continuously REGISTERS new topics (each createTopic emplace can
// rehash the by-value robin_map, move-constructing every TopicStorage) while a
// reader iterates a DIFFERENT, already-populated topic. Pre-lock the rehash
// dangles the reader's cached storage pointer -> UB/crash.
TEST(EngineThreadSafety, CreateTopicRehashDuringForeignTopicRead) {
  DataEngine engine;
  auto dataset = engine.createDataset(DatasetDescriptor{.source_name = "rehash", .time_domain_id = 0});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const TopicId base = makeTopic(engine, *dataset, "base", 512);

  std::atomic<bool> stop{false};
  std::thread creator([&]() {
    DataWriter w = engine.createWriter();
    for (int i = 0; i < 8000 && !stop.load(std::memory_order_relaxed); ++i) {
      (void)w.registerScalarSeries(*dataset, "t" + std::to_string(i), NumericType::kFloat64);
    }
  });

  std::atomic<int> reads{0};
  std::thread reader([&]() {
    DataReader r = engine.createReader();
    for (int i = 0; i < 20000; ++i) {
      (void)readAll(r, base);
      reads.fetch_add(1, std::memory_order_relaxed);
    }
  });

  reader.join();
  stop.store(true, std::memory_order_relaxed);
  creator.join();
  EXPECT_EQ(reads.load(), 20000);
}

// Hold a single RangeCursor and walk it element-by-element while another thread
// appends to the same topic. Pre-lock, push_back mutates the deque the cursor
// is indexing -> data race. Post-lock, the cursor's adopted shared lock blocks
// the append for the walk's duration.
TEST(EngineThreadSafety, RangeCursorHeldAcrossConcurrentAppend) {
  DataEngine engine;
  auto dataset = engine.createDataset(DatasetDescriptor{.source_name = "held", .time_domain_id = 0});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const TopicId topic = makeTopic(engine, *dataset, "v", 4096);

  std::atomic<bool> stop{false};
  // Block the writer after its first commit until the reader's first held-cursor
  // walk has run, then let it flood the remaining (bounded) batches — so walks
  // 2..N are guaranteed to overlap a live appender regardless of scheduling.
  std::latch first_walk_done{1};
  // Bound the writer's output. The original `while (!stop)` writer appended
  // without limit while the reader walked the FULL range 200x, so each walk was
  // O(rows-so-far) with rows climbing the whole time — a positive-feedback
  // blowup that ran this one subtest to ~18s on a contended single core (a
  // 90s-TIMEOUT risk under `ctest -j`). A few hundred batches still keep the
  // writer appending across the reader's walks, which is all this race needs.
  constexpr int kWriterBatches = 256;
  std::thread writer([&]() {
    DataWriter w = engine.createWriter();
    const ScalarSeriesHandle wh{topic, 0};
    Timestamp t = 100000;
    for (int batch = 0; batch < kWriterBatches && !stop.load(std::memory_order_relaxed); ++batch) {
      for (int i = 0; i < 64; ++i) {
        w.appendScalar(wh, t++, 2.0);
      }
      engine.commitChunks(w.flushAll());
      if (batch == 0) {
        first_walk_done.wait();
      }
    }
  });

  DataReader r = engine.createReader();
  std::size_t total = 0;
  for (int iter = 0; iter < 200; ++iter) {
    auto cursor = r.rangeQuery(QueryRange{.topic_id = topic, .t_min = kMinTs, .t_max = kMaxTs});
    ASSERT_TRUE(cursor.has_value());
    while (cursor->valid()) {
      (void)cursor->current();
      cursor->advance();
      ++total;
    }
    if (iter == 0) {
      first_walk_done.count_down();  // release the writer's remaining appends
    }
  }
  stop.store(true, std::memory_order_relaxed);
  writer.join();
  EXPECT_GT(total, 0u);
}

// enforceRetention() -> evictBefore() ERASES old chunks (topic_storage.cpp:37),
// freeing chunk memory. A reader iterating those chunks concurrently is a UAF
// identical to the original bug. A writer advances timeMax so eviction always
// has something to free.
TEST(EngineThreadSafety, EvictWithOpenCursorSerializes) {
  DataEngine engine;
  auto dataset = engine.createDataset(DatasetDescriptor{.source_name = "evict", .time_domain_id = 0});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const TopicId topic = makeTopic(engine, *dataset, "v", 256);

  std::atomic<bool> stop{false};
  std::thread writer([&]() {
    DataWriter w = engine.createWriter();
    const ScalarSeriesHandle wh{topic, 0};
    Timestamp t = 1000;
    while (!stop.load(std::memory_order_relaxed)) {
      for (int i = 0; i < 64; ++i) {
        w.appendScalar(wh, t++, 1.0);
      }
      engine.commitChunks(w.flushAll());
    }
  });
  std::thread evictor([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      engine.enforceRetention(/*retention_window_ns=*/100, *dataset);  // frees most chunks each call
    }
  });

  std::atomic<int> reads{0};
  std::thread reader([&]() {
    DataReader r = engine.createReader();
    for (int i = 0; i < 20000; ++i) {
      (void)readAll(r, topic);
      reads.fetch_add(1, std::memory_order_relaxed);
    }
  });

  reader.join();
  stop.store(true, std::memory_order_relaxed);
  writer.join();
  evictor.join();
  EXPECT_EQ(reads.load(), 20000);
}

// retireTopic() calls clearChunks(), freeing a topic's deque while a reader may
// be iterating it. Retire a pool of topics one by one under a hammering reader.
TEST(EngineThreadSafety, RetireTopicSerializesWithReader) {
  DataEngine engine;
  auto dataset = engine.createDataset(DatasetDescriptor{.source_name = "retire", .time_domain_id = 0});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  constexpr int kTopics = 200;
  std::vector<TopicId> topics;
  topics.reserve(kTopics);
  for (int i = 0; i < kTopics; ++i) {
    topics.push_back(makeTopic(engine, *dataset, "t" + std::to_string(i), 128));
  }

  std::atomic<bool> stop{false};
  std::thread retirer([&]() {
    for (TopicId topic : topics) {
      if (stop.load(std::memory_order_relaxed)) {
        break;
      }
      engine.retireTopic(topic);  // clearChunks() frees the deque
    }
  });

  std::atomic<int> reads{0};
  std::thread reader([&]() {
    DataReader r = engine.createReader();
    for (int i = 0; i < 40000; ++i) {
      (void)readAll(r, topics[static_cast<std::size_t>(i) % topics.size()]);
      reads.fetch_add(1, std::memory_order_relaxed);
    }
  });

  reader.join();
  stop.store(true, std::memory_order_relaxed);
  retirer.join();
  EXPECT_EQ(reads.load(), 40000);
}

// === C1: the REAL worker write path (C-ABI write host) vs GUI reads ===========
// Unlike the tests above (which write via the locked commitChunks()), production
// ingest drives the write host (WriteCore -> DataWriter), which mutates live
// TopicStorage through the unlocked getTopicStorage() — pre-C1 it raced GUI reads.

// R1 — worker drives the write host (ensureTopic + mid-stream ensureField, +
// appendBoundRecord) while the GUI reads the same topic. The layout is NOT
// pre-frozen: the worker grows it during the window — the production race that
// CatalogModel/series() reads of column_descriptors_ hit.
TEST(EngineThreadSafety, WorkerWriteHostConcurrentWithReaderNoRace) {
  using namespace PJ::sdk;
  DataEngine engine;
  ObjectStore store;
  DatastoreToolboxHost toolbox_impl{engine, store};
  ToolboxHostView toolbox{toolbox_impl.raw()};
  const auto source = *toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost host(engine, source);
  SourceWriteHostView writer(host.raw());
  const auto topic = *writer.ensureTopic("imu");
  const auto f0 = *writer.ensureField(topic, "f0", PrimitiveType::kFloat64);

  constexpr int kRecords = 4000;
  constexpr int kFieldEvery = 200;  // add a column every Nth record (un-frozen layout)
  std::atomic<bool> worker_done{false};
  std::atomic<int> extra_fields{0};  // columns the worker actually added mid-stream
  // The worker blocks on this after its first flush until the reader has done
  // one overlapping pass — without it, a scheduler that runs the worker to
  // completion before the reader's first turn (reliably so when both threads
  // share a core: CI under `ctest -j`, a single-vCPU runner, or `taskset -c 0`)
  // leaves reads == 0 and false-fails the EXPECT_GT below on correct code. The
  // worker holds no engine lock while waiting, so a correct reader always makes
  // progress; a real deadlock instead trips the 90s ctest TIMEOUT.
  std::latch first_read_done{1};

  std::thread worker([&]() {
    int added = 0;
    for (int i = 0; i < kRecords; ++i) {
      if (i > 0 && i % kFieldEvery == 0) {
        // Mid-stream column add: re-writes column_descriptors_, the vector a
        // concurrent read copies.
        if (writer.ensureField(topic, "f" + std::to_string(i), PrimitiveType::kFloat64).has_value()) {
          ++added;
        }
      }
      (void)writer.appendBoundRecord(
          topic, static_cast<Timestamp>(i), {{.field = f0, .value = static_cast<double>(i)}});
      if (i % 64 == 0) {
        host.flushPending();
      }
      if (i == 0) {
        // One record is now visible; wait for a reader pass so records 1..N-1
        // are guaranteed to overlap a live reader.
        first_read_done.wait();
      }
    }
    host.flushPending();
    extra_fields.store(added, std::memory_order_relaxed);
    worker_done.store(true, std::memory_order_release);
  });

  // Bound the reader pass count so the test runtime is deterministic rather than
  // fairness-dependent: an unbounded `while (!worker_done)` reader is O(passes*N)
  // with `passes` set by lock fairness, which on a contended single core walked
  // this binary into multi-second runs (a 90s-TIMEOUT risk under `ctest -j`). The
  // race fires regardless of pass count; the cap only ever catches a true hang.
  std::atomic<int> reads{0};
  constexpr int kMaxReaderPasses = 2000;
  std::thread reader([&]() {
    DataReader r = engine.createReader();
    for (int i = 0; i < kMaxReaderPasses && !worker_done.load(std::memory_order_acquire); ++i) {
      (void)r.getMetadata(topic.id);
      if (auto series = r.series(topic.id, 0); series.has_value()) {
        (void)series->bounds();
      }
      (void)readAll(r, topic.id);
      // Release the worker's first-flush wait on the very first pass (fetch_add
      // returns the prior value, so == 0 means "this is the first pass").
      if (reads.fetch_add(1, std::memory_order_relaxed) == 0) {
        first_read_done.count_down();
      }
      std::this_thread::yield();  // let the worker make progress (exclusive lock)
    }
  });

  worker.join();
  reader.join();

  EXPECT_GT(reads.load(), 0);  // the reader really did overlap the writer
  // Deterministic final state (the reader is read-only): the worker wrote
  // kRecords rows of f0 plus the columns it added mid-stream.
  const auto* storage = engine.getTopicStorage(topic.id);
  ASSERT_NE(storage, nullptr);
  EXPECT_EQ(
      storage->columnDescriptors().size(), static_cast<std::size_t>(1 + extra_fields.load(std::memory_order_relaxed)));
  DataReader verify = engine.createReader();
  EXPECT_EQ(readAll(verify, topic.id), static_cast<std::size_t>(kRecords));
}

// FlushToConcurrentNoDeadlock — the worker writes to `live` and replays ensures to
// `staging` (write host holds both via its dual std::lock) while the GUI loops
// staging.flushTo(live) (also std::lock(both)). Reaching the end without the ctest
// timeout firing IS the assertion.
//
// NOTE: this proves liveness, not that the dual std::lock is *required* — flushTo's
// std::lock is back-off-immune, so even a naive ordered write-host lock wouldn't
// ABBA against it. The dual std::lock is kept as defensive future-proofing.
TEST(EngineThreadSafety, FlushToConcurrentNoDeadlock) {
  using namespace PJ::sdk;
  DataEngine live;
  DataEngine staging;
  ObjectStore store;
  DatastoreToolboxHost toolbox_impl{live, store};
  ToolboxHostView toolbox{toolbox_impl.raw()};
  const auto source = *toolbox.createDataSource("sensor");
  // Lockstep the dataset id onto the staging engine so topic mirroring can address it.
  ASSERT_TRUE(staging.createDataset(DatasetDescriptor{.source_name = "sensor"}, source.id).has_value());

  DatastoreSourceWriteHost host(live, source);
  host.setSecondaryEngine(&staging);  // ensure* replays live -> staging
  SourceWriteHostView writer(host.raw());

  std::atomic<bool> stop{false};
  std::thread worker([&]() {
    Timestamp t = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      auto topic = writer.ensureTopic("imu");
      if (topic.has_value()) {
        auto field = writer.ensureField(*topic, "f0", PrimitiveType::kFloat64);
        if (field.has_value()) {
          (void)writer.appendBoundRecord(*topic, t++, {{.field = *field, .value = 1.0}});
        }
      }
      host.flushPending();
    }
  });
  std::thread flusher([&]() {
    for (int i = 0; i < 3000; ++i) {
      (void)staging.flushTo(live);  // std::lock(staging, live)
    }
    stop.store(true, std::memory_order_release);
  });

  worker.join();
  flusher.join();
  SUCCEED();  // no deadlock under ctest --timeout
}

// TypeRegistryRegisterDuringWorkerWrite — GUI addSisoTransform (registry write +
// createTopic rehash) vs a concurrent worker write host (append + raw
// getTopicStorage). Pins that the registry write + the rehash are safe under the
// engine lock. TSan-first: the UB rarely segfaults without it.
//
// NOTE: scalar ingest does NOT reach the worker-side typeRegistry().lookup() (the
// field_types cache + effectiveColumns early-return short-circuit it), so the
// find-vs-emplace race isn't exercised here — it's covered by the engine lock; a
// schema-backed-topic test that forces lookup() is a tracked follow-up.
TEST(EngineThreadSafety, TypeRegistryRegisterDuringWorkerWriteNoRace) {
  using namespace PJ::sdk;
  DataEngine engine;
  ObjectStore store;
  DatastoreToolboxHost toolbox_impl{engine, store};
  ToolboxHostView toolbox{toolbox_impl.raw()};
  const auto source = *toolbox.createDataSource("sensor");

  DatastoreSourceWriteHost host(engine, source);
  SourceWriteHostView writer(host.raw());
  const auto wtopic = *writer.ensureTopic("wt");
  const auto wfield = *writer.ensureField(wtopic, "v", PrimitiveType::kFloat64);
  const TopicId base = makeTopic(engine, source.id, "base", 256);  // derived-node input
  DerivedEngine derived(engine);

  std::atomic<bool> stop{false};
  std::thread worker([&]() {
    Timestamp t = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      (void)writer.appendBoundRecord(wtopic, t++, {{.field = wfield, .value = 1.0}});
      if (t % 32 == 0) {
        host.flushPending();
      }
    }
    host.flushPending();
  });

  for (int i = 0; i < 1500; ++i) {
    (void)derived.addSisoTransform(base, "d" + std::to_string(i), source.id, std::make_unique<DerivativeTransform>());
  }
  stop.store(true, std::memory_order_release);
  worker.join();
  SUCCEED();  // no TSan race on the TypeRegistry / topics maps
}

// Nested reads under a held cursor are legal under the recursive mutex: hold a
// RangeCursor (which adopts the lock) and do more reads on the same thread. A
// non-recursive mutex would self-deadlock here (caught by the ctest timeout) —
// pins the recursive choice against a "simplify to plain std::mutex" refactor.
TEST(EngineThreadSafety, NestedReadsUnderHeldCursorRecursiveOk) {
  DataEngine engine;
  auto dataset = engine.createDataset(DatasetDescriptor{.source_name = "nested", .time_domain_id = 0});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const TopicId topic = makeTopic(engine, *dataset, "v", 128);

  DataReader r = engine.createReader();
  auto cursor = r.rangeQuery(QueryRange{.topic_id = topic, .t_min = kMinTs, .t_max = kMaxTs});
  ASSERT_TRUE(cursor.has_value());
  // These re-acquire the engine lock on the same thread while the cursor holds it.
  (void)r.getMetadata(topic);
  auto series = r.series(topic, 0);
  ASSERT_TRUE(series.has_value());
  (void)series->bounds();
  // The held cursor is still valid + walkable after the nested reads.
  std::size_t rows = 0;
  cursor->forEach([&rows](const SampleRow&) { ++rows; });
  EXPECT_EQ(rows, 128u);
}

// Pause/resume WriteCore-swap race guard. A worker thread drives the C-ABI source
// write host (each call loads state_->core and dereferences the WriteCore) while
// the manager thread calls setTarget() in a loop — which publishes a freshly built
// WriteCore and drops the previous one. Before the fix this was a data race + heap
// use-after-free on the WriteCore: the swap reassigned a plain unique_ptr and
// destroyed the core the worker was still inside, and the engine lock did NOT cover
// it (both racers touch the WriteCore object, not engine state). After the fix
// state_->core is an atomic shared_ptr (AtomicSharedPtr<WriteCore> — a portable
// stand-in for std::atomic<std::shared_ptr<WriteCore>>, which libstdc++ provides
// only since GCC 12), so the worker pins the core for the whole call and the swap
// can never free it underneath (matching the object-store host's std::atomic<ObjectStore*>).
//
// This is a TSan-only regression guard: its real pass criterion is "ThreadSanitizer
// reports nothing", so it skips itself when not built with -fsanitize=thread (a
// plain build would just spin the stress loop for no signal). Run under ctest,
// which sets a per-test TIMEOUT so a regression that deadlocks fails loudly.
TEST(EngineThreadSafety, SetTargetSwapVsWorkerEnsureTopicRace) {
#ifndef PJ_TSAN_BUILD
  GTEST_SKIP() << "WriteCore-swap race guard is only meaningful under ThreadSanitizer "
                  "(configure with -DPJ_ENABLE_TSAN=ON, e.g. ./build.sh --tsan)";
#endif
  using namespace PJ::sdk;
  DataEngine live;
  DataEngine staging;
  ObjectStore store;
  DatastoreToolboxHost toolbox_impl{live, store};
  ToolboxHostView toolbox{toolbox_impl.raw()};
  const auto source = *toolbox.createDataSource("sensor");
  ASSERT_TRUE(staging.createDataset(DatasetDescriptor{.source_name = "sensor"}, source.id).has_value());

  DatastoreSourceWriteHost host(live, source);
  host.setSecondaryEngine(&staging);
  SourceWriteHostView writer(host.raw());

  std::atomic<bool> stop{false};
  std::thread worker([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      (void)writer.ensureTopic("imu");  // loads state_->core -> WriteCore::ensureTopic
    }
  });
  std::thread manager([&]() {
    for (int i = 0; i < 5000; ++i) {
      host.setTarget(i % 2 == 0 ? &staging : &live);  // publishes a new WriteCore, drops the old
    }
    stop.store(true, std::memory_order_release);
  });
  worker.join();
  manager.join();

  // The host must remain usable after the swap storm (the final target is `live`).
  const auto final_topic = writer.ensureTopic("imu");
  ASSERT_TRUE(final_topic.has_value()) << final_topic.error();
}

// The GUI merges two loaded datasets while a streaming worker keeps committing
// into (and creating topics in) another dataset of the same engine. mergeDatasets
// reads and rewrites topic storage; before it took the engine lock it raced the
// worker's commits and the topic-map rehash (TSan flags it).
TEST(EngineThreadSafety, MergeDatasetsSerializesWithWorkerCommits) {
  DataEngine engine;
  const DatasetId anchor = *engine.createDataset(DatasetDescriptor{.source_name = "anchor", .time_domain_id = 0});
  const TopicId anchor_topic = makeTopic(engine, anchor, "s", 64);
  const DatasetId live = *engine.createDataset(DatasetDescriptor{.source_name = "live", .time_domain_id = 0});

  // DataWriter callers hold the engine lock (as the C-ABI write host does per
  // call); mergeDatasets is called WITHOUT it, as SessionManager does — its own
  // lock is what this test exercises.
  std::atomic<bool> stop{false};
  std::thread worker([&]() {
    for (int i = 0; !stop.load(std::memory_order_relaxed); ++i) {
      const auto lock = engine.lockEngine();
      (void)makeTopic(engine, live, "w" + std::to_string(i), 8);  // commit + createTopic rehash
    }
  });

  constexpr int kMerges = 200;
  for (int m = 0; m < kMerges; ++m) {
    const DatasetId source =
        *engine.createDataset(DatasetDescriptor{.source_name = "src" + std::to_string(m), .time_domain_id = 0});
    {
      const auto lock = engine.lockEngine();
      (void)makeTopic(engine, source, "s", 16);
    }
    ASSERT_TRUE(engine.mergeDatasets(anchor, {{source, (m + 1) * 1000}}).has_value());
  }
  stop.store(true, std::memory_order_relaxed);
  worker.join();

  DataReader reader = engine.createReader();
  EXPECT_EQ(readAll(reader, anchor_topic), 64U + kMerges * 16U);
}

// A toolbox registers an object topic on an existing dataset while datasets are
// created concurrently (the datasets map rehashes). The dataset lookup must hold
// the engine lock (TSan flags the unlocked read).
TEST(EngineThreadSafety, ToolboxObjectTopicRegistrationVsDatasetCreation) {
  DataEngine engine;
  ObjectStore store;
  DatastoreToolboxHost toolbox_impl{engine, store};
  sdk::ToolboxHostView toolbox{toolbox_impl.raw()};
  const auto source = *toolbox.createDataSource("target");
  const auto dataset = static_cast<DatasetId>(source.id);

  constexpr int kIterations = 4000;
  std::thread creator([&]() {
    for (int i = 0; i < kIterations; ++i) {
      (void)engine.createDataset(DatasetDescriptor{.source_name = "d" + std::to_string(i), .time_domain_id = 0});
    }
  });
  int registered = 0;
  for (int i = 0; i < kIterations; ++i) {
    registered += toolbox.registerObjectTopicOnDataset(dataset, "__markers__/t", "{}").has_value() ? 1 : 0;
  }
  creator.join();
  EXPECT_EQ(registered, kIterations);
}

}  // namespace
}  // namespace PJ
