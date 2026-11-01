// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// WidgetRuntimeHost: the history slot of pj.widget_host.v1 over the real DataEngine and ObjectStore,
// driven through the C vtable a plugin would hold.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_plugins/sdk/widget_plugin_base.hpp"
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
                                  std::string_view message) { messages_.emplace_back(level, std::string(message)); }});
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
  wire.object_type_name = "kImage";
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

}  // namespace
}  // namespace PJ
