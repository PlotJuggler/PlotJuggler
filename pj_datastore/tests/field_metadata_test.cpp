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
#include "pj_datastore/object_store.hpp"
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
    return std::string(engine.getTopicStorage(f.topic.id)->fieldMetadata(f.id));
  }

  std::string topicMetadata(sdk::TopicHandle t) {
    const auto lock = engine.lockEngine();
    return std::string(engine.getTopicStorage(t.id)->topicMetadata());
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

// --- topic metadata: same document rules, stored on the topic ----------------

TEST_F(FieldMetadataTest, TopicMetadataStoresTheDocumentAsSent) {
  ASSERT_TRUE(writer->supportsTopicMetadata());
  const auto other = *writer->ensureTopic("gps");
  const std::string json = sdk::FieldMetadataBuilder().description("Inertial unit").string("frame", "base").build();

  ASSERT_TRUE(writer->setTopicMetadata(topic, json).has_value());

  EXPECT_EQ(topicMetadata(topic), json);
  EXPECT_TRUE(topicMetadata(other).empty());
}

TEST_F(FieldMetadataTest, TopicMetadataIsIndependentOfFieldMetadata) {
  const auto ax = field("ax");
  ASSERT_TRUE(writer->setFieldMetadata(ax, R"({"description":"field"})").has_value());
  ASSERT_TRUE(writer->setTopicMetadata(topic, R"({"description":"topic"})").has_value());

  EXPECT_EQ(metadata(ax), R"({"description":"field"})");
  EXPECT_EQ(topicMetadata(topic), R"({"description":"topic"})");
}

TEST_F(FieldMetadataTest, TopicMetadataReplacesAndClears) {
  ASSERT_TRUE(writer->setTopicMetadata(topic, R"({"description":"first","unit":"m"})").has_value());
  ASSERT_TRUE(writer->setTopicMetadata(topic, R"({"description":"second"})").has_value());
  EXPECT_EQ(topicMetadata(topic), R"({"description":"second"})");

  ASSERT_TRUE(writer->setTopicMetadata(topic, "").has_value());
  EXPECT_TRUE(topicMetadata(topic).empty());

  ASSERT_TRUE(writer->setTopicMetadata(topic, R"({"description":"x"})").has_value());
  ASSERT_TRUE(writer->setTopicMetadata(topic, "{}").has_value());
  EXPECT_TRUE(topicMetadata(topic).empty());
}

TEST_F(FieldMetadataTest, TopicMetadataRejectsBadDocumentsAndKeepsThePrevious) {
  const std::string kept = R"({"description":"kept"})";
  ASSERT_TRUE(writer->setTopicMetadata(topic, kept).has_value());

  EXPECT_FALSE(writer->setTopicMetadata(topic, "not json").has_value());
  EXPECT_FALSE(writer->setTopicMetadata(topic, R"(["description"])").has_value());
  EXPECT_FALSE(writer->setTopicMetadata(topic, nested(32)).has_value());
  EXPECT_FALSE(writer->setTopicMetadata(topic, std::string(2 * 1024 * 1024, ' ')).has_value());

  EXPECT_EQ(topicMetadata(topic), kept);
}

TEST_F(FieldMetadataTest, UnknownTopicIsRejected) {
  EXPECT_FALSE(writer->setTopicMetadata(sdk::TopicHandle{.id = 9999}, "{}").has_value());
}

TEST_F(FieldMetadataTest, TopicMetadataSurvivesClearingTheTopicChunks) {
  const std::string json = sdk::FieldMetadataBuilder().description("kept").build();
  ASSERT_TRUE(writer->setTopicMetadata(topic, json).has_value());
  {
    const auto lock = engine.lockEngine();
    engine.getTopicStorage(topic.id)->clearChunks();
  }
  EXPECT_EQ(topicMetadata(topic), json);
}

TEST_F(FieldMetadataTest, ParserHostStoresFieldMetadataOnItsBoundTopic) {
  const auto ax = field("ax");
  DatastoreParserWriteHost parser_impl(engine, PJ_topic_handle_t{topic.id});
  sdk::ParserWriteHostView parser(parser_impl.raw());
  ASSERT_TRUE(parser.supportsFieldMetadata());

  const std::string json = sdk::FieldMetadataBuilder().description("parsed ax").build();
  ASSERT_TRUE(parser.setFieldMetadata(ax, json).has_value());
  EXPECT_EQ(metadata(ax), json);

  ASSERT_TRUE(parser.setFieldMetadata(ax, "{}").has_value());
  EXPECT_TRUE(metadata(ax).empty());
}

TEST_F(FieldMetadataTest, ParserHostRejectsBadDocumentsAndForeignTopics) {
  const auto ax = field("ax");
  const auto other_topic = *writer->ensureTopic("gps");
  const auto other_field = *writer->ensureField(other_topic, "lat", PrimitiveType::kFloat64);
  DatastoreParserWriteHost parser_impl(engine, PJ_topic_handle_t{topic.id});
  sdk::ParserWriteHostView parser(parser_impl.raw());

  const std::string kept = R"({"description":"kept"})";
  ASSERT_TRUE(parser.setFieldMetadata(ax, kept).has_value());
  EXPECT_FALSE(parser.setFieldMetadata(ax, "not json").has_value());
  EXPECT_EQ(metadata(ax), kept);

  EXPECT_FALSE(parser.setFieldMetadata(other_field, kept).has_value());
  EXPECT_TRUE(metadata(other_field).empty());
}

TEST_F(FieldMetadataTest, ToolboxHostStoresFieldAndTopicMetadata) {
  ObjectStore object_store;
  DatastoreToolboxHost toolbox_impl(engine, object_store);
  sdk::ToolboxHostView toolbox(toolbox_impl.raw());
  ASSERT_TRUE(toolbox.supportsFieldMetadata());
  ASSERT_TRUE(toolbox.supportsTopicMetadata());

  const auto source = *toolbox.createDataSource("derived");
  const auto derived_topic = *toolbox.ensureTopic(source, "filtered");
  const auto derived_field = *toolbox.ensureField(derived_topic, "x", PrimitiveType::kFloat64);

  const std::string field_json = sdk::FieldMetadataBuilder().description("filtered x").build();
  const std::string topic_json = sdk::FieldMetadataBuilder().description("filter output").build();
  ASSERT_TRUE(toolbox.setFieldMetadata(derived_field, field_json).has_value());
  ASSERT_TRUE(toolbox.setTopicMetadata(derived_topic, topic_json).has_value());
  EXPECT_EQ(metadata(derived_field), field_json);
  EXPECT_EQ(topicMetadata(derived_topic), topic_json);

  EXPECT_FALSE(toolbox.setFieldMetadata(derived_field, "not json").has_value());
  EXPECT_FALSE(toolbox.setTopicMetadata(sdk::TopicHandle{.id = 9999}, "{}").has_value());
  EXPECT_EQ(metadata(derived_field), field_json);
}

}  // namespace
}  // namespace PJ
