// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Contention probe for the DataEngine lock: a worker commits at the FileLoader flush cadence and now and then creates
// a topic, while a GUI reader walks every series in full at about 60 Hz. The lock is exclusive, so a walk blocks the
// writer for its duration. Prints the writer's commit-call and the reader's walk latencies; always exits 0.
// Run: ./build/pj_datastore/concurrency_benchmark [duration_ms] [preseed_rows_per_topic]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "pj_base/dataset.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/writer.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using PJ::DataEngine;
using PJ::DataReader;
using PJ::DatasetDescriptor;
using PJ::DataWriter;
using PJ::NumericType;
using PJ::QueryRange;
using PJ::SampleRow;
using PJ::ScalarSeriesHandle;
using PJ::Timestamp;
using PJ::TopicId;

double msSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

struct Stats {
  std::vector<double> samples_ms;
  void add(double ms) {
    samples_ms.push_back(ms);
  }
  double max() const {
    return samples_ms.empty() ? 0.0 : *std::max_element(samples_ms.begin(), samples_ms.end());
  }
  double avg() const {
    if (samples_ms.empty()) {
      return 0.0;
    }
    double s = 0.0;
    for (double v : samples_ms) {
      s += v;
    }
    return s / static_cast<double>(samples_ms.size());
  }
  double p99() const {
    if (samples_ms.empty()) {
      return 0.0;
    }
    std::vector<double> v = samples_ms;
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(0.99 * static_cast<double>(v.size() - 1))];
  }
};

}  // namespace

int main(int argc, char** argv) {
  const int duration_ms = argc > 1 ? std::atoi(argv[1]) : 3000;
  // Rows per topic seeded before the threads start, so the reader's walks are long from the outset, as after a
  // multi-GB load.
  const long preseed_per_topic = argc > 2 ? std::atol(argv[2]) : 0;
  constexpr int kInitialTopics = 8;
  constexpr int kWriterCadenceMs = 50;  // FileLoader flush throttle
  constexpr int kBatchRows = 256;       // samples committed per topic per tick
  constexpr double kStarveThresholdMs = 200.0;

  DataEngine engine;
  auto dataset = engine.createDataset(DatasetDescriptor{.source_name = "bench", .time_domain_id = 0});
  std::vector<TopicId> topics;
  {
    DataWriter w = engine.createWriter();
    for (int i = 0; i < kInitialTopics; ++i) {
      auto h = w.registerScalarSeries(*dataset, "t" + std::to_string(i), NumericType::kFloat64);
      topics.push_back(h->topic_id);
    }
    engine.commitChunks(w.flushAll());
  }
  Timestamp seed_ts = 0;
  if (preseed_per_topic > 0) {
    DataWriter w = engine.createWriter();
    for (TopicId topic : topics) {
      const ScalarSeriesHandle wh{topic, 0};
      for (long i = 0; i < preseed_per_topic; ++i) {
        w.appendScalar(wh, static_cast<Timestamp>(i), 1.0);
      }
    }
    engine.commitChunks(w.flushAll());
    seed_ts = static_cast<Timestamp>(preseed_per_topic);
  }

  std::atomic<bool> stop{false};
  std::atomic<Timestamp> next_ts{seed_ts};
  Stats commit_stats;  // writer thread only
  Stats read_stats;    // reader thread only
  std::atomic<long> reads{0};
  std::atomic<long> commits{0};
  std::atomic<long> extra_topics{0};

  // A commitChunks() call's wall time includes the lock wait, so it shows the writer's starvation.
  std::thread writer([&]() {
    DataWriter w = engine.createWriter();
    int tick = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      const auto tick_start = Clock::now();
      for (TopicId topic : topics) {
        const ScalarSeriesHandle wh{topic, 0};
        Timestamp t = next_ts.fetch_add(kBatchRows, std::memory_order_relaxed);
        for (int i = 0; i < kBatchRows; ++i) {
          w.appendScalar(wh, t + i, 1.0);
        }
      }
      const auto commit_start = Clock::now();
      engine.commitChunks(w.flushAll());  // includes the lock wait
      commit_stats.add(msSince(commit_start));
      commits.fetch_add(1, std::memory_order_relaxed);

      if (++tick % 10 == 0) {
        DataWriter setup = engine.createWriter();
        auto h = setup.registerScalarSeries(*dataset, "x" + std::to_string(tick), NumericType::kFloat64);
        if (h.has_value()) {
          extra_topics.fetch_add(1, std::memory_order_relaxed);
        }
      }
      const double elapsed = msSince(tick_start);
      if (elapsed < kWriterCadenceMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kWriterCadenceMs - static_cast<int>(elapsed)));
      }
    }
  });

  // Each cursor holds the engine lock while it lives, so a long walk holds back the writer.
  std::thread reader([&]() {
    DataReader r = engine.createReader();
    while (!stop.load(std::memory_order_relaxed)) {
      const auto read_start = Clock::now();
      for (TopicId topic : topics) {
        auto cur = r.rangeQuery(
            QueryRange{
                .topic_id = topic,
                .t_min = std::numeric_limits<Timestamp>::min(),
                .t_max = std::numeric_limits<Timestamp>::max()});
        if (cur.has_value()) {
          std::size_t n = 0;
          cur->forEach([&n](const SampleRow&) { ++n; });
        }
      }
      read_stats.add(msSince(read_start));
      reads.fetch_add(1, std::memory_order_relaxed);
      std::this_thread::sleep_for(std::chrono::milliseconds(16));  // ~60Hz paint cadence
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
  stop.store(true, std::memory_order_relaxed);
  reader.join();
  writer.join();

  const double max_commit = commit_stats.max();
  std::printf("\n=== DataEngine lock contention probe (%d ms) ===\n", duration_ms);
  std::printf("topics: %d initial + %ld created mid-run\n", kInitialTopics, extra_topics.load());
  std::printf(
      "writer: %ld commits @ ~%dms cadence | commit-call ms: avg %.3f  p99 %.3f  MAX %.3f\n", commits.load(),
      kWriterCadenceMs, commit_stats.avg(), commit_stats.p99(), max_commit);
  std::printf(
      "reader: %ld full-range walks | walk ms: avg %.3f  p99 %.3f  MAX %.3f\n", reads.load(), read_stats.avg(),
      read_stats.p99(), read_stats.max());
  const char* verdict = max_commit > kStarveThresholdMs
                            ? "STARVED -> incremental rebuild justified"
                            : "not starved -> engine-wide lock OK, incremental rebuild deferrable";
  std::printf(
      "verdict: writer %s (max commit-call %.1fms vs %.0fms starvation threshold)\n", verdict, max_commit,
      kStarveThresholdMs);
  return 0;
}
