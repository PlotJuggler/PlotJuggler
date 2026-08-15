// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Live-follow streaming cost of DatastoreCurveAdapter: each tick commits a
// batch of fresh samples, invalidates the adapter, slides the visible window
// to the live edge, and (in the FullRead variant) reads every visible sample
// the way Qwt's point mappers do on a repaint. Run with fixed --benchmark
// iteration counts when comparing branches: the topic grows as ticks run, so
// free-running iteration counts are not apples-to-apples.

#include <QPointF>
#include <QRectF>
#include <cmath>
#include <cstddef>
#include <memory>

#include "benchmark/benchmark.h"
#include "pj_base/type_tree.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plotting/DatastoreCurveAdapter.h"
#include "pj_runtime/CurveDescriptor.h"
#include "pj_runtime/SessionManager.h"

namespace PJ {
namespace {

constexpr Timestamp kNs = 1'000'000'000;
constexpr Timestamp kSampleRateHz = 1000;  // source sample rate
constexpr Timestamp kTickRateHz = 30;      // GUI streaming tick rate
constexpr Timestamp kVisibleSeconds = 10;  // live-follow window width
constexpr Timestamp kPrefillSeconds = 60;  // history present before ticking
constexpr Timestamp kSamplePeriodNs = kNs / kSampleRateHz;
constexpr std::size_t kSamplesPerTick = kSampleRateHz / kTickRateHz;  // 33 (truncation intended)

// One streaming session: engine + topic + adapter, with a live-edge clock.
class StreamingFixture {
 public:
  StreamingFixture() {
    auto dataset_or = session_.dataEngine().createDataset(DatasetDescriptor{.source_name = "bench"});
    dataset_id_ = *dataset_or;

    auto writer = session_.dataEngine().createWriter();
    auto schema_or = writer.registerSchema("sample", makePrimitive("value", PrimitiveType::kFloat64));
    TopicDescriptor descriptor;
    descriptor.name = "/bench";
    descriptor.schema_id = *schema_or;
    auto topic_or = writer.registerTopic(dataset_id_, descriptor);
    topic_id_ = *topic_or;

    appendSamples(writer, kPrefillSeconds * kSampleRateHz);
    (void)session_.commitChunks(writer.flushAll());

    adapter_ = std::make_unique<DatastoreCurveAdapter>(
        &session_, CurveDescriptor{
                       .name = "/bench/value",
                       .topic_id = topic_id_,
                       .dataset_id = dataset_id_,
                       .column_index = 0,
                       .field_path = "value",
                   });
    slideWindowToLiveEdge();
    (void)adapter_->size();  // prime: the initial full build is not a per-tick cost
  }

  // The ingestion half of a tick (writer, encode, commit) — excluded from the
  // timed region so the benchmark isolates the ADAPTER maintenance cost.
  void ingest() {
    auto writer = session_.dataEngine().createWriter();
    (void)writer.bindTopicWriter(topic_id_);
    appendSamples(writer, kSamplesPerTick);
    (void)session_.commitChunks(writer.flushAll());
  }

  // The adapter half of a tick: invalidate and follow the live edge.
  void invalidateAndSlide() {
    adapter_->onTopicCommitted();
    slideWindowToLiveEdge();
  }

  // Post-run oracle: the served window must end at the live edge and hold
  // roughly one visible-window of samples — catches a stale or duplicated
  // cache that a pure timing loop would happily reward.
  void verifyServedWindow(benchmark::State& state) {
    const std::size_t count = adapter_->size();
    const std::size_t expected = kVisibleSeconds * kSampleRateHz;
    if (count < expected || count > expected + kSamplesPerTick + 2) {
      state.SkipWithError("served window count off: cache stale or duplicated");
      return;
    }
    const double live_edge_sec = static_cast<double>(next_timestamp_ - kSamplePeriodNs) / static_cast<double>(kNs);
    if (std::abs(adapter_->sample(count - 1).x() - live_edge_sec) > 1e-9) {
      state.SkipWithError("served window does not end at the live edge");
    }
  }

  // What Qwt's point mappers do per repaint: read every visible sample.
  double readAllVisible() {
    double sum = 0.0;
    const std::size_t count = adapter_->size();
    for (std::size_t i = 0; i < count; ++i) {
      sum += adapter_->sample(i).y();
    }
    return sum;
  }

  [[nodiscard]] std::size_t visibleCount() const {
    return adapter_->size();
  }

 private:
  void appendSamples(DataWriter& writer, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
      (void)writer.beginRow(topic_id_, next_timestamp_);
      writer.set(topic_id_, 0, static_cast<double>(next_timestamp_ % 997));
      (void)writer.finishRow(topic_id_);
      next_timestamp_ += kSamplePeriodNs;
    }
  }

  void slideWindowToLiveEdge() {
    const double live_edge_sec = static_cast<double>(next_timestamp_) / static_cast<double>(kNs);
    const double window_start_sec = live_edge_sec - static_cast<double>(kVisibleSeconds);
    adapter_->setRectOfInterest(QRectF(QPointF(window_start_sec, -1.0), QPointF(live_edge_sec, 1.0)));
  }

  SessionManager session_;
  DatasetId dataset_id_ = 0;
  TopicId topic_id_ = 0;
  Timestamp next_timestamp_ = 0;
  std::unique_ptr<DatastoreCurveAdapter> adapter_;
};

// Window maintenance alone: the adapter-side cost of one streaming tick
// (rebuild or incremental extend), without the per-repaint mapper sweep.
void BM_StreamTick_WindowMaintenance(benchmark::State& state) {
  StreamingFixture fixture;
  for (auto _ : state) {
    state.PauseTiming();
    fixture.ingest();
    state.ResumeTiming();
    fixture.invalidateAndSlide();
    benchmark::DoNotOptimize(fixture.visibleCount());
  }
  fixture.verifyServedWindow(state);
  state.counters["visible_samples"] = static_cast<double>(fixture.visibleCount());
}
BENCHMARK(BM_StreamTick_WindowMaintenance)->Iterations(600)->Unit(benchmark::kMicrosecond);

// Full tick: maintenance plus the Qwt-style read of every visible sample.
void BM_StreamTick_FullRead(benchmark::State& state) {
  StreamingFixture fixture;
  for (auto _ : state) {
    state.PauseTiming();
    fixture.ingest();
    state.ResumeTiming();
    fixture.invalidateAndSlide();
    benchmark::DoNotOptimize(fixture.readAllVisible());
  }
  fixture.verifyServedWindow(state);
  state.counters["visible_samples"] = static_cast<double>(fixture.visibleCount());
}
BENCHMARK(BM_StreamTick_FullRead)->Iterations(600)->Unit(benchmark::kMicrosecond);

}  // namespace
}  // namespace PJ

BENCHMARK_MAIN();
