// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/derived_engine.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include "pj_base/dataset.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/builtin_transforms.hpp"
#include "pj_datastore/chunk.hpp"
#include "pj_datastore/column_buffer.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_datastore/writer.hpp"

namespace PJ {
namespace {

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

// Create a dataset and return its id.
static PJ::DatasetId makeDataset(DataEngine& engine, const std::string& name = "test") {
  auto id_or = engine.createDataset(PJ::DatasetDescriptor{.source_name = name, .time_domain_id = 0});
  return *id_or;
}

// Write `n` rows to a float64 scalar topic with value = slope * (t_ns / 1e9).
// Commits the chunk. Returns the TopicId.
// Timestamps: 0, step_ns, 2*step_ns, ...
static PJ::TopicId makeLinearTopic(
    DataEngine& engine, PJ::DatasetId dataset_id, double slope, int n, PJ::Timestamp step_ns = 1'000'000'000LL) {
  DataWriter writer = engine.createWriter();
  auto handle_or = writer.registerScalarSeries(dataset_id, "src", PJ::NumericType::kFloat64);
  PJ::TopicId tid = handle_or->topic_id;
  for (int i = 0; i < n; ++i) {
    PJ::Timestamp ts = static_cast<PJ::Timestamp>(i) * step_ns;
    double v = slope * (static_cast<double>(i) * static_cast<double>(step_ns) * 1e-9);
    writer.appendScalar(*handle_or, ts, v);
  }
  // commit_chunks returns the changed topic IDs — pass directly to on_source_committed.
  engine.commitChunks(writer.flushAll());
  return tid;
}

// Append `n` more rows to an existing scalar topic (continuing timestamps from start_i).
static void appendLinearRows(
    DataEngine& engine, PJ::TopicId src_topic_id, double slope, int n, int start_i,
    PJ::Timestamp step_ns = 1'000'000'000LL) {
  // We need to write to an existing topic via begin_row / set_float64 / finish_row.
  DataWriter writer = engine.createWriter();
  for (int i = start_i; i < start_i + n; ++i) {
    PJ::Timestamp ts = static_cast<PJ::Timestamp>(i) * step_ns;
    double v = slope * (static_cast<double>(i) * static_cast<double>(step_ns) * 1e-9);
    auto s = writer.beginRow(src_topic_id, ts);
    (void)s;
    writer.set(src_topic_id, 0, v);
    auto s2 = writer.finishRow(src_topic_id);
    (void)s2;
  }
  engine.commitChunks(writer.flushAll());
}

// Collect all float64 values from a topic in timestamp order.
static std::vector<double> collectValues(DataEngine& engine, PJ::TopicId topic_id) {
  const TopicStorage* storage = engine.getTopicStorage(topic_id);
  if (!storage) {
    return {};
  }
  std::vector<double> out;
  auto cursor = rangeQuery(storage->sealedChunks(), 0, std::numeric_limits<PJ::Timestamp>::max());
  cursor.forEach([&](const SampleRow& row) { out.push_back(row.chunk->readNumericAsDouble(0, row.row_index)); });
  return out;
}

// Wrapper: on_source_committed from an initializer list (std::span can't take {}).
static void notify(DerivedEngine& derived, std::initializer_list<PJ::TopicId> topics) {
  std::vector<PJ::TopicId> v(topics);
  derived.onSourceCommitted(v);
}

// Collect (timestamp, value) pairs.
static std::vector<std::pair<PJ::Timestamp, double>> collectRows(DataEngine& engine, PJ::TopicId topic_id) {
  const TopicStorage* storage = engine.getTopicStorage(topic_id);
  if (!storage) {
    return {};
  }
  std::vector<std::pair<PJ::Timestamp, double>> out;
  auto cursor = rangeQuery(storage->sealedChunks(), 0, std::numeric_limits<PJ::Timestamp>::max());
  cursor.forEach(
      [&](const SampleRow& row) { out.emplace_back(row.timestamp, row.chunk->readNumericAsDouble(0, row.row_index)); });
  return out;
}

// ---------------------------------------------------------------------------
// DerivativeTransform — unit tests (no engine needed)
// ---------------------------------------------------------------------------

TEST(DerivativeTransformTest, SkipsFirstRow) {
  DerivativeTransform op;
  PJ::Timestamp t = 0;
  VarValue v = 0.0;
  EXPECT_FALSE(op.calculate(0, VarValue{0.0}, t, v));
}

TEST(DerivativeTransformTest, CorrectDerivative_ConstantRate) {
  DerivativeTransform op;
  PJ::Timestamp out_t;
  VarValue out_v = 0.0;

  // slope = 3.0, step = 1s → dy/dt should be 3.0
  EXPECT_FALSE(op.calculate(0, VarValue{0.0}, out_t, out_v));
  EXPECT_TRUE(op.calculate(1'000'000'000LL, VarValue{3.0}, out_t, out_v));
  EXPECT_EQ(out_t, 1'000'000'000LL);
  EXPECT_NEAR(std::get<double>(out_v), 3.0, 1e-9);

  EXPECT_TRUE(op.calculate(2'000'000'000LL, VarValue{6.0}, out_t, out_v));
  EXPECT_NEAR(std::get<double>(out_v), 3.0, 1e-9);
}

TEST(DerivativeTransformTest, ZeroDt_DoesNotDivideByZero) {
  // dt = 0 should produce 0.0, not inf/nan
  DerivativeTransform op;
  PJ::Timestamp out_t;
  VarValue out_v = 0.0;
  EXPECT_FALSE(op.calculate(0, VarValue{1.0}, out_t, out_v));
  EXPECT_TRUE(op.calculate(0, VarValue{2.0}, out_t, out_v));
  EXPECT_EQ(std::get<double>(out_v), 0.0);
}

TEST(DerivativeTransformTest, Reset_ClearsState) {
  DerivativeTransform op;
  PJ::Timestamp out_t;
  VarValue out_v = 0.0;

  // First pass
  EXPECT_FALSE(op.calculate(0, VarValue{0.0}, out_t, out_v));
  EXPECT_TRUE(op.calculate(1'000'000'000LL, VarValue{5.0}, out_t, out_v));

  // After reset, first row must be suppressed again
  op.reset();
  EXPECT_FALSE(op.calculate(0, VarValue{0.0}, out_t, out_v));
  EXPECT_TRUE(op.calculate(1'000'000'000LL, VarValue{5.0}, out_t, out_v));
  EXPECT_NEAR(std::get<double>(out_v), 5.0, 1e-9);
}

TEST(DerivativeTransformTest, OutputKind_IsFloat64) {
  DerivativeTransform op;
  EXPECT_EQ(op.outputKind(StorageKind::kFloat64), StorageKind::kFloat64);
  EXPECT_EQ(op.outputKind(StorageKind::kFloat32), StorageKind::kFloat64);
  EXPECT_EQ(op.outputKind(StorageKind::kInt64), StorageKind::kFloat64);
}

// ---------------------------------------------------------------------------
// add_siso_transform
// ---------------------------------------------------------------------------

TEST(DerivedEngineTest, AddTransform_CreatesOutputTopic) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId src = makeLinearTopic(engine, ds, 1.0, 5);
  auto node_or = derived.addSisoTransform(src, "deriv", ds, std::make_unique<DerivativeTransform>());
  ASSERT_TRUE(node_or.has_value()) << node_or.error();
  PJ::NodeId node = *node_or;
  EXPECT_TRUE(derived.hasNode(node));

  auto out_topics = derived.outputTopics(node);
  ASSERT_EQ(out_topics.size(), 1u);
  EXPECT_NE(engine.getTopicStorage(out_topics[0]), nullptr);
}

TEST(DerivedEngineTest, AddTransform_DuplicateOutputName_Fails) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId src = makeLinearTopic(engine, ds, 1.0, 5);
  ASSERT_TRUE(derived.addSisoTransform(src, "deriv", ds, std::make_unique<DerivativeTransform>()).has_value());
  // Same output name → should fail
  auto r = derived.addSisoTransform(src, "deriv", ds, std::make_unique<DerivativeTransform>());
  EXPECT_FALSE(r.has_value());
}

TEST(DerivedEngineTest, AddTransform_UnknownInputTopic_Fails) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  auto r = derived.addSisoTransform(9999u, "deriv", ds, std::make_unique<DerivativeTransform>());
  EXPECT_FALSE(r.has_value());
}

// ---------------------------------------------------------------------------
// Field selection: bind a SISO node to ONE column of a multi-column topic
// (single-column field selection; the "got 33 columns" fix).
// ---------------------------------------------------------------------------

// Pass-through SISO: emits the (float64) input value unchanged, so a test can
// assert exactly which input column the node read.
class IdentityTransform : public ISISOTransform {
 public:
  bool calculate(PJ::Timestamp time, const VarValue& input, PJ::Timestamp& out_time, VarValue& out_value) override {
    out_time = time;
    out_value = std::get<double>(input);
    return true;
  }
};

// Doubles the (float64) input value — used to verify an in-place op swap.
class ScaleByTwoTransform : public ISISOTransform {
 public:
  bool calculate(PJ::Timestamp time, const VarValue& input, PJ::Timestamp& out_time, VarValue& out_value) override {
    out_time = time;
    out_value = 2.0 * std::get<double>(input);
    return true;
  }
};

// Records reset order while otherwise passing samples through unchanged.
class ResetTracingIdentityTransform : public ISISOTransform {
 public:
  ResetTracingIdentityTransform(std::string label, std::vector<std::string>& reset_trace)
      : label_(std::move(label)), reset_trace_(reset_trace) {}

  void reset() override {
    reset_trace_.push_back(label_);
  }

  bool calculate(PJ::Timestamp time, const VarValue& input, PJ::Timestamp& out_time, VarValue& out_value) override {
    out_time = time;
    out_value = input;
    return true;
  }

 private:
  std::string label_;
  std::vector<std::string>& reset_trace_;
};

// Emits the first N rows (identity), then goes sticky-failed: calculate() returns false and
// failed()==true thereafter — models a Luau filter that errors mid-stream. reset() rearms it.
class FailAfterNTransform : public ISISOTransform {
 public:
  explicit FailAfterNTransform(int n) : n_(n) {}
  void reset() override {
    seen_ = 0;
    failed_ = false;
  }
  bool calculate(PJ::Timestamp time, const VarValue& input, PJ::Timestamp& out_time, VarValue& out_value) override {
    if (seen_ >= n_) {
      failed_ = true;
      return false;
    }
    ++seen_;
    out_time = time;
    out_value = std::get<double>(input);
    return true;
  }
  [[nodiscard]] bool failed() const override {
    return failed_;
  }
  [[nodiscard]] const std::string& error() const override {
    static const std::string kErr = "FailAfterNTransform: boom";
    return kErr;
  }

 private:
  int n_;
  int seen_ = 0;
  bool failed_ = false;
};

// Build a committed 3-column float64 topic "xyz". For row i in [0,n):
//   col0 (x) = i, col1 (y) = 100+i, col2 (z) = 200+i; ts = 1000+i (ascending).
static PJ::TopicId make_three_column_topic(DataEngine& engine, PJ::DatasetId ds, int n) {
  DataWriter writer = engine.createWriter();
  auto schema = makeStruct(
      "xyz", {makePrimitive("x", PrimitiveType::kFloat64), makePrimitive("y", PrimitiveType::kFloat64),
              makePrimitive("z", PrimitiveType::kFloat64)});
  PJ::SchemaId schema_id = *writer.registerSchema("xyz", schema);
  TopicDescriptor td;
  td.name = "xyz_topic";
  td.schema_id = schema_id;
  td.dataset_id = ds;
  PJ::TopicId tid = *writer.registerTopic(ds, td);
  for (int i = 0; i < n; ++i) {
    PJ::Timestamp ts = 1000 + i;
    (void)writer.beginRow(tid, ts);
    writer.set(tid, 0, static_cast<double>(i));
    writer.set(tid, 1, static_cast<double>(100 + i));
    writer.set(tid, 2, static_cast<double>(200 + i));
    (void)writer.finishRow(tid);
  }
  engine.commitChunks(writer.flushAll());
  return tid;
}

TEST(DerivedEngineFieldSelectTest, SisoBindsSelectedColumn) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);
  PJ::TopicId tid = make_three_column_topic(engine, ds, 5);

  // Bind to column 1 (y = 100+i) of a 3-column topic.
  auto node_or =
      derived.addSisoTransform(tid, "y_id", ds, std::make_unique<IdentityTransform>(), /*input_column_index=*/1);
  ASSERT_TRUE(node_or.has_value()) << node_or.error();
  ASSERT_TRUE(derived.scheduleAll().has_value());

  auto vals = collectValues(engine, derived.outputTopics(*node_or)[0]);
  ASSERT_EQ(vals.size(), 5u);
  for (int i = 0; i < 5; ++i) {
    EXPECT_DOUBLE_EQ(vals[static_cast<std::size_t>(i)], static_cast<double>(100 + i));  // col 1, not col 0 (=i)
  }
}

TEST(DerivedEngineFieldSelectTest, SisoColumnOutOfRange_Fails) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);
  PJ::TopicId tid = make_three_column_topic(engine, ds, 3);

  // Only columns 0..2 exist; column 3 is out of range.
  auto r = derived.addSisoTransform(tid, "bad", ds, std::make_unique<IdentityTransform>(), /*input_column_index=*/3);
  EXPECT_FALSE(r.has_value());
}

TEST(DerivedEngineFieldSelectTest, SisoDefaultColumnIsZero) {
  // Existing call sites pass no column index → must still bind column 0.
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);
  PJ::TopicId tid = make_three_column_topic(engine, ds, 4);

  auto node_or = derived.addSisoTransform(tid, "x_id", ds, std::make_unique<IdentityTransform>());
  ASSERT_TRUE(node_or.has_value()) << node_or.error();
  ASSERT_TRUE(derived.scheduleAll().has_value());

  auto vals = collectValues(engine, derived.outputTopics(*node_or)[0]);
  ASSERT_EQ(vals.size(), 4u);
  for (int i = 0; i < 4; ++i) {
    EXPECT_DOUBLE_EQ(vals[static_cast<std::size_t>(i)], static_cast<double>(i));  // col 0
  }
}

TEST(DerivedEngineFieldSelectTest, InputBindingState_SisoResolveRestoreAndReplay) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId dataset = makeDataset(engine);
  PJ::TopicId input_topic = make_three_column_topic(engine, dataset, 4);

  auto node = derived.addSisoTransform(input_topic, "selected", dataset, std::make_unique<IdentityTransform>(), 1);
  ASSERT_TRUE(node.has_value()) << node.error();

  auto original = derived.inputBindingState(*node);
  ASSERT_TRUE(original.has_value()) << original.error();
  EXPECT_EQ(original->columns, (std::vector<std::size_t>{1}));
  EXPECT_EQ(original->kinds, (std::vector<StorageKind>{StorageKind::kFloat64}));

  auto resolved = derived.resolvedInputBindingState(*node, {2});
  ASSERT_TRUE(resolved.has_value()) << resolved.error();
  EXPECT_EQ(resolved->columns, (std::vector<std::size_t>{2}));
  EXPECT_EQ(resolved->kinds, (std::vector<StorageKind>{StorageKind::kFloat64}));
  ASSERT_TRUE(derived.restoreInputBindingState(*node, *resolved).has_value());

  auto restored = derived.inputBindingState(*node);
  ASSERT_TRUE(restored.has_value()) << restored.error();
  EXPECT_EQ(restored->columns, resolved->columns);
  EXPECT_EQ(restored->kinds, resolved->kinds);
  ASSERT_TRUE(derived.scheduleAll().has_value());
  EXPECT_EQ(collectValues(engine, derived.outputTopics(*node)[0]), (std::vector<double>{200, 201, 202, 203}));
}

TEST(DerivedEngineFieldSelectTest, InputBindingState_RejectsInvalidRequests) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId dataset = makeDataset(engine);
  PJ::TopicId input_topic = make_three_column_topic(engine, dataset, 1);
  auto node = derived.addSisoTransform(input_topic, "selected", dataset, std::make_unique<IdentityTransform>());
  ASSERT_TRUE(node.has_value()) << node.error();

  EXPECT_FALSE(derived.inputBindingState(9999u).has_value());
  EXPECT_FALSE(derived.resolvedInputBindingState(9999u, {0}).has_value());
  EXPECT_FALSE(derived.resolvedInputBindingState(*node, {}).has_value());
  EXPECT_FALSE(derived.resolvedInputBindingState(*node, {3}).has_value());
  EXPECT_FALSE(derived.restoreInputBindingState(*node, DerivedEngine::InputBindingState{}).has_value());
  EXPECT_FALSE(derived.restoreInputBindingState(9999u, DerivedEngine::InputBindingState{}).has_value());
}

TEST(DerivedEngineFieldSelectTest, ReplaceSisoTransform_InPlaceRecompute) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);
  PJ::TopicId tid = make_three_column_topic(engine, ds, 5);

  // Identity on column 1 (y = 100+i).
  auto node_or = derived.addSisoTransform(tid, "y", ds, std::make_unique<IdentityTransform>(), 1);
  ASSERT_TRUE(node_or.has_value()) << node_or.error();
  ASSERT_TRUE(derived.scheduleAll().has_value());
  const PJ::TopicId out_tid = derived.outputTopics(*node_or)[0];

  auto before = collectValues(engine, out_tid);
  ASSERT_EQ(before.size(), 5u);
  EXPECT_DOUBLE_EQ(before[0], 100.0);

  // Swap identity for a doubling op: same node, same output topic id, recomputed.
  auto status = derived.replaceSisoTransform(*node_or, std::make_unique<ScaleByTwoTransform>());
  ASSERT_TRUE(status.has_value()) << status.error();
  EXPECT_EQ(derived.outputTopics(*node_or)[0], out_tid);

  auto after = collectValues(engine, out_tid);
  ASSERT_EQ(after.size(), 5u);
  for (int i = 0; i < 5; ++i) {
    EXPECT_DOUBLE_EQ(after[static_cast<std::size_t>(i)], 2.0 * static_cast<double>(100 + i));
  }
}

// [b] A filter that errors mid-stream must NOT commit its truncated prefix as if it were
// valid data: a batch that ends in failure is dropped entirely (Status carries the error).
TEST(DerivedEngineFieldSelectTest, PartialFailure_DoesNotCommitTruncatedOutput) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);
  PJ::TopicId tid = make_three_column_topic(engine, ds, 5);  // col0 = {0,1,2,3,4}

  auto node = derived.addSisoTransform(tid, "fail", ds, std::make_unique<FailAfterNTransform>(2));
  ASSERT_TRUE(node.has_value()) << node.error();

  EXPECT_FALSE(derived.scheduleAll().has_value());                             // failure surfaced as Status
  EXPECT_TRUE(collectValues(engine, derived.outputTopics(*node)[0]).empty());  // no truncated 2-row prefix
}

// An in-place op swap is transactional. A failed candidate must not leave either
// its truncated prefix or an empty graph: the known-good op and output are replayed.
TEST(DerivedEngineFieldSelectTest, ReplaceSisoTransform_FailureRestoresPreviousOutput) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);
  PJ::TopicId tid = make_three_column_topic(engine, ds, 5);

  auto node = derived.addSisoTransform(tid, "scale", ds, std::make_unique<ScaleByTwoTransform>());
  ASSERT_TRUE(node.has_value()) << node.error();
  ASSERT_TRUE(derived.scheduleAll().has_value());
  const PJ::TopicId out_tid = derived.outputTopics(*node)[0];
  ASSERT_EQ(collectValues(engine, out_tid).size(), 5u);  // healthy output first

  EXPECT_FALSE(derived.replaceSisoTransform(*node, std::make_unique<FailAfterNTransform>(2)).has_value());
  EXPECT_EQ(collectValues(engine, out_tid), (std::vector<double>{0, 2, 4, 6, 8}));
}

// [a] Editing a filter that feeds another filter must refresh the whole downstream chain,
// not just the edited node. x -> A -> B; swap A's op and assert B reflects the new A output.
TEST(DerivedEngineFieldSelectTest, ReplaceSisoTransform_RecomputesDownstreamChain) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);
  PJ::TopicId x = make_three_column_topic(engine, ds, 5);  // col0 = {0,1,2,3,4}

  auto a = derived.addSisoTransform(x, "A", ds, std::make_unique<IdentityTransform>());
  ASSERT_TRUE(a.has_value()) << a.error();
  const PJ::TopicId a_out = derived.outputTopics(*a)[0];
  auto b = derived.addSisoTransform(a_out, "B", ds, std::make_unique<IdentityTransform>());
  ASSERT_TRUE(b.has_value()) << b.error();
  const PJ::TopicId b_out = derived.outputTopics(*b)[0];
  ASSERT_TRUE(derived.scheduleAll().has_value());

  EXPECT_EQ(collectValues(engine, a_out), (std::vector<double>{0, 1, 2, 3, 4}));
  EXPECT_EQ(collectValues(engine, b_out), (std::vector<double>{0, 1, 2, 3, 4}));

  // Swap A identity -> doubling. The downstream B must follow to {0,2,4,6,8}, not stay stale.
  ASSERT_TRUE(derived.replaceSisoTransform(*a, std::make_unique<ScaleByTwoTransform>()).has_value());
  EXPECT_EQ(collectValues(engine, a_out), (std::vector<double>{0, 2, 4, 6, 8}));
  EXPECT_EQ(collectValues(engine, b_out), (std::vector<double>{0, 2, 4, 6, 8}));
  EXPECT_EQ(collectValues(engine, b_out).size(), 5u);  // exact N — guards a future incremental-append regression
}

TEST(DerivedEngineFieldSelectTest, ReplaceSisoTransform_RejectsBadNode) {
  DataEngine engine;
  DerivedEngine derived(engine);
  EXPECT_FALSE(derived.replaceSisoTransform(9999u, std::make_unique<IdentityTransform>()).has_value());
}

// ---------------------------------------------------------------------------
// topological_order
// ---------------------------------------------------------------------------

TEST(DerivedEngineTest, TopologicalOrder_SingleNode) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId src = makeLinearTopic(engine, ds, 1.0, 5);
  PJ::NodeId n = *derived.addSisoTransform(src, "d1", ds, std::make_unique<DerivativeTransform>());
  auto order = derived.topologicalOrder();
  ASSERT_EQ(order.size(), 1u);
  EXPECT_EQ(order[0], n);
}

TEST(DerivedEngineTest, TopologicalOrder_Chain_ABOrder) {
  // A → B: output of A is input of B. Order must be [A, B].
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId src = makeLinearTopic(engine, ds, 1.0, 10);
  PJ::NodeId a = *derived.addSisoTransform(src, "d1", ds, std::make_unique<DerivativeTransform>());
  PJ::TopicId a_out = derived.outputTopics(a)[0];
  PJ::NodeId b = *derived.addSisoTransform(a_out, "d2", ds, std::make_unique<DerivativeTransform>());

  auto order = derived.topologicalOrder();
  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(order[0], a);
  EXPECT_EQ(order[1], b);
}

TEST(DerivedEngineTest, TopologicalOrder_Fork) {
  // A → B and A → C: A must appear before both B and C.
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId src = makeLinearTopic(engine, ds, 1.0, 10);
  PJ::NodeId a = *derived.addSisoTransform(src, "d1", ds, std::make_unique<DerivativeTransform>());
  PJ::TopicId a_out = derived.outputTopics(a)[0];
  PJ::NodeId b = *derived.addSisoTransform(a_out, "d2", ds, std::make_unique<DerivativeTransform>());
  PJ::NodeId c = *derived.addSisoTransform(a_out, "d3", ds, std::make_unique<DerivativeTransform>());

  auto order = derived.topologicalOrder();
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], a);
  // B and C must both appear after A
  EXPECT_NE(std::find(order.begin(), order.end(), b), order.end());
  EXPECT_NE(std::find(order.begin(), order.end(), c), order.end());
}

// ---------------------------------------------------------------------------
// on_source_committed / dirty propagation
// ---------------------------------------------------------------------------

TEST(DerivedEngineTest, DirtyPropagation_SourceChanged) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId src = makeLinearTopic(engine, ds, 1.0, 5);
  PJ::NodeId n = *derived.addSisoTransform(src, "d1", ds, std::make_unique<DerivativeTransform>());

  // Run schedule to clear dirty flag
  notify(derived, {src});
  ASSERT_TRUE(derived.scheduleAll().has_value());

  // Append more data and notify
  appendLinearRows(engine, src, 1.0, 5, 5);
  notify(derived, {src});

  // Node must be dirty again — schedule should produce more rows
  auto before = collectValues(engine, derived.outputTopics(n)[0]).size();
  ASSERT_TRUE(derived.scheduleAll().has_value());
  auto after = collectValues(engine, derived.outputTopics(n)[0]).size();
  EXPECT_GT(after, before);
}

TEST(DerivedEngineTest, DirtyPropagation_Chain) {
  // A → B: committing source dirtifies A; schedule runs A and dirtifies B;
  // subsequent schedule runs B.
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId src = makeLinearTopic(engine, ds, 1.0, 10);
  PJ::NodeId a = *derived.addSisoTransform(src, "d1", ds, std::make_unique<DerivativeTransform>());
  PJ::TopicId a_out = derived.outputTopics(a)[0];
  PJ::NodeId b = *derived.addSisoTransform(a_out, "d2", ds, std::make_unique<DerivativeTransform>());

  notify(derived, {src});
  ASSERT_TRUE(derived.scheduleAll().has_value());

  // Both A and B should have been processed
  EXPECT_FALSE(collectValues(engine, derived.outputTopics(a)[0]).empty());
  EXPECT_FALSE(collectValues(engine, derived.outputTopics(b)[0]).empty());
}

// ---------------------------------------------------------------------------
// schedule (incremental)
// ---------------------------------------------------------------------------

TEST(DerivedEngineTest, Schedule_ProducesCorrectDerivative) {
  // slope=2.0, step=1s, 11 rows → derivative is always 2.0 (10 rows output)
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId src = makeLinearTopic(engine, ds, 2.0, 11);
  PJ::NodeId node = *derived.addSisoTransform(src, "deriv", ds, std::make_unique<DerivativeTransform>());
  notify(derived, {src});
  ASSERT_TRUE(derived.scheduleAll().has_value());

  auto vals = collectValues(engine, derived.outputTopics(node)[0]);
  ASSERT_EQ(vals.size(), 10u);
  for (double v : vals) {
    EXPECT_NEAR(v, 2.0, 1e-6);
  }
}

TEST(DerivedEngineTest, Schedule_SecondCallNoNewChunks_NoOp) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId src = makeLinearTopic(engine, ds, 1.0, 5);
  PJ::NodeId node = *derived.addSisoTransform(src, "d1", ds, std::make_unique<DerivativeTransform>());
  notify(derived, {src});
  ASSERT_TRUE(derived.scheduleAll().has_value());

  auto count1 = collectValues(engine, derived.outputTopics(node)[0]).size();

  // No new data — second schedule should not change output count
  ASSERT_TRUE(derived.scheduleAll().has_value());
  auto count2 = collectValues(engine, derived.outputTopics(node)[0]).size();
  EXPECT_EQ(count1, count2);
}

TEST(DerivedEngineTest, Schedule_Lazy_SkipsInactiveNode) {
  // Two independent source nodes. schedule({a}) should not run b.
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId src1 = makeLinearTopic(engine, ds, 1.0, 5);
  PJ::TopicId src2 = makeLinearTopic(engine, ds, 2.0, 5);

  PJ::NodeId a = *derived.addSisoTransform(src1, "da", ds, std::make_unique<DerivativeTransform>());
  PJ::NodeId b = *derived.addSisoTransform(src2, "db", ds, std::make_unique<DerivativeTransform>());

  notify(derived, {src1, src2});
  // Only process node A
  ASSERT_TRUE(derived.scheduleActive({a}).has_value());

  auto a_vals = collectValues(engine, derived.outputTopics(a)[0]);
  auto b_vals = collectValues(engine, derived.outputTopics(b)[0]);

  EXPECT_FALSE(a_vals.empty());  // A was processed
  EXPECT_TRUE(b_vals.empty());   // B was skipped
}

TEST(DerivedEngineTest, Schedule_Chain_BothNodesRun) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId src = makeLinearTopic(engine, ds, 1.0, 12);
  PJ::NodeId a = *derived.addSisoTransform(src, "d1", ds, std::make_unique<DerivativeTransform>());
  PJ::TopicId a_out = derived.outputTopics(a)[0];
  PJ::NodeId b = *derived.addSisoTransform(a_out, "d2", ds, std::make_unique<DerivativeTransform>());

  notify(derived, {src});
  ASSERT_TRUE(derived.scheduleAll().has_value());

  // A: 11 derivative rows of linear → all constant
  auto a_vals = collectValues(engine, derived.outputTopics(a)[0]);
  EXPECT_EQ(a_vals.size(), 11u);

  // B: derivative of constant → all zero (10 rows, first suppressed)
  auto b_vals = collectValues(engine, derived.outputTopics(b)[0]);
  EXPECT_EQ(b_vals.size(), 10u);
  for (double v : b_vals) {
    EXPECT_NEAR(v, 0.0, 1e-6);
  }
}

// ---------------------------------------------------------------------------
// recompute_batch
// ---------------------------------------------------------------------------

TEST(DerivedEngineTest, RecomputeBatch_ClearsAndRegenerates) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId src = makeLinearTopic(engine, ds, 1.0, 6);
  PJ::NodeId node = *derived.addSisoTransform(src, "d1", ds, std::make_unique<DerivativeTransform>());
  notify(derived, {src});
  ASSERT_TRUE(derived.scheduleAll().has_value());

  auto before = collectValues(engine, derived.outputTopics(node)[0]);
  ASSERT_FALSE(before.empty());

  // recompute_batch replays from scratch and produces the same result
  ASSERT_TRUE(derived.recomputeBatch(node).has_value());

  auto after = collectValues(engine, derived.outputTopics(node)[0]);
  EXPECT_EQ(before.size(), after.size());
  for (std::size_t i = 0; i < before.size(); ++i) {
    EXPECT_NEAR(before[i], after[i], 1e-9);
  }
}

// A successful recompute REPLACES the output rather than appending to it: growing the input
// and recomputing must yield exactly the new row count, not old-plus-new.
TEST(DerivedEngineTest, RecomputeBatch_SuccessReplacesOutputRatherThanAppending) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId src = makeLinearTopic(engine, ds, 1.0, 5);
  PJ::NodeId node = *derived.addSisoTransform(src, "id", ds, std::make_unique<IdentityTransform>());
  ASSERT_TRUE(derived.scheduleAll().has_value());
  const PJ::TopicId out_tid = derived.outputTopics(node)[0];
  ASSERT_EQ(collectValues(engine, out_tid).size(), 5u);

  appendLinearRows(engine, src, 1.0, 2, /*start_i=*/5);
  ASSERT_TRUE(derived.recomputeBatch(node).has_value());

  // 7 values (0..6), never 12 (5 stale + 7 fresh) — the detached prior output must be
  // discarded on success, not merged back in alongside the freshly-replayed rows.
  EXPECT_EQ(collectValues(engine, out_tid), (std::vector<double>{0, 1, 2, 3, 4, 5, 6}));
}

// The rollback-fidelity bug this guards: a sticky-failing processor (one that keeps failing
// regardless of input, e.g. a broken Luau filter) must not empty out output that was correct
// going into the failed recompute. recomputeBatch's contract is that a failed replay leaves a
// node's output exactly as it was before the call.
TEST(DerivedEngineTest, RecomputeBatch_StickyFailureRestoresPriorOutputValues) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId src = makeLinearTopic(engine, ds, 1.0, 5);
  PJ::NodeId node = *derived.addSisoTransform(src, "sticky", ds, std::make_unique<FailAfterNTransform>(5));
  ASSERT_TRUE(derived.scheduleAll().has_value());  // exactly 5 rows: the op is not yet sticky-failed

  const PJ::TopicId out_tid = derived.outputTopics(node)[0];
  const std::vector<double> before = collectValues(engine, out_tid);
  ASSERT_EQ(before, (std::vector<double>{0, 1, 2, 3, 4}));

  // Grow the input past the op's success threshold, then recompute directly: reset() rearms
  // the sticky op, the full replay reproduces rows 0..4 and fails on row 5.
  appendLinearRows(engine, src, 1.0, 2, /*start_i=*/5);
  EXPECT_FALSE(derived.recomputeBatch(node).has_value());

  // The failed replay must not commit a truncated prefix (pre-existing contract) AND must not
  // have emptied the prior output either (the bug under test): the exact pre-call values survive.
  EXPECT_EQ(collectValues(engine, out_tid), before);
}

// Identity pass-through that THROWS once it has emitted `throw_after` rows — models a
// transform (a script binding, an allocation) that raises instead of reporting the failure
// through failed(). disarm() makes it a plain identity again.
class ThrowAfterNTransform : public ISISOTransform {
 public:
  explicit ThrowAfterNTransform(int throw_after) : throw_after_(throw_after) {}

  void reset() override {
    seen_ = 0;
  }

  void disarm() {
    throw_after_ = -1;
  }

  bool calculate(PJ::Timestamp time, const VarValue& input, PJ::Timestamp& out_time, VarValue& out_value) override {
    if (throw_after_ >= 0 && seen_ >= throw_after_) {
      throw std::runtime_error("ThrowAfterNTransform: boom");
    }
    ++seen_;
    out_time = time;
    out_value = std::get<double>(input);
    return true;
  }

 private:
  int throw_after_;
  int seen_ = 0;
};

// The failure guarantee must hold for an EXCEPTION out of calculate(), not just for an error
// Status: the detached output is owned by a guard whose destructor runs while unwinding.
// Without it the prior chunks die with the stack frame and the topic stays empty forever.
TEST(DerivedEngineTest, RecomputeBatch_ThrowingTransformRestoresPriorOutputAndReplaysOnce) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId src = makeLinearTopic(engine, ds, 1.0, 5);
  auto owned_op = std::make_unique<ThrowAfterNTransform>(5);
  ThrowAfterNTransform* op = owned_op.get();
  PJ::NodeId node = *derived.addSisoTransform(src, "throwing", ds, std::move(owned_op));
  ASSERT_TRUE(derived.scheduleAll().has_value());  // 5 rows: the 6th call is the one that throws

  const PJ::TopicId out_tid = derived.outputTopics(node)[0];
  const std::vector<double> before = collectValues(engine, out_tid);
  ASSERT_EQ(before, (std::vector<double>{0, 1, 2, 3, 4}));

  // Grow the input past the throw threshold and replay: row 5 throws mid-replay.
  appendLinearRows(engine, src, 1.0, 2, /*start_i=*/5);
  EXPECT_THROW(
      {
        auto ignored = derived.recomputeBatch(node);
        (void)ignored;
      },
      std::runtime_error);
  EXPECT_EQ(collectValues(engine, out_tid), before);

  // The throw left the node's watermarks rewound while its output was rolled back, so the
  // next pass must REPLACE the output, not append a second copy of the replayed history.
  op->disarm();
  notify(derived, {src});
  ASSERT_TRUE(derived.scheduleAll().has_value());
  EXPECT_EQ(collectValues(engine, out_tid), (std::vector<double>{0, 1, 2, 3, 4, 5, 6}));
}

TEST(DerivedEngineTest, RecomputeBatch_MultipleRootsRecomputesDiamondOnceInTopologicalOrder) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId dataset = makeDataset(engine);
  PJ::TopicId source = makeLinearTopic(engine, dataset, 1.0, 4);
  std::vector<std::string> reset_trace;

  PJ::NodeId node_a = *derived.addSisoTransform(
      source, "A", dataset, std::make_unique<ResetTracingIdentityTransform>("A", reset_trace));
  PJ::TopicId output_a = derived.outputTopics(node_a)[0];
  PJ::NodeId node_b = *derived.addSisoTransform(
      output_a, "B", dataset, std::make_unique<ResetTracingIdentityTransform>("B", reset_trace));
  PJ::NodeId node_c = *derived.addSisoTransform(
      output_a, "C", dataset, std::make_unique<ResetTracingIdentityTransform>("C", reset_trace));

  class ResetTracingSumTransform : public IMIMOTransform {
   public:
    explicit ResetTracingSumTransform(std::vector<std::string>& reset_trace) : reset_trace_(reset_trace) {}

    void reset() override {
      reset_trace_.push_back("D");
    }

    std::vector<StorageKind> outputKinds(PJ::Span<const StorageKind> /*input_kinds*/) const override {
      return {StorageKind::kFloat64};
    }

    bool calculate(
        PJ::Timestamp time, PJ::Span<const VarValue> inputs, PJ::Timestamp& out_time,
        std::vector<VarValue>& output) override {
      out_time = time;
      output[0] = std::get<double>(inputs[0]) + std::get<double>(inputs[1]);
      return true;
    }

   private:
    std::vector<std::string>& reset_trace_;
  };

  PJ::NodeId node_d = *derived.addMimoTransform(
      {derived.outputTopics(node_b)[0], derived.outputTopics(node_c)[0]}, {"D"}, dataset,
      std::make_unique<ResetTracingSumTransform>(reset_trace));
  ASSERT_TRUE(derived.scheduleAll().has_value());

  reset_trace.clear();
  const std::vector<PJ::NodeId> roots = {node_a, node_c};
  ASSERT_TRUE(derived.recomputeBatch(roots).has_value());
  EXPECT_EQ(reset_trace, (std::vector<std::string>{"A", "B", "C", "D"}));
  EXPECT_EQ(collectValues(engine, derived.outputTopics(node_d)[0]), (std::vector<double>{0, 2, 4, 6}));
}

TEST(DerivedEngineTest, SameNamedTopicsInDifferentDatasetsStayDependencyIsolated) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId first_dataset = makeDataset(engine, "first");
  PJ::DatasetId second_dataset = makeDataset(engine, "second");
  PJ::TopicId first_source = makeLinearTopic(engine, first_dataset, 1.0, 5);
  PJ::TopicId second_source = makeLinearTopic(engine, second_dataset, 1.0, 5);

  PJ::NodeId first_root =
      *derived.addSisoTransform(first_source, "same_root", first_dataset, std::make_unique<IdentityTransform>());
  PJ::NodeId second_root =
      *derived.addSisoTransform(second_source, "same_root", second_dataset, std::make_unique<IdentityTransform>());
  PJ::NodeId first_child = *derived.addSisoTransform(
      derived.outputTopics(first_root)[0], "same_child", first_dataset, std::make_unique<IdentityTransform>());
  PJ::NodeId second_child = *derived.addSisoTransform(
      derived.outputTopics(second_root)[0], "same_child", second_dataset, std::make_unique<IdentityTransform>());
  ASSERT_TRUE(derived.scheduleAll().has_value());

  const std::size_t first_count_before = collectValues(engine, derived.outputTopics(first_child)[0]).size();
  const std::size_t second_count_before = collectValues(engine, derived.outputTopics(second_child)[0]).size();
  appendLinearRows(engine, first_source, 1.0, 1, 5);
  notify(derived, {first_source});
  ASSERT_TRUE(derived.scheduleAll().has_value());

  EXPECT_EQ(collectValues(engine, derived.outputTopics(first_child)[0]).size(), first_count_before + 1);
  EXPECT_EQ(collectValues(engine, derived.outputTopics(second_child)[0]).size(), second_count_before);
}

// ---------------------------------------------------------------------------
// Parity: incremental == batch
// ---------------------------------------------------------------------------

TEST(DerivedEngineTest, Parity_SingleChunk) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId src = makeLinearTopic(engine, ds, 3.0, 11);
  PJ::NodeId node = *derived.addSisoTransform(src, "d1", ds, std::make_unique<DerivativeTransform>());
  notify(derived, {src});
  ASSERT_TRUE(derived.scheduleAll().has_value());
  auto incremental = collectValues(engine, derived.outputTopics(node)[0]);

  ASSERT_TRUE(derived.recomputeBatch(node).has_value());
  auto batch = collectValues(engine, derived.outputTopics(node)[0]);

  ASSERT_EQ(incremental.size(), batch.size());
  for (std::size_t i = 0; i < batch.size(); ++i) {
    EXPECT_NEAR(incremental[i], batch[i], 1e-9) << "mismatch at row " << i;
  }
}

TEST(DerivedEngineTest, Parity_TwoChunks_CrossBoundary) {
  // The cross-chunk boundary row is the key correctness test: state must carry
  // over naturally in the incremental path.
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  // Chunk 1: 20 rows (forces auto-chunk at 1024 capacity — but step_ns large enough
  // that all rows stay in one chunk unless we push more)
  PJ::TopicId src = makeLinearTopic(engine, ds, 2.0, 20);
  PJ::NodeId node = *derived.addSisoTransform(src, "d1", ds, std::make_unique<DerivativeTransform>());
  notify(derived, {src});
  ASSERT_TRUE(derived.scheduleAll().has_value());

  // Chunk 2: append 20 more rows
  appendLinearRows(engine, src, 2.0, 20, 20);
  notify(derived, {src});
  ASSERT_TRUE(derived.scheduleAll().has_value());

  auto incremental = collectValues(engine, derived.outputTopics(node)[0]);

  // Batch recompute and compare
  ASSERT_TRUE(derived.recomputeBatch(node).has_value());
  auto batch = collectValues(engine, derived.outputTopics(node)[0]);

  ASSERT_EQ(incremental.size(), batch.size());
  for (std::size_t i = 0; i < batch.size(); ++i) {
    EXPECT_NEAR(incremental[i], batch[i], 1e-9) << "mismatch at row " << i;
  }
}

TEST(DerivedEngineTest, Parity_ThreeChunks) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId src = makeLinearTopic(engine, ds, 5.0, 10);
  PJ::NodeId node = *derived.addSisoTransform(src, "d1", ds, std::make_unique<DerivativeTransform>());
  notify(derived, {src});
  ASSERT_TRUE(derived.scheduleAll().has_value());

  appendLinearRows(engine, src, 5.0, 10, 10);
  notify(derived, {src});
  ASSERT_TRUE(derived.scheduleAll().has_value());

  appendLinearRows(engine, src, 5.0, 10, 20);
  notify(derived, {src});
  ASSERT_TRUE(derived.scheduleAll().has_value());

  auto incremental = collectValues(engine, derived.outputTopics(node)[0]);

  ASSERT_TRUE(derived.recomputeBatch(node).has_value());
  auto batch = collectValues(engine, derived.outputTopics(node)[0]);

  ASSERT_EQ(incremental.size(), batch.size());
  for (std::size_t i = 0; i < batch.size(); ++i) {
    EXPECT_NEAR(incremental[i], batch[i], 1e-9) << "mismatch at row " << i;
  }
}

// commit_chunks returns deduplicated changed topic IDs usable directly with
// on_source_committed — the streaming frame-loop pattern.
TEST(DerivedEngineTest, CommitCycle_ReturnValueDrivesNotify) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  DataWriter writer = engine.createWriter();
  auto handle = *writer.registerScalarSeries(ds, "sig", PJ::NumericType::kFloat64);
  PJ::TopicId src = handle.topic_id;

  PJ::NodeId node = *derived.addSisoTransform(src, "d_sig", ds, std::make_unique<DerivativeTransform>());
  PJ::TopicId out = derived.outputTopics(node)[0];

  // Frame 1: write 5 samples and use the one-liner pattern.
  for (int i = 0; i < 5; ++i) {
    writer.appendScalar(handle, static_cast<PJ::Timestamp>(i) * 1'000'000'000LL, static_cast<double>(i));
  }
  derived.onSourceCommitted(engine.commitChunks(writer.flushAll()));
  ASSERT_TRUE(derived.scheduleAll().has_value());
  auto after_frame1 = collectValues(engine, out).size();
  EXPECT_GT(after_frame1, 0u);

  // Frame 2: write 5 more samples.
  for (int i = 5; i < 10; ++i) {
    writer.appendScalar(handle, static_cast<PJ::Timestamp>(i) * 1'000'000'000LL, static_cast<double>(i));
  }
  derived.onSourceCommitted(engine.commitChunks(writer.flushAll()));
  ASSERT_TRUE(derived.scheduleAll().has_value());
  auto after_frame2 = collectValues(engine, out).size();
  EXPECT_GT(after_frame2, after_frame1);

  // Verify return value: single topic flushed → exactly one ID returned.
  for (int i = 10; i < 13; ++i) {
    writer.appendScalar(handle, static_cast<PJ::Timestamp>(i) * 1'000'000'000LL, static_cast<double>(i));
  }
  auto changed = engine.commitChunks(writer.flushAll());
  ASSERT_EQ(changed.size(), 1u);
  EXPECT_EQ(changed[0], src);
}

// Regression: add_siso_transform must work on a series created via
// register_scalar_series even when no chunk has been committed yet
// (fewer rows than max_chunk_rows, or no flush/commit called at all).
TEST(DerivedEngineTest, AddTransform_NoCommittedChunks_Succeeds) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  // Create a topic with a few rows but do NOT commit any chunks.
  DataWriter writer = engine.createWriter();
  auto handle = *writer.registerScalarSeries(ds, "tiny_series", PJ::NumericType::kFloat64);
  PJ::TopicId src = handle.topic_id;

  // Write 3 rows (well below default max_chunk_rows=1024) — no commit.
  for (int i = 0; i < 3; ++i) {
    writer.appendScalar(handle, static_cast<PJ::Timestamp>(i) * 1'000'000'000LL, static_cast<double>(i));
  }
  // Deliberately skip flush / commit_chunks.

  // add_siso_transform must succeed without any committed chunks in storage.
  auto result = derived.addSisoTransform(src, "d_tiny", ds, std::make_unique<DerivativeTransform>());
  EXPECT_TRUE(result.has_value()) << "Expected success, got: " << (result.has_value() ? "" : result.error());
}

// ---------------------------------------------------------------------------
// MIMO transform helpers
// ---------------------------------------------------------------------------

// SumMimoTransform: N inputs → 1 output (sum of all inputs as double).
class SumMimoTransform : public IMIMOTransform {
 public:
  std::vector<StorageKind> outputKinds(PJ::Span<const StorageKind> /*input_kinds*/) const override {
    return {StorageKind::kFloat64};
  }

  bool calculate(
      PJ::Timestamp time, PJ::Span<const VarValue> inputs, PJ::Timestamp& out_time,
      std::vector<VarValue>& output) override {
    out_time = time;
    double sum = 0.0;
    for (const auto& v : inputs) {
      sum += std::get<double>(v);
    }
    output[0] = sum;
    return true;
  }
};

// DiffMimoTransform: 2 inputs → 1 output (inputs[0] - inputs[1]).
class DiffMimoTransform : public IMIMOTransform {
 public:
  std::vector<StorageKind> outputKinds(PJ::Span<const StorageKind> /*input_kinds*/) const override {
    return {StorageKind::kFloat64};
  }

  bool calculate(
      PJ::Timestamp time, PJ::Span<const VarValue> inputs, PJ::Timestamp& out_time,
      std::vector<VarValue>& output) override {
    out_time = time;
    output[0] = std::get<double>(inputs[0]) - std::get<double>(inputs[1]);
    return true;
  }
};

// A MIMO sum whose failure latch SURVIVES reset(), like a filter whose script stopped
// compiling: every run reports the failure before consuming any input. heal() clears it.
class LatchedFailureSumMimoTransform : public IMIMOTransform {
 public:
  std::vector<StorageKind> outputKinds(PJ::Span<const StorageKind> /*input_kinds*/) const override {
    return {StorageKind::kFloat64};
  }

  bool calculate(
      PJ::Timestamp time, PJ::Span<const VarValue> inputs, PJ::Timestamp& out_time,
      std::vector<VarValue>& output) override {
    out_time = time;
    double sum = 0.0;
    for (const auto& value : inputs) {
      sum += std::get<double>(value);
    }
    output[0] = sum;
    return !failed_;
  }

  [[nodiscard]] bool failed() const override {
    return failed_;
  }

  [[nodiscard]] const std::string& error() const override {
    static const std::string kErr = "LatchedFailureSumMimoTransform: broken";
    return kErr;
  }

  void fail() {
    failed_ = true;
  }

  void heal() {
    failed_ = false;
  }

 private:
  bool failed_ = false;
};

// A recompute rewinds the node's watermarks before replaying, so a replay that fails and
// rolls the output back leaves watermarks that no longer describe the retained rows. The
// incremental path APPENDS, so an ordinary scheduling pass afterwards would replay the whole
// history on top of the rolled-back rows and double them. Such a node must stay on the full
// (output-replacing) recompute path until a replay finally succeeds.
TEST(DerivedEngineTest, FailedRecomputeRollback_LaterSchedulePassReplacesOutputInsteadOfAppending) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId src = make_three_column_topic(engine, ds, 5);
  auto owned_op = std::make_unique<LatchedFailureSumMimoTransform>();
  LatchedFailureSumMimoTransform* op = owned_op.get();
  PJ::NodeId node = *derived.addMimoTransform({src, src}, {"sum"}, ds, std::move(owned_op), {0, 1});
  ASSERT_TRUE(derived.scheduleAll().has_value());

  const PJ::TopicId out_tid = derived.outputTopics(node)[0];
  const std::vector<double> before = collectValues(engine, out_tid);
  ASSERT_EQ(before, (std::vector<double>{100, 102, 104, 106, 108}));

  // The op breaks (its latch survives reset()) and a recompute is attempted: it fails before
  // consuming any input, so the rollback restores the good rows over rewound watermarks.
  op->fail();
  EXPECT_FALSE(derived.recomputeBatch(node).has_value());
  EXPECT_EQ(collectValues(engine, out_tid), before);

  // The op recovers and an ordinary scheduling pass runs it: five rows, never ten.
  op->heal();
  notify(derived, {src});
  ASSERT_TRUE(derived.scheduleAll().has_value());
  EXPECT_EQ(collectValues(engine, out_tid), before);
}

TEST(MimoTransformTest, InputBindingState_MimoResolveAndRestore) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId dataset = makeDataset(engine);
  PJ::TopicId input_topic = make_three_column_topic(engine, dataset, 3);
  auto node = derived.addMimoTransform(
      {input_topic, input_topic}, {"sum"}, dataset, std::make_unique<SumMimoTransform>(), {0, 1});
  ASSERT_TRUE(node.has_value()) << node.error();

  auto original = derived.inputBindingState(*node);
  ASSERT_TRUE(original.has_value()) << original.error();
  EXPECT_EQ(original->columns, (std::vector<std::size_t>{0, 1}));
  EXPECT_EQ(original->kinds, (std::vector<StorageKind>{StorageKind::kFloat64, StorageKind::kFloat64}));

  auto resolved = derived.resolvedInputBindingState(*node, {1, 2});
  ASSERT_TRUE(resolved.has_value()) << resolved.error();
  ASSERT_TRUE(derived.restoreInputBindingState(*node, *resolved).has_value());
  ASSERT_TRUE(derived.scheduleAll().has_value());
  EXPECT_EQ(collectValues(engine, derived.outputTopics(*node)[0]), (std::vector<double>{300, 302, 304}));
}

// Collect (timestamp, value) pairs for a given column index.
static std::vector<std::pair<PJ::Timestamp, double>> collectRowsCol(
    DataEngine& engine, PJ::TopicId topic_id, std::size_t col = 0) {
  const TopicStorage* storage = engine.getTopicStorage(topic_id);
  if (!storage) {
    return {};
  }
  std::vector<std::pair<PJ::Timestamp, double>> out;
  auto cursor = rangeQuery(storage->sealedChunks(), 0, std::numeric_limits<PJ::Timestamp>::max());
  cursor.forEach([&](const SampleRow& row) {
    out.emplace_back(row.timestamp, row.chunk->readNumericAsDouble(col, row.row_index));
  });
  return out;
}

// ---------------------------------------------------------------------------
// MIMO — add_mimo_transform: basic registration
// ---------------------------------------------------------------------------

TEST(MimoTransformTest, AddMimo_CreatesOutputTopic) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId t1 = makeLinearTopic(engine, ds, 1.0, 5);
  PJ::TopicId t2 = makeLinearTopic(engine, ds, 2.0, 5);

  auto node_or = derived.addMimoTransform({t1, t2}, {"sum_out"}, ds, std::make_unique<SumMimoTransform>());
  ASSERT_TRUE(node_or.has_value()) << node_or.error();

  PJ::NodeId node = *node_or;
  EXPECT_TRUE(derived.hasNode(node));
  auto outs = derived.outputTopics(node);
  ASSERT_EQ(outs.size(), 1u);
  EXPECT_NE(engine.getTopicStorage(outs[0]), nullptr);
}

TEST(MimoTransformTest, AddMimo_UnknownInputTopic_Fails) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  auto r = derived.addMimoTransform({9999u}, {"out"}, ds, std::make_unique<SumMimoTransform>());
  EXPECT_FALSE(r.has_value());
}

TEST(MimoTransformTest, AddMimo_DuplicateOutputName_Fails) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId t1 = makeLinearTopic(engine, ds, 1.0, 5);
  PJ::TopicId t2 = makeLinearTopic(engine, ds, 2.0, 5);

  ASSERT_TRUE(derived.addMimoTransform({t1, t2}, {"dup"}, ds, std::make_unique<SumMimoTransform>()).has_value());
  // Same output name in same dataset must fail.
  auto r = derived.addMimoTransform({t1, t2}, {"dup"}, ds, std::make_unique<SumMimoTransform>());
  EXPECT_FALSE(r.has_value());
}

TEST(MimoTransformTest, AddMimo_MultipleOutputTopics) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId t1 = makeLinearTopic(engine, ds, 1.0, 5);
  PJ::TopicId t2 = makeLinearTopic(engine, ds, 2.0, 5);

  // DiffMimoTransform produces 1 output; use two separate nodes for two outputs.
  auto node_or = derived.addMimoTransform({t1, t2}, {"diff_out"}, ds, std::make_unique<DiffMimoTransform>());
  ASSERT_TRUE(node_or.has_value()) << node_or.error();
  ASSERT_EQ(derived.outputTopics(*node_or).size(), 1u);
}

// ---------------------------------------------------------------------------
// MIMO — join semantics (inner join on exact timestamps)
// ---------------------------------------------------------------------------

TEST(MimoTransformTest, JoinSemantics_OnlyMatchingTimestamps) {
  // Topic A: t=0,1,2,3,4 s (all 5 timestamps)
  // Topic B: t=0,2,4 s     (3 timestamps, a subset)
  // Sum at t=0: 0+0=0, t=2: 2+4=6, t=4: 4+8=12
  // t=1 and t=3 are in A but not B → no output row.
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  // Topic A: t = 0,1,2,3,4 s, value = i seconds
  PJ::TopicId ta;
  {
    DataWriter w = engine.createWriter();
    auto h = *w.registerScalarSeries(ds, "a", PJ::NumericType::kFloat64);
    ta = h.topic_id;
    for (int i = 0; i < 5; ++i) {
      w.appendScalar(h, static_cast<PJ::Timestamp>(i) * 1'000'000'000LL, static_cast<double>(i));
    }
    engine.commitChunks(w.flushAll());
  }

  // Topic B: t = 0,2,4 s, value = 0,4,8
  PJ::TopicId tb;
  {
    DataWriter w = engine.createWriter();
    auto h = *w.registerScalarSeries(ds, "b", PJ::NumericType::kFloat64);
    tb = h.topic_id;
    for (int i = 0; i < 3; ++i) {
      w.appendScalar(h, static_cast<PJ::Timestamp>(i * 2) * 1'000'000'000LL, static_cast<double>(i * 2 * 2));
    }
    engine.commitChunks(w.flushAll());
  }

  auto node_or = derived.addMimoTransform({ta, tb}, {"sum"}, ds, std::make_unique<SumMimoTransform>());
  ASSERT_TRUE(node_or.has_value()) << node_or.error();

  notify(derived, {ta, tb});
  ASSERT_TRUE(derived.scheduleAll().has_value());

  auto rows = collectRowsCol(engine, derived.outputTopics(*node_or)[0]);
  ASSERT_EQ(rows.size(), 3u);               // only t=0,2,4 produce output
  EXPECT_NEAR(rows[0].second, 0.0, 1e-9);   // 0+0
  EXPECT_NEAR(rows[1].second, 6.0, 1e-9);   // 2+4
  EXPECT_NEAR(rows[2].second, 12.0, 1e-9);  // 4+8
}

TEST(MimoTransformTest, JoinSemantics_NoCommonTimestamps_NoOutput) {
  // A: t=0,1 s;  B: t=2,3 s → no overlap → no output rows
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId ta;
  {
    DataWriter w = engine.createWriter();
    auto h = *w.registerScalarSeries(ds, "a2", PJ::NumericType::kFloat64);
    ta = h.topic_id;
    for (int i = 0; i < 2; ++i) {
      w.appendScalar(h, static_cast<PJ::Timestamp>(i) * 1'000'000'000LL, static_cast<double>(i));
    }
    engine.commitChunks(w.flushAll());
  }
  PJ::TopicId tb;
  {
    DataWriter w = engine.createWriter();
    auto h = *w.registerScalarSeries(ds, "b2", PJ::NumericType::kFloat64);
    tb = h.topic_id;
    for (int i = 2; i < 4; ++i) {
      w.appendScalar(h, static_cast<PJ::Timestamp>(i) * 1'000'000'000LL, static_cast<double>(i));
    }
    engine.commitChunks(w.flushAll());
  }

  auto node_or = derived.addMimoTransform({ta, tb}, {"sum"}, ds, std::make_unique<SumMimoTransform>());
  ASSERT_TRUE(node_or.has_value()) << node_or.error();
  notify(derived, {ta, tb});
  ASSERT_TRUE(derived.scheduleAll().has_value());

  EXPECT_TRUE(collectValues(engine, derived.outputTopics(*node_or)[0]).empty());
}

// ---------------------------------------------------------------------------
// MIMO — schedule (incremental)
// ---------------------------------------------------------------------------

TEST(MimoTransformTest, Schedule_ProducesCorrectSum) {
  // Two topics with the same timestamps (0,1,2,...,9 seconds).
  // A[i] = 1.0 * i, B[i] = 2.0 * i. Sum[i] = 3.0 * i.
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId t1 = makeLinearTopic(engine, ds, 1.0, 10);
  PJ::TopicId t2 = makeLinearTopic(engine, ds, 2.0, 10);

  auto node_or = derived.addMimoTransform({t1, t2}, {"sum"}, ds, std::make_unique<SumMimoTransform>());
  ASSERT_TRUE(node_or.has_value()) << node_or.error();
  PJ::TopicId out = derived.outputTopics(*node_or)[0];

  notify(derived, {t1, t2});
  ASSERT_TRUE(derived.scheduleAll().has_value());

  auto rows = collectRowsCol(engine, out);
  ASSERT_EQ(rows.size(), 10u);
  for (int i = 0; i < 10; ++i) {
    double expected = 3.0 * static_cast<double>(i);  // (1.0 + 2.0) * i
    EXPECT_NEAR(rows[i].second, expected, 1e-9) << "row " << i;
  }
}

TEST(MimoTransformTest, Schedule_IncrementalTwoChunks) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId t1 = makeLinearTopic(engine, ds, 1.0, 10);
  PJ::TopicId t2 = makeLinearTopic(engine, ds, 2.0, 10);

  auto node_or = derived.addMimoTransform({t1, t2}, {"sum"}, ds, std::make_unique<SumMimoTransform>());
  ASSERT_TRUE(node_or.has_value()) << node_or.error();
  PJ::TopicId out = derived.outputTopics(*node_or)[0];

  notify(derived, {t1, t2});
  ASSERT_TRUE(derived.scheduleAll().has_value());
  std::size_t after_first = collectValues(engine, out).size();
  EXPECT_EQ(after_first, 10u);

  // Second batch of data
  appendLinearRows(engine, t1, 1.0, 10, 10);
  appendLinearRows(engine, t2, 2.0, 10, 10);
  notify(derived, {t1, t2});
  ASSERT_TRUE(derived.scheduleAll().has_value());

  std::size_t after_second = collectValues(engine, out).size();
  EXPECT_EQ(after_second, 20u);
}

// ---------------------------------------------------------------------------
// MIMO — recompute_batch + parity
// ---------------------------------------------------------------------------

TEST(MimoTransformTest, Parity_IncrementalMatchesBatch_SingleChunk) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId t1 = makeLinearTopic(engine, ds, 1.0, 10);
  PJ::TopicId t2 = makeLinearTopic(engine, ds, 3.0, 10);

  auto node_or = derived.addMimoTransform({t1, t2}, {"sum"}, ds, std::make_unique<SumMimoTransform>());
  ASSERT_TRUE(node_or.has_value()) << node_or.error();
  PJ::NodeId node = *node_or;
  PJ::TopicId out = derived.outputTopics(node)[0];

  notify(derived, {t1, t2});
  ASSERT_TRUE(derived.scheduleAll().has_value());
  auto incremental = collectValues(engine, out);

  ASSERT_TRUE(derived.recomputeBatch(node).has_value());
  auto batch = collectValues(engine, out);

  ASSERT_EQ(incremental.size(), batch.size());
  for (std::size_t i = 0; i < batch.size(); ++i) {
    EXPECT_NEAR(incremental[i], batch[i], 1e-9) << "mismatch at row " << i;
  }
}

TEST(MimoTransformTest, Parity_IncrementalMatchesBatch_MultipleChunks) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId t1 = makeLinearTopic(engine, ds, 1.0, 10);
  PJ::TopicId t2 = makeLinearTopic(engine, ds, 2.0, 10);

  auto node_or = derived.addMimoTransform({t1, t2}, {"sum"}, ds, std::make_unique<SumMimoTransform>());
  ASSERT_TRUE(node_or.has_value()) << node_or.error();
  PJ::NodeId node = *node_or;
  PJ::TopicId out = derived.outputTopics(node)[0];

  notify(derived, {t1, t2});
  ASSERT_TRUE(derived.scheduleAll().has_value());

  appendLinearRows(engine, t1, 1.0, 10, 10);
  appendLinearRows(engine, t2, 2.0, 10, 10);
  notify(derived, {t1, t2});
  ASSERT_TRUE(derived.scheduleAll().has_value());

  auto incremental = collectValues(engine, out);

  ASSERT_TRUE(derived.recomputeBatch(node).has_value());
  auto batch = collectValues(engine, out);

  ASSERT_EQ(incremental.size(), batch.size());
  for (std::size_t i = 0; i < batch.size(); ++i) {
    EXPECT_NEAR(incremental[i], batch[i], 1e-9) << "mismatch at row " << i;
  }
}

// ---------------------------------------------------------------------------
// MIMO — chained with SISO
// ---------------------------------------------------------------------------

TEST(MimoTransformTest, ChainedSisoThenMimo) {
  // Compute derivative of two source series (SISO), then sum the derivatives (MIMO).
  // src1: y = 2*t → dy/dt = 2.0
  // src2: y = 3*t → dy/dt = 3.0
  // MIMO sum = 5.0 for each output row.
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId src1 = makeLinearTopic(engine, ds, 2.0, 11);
  PJ::TopicId src2 = makeLinearTopic(engine, ds, 3.0, 11);

  PJ::NodeId n1 = *derived.addSisoTransform(src1, "d1", ds, std::make_unique<DerivativeTransform>());
  PJ::NodeId n2 = *derived.addSisoTransform(src2, "d2", ds, std::make_unique<DerivativeTransform>());
  PJ::TopicId d1_out = derived.outputTopics(n1)[0];
  PJ::TopicId d2_out = derived.outputTopics(n2)[0];

  auto node_or = derived.addMimoTransform({d1_out, d2_out}, {"sum_deriv"}, ds, std::make_unique<SumMimoTransform>());
  ASSERT_TRUE(node_or.has_value()) << node_or.error();
  PJ::TopicId sum_out = derived.outputTopics(*node_or)[0];

  notify(derived, {src1, src2});
  ASSERT_TRUE(derived.scheduleAll().has_value());

  auto vals = collectValues(engine, sum_out);
  EXPECT_FALSE(vals.empty());
  for (double v : vals) {
    EXPECT_NEAR(v, 5.0, 1e-6);
  }
}

// ---------------------------------------------------------------------------
// MIMO — duplicate timestamp bug (C1)
// ---------------------------------------------------------------------------

// When topic A has two rows at t=5 (equal timestamps are permitted by the
// engine), the N-way join must produce exactly one output row at t=5 using
// consistent (last-write-wins) values — NOT two output rows at t=5 (duplicate).
TEST(MimoTransformTest, DuplicateTimestamp_ProducesOneOutputRow) {
  // Topic A: t=0,5,5,10 ns — t=5 appears twice (values 1.0 and 2.0)
  // Topic B: t=0,5,10 ns   — normal, no duplicates
  // Expected join at t=0,5,10 → 3 output rows.
  // Bug: without dedup, joined_ts=[0,5,5,10] → 4 output rows, t=5 appears twice.
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId ta;
  {
    DataWriter w = engine.createWriter();
    auto h = *w.registerScalarSeries(ds, "a_dup", PJ::NumericType::kFloat64);
    ta = h.topic_id;
    w.appendScalar(h, 0LL, 0.0);
    w.appendScalar(h, 5LL, 1.0);  // first row at t=5
    w.appendScalar(h, 5LL, 2.0);  // second row at t=5 (equal timestamp allowed)
    w.appendScalar(h, 10LL, 3.0);
    engine.commitChunks(w.flushAll());
  }

  PJ::TopicId tb;
  {
    DataWriter w = engine.createWriter();
    auto h = *w.registerScalarSeries(ds, "b_dup", PJ::NumericType::kFloat64);
    tb = h.topic_id;
    w.appendScalar(h, 0LL, 10.0);
    w.appendScalar(h, 5LL, 20.0);
    w.appendScalar(h, 10LL, 30.0);
    engine.commitChunks(w.flushAll());
  }

  auto node_or = derived.addMimoTransform({ta, tb}, {"sum_dup"}, ds, std::make_unique<SumMimoTransform>());
  ASSERT_TRUE(node_or.has_value()) << node_or.error();

  notify(derived, {ta, tb});
  ASSERT_TRUE(derived.scheduleAll().has_value());

  auto rows = collectRowsCol(engine, derived.outputTopics(*node_or)[0]);
  // Must produce exactly 3 rows (t=0, t=5, t=10), not 4.
  ASSERT_EQ(rows.size(), 3u) << "Duplicate timestamp in input caused duplicate output rows";
  EXPECT_NEAR(rows[0].second, 10.0, 1e-9);  // 0+10
  // t=5: last-write-wins for A → 2.0; B=20.0 → sum=22.0
  EXPECT_NEAR(rows[1].second, 22.0, 1e-9);
  EXPECT_NEAR(rows[2].second, 33.0, 1e-9);  // 3+30
}

// ---------------------------------------------------------------------------
// MIMO — staggered cross-chunk parity (I1)
// ---------------------------------------------------------------------------

// Parity with staggered inputs: A advances ahead of B across chunk boundaries.
// This tests the watermark's correctness when topic rates differ.
TEST(MimoTransformTest, Parity_StaggeredChunks_IncrementalMatchesBatch) {
  // Chunk 1: A commits t=0..9 (10 rows), B commits t=0,2,4,6,8 (5 rows, even only)
  // Chunk 2: A commits t=10..14 (5 rows), B commits t=10,12,14 (3 rows, even only)
  // Expected joins: t=0,2,4,6,8,10,12,14 (8 rows)
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  // Register both topics upfront
  DataWriter wa = engine.createWriter();
  auto ha = *wa.registerScalarSeries(ds, "stag_a", PJ::NumericType::kFloat64);
  PJ::TopicId ta = ha.topic_id;

  DataWriter wb = engine.createWriter();
  auto hb = *wb.registerScalarSeries(ds, "stag_b", PJ::NumericType::kFloat64);
  PJ::TopicId tb = hb.topic_id;

  auto node_or = derived.addMimoTransform({ta, tb}, {"stag_sum"}, ds, std::make_unique<SumMimoTransform>());
  ASSERT_TRUE(node_or.has_value()) << node_or.error();
  PJ::NodeId node = *node_or;
  PJ::TopicId out = derived.outputTopics(node)[0];

  // Chunk 1
  for (int i = 0; i < 10; ++i) {
    wa.appendScalar(ha, static_cast<PJ::Timestamp>(i) * 1'000'000'000LL, static_cast<double>(i));
  }
  for (int i = 0; i < 5; ++i) {
    wb.appendScalar(hb, static_cast<PJ::Timestamp>(i * 2) * 1'000'000'000LL, static_cast<double>(i * 2));
  }
  derived.onSourceCommitted(engine.commitChunks(wa.flushAll()));
  derived.onSourceCommitted(engine.commitChunks(wb.flushAll()));
  ASSERT_TRUE(derived.scheduleAll().has_value());

  // Chunk 2
  for (int i = 10; i < 15; ++i) {
    wa.appendScalar(ha, static_cast<PJ::Timestamp>(i) * 1'000'000'000LL, static_cast<double>(i));
  }
  for (int i = 5; i < 8; ++i) {
    wb.appendScalar(hb, static_cast<PJ::Timestamp>(i * 2) * 1'000'000'000LL, static_cast<double>(i * 2));
  }
  derived.onSourceCommitted(engine.commitChunks(wa.flushAll()));
  derived.onSourceCommitted(engine.commitChunks(wb.flushAll()));
  ASSERT_TRUE(derived.scheduleAll().has_value());

  auto incremental = collectRowsCol(engine, out);
  ASSERT_EQ(incremental.size(), 8u) << "Expected 8 joined rows (even timestamps 0..14)";

  ASSERT_TRUE(derived.recomputeBatch(node).has_value());
  auto batch = collectRowsCol(engine, out);

  ASSERT_EQ(incremental.size(), batch.size());
  for (std::size_t i = 0; i < batch.size(); ++i) {
    EXPECT_EQ(incremental[i].first, batch[i].first) << "timestamp mismatch at row " << i;
    EXPECT_NEAR(incremental[i].second, batch[i].second, 1e-9) << "value mismatch at row " << i;
  }
}

// ---------------------------------------------------------------------------
// MIMO — stateful transform reset (I2)
// ---------------------------------------------------------------------------

// A stateful MIMO transform that accumulates a running sum across calls.
// recompute_batch must call reset() and produce the same result as incremental.
class AccumulatingSumMimoTransform : public IMIMOTransform {
 public:
  std::vector<StorageKind> outputKinds(PJ::Span<const StorageKind> /*input_kinds*/) const override {
    return {StorageKind::kFloat64};
  }

  void reset() override {
    running_sum_ = 0.0;
  }

  bool calculate(
      PJ::Timestamp time, PJ::Span<const VarValue> inputs, PJ::Timestamp& out_time,
      std::vector<VarValue>& output) override {
    out_time = time;
    running_sum_ += std::get<double>(inputs[0]) + std::get<double>(inputs[1]);
    output[0] = running_sum_;
    return true;
  }

 private:
  double running_sum_ = 0.0;
};

TEST(MimoTransformTest, StatefulTransform_RecomputeBatchCallsReset) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId t1 = makeLinearTopic(engine, ds, 1.0, 5);
  PJ::TopicId t2 = makeLinearTopic(engine, ds, 1.0, 5);

  auto node_or = derived.addMimoTransform({t1, t2}, {"acc"}, ds, std::make_unique<AccumulatingSumMimoTransform>());
  ASSERT_TRUE(node_or.has_value()) << node_or.error();
  PJ::NodeId node = *node_or;
  PJ::TopicId out = derived.outputTopics(node)[0];

  notify(derived, {t1, t2});
  ASSERT_TRUE(derived.scheduleAll().has_value());
  auto incremental = collectValues(engine, out);

  // recompute_batch must reset the transform and produce identical output
  ASSERT_TRUE(derived.recomputeBatch(node).has_value());
  auto batch = collectValues(engine, out);

  ASSERT_EQ(incremental.size(), batch.size());
  for (std::size_t i = 0; i < batch.size(); ++i) {
    EXPECT_NEAR(incremental[i], batch[i], 1e-9) << "mismatch at row " << i;
  }
}

// ---------------------------------------------------------------------------
// MIMO — on_source_committed with partial inputs (I4)
// ---------------------------------------------------------------------------

// Notifying about only one of two MIMO inputs should cause the node to run
// but find no new data from the other input and produce no output.
// The watermark must NOT advance, so future data from the other input can match.
TEST(MimoTransformTest, PartialNotify_DoesNotAdvanceWatermark) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  // Register both topics; initially only commit data for t1
  DataWriter w1 = engine.createWriter();
  auto h1 = *w1.registerScalarSeries(ds, "p1", PJ::NumericType::kFloat64);
  PJ::TopicId t1 = h1.topic_id;

  DataWriter w2 = engine.createWriter();
  auto h2 = *w2.registerScalarSeries(ds, "p2", PJ::NumericType::kFloat64);
  PJ::TopicId t2 = h2.topic_id;

  auto node_or = derived.addMimoTransform({t1, t2}, {"partial_sum"}, ds, std::make_unique<SumMimoTransform>());
  ASSERT_TRUE(node_or.has_value()) << node_or.error();
  PJ::TopicId out = derived.outputTopics(*node_or)[0];

  // Commit t1 only, notify only t1
  for (int i = 0; i < 5; ++i) {
    w1.appendScalar(h1, static_cast<PJ::Timestamp>(i) * 1'000'000'000LL, static_cast<double>(i));
  }
  derived.onSourceCommitted(engine.commitChunks(w1.flushAll()));
  ASSERT_TRUE(derived.scheduleAll().has_value());

  // No output: t2 has no data yet
  EXPECT_TRUE(collectValues(engine, out).empty());

  // Now commit t2 with the SAME timestamps as t1
  for (int i = 0; i < 5; ++i) {
    w2.appendScalar(h2, static_cast<PJ::Timestamp>(i) * 1'000'000'000LL, static_cast<double>(i * 2));
  }
  derived.onSourceCommitted(engine.commitChunks(w2.flushAll()));
  ASSERT_TRUE(derived.scheduleAll().has_value());

  // Now all 5 joins should be found (watermark was NOT advanced by the first schedule)
  auto rows = collectRowsCol(engine, out);
  ASSERT_EQ(rows.size(), 5u) << "Watermark advanced incorrectly; missed joins after lazy topic";
  for (int i = 0; i < 5; ++i) {
    EXPECT_NEAR(rows[i].second, static_cast<double>(i) + static_cast<double>(i * 2), 1e-9);
  }
}

// ---------------------------------------------------------------------------
// MIMO — wrong output_kinds count is rejected (M4)
// ---------------------------------------------------------------------------

class WrongOutputKindsMimoTransform : public IMIMOTransform {
 public:
  std::vector<StorageKind> outputKinds(PJ::Span<const StorageKind> /*input_kinds*/) const override {
    return {StorageKind::kFloat64, StorageKind::kFloat64};  // returns 2 but caller expects 1
  }

  bool calculate(PJ::Timestamp, PJ::Span<const VarValue>, PJ::Timestamp&, std::vector<VarValue>&) override {
    return false;
  }
};

TEST(MimoTransformTest, WrongOutputKindsCount_Fails) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId t1 = makeLinearTopic(engine, ds, 1.0, 5);
  // One output topic name but op returns two kinds → error
  auto r = derived.addMimoTransform({t1}, {"single_out"}, ds, std::make_unique<WrongOutputKindsMimoTransform>());
  EXPECT_FALSE(r.has_value());
}

TEST(MimoTransformTest, TopologicalOrder_MimoComesAfterSiso) {
  // n1: src→d1, n2: src→d2, n_mimo: (d1,d2)→sum. Order: n1,n2 before n_mimo.
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  PJ::TopicId src = makeLinearTopic(engine, ds, 1.0, 5);
  PJ::NodeId n1 = *derived.addSisoTransform(src, "d1", ds, std::make_unique<DerivativeTransform>());
  PJ::NodeId n2 = *derived.addSisoTransform(src, "d2", ds, std::make_unique<DerivativeTransform>());
  PJ::TopicId d1_out = derived.outputTopics(n1)[0];
  PJ::TopicId d2_out = derived.outputTopics(n2)[0];

  PJ::NodeId n_mimo = *derived.addMimoTransform({d1_out, d2_out}, {"sum"}, ds, std::make_unique<SumMimoTransform>());

  auto order = derived.topologicalOrder();
  ASSERT_EQ(order.size(), 3u);

  auto pos = [&](PJ::NodeId id) {
    return static_cast<std::size_t>(std::find(order.begin(), order.end(), id) - order.begin());
  };
  EXPECT_LT(pos(n1), pos(n_mimo));
  EXPECT_LT(pos(n2), pos(n_mimo));
}

// ---------------------------------------------------------------------------
// Uint64 precision round-trip (BUG-001)
// ---------------------------------------------------------------------------

// Identity SISO transform: passes value through unchanged, preserving StorageKind.
class Uint64IdentityTransform : public ISISOTransform {
 public:
  StorageKind outputKind(StorageKind input_kind) const override {
    return input_kind;
  }

  bool calculate(PJ::Timestamp time, const VarValue& input, PJ::Timestamp& out_time, VarValue& out_value) override {
    out_time = time;
    out_value = input;
    return true;
  }
};

TEST(DerivedEngine, Uint64PrecisionRoundTrip) {
  DataEngine engine;
  auto ds = makeDataset(engine);

  // Create a uint64 scalar topic with values that exceed double precision (>2^53)
  // and exceed int64_t range (>INT64_MAX).
  DataWriter writer = engine.createWriter();
  auto handle_or = writer.registerScalarSeries(ds, "u64_src", PJ::NumericType::kUint64);
  ASSERT_TRUE(handle_or.has_value());
  PJ::TopicId src_tid = handle_or->topic_id;

  const std::vector<uint64_t> test_values = {
      0ULL,
      42ULL,
      (1ULL << 53) + 1,                     // first value not exactly representable as double
      std::numeric_limits<int64_t>::max(),  // INT64_MAX
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1,  // INT64_MAX + 1
      std::numeric_limits<uint64_t>::max(),                            // UINT64_MAX
  };

  for (std::size_t i = 0; i < test_values.size(); ++i) {
    auto ts = static_cast<PJ::Timestamp>(i * 1'000'000'000LL);
    writer.appendScalar(*handle_or, ts, test_values[i]);
  }
  engine.commitChunks(writer.flushAll());

  // Register identity transform: kUint64 → kUint64
  DerivedEngine derived(engine);
  auto node_or = derived.addSisoTransform(src_tid, "u64_out", ds, std::make_unique<Uint64IdentityTransform>());
  ASSERT_TRUE(node_or.has_value());

  derived.onSourceCommitted({&src_tid, 1});
  auto s = derived.scheduleAll();
  ASSERT_TRUE(s.has_value()) << s.error();

  // Read back and verify exact bit-for-bit equality.
  auto out_topics = derived.outputTopics(*node_or);
  ASSERT_EQ(out_topics.size(), 1u);

  const TopicStorage* out_storage = engine.getTopicStorage(out_topics[0]);
  ASSERT_NE(out_storage, nullptr);
  const auto& chunks = out_storage->sealedChunks();
  ASSERT_EQ(chunks.size(), 1u);
  ASSERT_EQ(chunks[0].stats.row_count, test_values.size());

  for (std::size_t i = 0; i < test_values.size(); ++i) {
    uint64_t actual = chunks[0].readNumericAsUint64(0, i);
    EXPECT_EQ(actual, test_values[i]) << "Mismatch at row " << i;
  }
}

// ---------------------------------------------------------------------------
// Late (out-of-order) input commits
//
// Transforms have a strict sequential contract (ascending timestamps, state
// across calls), so an input commit that lands at or before a node's already-
// processed watermark cannot be applied incrementally — the node must reset
// and fully replay its now-time-sorted input.
// ---------------------------------------------------------------------------

// Append one row (ts, value) to an existing scalar topic and commit.
static void appendRow(DataEngine& engine, PJ::TopicId topic_id, PJ::Timestamp ts, double value) {
  DataWriter writer = engine.createWriter();
  auto s = writer.beginRow(topic_id, ts);
  (void)s;
  writer.set(topic_id, 0, value);
  auto s2 = writer.finishRow(topic_id);
  (void)s2;
  engine.commitChunks(writer.flushAll());
}

TEST(DerivedEngineTest, SisoLateInputCommitTriggersFullRecompute) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  // Rows at t = 0..4 s on a slope-2 line: derivative is 2.0 everywhere.
  PJ::TopicId src = makeLinearTopic(engine, ds, 2.0, 5);
  PJ::NodeId node = *derived.addSisoTransform(src, "deriv", ds, std::make_unique<DerivativeTransform>());
  notify(derived, {src});
  ASSERT_TRUE(derived.scheduleAll().has_value());
  ASSERT_EQ(collectValues(engine, derived.outputTopics(node)[0]).size(), 4u);

  // Late commit BEFORE the processed watermark: (2.5 s, 100.0), far off the line.
  appendRow(engine, src, 2'500'000'000LL, 100.0);
  notify(derived, {src});
  ASSERT_TRUE(derived.scheduleAll().has_value());

  // Correct output = derivative over the merged, time-sorted input
  // {0:0, 1:2, 2:4, 2.5:100, 3:6, 4:8}. A stateful incremental run that feeds
  // the late row after t=4 (or skips it) produces different values.
  const std::vector<std::pair<PJ::Timestamp, double>> expected = {
      {1'000'000'000LL, 2.0},    {2'000'000'000LL, 2.0}, {2'500'000'000LL, 192.0},
      {3'000'000'000LL, -188.0}, {4'000'000'000LL, 2.0},
  };
  const auto rows = collectRowsCol(engine, derived.outputTopics(node)[0]);
  ASSERT_EQ(rows.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(rows[i].first, expected[i].first) << "timestamp at row " << i;
    EXPECT_NEAR(rows[i].second, expected[i].second, 1e-6) << "value at row " << i;
  }
}

TEST(MimoTransformTest, MimoLateInputCommitTriggersFullRecompute) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  // Matching timestamps t = 0..4 s on both inputs; sum = 3t.
  PJ::TopicId t1 = makeLinearTopic(engine, ds, 1.0, 5);
  PJ::TopicId t2 = makeLinearTopic(engine, ds, 2.0, 5);
  PJ::NodeId node = *derived.addMimoTransform({t1, t2}, {"sum_out"}, ds, std::make_unique<SumMimoTransform>());
  notify(derived, {t1, t2});
  ASSERT_TRUE(derived.scheduleAll().has_value());
  ASSERT_EQ(collectRowsCol(engine, derived.outputTopics(node)[0]).size(), 5u);

  // Late commits BEFORE the watermark: both inputs gain a row at t = 2.5 s.
  appendRow(engine, t1, 2'500'000'000LL, 10.0);
  appendRow(engine, t2, 2'500'000'000LL, 20.0);
  notify(derived, {t1, t2});
  ASSERT_TRUE(derived.scheduleAll().has_value());

  // The joined timestamp set now includes 2.5 s; a timestamp-watermark
  // incremental run silently drops it.
  const std::vector<std::pair<PJ::Timestamp, double>> expected = {
      {0, 0.0},
      {1'000'000'000LL, 3.0},
      {2'000'000'000LL, 6.0},
      {2'500'000'000LL, 30.0},
      {3'000'000'000LL, 9.0},
      {4'000'000'000LL, 12.0},
  };
  const auto rows = collectRowsCol(engine, derived.outputTopics(node)[0]);
  ASSERT_EQ(rows.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(rows[i].first, expected[i].first) << "timestamp at row " << i;
    EXPECT_NEAR(rows[i].second, expected[i].second, 1e-6) << "value at row " << i;
  }
}

// Reproduces bug #3: a MIMO input whose column appears MID-STREAM. An early chunk is
// sealed with 1 column; a 2nd column is added later (ensureColumn); the MIMO reads
// column index 1. When the join processes a timestamp that maps to the OLD chunk (only
// 1 column), the MIMO decode path reads columns[1] OUT OF BOUNDS (the SISO path guards
// this at derived_engine.cpp; the MIMO path did not). Run under AddressSanitizer:
// before the fix this trips a heap-buffer-overflow; after the fix the old-chunk rows
// are skipped and it completes cleanly.
TEST(MimoTransformTest, MidStreamColumn_OldChunkNotOutOfBounds) {
  DataEngine engine;
  DerivedEngine derived(engine);
  PJ::DatasetId ds = makeDataset(engine);

  // Input A: schemaless topic; column "a" first (chunk with 1 column), then "b" added
  // mid-stream (chunk with 2 columns).
  DataWriter w = engine.createWriter();
  auto ta_or = w.registerTopic(ds, TopicDescriptor{.name = "multi", .schema_id = 0, .dataset_id = ds});
  ASSERT_TRUE(ta_or.has_value()) << ta_or.error();
  const PJ::TopicId ta = *ta_or;
  ASSERT_TRUE(w.ensureColumn(ta, "a", PJ::PrimitiveType::kFloat64).has_value());
  for (int i = 0; i < 3; ++i) {  // ts 0,1,2 — column 0 only
    const PJ::Timestamp ts = static_cast<PJ::Timestamp>(i) * 1'000'000'000LL;
    ASSERT_TRUE(w.beginRow(ta, ts).has_value());
    w.set<double>(ta, 0, static_cast<double>(i));
    ASSERT_TRUE(w.finishRow(ta).has_value());
  }
  engine.commitChunks(w.flushAll());  // seals chunk A (1 column)

  DataWriter w2 = engine.createWriter();
  ASSERT_TRUE(w2.ensureColumn(ta, "b", PJ::PrimitiveType::kFloat64).has_value());  // 2nd column, seals layout
  for (int i = 3; i < 6; ++i) {                                                    // ts 3,4,5 — columns 0 and 1
    const PJ::Timestamp ts = static_cast<PJ::Timestamp>(i) * 1'000'000'000LL;
    ASSERT_TRUE(w2.beginRow(ta, ts).has_value());
    w2.set<double>(ta, 0, static_cast<double>(i));
    w2.set<double>(ta, 1, 10.0 * static_cast<double>(i));
    ASSERT_TRUE(w2.finishRow(ta).has_value());
  }
  engine.commitChunks(w2.flushAll());  // chunk B (2 columns)

  // Input B: single scalar topic, ts 0..5 (overlaps A on every timestamp).
  const PJ::TopicId tb = makeLinearTopic(engine, ds, 2.0, 6);

  // MIMO reads column 1 of A (valid in the current 2-column schema) and column 0 of B.
  auto node_or =
      derived.addMimoTransform({ta, tb}, {"sum"}, ds, std::make_unique<SumMimoTransform>(), /*input_columns=*/{1, 0});
  ASSERT_TRUE(node_or.has_value()) << node_or.error();

  notify(derived, {ta, tb});
  ASSERT_TRUE(derived.scheduleAll().has_value());

  // ts 0,1,2 map to chunk A which lacks column 1 → those rows are skipped; only ts
  // 3,4,5 produce output (sum of A.b + B.value).
  const auto rows = collectRowsCol(engine, derived.outputTopics(*node_or)[0]);
  EXPECT_EQ(rows.size(), 3u);
}

}  // namespace
}  // namespace PJ
