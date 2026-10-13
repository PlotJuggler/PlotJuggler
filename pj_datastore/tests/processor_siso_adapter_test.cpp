// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/processor_siso_adapter.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "pj_base/dataset.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/data_processor.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_datastore/writer.hpp"

namespace PJ::proc {
namespace {

// Minimal DataProcessor stand-ins — the C++ builtins were retired (M9); the
// adapter is exercised with these instead of a real filter.
struct PassThrough : DataProcessor {
  [[nodiscard]] const char* id() const override {
    return "passthrough";
  }
  [[nodiscard]] const char* bracketLabel() const override {
    return "PassThrough";
  }
  [[nodiscard]] TraitMask traits() const override {
    return kStatelessOneToOne;
  }
  [[nodiscard]] bool isStreamSafe() const override {
    return true;
  }
  void reset() override {}
  [[nodiscard]] std::optional<Sample> calculateNextPoint(const Sample& in) override {
    return in;
  }
};
struct AbsStub : DataProcessor {
  [[nodiscard]] const char* id() const override {
    return "abs_stub";
  }
  [[nodiscard]] const char* bracketLabel() const override {
    return "Abs";
  }
  [[nodiscard]] TraitMask traits() const override {
    return kStatelessOneToOne;
  }
  [[nodiscard]] bool isStreamSafe() const override {
    return true;
  }
  void reset() override {}
  [[nodiscard]] std::optional<Sample> calculateNextPoint(const Sample& in) override {
    return Sample::scalar(in.raw_ts_ns, PJ::VarValue{std::fabs(std::get<double>(in.value()))});
  }
};

PJ::TopicId writeScalarTopic(
    PJ::DataEngine& engine, PJ::DatasetId ds, const std::vector<std::pair<PJ::Timestamp, double>>& rows) {
  PJ::DataWriter writer = engine.createWriter();
  auto handle = writer.registerScalarSeries(ds, "src", PJ::NumericType::kFloat64);
  const PJ::TopicId tid = handle->topic_id;
  for (const auto& [ts, v] : rows) {
    writer.appendScalar(*handle, ts, v);
  }
  engine.commitChunks(writer.flushAll());
  return tid;
}

std::vector<double> readValues(PJ::DataEngine& engine, PJ::TopicId tid) {
  std::vector<double> out;
  const PJ::TopicStorage* storage = engine.getTopicStorage(tid);
  if (!storage) {
    return out;
  }
  auto cursor = PJ::rangeQuery(storage->sealedChunks(), 0, std::numeric_limits<PJ::Timestamp>::max());
  cursor.forEach([&](const PJ::SampleRow& row) { out.push_back(row.chunk->readNumericAsDouble(0, row.row_index)); });
  return out;
}

// The keystone: a DataProcessor, wrapped by the adapter, installed as a real
// IncrementalEngine node, materializes a correct filtered output topic.
TEST(ProcessorSisoAdapterTest, AbsoluteRunsThroughEngineAndMaterializesOutput) {
  PJ::DataEngine engine;
  PJ::IncrementalEngine derived(engine);
  const PJ::DatasetId ds = *engine.createDataset(PJ::DatasetDescriptor{.source_name = "t", .time_domain_id = 0});
  const PJ::TopicId src = writeScalarTopic(engine, ds, {{0, -1.0}, {1'000'000'000LL, 2.0}, {2'000'000'000LL, -3.0}});

  auto processor = std::make_shared<AbsStub>();
  auto node = derived.addSisoTransform(src, "src[Absolute]", ds, std::make_unique<ProcessorSisoAdapter>(processor));
  ASSERT_TRUE(node.has_value()) << node.error();

  derived.onSourceCommitted(std::vector<PJ::TopicId>{src});
  ASSERT_TRUE(derived.scheduleAll().has_value());

  const auto outs = derived.outputTopics(*node);
  ASSERT_EQ(outs.size(), 1u);
  const auto vals = readValues(engine, outs[0]);
  ASSERT_EQ(vals.size(), 3u);
  EXPECT_DOUBLE_EQ(vals[0], 1.0);
  EXPECT_DOUBLE_EQ(vals[1], 2.0);
  EXPECT_DOUBLE_EQ(vals[2], 3.0);
}

// A suppressing processor (None always emits, but exercise the per-sample bridge
// for timestamp + value pass-through) keeps timestamp and value through the adapter.
TEST(ProcessorSisoAdapterTest, BridgesTimestampAndValuePerSample) {
  auto processor = std::make_shared<PassThrough>();
  const long before = processor.use_count();
  ProcessorSisoAdapter adapter(processor);
  EXPECT_GT(processor.use_count(), before);  // adapter shares ownership

  PJ::Timestamp out_time = 0;
  PJ::VarValue out_value;
  ASSERT_TRUE(adapter.calculate(7, PJ::VarValue{1.5}, out_time, out_value));
  EXPECT_EQ(out_time, 7);
  EXPECT_DOUBLE_EQ(std::get<double>(out_value), 1.5);
}

}  // namespace
}  // namespace PJ::proc
