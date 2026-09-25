// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A field whose value type drifts across records (JSON sign flips, 1 vs 1.5,
// ...) keeps the column type it was created with: values that convert exactly
// are stored converted, the rest become null for that field only — the other
// fields of the record still land. PlotJuggler#1427.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/dataset.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/plugin_data_host.hpp"
#include "pj_datastore/topic_storage.hpp"

namespace PJ {
namespace {

std::vector<std::string> g_warnings;

struct Fixture : ::testing::Test {
  DataEngine engine;
  std::optional<DatastoreSourceWriteHost> host;
  std::optional<sdk::SourceWriteHostView> writer;
  sdk::TopicHandle topic{};

  void SetUp() override {
    g_warnings.clear();
    setIngestWarningHandler([](std::string_view message) { g_warnings.emplace_back(message); });
    const auto td = engine.createTimeDomain("td");
    const auto ds = engine.createDataset(DatasetDescriptor{.source_name = "src", .time_domain_id = *td});
    host.emplace(engine, PJ_data_source_handle_t{static_cast<uint32_t>(*ds)});
    writer.emplace(host->raw());
    topic = *writer->ensureTopic("battery");
  }

  void TearDown() override {
    setIngestWarningHandler(nullptr);
  }

  [[nodiscard]] bool append(Timestamp ts, std::vector<sdk::NamedFieldValue> fields) {
    return writer->appendRecord(topic, ts, fields).has_value();
  }

  // Stored values of `field` in row order; nullopt for null rows.
  [[nodiscard]] std::vector<std::optional<double>> values(std::string_view field) {
    host->flushPending();
    const auto lock = engine.lockEngine();
    std::vector<std::optional<double>> out;
    for (const auto& chunk : engine.getTopicStorage(topic.id)->sealedChunks()) {
      for (std::size_t c = 0; c < chunk.columns.size(); ++c) {
        if (chunk.columns[c].descriptor->field_path != field) {
          continue;
        }
        for (std::size_t row = 0; row < chunk.timestamps.size(); ++row) {
          out.push_back(chunk.isNull(c, row) ? std::nullopt : std::optional(chunk.readNumericAsDouble(c, row)));
        }
      }
    }
    return out;
  }

  [[nodiscard]] PrimitiveType columnType(std::string_view field) {
    const auto lock = engine.lockEngine();
    for (const auto& col : engine.getTopicStorage(topic.id)->columnDescriptors()) {
      if (col.field_path == field) {
        return col.logical_type;
      }
    }
    return PrimitiveType::kUnspecified;
  }
};

using IngestTypeCoercionTest = Fixture;
using Values = std::vector<std::optional<double>>;

TEST_F(IngestTypeCoercionTest, SignFlipKeepsSignedColumn) {
  ASSERT_TRUE(append(1, {{.name = "current", .value = int64_t{-20}}}));
  ASSERT_TRUE(append(2, {{.name = "current", .value = uint64_t{0}}}));
  ASSERT_TRUE(append(3, {{.name = "current", .value = uint64_t{15}}}));
  EXPECT_EQ(values("current"), (Values{-20.0, 0.0, 15.0}));
  EXPECT_EQ(columnType("current"), PrimitiveType::kInt64);
  EXPECT_TRUE(g_warnings.empty());
}

TEST_F(IngestTypeCoercionTest, IntegersIntoFloatColumn) {
  ASSERT_TRUE(append(1, {{.name = "x", .value = 1.5}}));
  ASSERT_TRUE(append(2, {{.name = "x", .value = int64_t{3}}}));
  ASSERT_TRUE(append(3, {{.name = "x", .value = uint64_t{1} << 53}}));
  EXPECT_EQ(values("x"), (Values{1.5, 3.0, 9007199254740992.0}));
  EXPECT_EQ(columnType("x"), PrimitiveType::kFloat64);
}

TEST_F(IngestTypeCoercionTest, WholeDoubleIntoIntegerColumn) {
  ASSERT_TRUE(append(1, {{.name = "x", .value = int64_t{1}}}));
  ASSERT_TRUE(append(2, {{.name = "x", .value = 2.0}}));
  EXPECT_EQ(values("x"), (Values{1.0, 2.0}));
  EXPECT_TRUE(g_warnings.empty());
}

TEST_F(IngestTypeCoercionTest, InexactValueNullsOnlyThatFieldAndWarnsOnce) {
  ASSERT_TRUE(append(1, {{.name = "x", .value = int64_t{1}}, {.name = "y", .value = int64_t{10}}}));
  ASSERT_TRUE(append(2, {{.name = "x", .value = 1.5}, {.name = "y", .value = int64_t{20}}}));
  ASSERT_TRUE(append(3, {{.name = "x", .value = 2.5}, {.name = "y", .value = int64_t{30}}}));
  EXPECT_EQ(values("x"), (Values{1.0, std::nullopt, std::nullopt}));
  EXPECT_EQ(values("y"), (Values{10.0, 20.0, 30.0}));
  ASSERT_EQ(g_warnings.size(), 1U);
  EXPECT_NE(g_warnings[0].find("battery/x"), std::string::npos) << g_warnings[0];
}

TEST_F(IngestTypeCoercionTest, OutOfRangeAndNonNumericValuesBecomeNull) {
  ASSERT_TRUE(append(1, {{.name = "i", .value = int64_t{0}}, {.name = "f", .value = 0.0}}));
  ASSERT_TRUE(append(
      2, {{.name = "i", .value = std::numeric_limits<uint64_t>::max()},
          {.name = "f", .value = std::numeric_limits<int64_t>::max()}}));  // 2^63-1: not exact in double
  ASSERT_TRUE(append(3, {{.name = "i", .value = std::nan("")}, {.name = "f", .value = std::string_view("1")}}));
  ASSERT_TRUE(append(4, {{.name = "i", .value = true}, {.name = "f", .value = (int64_t{1} << 53) + 1}}));
  EXPECT_EQ(values("i"), (Values{0.0, std::nullopt, std::nullopt, std::nullopt}));
  EXPECT_EQ(values("f"), (Values{0.0, std::nullopt, std::nullopt, std::nullopt}));
  EXPECT_EQ(g_warnings.size(), 2U);  // once per field
}

TEST_F(IngestTypeCoercionTest, NarrowIntegerColumnChecksRange) {
  ASSERT_TRUE(writer->ensureField(topic, "n", PrimitiveType::kInt16).has_value());
  ASSERT_TRUE(append(1, {{.name = "n", .value = int64_t{-32768}}}));
  ASSERT_TRUE(append(2, {{.name = "n", .value = uint64_t{32768}}}));
  EXPECT_EQ(values("n"), (Values{-32768.0, std::nullopt}));
}

TEST_F(IngestTypeCoercionTest, BoundRecordConvertsToDeclaredType) {
  const auto field = *writer->ensureField(topic, "ax", PrimitiveType::kFloat64);
  const std::vector<sdk::BoundFieldValue> ok = {{.field = field, .value = int32_t{42}}};
  ASSERT_TRUE(writer->appendBoundRecord(topic, 1, ok).has_value());
  const std::vector<sdk::BoundFieldValue> bad = {{.field = field, .value = std::string_view("x")}};
  ASSERT_TRUE(writer->appendBoundRecord(topic, 2, bad).has_value());
  EXPECT_EQ(values("ax"), (Values{42.0, std::nullopt}));
  EXPECT_EQ(g_warnings.size(), 1U);
}

TEST_F(IngestTypeCoercionTest, ExplicitEnsureFieldStillRejectsTypeChange) {
  ASSERT_TRUE(writer->ensureField(topic, "ax", PrimitiveType::kInt64).has_value());
  EXPECT_FALSE(writer->ensureField(topic, "ax", PrimitiveType::kFloat64).has_value());
}

// setTarget() starts a fresh write core with empty caches: the existing column
// must still be found (by name) and the value converted, not rejected.
TEST_F(IngestTypeCoercionTest, FreshCoreAfterSetTargetAdoptsExistingColumnType) {
  ASSERT_TRUE(append(1, {{.name = "current", .value = int64_t{-20}}}));
  host->setTarget(&engine);
  ASSERT_TRUE(append(2, {{.name = "current", .value = uint64_t{5}}}));
  EXPECT_EQ(values("current"), (Values{-20.0, 5.0}));
  EXPECT_EQ(columnType("current"), PrimitiveType::kInt64);
}

}  // namespace
}  // namespace PJ
