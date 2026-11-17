// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <optional>
#include <type_traits>
#include <variant>

#include "pj_base/dataset.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/data_processor.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/sample.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plotting/FilteredCurveAdapter.h"
#include "pj_runtime/CurveDescriptor.h"
#include "pj_runtime/SessionManager.h"

namespace PJ {
namespace {

// A trivial stateless filter used to drive the adapter without pulling in the
// Luau engine: y -> -y. Proves the adapter runs an arbitrary DataProcessor over
// the input column and serves the OUTPUT, distinct from the raw ghost values.
class NegateProcessor : public proc::DataProcessor {
 public:
  [[nodiscard]] const char* id() const override {
    return "negate";
  }
  [[nodiscard]] const char* bracketLabel() const override {
    return "Negate";
  }
  [[nodiscard]] proc::TraitMask traits() const override {
    return proc::kStatelessOneToOne;
  }
  [[nodiscard]] bool isStreamSafe() const override {
    return true;
  }
  void reset() override {}
  [[nodiscard]] std::optional<proc::Sample> calculateNextPoint(const proc::Sample& in) override {
    const double v = std::visit(
        [](auto&& value) -> double {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, std::string>) {
            return 0.0;
          } else {
            return static_cast<double>(value);
          }
        },
        in.value());
    return proc::Sample::scalar(in.raw_ts_ns, VarValue(-v));
  }
};

// A filter whose output is always NaN (e.g. a user function like `return 0/0`).
// Exercises the all-non-finite path in FilteredCurveAdapter's bounds computation.
class NanProcessor : public proc::DataProcessor {
 public:
  [[nodiscard]] const char* id() const override {
    return "nan";
  }
  [[nodiscard]] const char* bracketLabel() const override {
    return "NaN";
  }
  [[nodiscard]] proc::TraitMask traits() const override {
    return proc::kStatelessOneToOne;
  }
  [[nodiscard]] bool isStreamSafe() const override {
    return true;
  }
  void reset() override {}
  [[nodiscard]] std::optional<proc::Sample> calculateNextPoint(const proc::Sample& in) override {
    return proc::Sample::scalar(in.raw_ts_ns, VarValue(std::numeric_limits<double>::quiet_NaN()));
  }
};

class FilteredCurveAdapterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto dataset = session_.dataEngine().createDataset(DatasetDescriptor{.source_name = "stream"});
    ASSERT_TRUE(dataset.has_value()) << dataset.error();
    dataset_id_ = *dataset;

    writer_.emplace(session_.dataEngine().createWriter());
    auto handle = writer_->registerScalarSeries(dataset_id_, "/random", NumericType::kFloat64);
    ASSERT_TRUE(handle.has_value()) << handle.error();
    handle_ = *handle;

    appendAndCommit(0, 3);

    input_ = CurveDescriptor{
        .name = "/random",
        .topic_id = handle_.topic_id,
        .dataset_id = dataset_id_,
        .column_index = 0,
        .field_path = "",
    };
  }

  // Appends [first, last) as samples valued i, then commits (emitting
  // samplesIngested + sealing the chunks the adapter reads).
  void appendAndCommit(int first, int last) {
    for (int i = first; i < last; ++i) {
      writer_->appendScalar(handle_, static_cast<Timestamp>(i + 1) * 1'000'000, static_cast<double>(i));
    }
    ASSERT_FALSE(session_.commitChunks(writer_->flushAll()).empty());
  }

  static FilteredCurveAdapter::ProcessorFactory negateFactory() {
    return []() -> std::unique_ptr<proc::DataProcessor> { return std::make_unique<NegateProcessor>(); };
  }

  SessionManager session_;
  std::optional<DataWriter> writer_;
  ScalarSeriesHandle handle_{};
  DatasetId dataset_id_ = 0;
  CurveDescriptor input_;
};

// The adapter computes the filter over the whole input column on first read.
TEST_F(FilteredCurveAdapterTest, ServesFilteredSamplesOnFirstRead) {
  FilteredCurveAdapter adapter(&session_, input_, negateFactory());

  ASSERT_EQ(adapter.size(), 3U);
  for (std::size_t i = 0; i < adapter.size(); ++i) {
    EXPECT_DOUBLE_EQ(adapter.sample(i).y(), -static_cast<double>(i)) << "row " << i;
  }
}

// The adapter reports the INPUT topic id, so PlotWidget's existing samplesIngested
// handler matches it and calls onTopicCommitted() on the input's commit.
TEST_F(FilteredCurveAdapterTest, SourceReportsInputTopic) {
  FilteredCurveAdapter adapter(&session_, input_, negateFactory());
  EXPECT_EQ(adapter.source().topic_id, handle_.topic_id);
}

// The core of Option B: after the input streams more samples, onTopicCommitted()
// re-runs the filter so the curve TRACKS the live data instead of freezing — no
// timer, no panel wiring, just the same invalidation the ghost already rides.
TEST_F(FilteredCurveAdapterTest, RecomputesAndTracksStreamingIngest) {
  FilteredCurveAdapter adapter(&session_, input_, negateFactory());
  ASSERT_EQ(adapter.size(), 3U);

  appendAndCommit(3, 6);
  adapter.onTopicCommitted();

  ASSERT_EQ(adapter.size(), 6U);
  for (std::size_t i = 0; i < adapter.size(); ++i) {
    EXPECT_DOUBLE_EQ(adapter.sample(i).y(), -static_cast<double>(i)) << "row " << i;
  }
}

// boundingRect must reflect the FILTERED values (negated here), not the raw input
// bounds — otherwise auto-fit frames the wrong y-range.
TEST_F(FilteredCurveAdapterTest, BoundingRectCoversFilteredValues) {
  FilteredCurveAdapter adapter(&session_, input_, negateFactory());
  ASSERT_EQ(adapter.size(), 3U);  // values 0,1,2 -> filtered 0,-1,-2

  const QRectF rect = adapter.boundingRect();
  EXPECT_DOUBLE_EQ(rect.top(), -2.0);
  EXPECT_DOUBLE_EQ(rect.bottom(), 0.0);
}

// When every filtered value is non-finite (NaN) the bounds must not come out inverted: NaN is ignored by
// std::min/std::max, so a naive fold leaves y_min at +max and y_max at lowest(), an inverted range that corrupts
// auto-fit. The adapter reports no valid bounds (invalid rect, empty y-range), as an all-empty curve does.
TEST_F(FilteredCurveAdapterTest, AllNanOutputYieldsNoBounds) {
  FilteredCurveAdapter adapter(
      &session_, input_, []() -> std::unique_ptr<proc::DataProcessor> { return std::make_unique<NanProcessor>(); });
  ASSERT_EQ(adapter.size(), 3U);  // 3 NaN points still exist as samples

  const QRectF rect = adapter.boundingRect();
  const auto y_range = adapter.visibleYRange(Range<double>{.min = -1e30, .max = 1e30});

  // With no finite value there are no valid bounds (not a giant normalized rect [lowest, max], nor an inverted y range
  // {min = +max, max = lowest}).
  EXPECT_FALSE(rect.isValid()) << "bounding rect must be invalid when no finite value exists";
  if (y_range.has_value()) {
    EXPECT_LE(y_range->min, y_range->max) << "y-range must never be inverted (min > max)";
  }
}

// A null factory / "No Transform" yields an empty curve (nothing to overlay).
TEST_F(FilteredCurveAdapterTest, NullProcessorYieldsEmptyCurve) {
  FilteredCurveAdapter adapter(&session_, input_, []() -> std::unique_ptr<proc::DataProcessor> { return nullptr; });
  EXPECT_EQ(adapter.size(), 0U);
}

}  // namespace
}  // namespace PJ
