// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Worker-write vs GUI-read race that progressive file loading exposes: a worker thread commits chunks (a
// push_back on the topic's sealed-chunk deque) while a reader thread iterates the same deque via
// getMetadata()/rangeQuery(). Without engine synchronization this is a std::deque data race; with the engine
// lock it must run to completion with a consistent final row count. A lock-order deadlock instead hangs both
// threads and trips the 90 s ctest TIMEOUT.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <latch>
#include <limits>
#include <thread>

#include "pj_base/dataset.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/writer.hpp"

namespace PJ {
namespace {

TEST(EngineConcurrencyTest, ConcurrentCommitAndReadDoesNotRace) {
  DataEngine engine;
  auto dataset = engine.createDataset(DatasetDescriptor{.source_name = "race", .time_domain_id = 0});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();

  DataWriter setup = engine.createWriter();
  auto handle = setup.registerScalarSeries(*dataset, "v", NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value()) << handle.error();
  const TopicId topic = handle->topic_id;

  // Both the fill and the reader passes are bounded: each full-range read serializes against the writer, so an
  // unbounded reader costs O(passes * N) with `passes` set by lock fairness, which can exceed the ctest TIMEOUT
  // on correct code. The race fires regardless of size, so the caps only ever catch a true hang.
  constexpr int kBatches = 50;  // 3.2k samples
  constexpr int kSamplesPerBatch = 64;
  constexpr int kMaxReaderPasses = 1000;  // bounds the O(passes*N) reader walk
  std::atomic<bool> writer_done{false};
  std::atomic<int> completed_reads{0};
  // The writer blocks on this after its first commit until the reader has completed one pass, so the
  // commit/read overlap is deterministic: when both threads share a core (ctest -j, a single-vCPU runner,
  // taskset -c 0) the scheduler would otherwise run the tiny writer to completion first, leaving
  // completed_reads == 0 and failing the EXPECT_GT below on correct code.
  std::latch first_read_done{1};

  // Writer: commits kBatches chunks back-to-back; after the first commit it waits for one reader pass (see
  // `first_read_done`), so the remaining batches overlap a live reader. It holds no engine lock while waiting,
  // so a correct reader always makes progress.
  std::thread writer([&]() {
    DataWriter w = engine.createWriter();
    const ScalarSeriesHandle wh{topic, 0};
    Timestamp t = 0;
    for (int batch = 0; batch < kBatches; ++batch) {
      for (int i = 0; i < kSamplesPerBatch; ++i) {
        w.appendScalar(wh, t++, 1.0);
      }
      engine.commitChunks(w.flushAll());
      if (batch == 0) {
        first_read_done.wait();
      }
      // Hand the core to the reader so the remaining commits interleave with
      // reads rather than draining in a single scheduling quantum.
      std::this_thread::yield();
    }
    writer_done.store(true, std::memory_order_release);
  });

  // Reader: reads metadata + iterates the full range for up to kMaxReaderPasses passes while the writer fills,
  // mirroring the GUI's catalog rebuild + plot reads during a load. Each read holds the engine lock for its
  // whole forEach, so it never observes a deque torn mid-push_back. If the reader caps out first, the writer
  // finishes the remaining batches uncontended.
  std::thread reader([&]() {
    DataReader r = engine.createReader();
    for (int i = 0; i < kMaxReaderPasses && !writer_done.load(std::memory_order_acquire); ++i) {
      (void)r.getMetadata(topic);
      auto cursor = r.rangeQuery(
          QueryRange{
              .topic_id = topic,
              .t_min = std::numeric_limits<Timestamp>::min(),
              .t_max = std::numeric_limits<Timestamp>::max()});
      if (cursor.has_value()) {
        std::size_t rows = 0;
        cursor->forEach([&rows](const SampleRow&) { ++rows; });
      }
      // Release the writer's first-commit wait on the very first pass, so the
      // remaining writes overlap a confirmed-live reader (fetch_add returns the
      // prior value, so == 0 means "this is the first pass").
      if (completed_reads.fetch_add(1, std::memory_order_relaxed) == 0) {
        first_read_done.count_down();
      }
      // Yield so the writer (blocked on the same exclusive lock) gets a turn:
      // without this the reader re-acquires the lock before the writer's wakeup
      // wins, starving the writer's commit loop into an effective livelock.
      std::this_thread::yield();
    }
  });

  reader.join();
  writer.join();

  // The reader overlapped the writer (no crash / no timeout = reads serialized safely)...
  EXPECT_GT(completed_reads.load(), 0);
  // ...and after the run every committed row is visible and consistent.
  DataReader verify = engine.createReader();
  auto final_cursor = verify.rangeQuery(
      QueryRange{
          .topic_id = topic,
          .t_min = std::numeric_limits<Timestamp>::min(),
          .t_max = std::numeric_limits<Timestamp>::max()});
  ASSERT_TRUE(final_cursor.has_value());
  std::size_t total = 0;
  final_cursor->forEach([&total](const SampleRow&) { ++total; });
  EXPECT_EQ(total, static_cast<std::size_t>(kBatches) * kSamplesPerBatch);
}

}  // namespace
}  // namespace PJ
