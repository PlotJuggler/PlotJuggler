// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// WidgetRuntimeHost: the history slot of pj.widget_host.v1 over the real DataEngine and ObjectStore,
// driven through the C vtable a plugin would hold.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/builtin/log_codec.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_plugins/sdk/widget_plugin_base.hpp"
#include "pj_runtime/CanonicalObjectPayload.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/WidgetRuntimeHost.h"

namespace PJ {
namespace {

constexpr int64_t kSecond = 1'000'000'000;

struct Sample {
  uint32_t input = 0;
  uint32_t slot = 0;
  PJ_widget_value_kind_t kind = PJ_WIDGET_VALUE_NONE;
  double time = 0.0;
  double number = 0.0;
  std::string text;
  std::vector<uint8_t> bytes;
  std::string type_name;
};

/// What the sink receives, and after how many samples it asks to stop (0 = never).
struct Collector {
  std::vector<Sample>* out = nullptr;
  std::size_t stop_after = 0;
};

bool collect(void* user, const PJ_widget_value_t* value) noexcept {
  auto* collector = static_cast<Collector*>(user);
  auto* out = collector->out;
  Sample sample;
  sample.input = value->input;
  sample.slot = value->slot;
  sample.kind = value->kind;
  sample.time = value->time;
  sample.number = value->number;
  if (value->text.data != nullptr) {
    sample.text.assign(value->text.data, value->text.size);
  }
  if (value->bytes.data != nullptr) {
    sample.bytes.assign(value->bytes.data, value->bytes.data + value->bytes.size);
  }
  if (value->type_name.data != nullptr) {
    sample.type_name.assign(value->type_name.data, value->type_name.size);
  }
  out->push_back(std::move(sample));
  return collector->stop_after == 0 || out->size() < collector->stop_after;
}

class WidgetRuntimeHostTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto dataset = engine_.createDataset(DatasetDescriptor{.source_name = "robot.mcap"});
    ASSERT_TRUE(dataset.has_value()) << dataset.error();
    dataset_ = *dataset;
    reader_ = std::make_unique<DataReader>(engine_);
    host_ = std::make_unique<WidgetRuntimeHost>(
        *reader_, objects_,
        [this](uint32_t input, uint32_t slot) -> std::optional<WidgetRuntimeHost::ResolvedWire> {
          if (slot != 0 || input >= wires_.size()) {
            return std::nullopt;
          }
          return WidgetRuntimeHost::ResolvedWire{wires_[input], {}};
        },
        WidgetRuntimeHost::Callbacks{
            .inputs_changed = [this] { ++inputs_changed_; },
            .report_message = [this](
                                  WidgetMessageLevel level,
                                  std::string_view message) { messages_.emplace_back(level, std::string(message)); }},
        ObjectPayloadEncoder{});  // no parsers here: the stored bytes are what a widget receives
  }

  /// 10 samples at t = 0..9 s (value = 10 * t) on a numeric topic; returns the wire.
  WireTarget addNumberWire(int64_t base_ns = 0) {
    DataWriter writer = engine_.createWriter();
    auto handle = writer.registerScalarSeries(dataset_, "/battery", NumericType::kFloat64);
    EXPECT_TRUE(handle.has_value());
    for (int i = 0; i < 10; ++i) {
      writer.appendScalar(*handle, base_ns + i * kSecond, 10.0 * i);
    }
    EXPECT_FALSE(engine_.commitChunks(writer.flushAll()).empty());
    WireTarget wire;
    wire.kind = WidgetInputKind::kNumber;
    wire.topic_id = handle->topic_id;
    wire.column = 0;
    wire.offset = DisplayOffset{Duration{base_ns}};
    wires_.push_back(wire);
    return wire;
  }

  bool query(
      uint32_t input, uint32_t slot, double from, double to, uint32_t max, std::vector<Sample>& out,
      PJ_error_t* error = nullptr, uint32_t* total = nullptr, std::size_t stop_after = 0) {
    PJ_error_t local{};
    const PJ_widget_host_t host = host_->raw();
    Collector collector{&out, stop_after};
    return host.vtable->query_history(
        host.ctx, input, slot, from, to, max, &collect, &collector, total, error != nullptr ? error : &local);
  }

  DataEngine engine_;
  ObjectStore objects_;
  std::unique_ptr<DataReader> reader_;
  std::unique_ptr<WidgetRuntimeHost> host_;
  DatasetId dataset_ = 0;
  std::vector<WireTarget> wires_;
  int inputs_changed_ = 0;
  std::vector<std::pair<WidgetMessageLevel, std::string>> messages_;
};

TEST_F(WidgetRuntimeHostTest, NumberHistoryIsAscendingInclusiveAndOnTheDisplayAxis) {
  addNumberWire();
  std::vector<Sample> out;
  ASSERT_TRUE(query(0, 0, 2.0, 5.0, 0, out));
  ASSERT_EQ(out.size(), 4U);
  for (std::size_t i = 0; i < out.size(); ++i) {
    EXPECT_EQ(out[i].kind, PJ_WIDGET_VALUE_NUMBER);
    EXPECT_DOUBLE_EQ(out[i].time, 2.0 + static_cast<double>(i));
    EXPECT_DOUBLE_EQ(out[i].number, 10.0 * (2.0 + static_cast<double>(i)));
    EXPECT_EQ(out[i].input, 0U);
  }
}

TEST_F(WidgetRuntimeHostTest, DisplayOffsetShiftsTheAxis) {
  // Stored at 1000 s + i; the dataset's display offset is 1000 s, so display time is i.
  addNumberWire(1000 * kSecond);
  std::vector<Sample> out;
  ASSERT_TRUE(query(0, 0, 3.0, 4.0, 0, out));
  ASSERT_EQ(out.size(), 2U);
  EXPECT_DOUBLE_EQ(out[0].time, 3.0);
  EXPECT_DOUBLE_EQ(out[0].number, 30.0);
  EXPECT_DOUBLE_EQ(out[1].time, 4.0);
}

TEST_F(WidgetRuntimeHostTest, CapKeepsFirstAndLastAndStaysAscending) {
  addNumberWire();
  std::vector<Sample> out;
  ASSERT_TRUE(query(0, 0, -1e300, 1e300, 4, out));  // an unbounded range is legitimate
  ASSERT_EQ(out.size(), 4U);
  EXPECT_DOUBLE_EQ(out.front().time, 0.0);
  EXPECT_DOUBLE_EQ(out.back().time, 9.0);
  for (std::size_t i = 1; i < out.size(); ++i) {
    EXPECT_LT(out[i - 1].time, out[i].time);
  }
}

TEST_F(WidgetRuntimeHostTest, ZeroMaxSamplesTakesTheDefaultAndAnExplicitOneIsHonoured) {
  // More samples than the default cap, and more than any fixed per-call limit a widget could hit when
  // it counts a whole log and then asks for all of it.
  constexpr uint32_t kCount = 150'000;
  DataWriter writer = engine_.createWriter();
  auto handle = writer.registerScalarSeries(dataset_, "/dense", NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value());
  for (uint32_t i = 0; i < kCount; ++i) {
    writer.appendScalar(*handle, static_cast<int64_t>(i) * 1'000'000, static_cast<double>(i));
  }
  ASSERT_FALSE(engine_.commitChunks(writer.flushAll()).empty());
  WireTarget wire;
  wire.kind = WidgetInputKind::kNumber;
  wire.topic_id = handle->topic_id;
  wires_.push_back(wire);

  std::vector<Sample> out;
  uint32_t total = 0;
  ASSERT_TRUE(query(0, 0, -1e300, 1e300, 0, out, nullptr, &total));
  EXPECT_EQ(out.size(), WidgetRuntimeHost::kDefaultMaxSamples);
  EXPECT_EQ(total, kCount);

  out.clear();
  ASSERT_TRUE(query(0, 0, -1e300, 1e300, total, out, nullptr, &total));
  ASSERT_EQ(out.size(), kCount) << "an explicit max_samples is not clamped";
  EXPECT_DOUBLE_EQ(out.back().number, static_cast<double>(kCount - 1));
}

TEST_F(WidgetRuntimeHostTest, TotalCountsTheRangeBeforeTheCap) {
  addNumberWire();
  std::vector<Sample> out;
  uint32_t total = 99;
  ASSERT_TRUE(query(0, 0, -1e300, 1e300, 4, out, nullptr, &total));
  EXPECT_EQ(out.size(), 4U);
  EXPECT_EQ(total, 10U) << "more than delivered: the widget can tell the history was reduced";
  out.clear();
  ASSERT_TRUE(query(0, 0, 2.0, 5.0, 0, out, nullptr, &total));
  EXPECT_EQ(total, 4U);
  out.clear();
  ASSERT_TRUE(query(0, 0, 100.0, 200.0, 0, out, nullptr, &total));
  EXPECT_EQ(total, 0U);
  PJ_error_t error{};
  total = 99;
  EXPECT_FALSE(query(3, 0, 0.0, 9.0, 0, out, &error, &total));
  EXPECT_EQ(total, 0U) << "0 on failure";
}

TEST_F(WidgetRuntimeHostTest, SinkReturningFalseStopsTheDelivery) {
  addNumberWire();
  std::vector<Sample> out;
  uint32_t total = 0;
  ASSERT_TRUE(query(0, 0, 0.0, 9.0, 0, out, nullptr, &total, 3)) << "stopping early is still a success";
  ASSERT_EQ(out.size(), 3U);
  EXPECT_DOUBLE_EQ(out.back().time, 2.0);
  EXPECT_EQ(total, 10U);
}

TEST_F(WidgetRuntimeHostTest, EmptyRangeAndInvertedRangeDeliverNothing) {
  addNumberWire();
  std::vector<Sample> out;
  ASSERT_TRUE(query(0, 0, 100.0, 200.0, 0, out));
  EXPECT_TRUE(out.empty());
  ASSERT_TRUE(query(0, 0, 5.0, 2.0, 0, out));
  EXPECT_TRUE(out.empty());
}

TEST_F(WidgetRuntimeHostTest, UnwiredInputIsRefusedWithoutCallingTheSink) {
  addNumberWire();
  std::vector<Sample> out;
  PJ_error_t error{};
  EXPECT_FALSE(query(3, 0, 0.0, 9.0, 0, out, &error));
  EXPECT_TRUE(out.empty());
  EXPECT_NE(std::string(error.message).find("not wired"), std::string::npos);
  EXPECT_FALSE(query(0, 1, 0.0, 9.0, 0, out));  // unknown slot
  EXPECT_FALSE(query(0, 0, std::nan(""), 9.0, 0, out));
}

TEST_F(WidgetRuntimeHostTest, TextHistoryDeliversStrings) {
  DataWriter writer = engine_.createWriter();
  auto topic = writer.registerTopic(dataset_, TopicDescriptor{.name = "/mode"});
  ASSERT_TRUE(topic.has_value()) << topic.error();
  ASSERT_TRUE(writer.ensureColumn(*topic, "state", PrimitiveType::kString).has_value());
  const char* states[] = {"IDLE", "ARMED", "FLYING"};
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(writer.beginRow(*topic, i * kSecond).has_value());
    writer.set<std::string_view>(*topic, 0, states[i]);
    ASSERT_TRUE(writer.finishRow(*topic).has_value());
  }
  ASSERT_FALSE(engine_.commitChunks(writer.flushAll()).empty());
  WireTarget wire;
  wire.kind = WidgetInputKind::kText;
  wire.topic_id = *topic;
  wire.column = 0;
  wires_.push_back(wire);

  std::vector<Sample> out;
  ASSERT_TRUE(query(0, 0, 0.0, 2.0, 0, out));
  ASSERT_EQ(out.size(), 3U);
  EXPECT_EQ(out[0].kind, PJ_WIDGET_VALUE_TEXT);
  EXPECT_EQ(out[0].text, "IDLE");
  EXPECT_EQ(out[2].text, "FLYING");
  EXPECT_DOUBLE_EQ(out[1].time, 1.0);

  out.clear();
  uint32_t total = 0;
  ASSERT_TRUE(query(0, 0, 0.0, 2.0, 0, out, nullptr, &total, 1));
  EXPECT_EQ(out.size(), 1U);
  EXPECT_EQ(total, 3U);
}

TEST_F(WidgetRuntimeHostTest, ObjectHistoryDeliversPayloadAndTypeName) {
  auto topic = objects_.registerTopic(ObjectTopicDescriptor{.dataset_id = dataset_, .topic_name = "/camera"});
  ASSERT_TRUE(topic.has_value()) << topic.error();
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(objects_.pushOwned(*topic, i * kSecond, {static_cast<uint8_t>(i), 0xAB}).has_value());
  }
  WireTarget wire;
  wire.kind = WidgetInputKind::kObject;
  wire.object_topic = *topic;
  wire.object_type = sdk::BuiltinObjectType::kImage;
  wires_.push_back(wire);

  std::vector<Sample> out;
  ASSERT_TRUE(query(0, 0, 1.0, 3.0, 0, out));  // both bounds inclusive
  ASSERT_EQ(out.size(), 3U);
  EXPECT_EQ(out[0].kind, PJ_WIDGET_VALUE_OBJECT);
  EXPECT_DOUBLE_EQ(out[0].time, 1.0);
  EXPECT_EQ(out[0].bytes, (std::vector<uint8_t>{1, 0xAB}));
  EXPECT_EQ(out[0].type_name, "kImage");
  EXPECT_EQ(out[2].bytes.front(), 3);

  out.clear();
  uint32_t total = 0;
  ASSERT_TRUE(query(0, 0, 0.0, 4.0, 2, out, nullptr, &total));
  ASSERT_EQ(out.size(), 2U);
  EXPECT_DOUBLE_EQ(out.front().time, 0.0);
  EXPECT_DOUBLE_EQ(out.back().time, 4.0);
  EXPECT_EQ(total, 5U);

  out.clear();
  ASSERT_TRUE(query(0, 0, 0.0, 4.0, 0, out, nullptr, &total, 2));
  EXPECT_EQ(out.size(), 2U);
  EXPECT_DOUBLE_EQ(out.back().time, 1.0);
}

/// Registers an object topic holding one entry per element of `seconds` (non-decreasing), whose first payload
/// byte is the entry's index, and wires it as input 0.
class WidgetRuntimeHostObjectHistoryTest : public WidgetRuntimeHostTest {
 protected:
  void addObjectWire(const std::vector<int>& seconds) {
    auto topic = objects_.registerTopic(ObjectTopicDescriptor{.dataset_id = dataset_, .topic_name = "/log"});
    ASSERT_TRUE(topic.has_value()) << topic.error();
    for (std::size_t i = 0; i < seconds.size(); ++i) {
      ASSERT_TRUE(objects_.pushOwned(*topic, seconds[i] * kSecond, {static_cast<uint8_t>(i)}).has_value());
    }
    WireTarget wire;
    wire.kind = WidgetInputKind::kObject;
    wire.object_topic = *topic;
    wire.object_type = sdk::BuiltinObjectType::kLog;
    wires_.push_back(wire);
  }

  static std::vector<int> indices(const std::vector<Sample>& out) {
    std::vector<int> result;
    for (const Sample& sample : out) {
      result.push_back(sample.bytes.front());
    }
    return result;
  }
};

TEST_F(WidgetRuntimeHostObjectHistoryTest, MaxSamplesOneDeliversTheFirstEntryAndCountsTheRange) {
  std::vector<int> seconds;
  for (int i = 0; i < 50; ++i) {
    seconds.push_back(i);
  }
  addObjectWire(seconds);

  std::vector<Sample> out;
  uint32_t total = 0;
  ASSERT_TRUE(query(0, 0, 0.0, 49.0, 1, out, nullptr, &total));
  EXPECT_EQ(total, 50U);
  EXPECT_EQ(indices(out), std::vector<int>{0});

  // A sub-range delivers the first entry of that range.
  out.clear();
  ASSERT_TRUE(query(0, 0, 10.0, 19.0, 1, out, nullptr, &total));
  EXPECT_EQ(total, 10U);
  EXPECT_EQ(indices(out), std::vector<int>{10});

  // A sink refusing after the first sample ends the delivery; the count stays that of the range.
  out.clear();
  ASSERT_TRUE(query(0, 0, 0.0, 49.0, 0, out, nullptr, &total, 1));
  EXPECT_EQ(total, 50U);
  EXPECT_EQ(indices(out), std::vector<int>{0});
}

TEST_F(WidgetRuntimeHostObjectHistoryTest, DecimationOverSharedTimestampsPicksByRankWithinTheStamp) {
  // 36 entries: index 0 at t=0; indices 1..30 five per stamp at t=1..6; indices 31..35 unique at t=7..11.
  std::vector<int> seconds{0};
  for (int stamp = 1; stamp <= 6; ++stamp) {
    seconds.insert(seconds.end(), 5, stamp);
  }
  for (int stamp = 7; stamp <= 11; ++stamp) {
    seconds.push_back(stamp);
  }
  ASSERT_EQ(seconds.size(), 36U);
  addObjectWire(seconds);

  // The j-th delivered entry is the one at index j * (total - 1) / (cap - 1) of the time-ordered range.
  std::vector<Sample> out;
  uint32_t total = 0;
  ASSERT_TRUE(query(0, 0, 0.0, 11.0, 7, out, nullptr, &total));
  EXPECT_EQ(total, 36U);
  EXPECT_EQ(indices(out), (std::vector<int>{0, 5, 11, 17, 23, 29, 35}));  // 0, 35/6, 70/6, ... 210/6

  // cap 20: picks 1 and 2 (indices 1 and 3) share the stamp t=1 with ranks 0 and 2.
  out.clear();
  ASSERT_TRUE(query(0, 0, 0.0, 11.0, 20, out, nullptr, &total));
  EXPECT_EQ(total, 36U);
  std::vector<int> expected;
  for (int j = 0; j < 20; ++j) {
    expected.push_back(j * 35 / 19);
  }
  EXPECT_EQ(expected.size(), 20U);
  EXPECT_EQ(indices(out), expected);
  EXPECT_EQ(out[1].time, out[2].time) << "two picks on one timestamp";
  EXPECT_NE(out[1].bytes, out[2].bytes) << "with different ranks";

  // A range starting inside a shared stamp decimates over that range alone: t=2..4 holds indices 6..20.
  out.clear();
  ASSERT_TRUE(query(0, 0, 2.0, 4.0, 4, out, nullptr, &total));
  EXPECT_EQ(total, 15U);
  EXPECT_EQ(indices(out), (std::vector<int>{6, 10, 15, 20}));  // 6 + {0, 14/3, 28/3, 14}
}

TEST_F(WidgetRuntimeHostObjectHistoryTest, RangeBoundsAreInclusiveAndAnEmptyRangeNeverCallsTheSink) {
  addObjectWire({1, 2, 2, 3, 5, 5, 8});
  std::vector<Sample> out;
  uint32_t total = 0;

  // Bounds equal to entry timestamps keep every entry stamped there.
  ASSERT_TRUE(query(0, 0, 2.0, 5.0, 0, out, nullptr, &total));
  EXPECT_EQ(total, 5U);
  EXPECT_EQ(indices(out), (std::vector<int>{1, 2, 3, 4, 5}));

  out.clear();
  ASSERT_TRUE(query(0, 0, 1.0, 8.0, 0, out, nullptr, &total));
  EXPECT_EQ(total, 7U);
  EXPECT_EQ(indices(out), (std::vector<int>{0, 1, 2, 3, 4, 5, 6}));

  out.clear();
  ASSERT_TRUE(query(0, 0, 5.0, 5.0, 0, out, nullptr, &total));
  EXPECT_EQ(total, 2U);
  EXPECT_EQ(indices(out), (std::vector<int>{4, 5}));

  // Decimated, the bounds stay inclusive and both ends are delivered.
  out.clear();
  ASSERT_TRUE(query(0, 0, 2.0, 5.0, 2, out, nullptr, &total));
  EXPECT_EQ(total, 5U);
  EXPECT_EQ(indices(out), (std::vector<int>{1, 5}));

  // Empty ranges: between stamps, before the first, after the last.
  for (const auto& [from, to] : std::vector<std::pair<double, double>>{{6.0, 7.0}, {-5.0, 0.5}, {9.0, 20.0}}) {
    out.clear();
    total = 99;
    ASSERT_TRUE(query(0, 0, from, to, 0, out, nullptr, &total));
    EXPECT_EQ(total, 0U);
    EXPECT_TRUE(out.empty());
  }
}

TEST_F(WidgetRuntimeHostObjectHistoryTest, ARangePastEveryInstantKeepsTheEntriesAtTheInt64Limits) {
  auto topic = objects_.registerTopic(ObjectTopicDescriptor{.dataset_id = dataset_, .topic_name = "/limits"});
  ASSERT_TRUE(topic.has_value()) << topic.error();
  ASSERT_TRUE(objects_.pushOwned(*topic, 0, {0}).has_value());
  ASSERT_TRUE(objects_.pushOwned(*topic, std::numeric_limits<Timestamp>::max(), {1}).has_value());
  WireTarget wire;
  wire.kind = WidgetInputKind::kObject;
  wire.object_topic = *topic;
  wire.object_type = sdk::BuiltinObjectType::kLog;
  wires_.push_back(wire);

  std::vector<Sample> out;
  uint32_t total = 0;
  ASSERT_TRUE(query(0, 0, -1e300, 1e300, 0, out, nullptr, &total));
  EXPECT_EQ(total, 2U);
  EXPECT_EQ(indices(out), (std::vector<int>{0, 1}));

  // A range starting past every instant holds nothing, not the entry at the last one.
  out.clear();
  ASSERT_TRUE(query(0, 0, 1e300, 1e301, 0, out, nullptr, &total));
  EXPECT_EQ(total, 0U);
  EXPECT_TRUE(out.empty());
}

TEST_F(WidgetRuntimeHostTest, AHistoryTimeConvertsBackToItsSampleFarFromTheOrigin) {
  // Near an epoch a double steps ~238 ns: the nearest one to a stamp may convert back before it.
  auto topic = objects_.registerTopic(ObjectTopicDescriptor{.dataset_id = dataset_, .topic_name = "/epoch"});
  ASSERT_TRUE(topic.has_value());
  constexpr int64_t kFirst = 1'731'234'567'000'000'005;
  constexpr int64_t kSpacing = 1'000'003;
  constexpr int kEntries = 64;
  for (int k = 0; k < kEntries; ++k) {
    ASSERT_TRUE(objects_.pushOwned(*topic, kFirst + k * kSpacing, {static_cast<uint8_t>(k)}).has_value());
  }
  WireTarget wire;
  wire.kind = WidgetInputKind::kObject;
  wire.object_topic = *topic;
  wires_.push_back(wire);

  std::vector<Sample> out;
  ASSERT_TRUE(query(0, 0, -1e300, 1e300, 0, out));
  ASSERT_EQ(out.size(), static_cast<std::size_t>(kEntries));
  for (int k = 0; k < kEntries; ++k) {
    const Timestamp back = displaySecondsToRaw(displaySeconds(out[k].time), DisplayOffset{});
    EXPECT_GE(back, kFirst + k * kSpacing) << "entry " << k;
    EXPECT_LT(back, kFirst + (k + 1) * kSpacing) << "entry " << k;
  }
}

/// A widget queries back with the times the host handed it (a log list reading the tail after the last
/// entry it holds): each delivered time is a bound that keeps its own sample, at epoch display times, where a
/// double steps ~238 ns, and near the origin, where many doubles share a nanosecond.
class WidgetRuntimeHostQueryBackTest : public WidgetRuntimeHostTest, public ::testing::WithParamInterface<int64_t> {};

TEST_P(WidgetRuntimeHostQueryBackTest, ADeliveredTimeBoundsTheRangeAtItsOwnSample) {
  auto topic = objects_.registerTopic(ObjectTopicDescriptor{.dataset_id = dataset_, .topic_name = "/rosout"});
  ASSERT_TRUE(topic.has_value());
  const int64_t first = GetParam();
  constexpr int kEntries = 64;
  for (int k = 0; k < kEntries; ++k) {
    // Spacings of 1..63 ns put neighbours inside one double step at epoch times.
    const int64_t stamp = first + int64_t{k} * (k + 1) / 2;
    ASSERT_TRUE(objects_.pushOwned(*topic, stamp, {static_cast<uint8_t>(k)}).has_value());
  }
  WireTarget wire;
  wire.kind = WidgetInputKind::kObject;
  wire.object_topic = *topic;
  wires_.push_back(wire);

  std::vector<Sample> all;
  ASSERT_TRUE(query(0, 0, -1e300, 1e300, 0, all));
  ASSERT_EQ(all.size(), static_cast<std::size_t>(kEntries));

  for (int k = 0; k < kEntries; ++k) {
    // Every entry delivered at that time, and only those.
    uint32_t same_time = 0;
    uint32_t from_here = 0;
    for (const Sample& sample : all) {
      same_time += sample.time == all[k].time ? 1U : 0U;
      from_here += sample.time >= all[k].time ? 1U : 0U;
    }
    std::vector<Sample> out;
    uint32_t total = 0;
    ASSERT_TRUE(query(0, 0, all[k].time, all[k].time, 0, out, nullptr, &total));
    EXPECT_EQ(total, same_time) << "entry " << k;
    for (const Sample& sample : out) {
      EXPECT_EQ(sample.time, all[k].time) << "entry " << k;
    }
    out.clear();
    ASSERT_TRUE(query(0, 0, all[k].time, 1e300, 0, out, nullptr, &total));
    EXPECT_EQ(total, from_here) << "entry " << k;
    out.clear();
    ASSERT_TRUE(query(0, 0, -1e300, all[k].time, 0, out, nullptr, &total));
    EXPECT_EQ(total, static_cast<uint32_t>(kEntries) - from_here + same_time) << "entry " << k;
  }
}

INSTANTIATE_TEST_SUITE_P(
    DisplayTimes, WidgetRuntimeHostQueryBackTest, ::testing::Values(int64_t{1'731'234'567'000'000'005}, int64_t{3}),
    [](const ::testing::TestParamInfo<int64_t>& param_info) {
      return param_info.param > kSecond ? std::string("Epoch") : std::string("Origin");
    });

TEST_F(WidgetRuntimeHostTest, ANumberTimeBoundsTheRangeAtItsOwnSampleAtEpochTimes) {
  addNumberWire(1'731'234'567'000'000'005);
  wires_.back().offset = DisplayOffset{};  // epoch display times
  std::vector<Sample> all;
  ASSERT_TRUE(query(0, 0, -1e300, 1e300, 0, all));
  ASSERT_EQ(all.size(), 10U);
  for (std::size_t k = 0; k < all.size(); ++k) {
    std::vector<Sample> out;
    uint32_t total = 0;
    ASSERT_TRUE(query(0, 0, all[k].time, 1e300, 0, out, nullptr, &total));
    EXPECT_EQ(total, all.size() - k) << "sample " << k;
    ASSERT_FALSE(out.empty());
    EXPECT_EQ(out.front().number, all[k].number) << "sample " << k;
  }
}

TEST_F(WidgetRuntimeHostTest, InputsChangedAndMessagesReachTheCallbacks) {
  const PJ_widget_host_t host = host_->raw();
  host.vtable->notify_inputs_changed(host.ctx);
  EXPECT_EQ(inputs_changed_, 1);
  const char text[] = "hello";
  host.vtable->report_widget_message(host.ctx, PJ_WIDGET_MESSAGE_WARNING, PJ_string_view_t{text, 5});
  ASSERT_EQ(messages_.size(), 1U);
  EXPECT_EQ(messages_[0].first, WidgetMessageLevel::kWarning);
  EXPECT_EQ(messages_[0].second, "hello");
}

TEST_F(WidgetRuntimeHostTest, RegistersTheServiceUnderItsName) {
  ServiceRegistryBuilder registry;
  ASSERT_TRUE(host_->registerServices(registry).has_value());
  EXPECT_EQ(registry.size(), 1U);
  // One service per panel: a second registration of the name in the same registry is a wiring bug.
  EXPECT_FALSE(host_->registerServices(registry).has_value());
}

// --- Canonical object bytes (a parser-backed topic stores wire messages, not canonical objects) ---

constexpr std::string_view kLogSchema = "test_log";

/// Decodes a payload of plain text into a kLog whose message is that text; "bad" fails, as a corrupt
/// wire message would.
class TextLogParser : public MessageParserPluginBase {
 public:
  TextLogParser() {
    sdk::SchemaHandler handler;
    handler.object_type = sdk::BuiltinObjectType::kLog;
    handler.parse_object = [](Timestamp ts, sdk::PayloadView payload) -> Expected<sdk::ObjectRecord> {
      const std::string text(reinterpret_cast<const char*>(payload.bytes.data()), payload.bytes.size());
      if (text == "bad") {
        return unexpected(std::string("corrupt message"));
      }
      sdk::Log log;
      log.timestamp_ns = ts;
      log.level = sdk::Log::Level::kWarning;
      log.message = text;
      log.source = "node";
      log.category = "motion";
      return sdk::ObjectRecord{ts, sdk::BuiltinObject{log}};
    };
    registerSchemaHandler(kLogSchema, std::move(handler));
  }
};

std::unique_ptr<MessageParserHandle> makeTextLogParser() {
  static constexpr const char* kManifest =
      R"({"id":"text-log-parser","name":"Text Log Parser","version":"1.0.0","encoding":["mock"]})";
  auto handle = std::make_unique<MessageParserHandle>(
      MessageParserPluginBase::vtableWithCreate([]() noexcept -> void* { return new TextLogParser(); }, kManifest));
  EXPECT_TRUE(handle->valid());
  EXPECT_TRUE(handle->bindSchema(kLogSchema, {}).has_value());
  return handle;
}

class WidgetRuntimeHostCanonicalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto dataset = session_.dataEngine().createDataset(DatasetDescriptor{.source_name = "robot.mcap"});
    ASSERT_TRUE(dataset.has_value()) << dataset.error();
    dataset_ = *dataset;
    reader_ = std::make_unique<DataReader>(session_.createReader());
    host_ = std::make_unique<WidgetRuntimeHost>(
        *reader_, session_.objectStore(),
        [this](uint32_t input, uint32_t slot) -> std::optional<WidgetRuntimeHost::ResolvedWire> {
          if (slot != 0 || input >= wires_.size()) {
            return std::nullopt;
          }
          return WidgetRuntimeHost::ResolvedWire{wires_[input], {}};
        },
        WidgetRuntimeHost::Callbacks{}, canonicalObjectEncoder(session_));
  }

  /// An object topic of `type` holding one entry per payload, at t = 0, 1, 2... s; wired as input 0.
  ObjectTopicId addObjectWire(sdk::BuiltinObjectType type, const std::vector<std::string>& payloads) {
    auto topic =
        session_.objectStore().registerTopic(ObjectTopicDescriptor{.dataset_id = dataset_, .topic_name = "/rosout"});
    EXPECT_TRUE(topic.has_value());
    for (std::size_t i = 0; i < payloads.size(); ++i) {
      EXPECT_TRUE(session_.objectStore()
                      .pushOwned(
                          *topic, static_cast<int64_t>(i) * kSecond,
                          std::vector<uint8_t>(payloads[i].begin(), payloads[i].end()))
                      .has_value());
    }
    WireTarget wire;
    wire.kind = WidgetInputKind::kObject;
    wire.object_topic = *topic;
    wire.object_type = type;
    wires_.push_back(wire);
    return *topic;
  }

  bool query(double from, double to, std::vector<Sample>& out, uint32_t* total = nullptr) {
    PJ_error_t error{};
    const PJ_widget_host_t host = host_->raw();
    Collector collector{&out, 0};
    return host.vtable->query_history(host.ctx, 0, 0, from, to, 0, &collect, &collector, total, &error);
  }

  SessionManager session_;
  std::unique_ptr<DataReader> reader_;
  std::unique_ptr<WidgetRuntimeHost> host_;
  DatasetId dataset_ = 0;
  std::vector<WireTarget> wires_;
};

TEST_F(WidgetRuntimeHostCanonicalTest, AParserBackedTopicDeliversCanonicalBytes) {
  const ObjectTopicId topic = addObjectWire(sdk::BuiltinObjectType::kLog, {"started", "running"});
  session_.registerObjectTopicParser(topic, makeTextLogParser());

  std::vector<Sample> out;
  ASSERT_TRUE(query(0.0, 10.0, out));
  ASSERT_EQ(out.size(), 2U);
  EXPECT_EQ(out[0].type_name, "kLog");
  const auto log = deserializeLog(out[1].bytes.data(), out[1].bytes.size());
  ASSERT_TRUE(log.has_value()) << log.error();
  EXPECT_EQ(log->message, "running");
  EXPECT_EQ(log->level, sdk::Log::Level::kWarning);
  EXPECT_EQ(log->category, "motion");  // canonical bytes keep every Log field
  EXPECT_EQ(log->timestamp_ns, kSecond);
}

TEST_F(WidgetRuntimeHostCanonicalTest, AnEntryThatDoesNotDecodeIsNotCountedWhenNothingIsDecimated) {
  const ObjectTopicId topic = addObjectWire(sdk::BuiltinObjectType::kLog, {"one", "bad", "three"});
  session_.registerObjectTopicParser(topic, makeTextLogParser());

  std::vector<Sample> out;
  uint32_t total = 0;
  ASSERT_TRUE(query(0.0, 10.0, out, &total));
  ASSERT_EQ(out.size(), 2U);
  EXPECT_DOUBLE_EQ(out[0].time, 0.0);
  EXPECT_DOUBLE_EQ(out[1].time, 2.0);
  EXPECT_EQ(total, 2U) << "an undecimated history reports what it delivered: total == delivered means complete";
}

TEST_F(WidgetRuntimeHostCanonicalTest, ATimeShiftedMergeMovesTheLogStampOntoTheDatasetClock) {
  // A source that pushed canonical log bytes: their own stamp is the time the store holds them at.
  sdk::Log log;
  log.timestamp_ns = kSecond;
  log.message = "started";
  const std::vector<uint8_t> bytes = serializeLog(log);
  addObjectWire(sdk::BuiltinObjectType::kLog, {std::string(bytes.begin(), bytes.end())});
  auto anchor = session_.dataEngine().createDataset(DatasetDescriptor{.source_name = "anchor.mcap"});
  ASSERT_TRUE(anchor.has_value()) << anchor.error();
  ASSERT_TRUE(session_.objectStore()
                  .registerTopic(ObjectTopicDescriptor{.dataset_id = *anchor, .topic_name = "/a"})
                  .has_value());
  constexpr Timestamp kShift = 5 * kSecond;
  ASSERT_TRUE(session_.objectStore()
                  .mergeDatasets(*anchor, {DatasetMergeSource{.dataset_id = dataset_, .raw_shift_ns = kShift}})
                  .has_value());

  std::vector<Sample> out;
  ASSERT_TRUE(query(-100.0, 100.0, out));
  ASSERT_EQ(out.size(), 1U);
  const auto delivered = deserializeLog(out[0].bytes.data(), out[0].bytes.size());
  ASSERT_TRUE(delivered.has_value()) << delivered.error();
  EXPECT_EQ(delivered->timestamp_ns, kSecond + kShift) << "the log's own stamp is on the clock its entry moved to";
}

TEST_F(WidgetRuntimeHostCanonicalTest, ATopicWithoutParserDeliversItsStoredBytes) {
  addObjectWire(sdk::BuiltinObjectType::kImage, {"raw"});
  std::vector<Sample> out;
  ASSERT_TRUE(query(0.0, 10.0, out));
  ASSERT_EQ(out.size(), 1U);
  EXPECT_EQ(out[0].bytes, (std::vector<uint8_t>{'r', 'a', 'w'}));
}

}  // namespace
}  // namespace PJ
