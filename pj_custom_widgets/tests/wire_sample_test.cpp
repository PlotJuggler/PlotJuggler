// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// readWireSample / fetchObjectPayload / mixWireSample / makeWidgetValue over the real stores.

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "dock_test_support.h"
#include "pj_custom_widgets/WireSample.h"
#include "pj_datastore/object_store.hpp"

namespace PJ {
namespace {

using pj_cw_test::kSecond;

class WireSampleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    app_ = std::make_unique<AppSession>();
    dataset_ = pj_cw_test::createDataset(*app_);
    reader_ = std::make_unique<DataReader>(app_->sessionManager().dataEngine());
  }

  WireTarget scalarTarget(const QString& key, WidgetInputKind kind) {
    const auto curve = app_->catalogModel().curveDescriptor(
        key, kind == WidgetInputKind::kNumber ? SeriesCapability::kPlottable : SeriesCapability::kDiscrete);
    EXPECT_TRUE(curve.has_value());
    WireTarget target;
    target.kind = kind;
    target.topic_id = curve->topic_id;
    target.column = curve->column_index;
    return target;
  }

  std::unique_ptr<AppSession> app_;
  std::unique_ptr<DataReader> reader_;
  DatasetId dataset_ = 0;
};

TEST_F(WireSampleTest, NumberIsTheLatestSampleAtOrBeforeTheInstant) {
  const QString key = pj_cw_test::addNumberTopic(*app_, dataset_, "/v", {{1.0, 10.0}, {2.0, 20.0}, {3.0, 30.0}});
  const WireTarget target = scalarTarget(key, WidgetInputKind::kNumber);
  const ObjectStore& objects = app_->sessionManager().objectStore();

  const WireSample before = readWireSample(*reader_, objects, target, fromRaw(kSecond / 2));
  EXPECT_EQ(before.kind, PJ_WIDGET_VALUE_NONE);

  const WireSample hold = readWireSample(*reader_, objects, target, fromRaw(5 * kSecond / 2));
  ASSERT_EQ(hold.kind, PJ_WIDGET_VALUE_NUMBER);
  EXPECT_EQ(hold.stamp, 2 * kSecond);
  EXPECT_DOUBLE_EQ(hold.number, 20.0);

  const WireSample exact = readWireSample(*reader_, objects, target, fromRaw(3 * kSecond));
  EXPECT_DOUBLE_EQ(exact.number, 30.0);
}

TEST_F(WireSampleTest, TextCarriesTheStringAndTheRowStamp) {
  const QString key = pj_cw_test::addTextTopic(*app_, dataset_, "/mode", {{1.0, "IDLE"}, {4.0, "ARMED"}});
  ASSERT_FALSE(key.isEmpty());
  const WireTarget target = scalarTarget(key, WidgetInputKind::kText);
  const ObjectStore& objects = app_->sessionManager().objectStore();

  const WireSample sample = readWireSample(*reader_, objects, target, fromRaw(5 * kSecond));
  ASSERT_EQ(sample.kind, PJ_WIDGET_VALUE_TEXT);
  EXPECT_EQ(sample.text, "ARMED");
  EXPECT_EQ(sample.stamp, 4 * kSecond);

  const WireSample early = readWireSample(*reader_, objects, target, fromRaw(2 * kSecond));
  EXPECT_EQ(early.text, "IDLE");
}

TEST_F(WireSampleTest, ObjectResolvesIdentityFirstAndPayloadOnFetch) {
  ObjectStore& objects = app_->sessionManager().objectStore();
  auto topic =
      objects.registerTopic(ObjectTopicDescriptor{.dataset_id = dataset_, .topic_name = "/cam", .metadata_json = {}});
  ASSERT_TRUE(topic.has_value());
  ASSERT_TRUE(objects.pushOwned(*topic, kSecond, {1, 2, 3}).has_value());
  ASSERT_TRUE(objects.pushOwned(*topic, 2 * kSecond, {4, 5}).has_value());
  WireTarget target;
  target.kind = WidgetInputKind::kObject;
  target.object_topic = *topic;
  target.object_type_name = "kImage";

  WireSample sample = readWireSample(*reader_, objects, target, fromRaw(3 * kSecond / 2));
  ASSERT_EQ(sample.kind, PJ_WIDGET_VALUE_OBJECT);
  EXPECT_FALSE(sample.object.has_value()) << "the identity pass must not resolve the payload";
  EXPECT_EQ(sample.stamp, kSecond);
  fetchObjectPayload(objects, target, sample);
  ASSERT_TRUE(sample.object.has_value());

  const PJ_widget_value_t value = makeWidgetValue(2, 1, target, sample, "/cam");
  EXPECT_EQ(value.kind, PJ_WIDGET_VALUE_OBJECT);
  EXPECT_EQ(value.input, 2U);
  EXPECT_EQ(value.slot, 1U);
  ASSERT_EQ(value.bytes.size, 3U);
  EXPECT_EQ(value.bytes.data[2], 3);
  EXPECT_EQ(std::string(value.type_name.data, value.type_name.size), "kImage");
  EXPECT_EQ(std::string(value.label.data, value.label.size), "/cam");
  EXPECT_DOUBLE_EQ(value.time, 1.0);
}

TEST_F(WireSampleTest, AnEntryStillShownIsNotFetchedAgain) {
  ObjectStore& objects = app_->sessionManager().objectStore();
  auto topic =
      objects.registerTopic(ObjectTopicDescriptor{.dataset_id = dataset_, .topic_name = "/cam", .metadata_json = {}});
  ASSERT_TRUE(topic.has_value());
  auto bytes = std::make_shared<std::vector<uint8_t>>(std::vector<uint8_t>{1, 2, 3});
  int fetches = 0;
  ASSERT_TRUE(objects
                  .pushLazy(
                      *topic, kSecond,
                      [bytes, &fetches]() -> sdk::PayloadView {
                        ++fetches;
                        return sdk::PayloadView{
                            Span<const uint8_t>{bytes->data(), bytes->size()},
                            sdk::BufferAnchor{bytes},
                        };
                      })
                  .has_value());
  ASSERT_TRUE(objects.pushOwned(*topic, 2 * kSecond, {4}).has_value());
  WireTarget target;
  target.kind = WidgetInputKind::kObject;
  target.object_topic = *topic;

  WireSample sample;
  refreshWireSample(*reader_, objects, target, fromRaw(kSecond), sample);
  fetchObjectPayload(objects, target, sample);
  ASSERT_TRUE(sample.object.has_value());
  EXPECT_EQ(fetches, 1);

  refreshWireSample(*reader_, objects, target, fromRaw(3 * kSecond / 2), sample);
  fetchObjectPayload(objects, target, sample);
  ASSERT_TRUE(sample.object.has_value());
  EXPECT_EQ(fetches, 1) << "same entry: the payload already held is pushed again";

  refreshWireSample(*reader_, objects, target, fromRaw(2 * kSecond), sample);
  EXPECT_FALSE(sample.object.has_value()) << "another entry: the old payload is gone";
}

TEST_F(WireSampleTest, NoneValueStillCarriesTheLabel) {
  const WireSample none;
  const PJ_widget_value_t value = makeWidgetValue(0, 0, WireTarget{}, none, "/x/y");
  EXPECT_EQ(value.kind, PJ_WIDGET_VALUE_NONE);
  EXPECT_EQ(std::string(value.label.data, value.label.size), "/x/y");
}

TEST_F(WireSampleTest, KeyChangesWithTheSampleAndWithTheOffsetOnly) {
  const QString key = pj_cw_test::addNumberTopic(*app_, dataset_, "/v", {{1.0, 10.0}, {2.0, 20.0}});
  const WireTarget target = scalarTarget(key, WidgetInputKind::kNumber);
  const ObjectStore& objects = app_->sessionManager().objectStore();
  const DisplayOffset zero;
  const auto mix = [&](double seconds, DisplayOffset offset) {
    const WireSample s =
        readWireSample(*reader_, objects, target, fromRaw(static_cast<std::int64_t>(seconds * kSecond)));
    return mixWireSample(1, s, offset);
  };
  EXPECT_EQ(mix(1.2, zero), mix(1.9, zero)) << "same held sample";
  EXPECT_NE(mix(1.2, zero), mix(2.1, zero));
  EXPECT_NE(mix(1.2, zero), mix(1.2, DisplayOffset{Duration{kSecond}}));
}

}  // namespace
}  // namespace PJ
