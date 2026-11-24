// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A source attaches a JSON metadata object to a field; the store keeps it per column.

#include "pj_base/sdk/field_metadata.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "pj_base/dataset.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/plugin_data_host.hpp"
#include "pj_datastore/topic_storage.hpp"

namespace PJ {
namespace {

struct FieldMetadataTest : ::testing::Test {
  DataEngine engine;
  std::optional<DatastoreSourceWriteHost> host;
  std::optional<sdk::SourceWriteHostView> writer;
  sdk::TopicHandle topic{};

  void SetUp() override {
    const auto td = engine.createTimeDomain("td");
    const auto ds = engine.createDataset(DatasetDescriptor{.source_name = "src", .time_domain_id = *td});
    host.emplace(engine, PJ_data_source_handle_t{static_cast<uint32_t>(*ds)});
    writer.emplace(host->raw());
    topic = *writer->ensureTopic("imu");
  }

  sdk::FieldHandle field(const char* name) {
    return *writer->ensureField(topic, name, PrimitiveType::kFloat32);
  }

  std::string metadata(sdk::FieldHandle f) {
    const auto lock = engine.lockEngine();
    return std::string(engine.getTopicStorage(topic.id)->fieldMetadata(f.id));
  }

  // {"k":{"k":...{"k":1}...}} with `depth` objects.
  static std::string nested(int depth) {
    std::string json;
    for (int i = 0; i < depth; ++i) {
      json += R"({"k":)";
    }
    json += "1";
    json.append(static_cast<std::size_t>(depth), '}');
    return json;
  }
};

TEST_F(FieldMetadataTest, StoresTheDocumentAsSent) {
  ASSERT_TRUE(writer->supportsFieldMetadata());
  const auto ax = field("ax");
  const auto ay = field("ay");
  const std::string json = sdk::FieldMetadataBuilder().description("Acceleration along x [m/s^2]").build();

  ASSERT_TRUE(writer->setFieldMetadata(ax, json).has_value());

  EXPECT_EQ(metadata(ax), json);
  EXPECT_TRUE(metadata(ay).empty());
}

TEST_F(FieldMetadataTest, EnsureFieldLeavesExistingMetadataAlone) {
  const auto ax = field("ax");
  const std::string json = sdk::FieldMetadataBuilder().description("kept").build();
  ASSERT_TRUE(writer->setFieldMetadata(ax, json).has_value());

  (void)field("ax");

  EXPECT_EQ(metadata(ax), json);
}

TEST_F(FieldMetadataTest, SetReplacesWholeDocument) {
  const auto ax = field("ax");
  ASSERT_TRUE(writer->setFieldMetadata(ax, R"({"description":"first","unit":"m"})").has_value());

  ASSERT_TRUE(writer->setFieldMetadata(ax, R"({"description":"second"})").has_value());

  EXPECT_EQ(metadata(ax), R"({"description":"second"})");
}

TEST_F(FieldMetadataTest, EmptyTextAndEmptyObjectClear) {
  const auto ax = field("ax");

  ASSERT_TRUE(writer->setFieldMetadata(ax, R"({"description":"x"})").has_value());
  ASSERT_TRUE(writer->setFieldMetadata(ax, "").has_value());
  EXPECT_TRUE(metadata(ax).empty());

  ASSERT_TRUE(writer->setFieldMetadata(ax, R"({"description":"x"})").has_value());
  ASSERT_TRUE(writer->setFieldMetadata(ax, "{}").has_value());
  EXPECT_TRUE(metadata(ax).empty());

  ASSERT_TRUE(writer->setFieldMetadata(ax, R"({"description":"x"})").has_value());
  ASSERT_TRUE(writer->setFieldMetadata(ax, sdk::FieldMetadataBuilder().build()).has_value());
  EXPECT_TRUE(metadata(ax).empty());
}

TEST_F(FieldMetadataTest, KeepsKeysTheHostDoesNotKnow) {
  const auto ax = field("ax");
  const std::string json = R"({"vendor.range":[0,10],"description":"x"})";

  ASSERT_TRUE(writer->setFieldMetadata(ax, json).has_value());
  EXPECT_EQ(metadata(ax), json);
}

TEST_F(FieldMetadataTest, RejectsDocumentsThatAreNotJsonObjects) {
  const auto ax = field("ax");
  const std::string kept = sdk::FieldMetadataBuilder().description("kept").build();
  ASSERT_TRUE(writer->setFieldMetadata(ax, kept).has_value());

  EXPECT_FALSE(writer->setFieldMetadata(ax, "not json").has_value());
  EXPECT_FALSE(writer->setFieldMetadata(ax, R"({"description":)").has_value());
  EXPECT_FALSE(writer->setFieldMetadata(ax, R"({"a":1} trailing)").has_value());
  EXPECT_FALSE(writer->setFieldMetadata(ax, R"(["description"])").has_value());
  EXPECT_FALSE(writer->setFieldMetadata(ax, R"("description")").has_value());
  EXPECT_FALSE(writer->setFieldMetadata(ax, "42").has_value());
  EXPECT_EQ(metadata(ax), kept);
}

TEST_F(FieldMetadataTest, RejectsOversizedDocument) {
  const auto ax = field("ax");
  const std::string huge = sdk::FieldMetadataBuilder().description(std::string(70 * 1024, 'a')).build();

  EXPECT_FALSE(writer->setFieldMetadata(ax, huge).has_value());
  EXPECT_TRUE(metadata(ax).empty());
}

TEST_F(FieldMetadataTest, BoundsNestingDepth) {
  const auto ax = field("ax");

  // Root object = depth 1, innermost value one deeper.
  EXPECT_TRUE(writer->setFieldMetadata(ax, nested(31)).has_value());
  EXPECT_FALSE(writer->setFieldMetadata(ax, nested(32)).has_value());
  EXPECT_FALSE(writer->setFieldMetadata(ax, nested(5000)).has_value());
}

TEST_F(FieldMetadataTest, BoundsNodeCount) {
  const auto ax = field("ax");
  const auto array_of = [](int count) {
    std::string json = R"({"v":[)";
    for (int i = 0; i < count; ++i) {
      json += i == 0 ? "1" : ",1";
    }
    return json + "]}";
  };

  // The root object and the array are two values, and each element is one more.
  EXPECT_TRUE(writer->setFieldMetadata(ax, array_of(16382)).has_value());
  EXPECT_FALSE(writer->setFieldMetadata(ax, array_of(16383)).has_value());
}

TEST_F(FieldMetadataTest, UnknownFieldIsRejected) {
  EXPECT_FALSE(writer->setFieldMetadata(sdk::FieldHandle{.topic = topic, .id = 99}, "{}").has_value());
}

TEST_F(FieldMetadataTest, SurvivesClearingTheDatasetChunks) {
  const auto ax = field("ax");
  const std::string json = sdk::FieldMetadataBuilder().description("kept").build();
  ASSERT_TRUE(writer->setFieldMetadata(ax, json).has_value());
  {
    const auto lock = engine.lockEngine();
    engine.getTopicStorage(topic.id)->clearChunks();
  }
  EXPECT_EQ(metadata(ax), json);
}

}  // namespace
}  // namespace PJ
