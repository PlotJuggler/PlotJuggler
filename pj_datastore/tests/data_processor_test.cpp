// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/data_processor.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <variant>
#include <vector>

namespace PJ::proc {
namespace {

// Minimal stand-in to exercise the DataProcessor base batch helpers.
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

TEST(DataProcessorTest, ApplyBatchRunsEverySample) {
  PassThrough proc;
  std::vector<Sample> in;
  in.push_back(Sample::scalar(1, PJ::VarValue{1.0}));
  in.push_back(Sample::scalar(2, PJ::VarValue{2.0}));
  in.push_back(Sample::scalar(3, PJ::VarValue{3.0}));
  const auto out = proc.applyBatch(in);
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[2].raw_ts_ns, 3);
}

TEST(DataProcessorTest, AppendTailAppendsToExistingOutput) {
  PassThrough proc;
  std::vector<Sample> out;
  out.push_back(Sample::scalar(0, PJ::VarValue{99.0}));
  std::vector<Sample> tail;
  tail.push_back(Sample::scalar(1, PJ::VarValue{1.0}));
  proc.appendTail(tail, out);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_DOUBLE_EQ(std::get<double>(out[0].value()), 99.0);
  EXPECT_DOUBLE_EQ(std::get<double>(out[1].value()), 1.0);
}

}  // namespace
}  // namespace PJ::proc
