// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A source attaches a JSON metadata object to a field; the store keeps it per column.

#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "pj_base/dataset.hpp"
#include "pj_base/sdk/metadata_document.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/plugin_data_host.hpp"
#include "pj_datastore/topic_storage.hpp"

namespace PJ {
namespace {

struct FieldMetadataTest : ::testing::Test {
  DataEngine engine;
  // The streaming pause/resume staging engine; empty until mirrorToStaging().
  DataEngine staging;
  TimeDomainId time_domain_id = 0;
  DatasetId dataset_id = 0;
  std::optional<DatastoreSourceWriteHost> host;
  std::optional<sdk::SourceWriteHostView> writer;
  sdk::TopicHandle topic{};

  void SetUp() override {
    const auto td = engine.createTimeDomain("td");
    const auto ds = engine.createDataset(DatasetDescriptor{.source_name = "src", .time_domain_id = *td});
    time_domain_id = *td;
    dataset_id = *ds;
    host.emplace(engine, PJ_data_source_handle_t{static_cast<uint32_t>(*ds)});
    writer.emplace(host->raw());
    topic = *writer->ensureTopic("imu");
  }

  // As StreamingSourceManager does: the staging engine gets the same time domain and dataset ids, then the host
  // mirrors every topic and field it creates from here on.
  void mirrorToStaging() {
    const auto mirrored_td = staging.createTimeDomain("td", time_domain_id);
    ASSERT_TRUE(mirrored_td.has_value());
    ASSERT_EQ(*mirrored_td, time_domain_id);
    ASSERT_TRUE(
        staging.createDataset(DatasetDescriptor{.source_name = "src", .time_domain_id = time_domain_id}, dataset_id)
            .has_value());
    host->setSecondaryEngine(&staging);
  }

  sdk::FieldHandle field(const char* name) {
    return *writer->ensureField(topic, name, PrimitiveType::kFloat32);
  }

  static std::string metadataIn(const DataEngine& e, sdk::FieldHandle f) {
    const auto lock = e.lockEngine();
    const TopicStorage* storage = e.getTopicStorage(f.topic.id);
    return storage != nullptr ? std::string(storage->fieldMetadata(f.id)) : std::string();
  }

  static std::string topicMetadataIn(const DataEngine& e, sdk::TopicHandle t) {
    const auto lock = e.lockEngine();
    const TopicStorage* storage = e.getTopicStorage(t.id);
    return storage != nullptr ? std::string(storage->topicMetadata()) : std::string();
  }

  std::string metadata(sdk::FieldHandle f) {
    return metadataIn(engine, f);
  }

  std::string topicMetadata(sdk::TopicHandle t) {
    return topicMetadataIn(engine, t);
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
  const std::string json = sdk::MetadataDocumentBuilder().description("Acceleration along x [m/s^2]").build();

  ASSERT_TRUE(writer->setFieldMetadata(ax, json).has_value());

  EXPECT_EQ(metadata(ax), json);
  EXPECT_TRUE(metadata(ay).empty());
}

TEST_F(FieldMetadataTest, EnsureFieldLeavesExistingMetadataAlone) {
  const auto ax = field("ax");
  const std::string json = sdk::MetadataDocumentBuilder().description("kept").build();
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
  ASSERT_TRUE(writer->setFieldMetadata(ax, sdk::MetadataDocumentBuilder().build()).has_value());
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
  const std::string kept = sdk::MetadataDocumentBuilder().description("kept").build();
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
  const std::string huge = sdk::MetadataDocumentBuilder().description(std::string(70 * 1024, 'a')).build();

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
  const std::string json = sdk::MetadataDocumentBuilder().description("kept").build();
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
  const std::string json = sdk::MetadataDocumentBuilder().description("Inertial unit").string("frame", "base").build();

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
  const std::string json = sdk::MetadataDocumentBuilder().description("kept").build();
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

  const std::string json = sdk::MetadataDocumentBuilder().description("parsed ax").build();
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

  const std::string field_json = sdk::MetadataDocumentBuilder().description("filtered x").build();
  const std::string topic_json = sdk::MetadataDocumentBuilder().description("filter output").build();
  ASSERT_TRUE(toolbox.setFieldMetadata(derived_field, field_json).has_value());
  ASSERT_TRUE(toolbox.setTopicMetadata(derived_topic, topic_json).has_value());
  EXPECT_EQ(metadata(derived_field), field_json);
  EXPECT_EQ(topicMetadata(derived_topic), topic_json);

  EXPECT_FALSE(toolbox.setFieldMetadata(derived_field, "not json").has_value());
  EXPECT_FALSE(toolbox.setTopicMetadata(sdk::TopicHandle{.id = 9999}, "{}").has_value());
  EXPECT_EQ(metadata(derived_field), field_json);
}

// --- streaming pause/resume: the write core writes the live engine and its staging mirror -------------------------

TEST_F(FieldMetadataTest, WritesTheStagingMirrorToo) {
  mirrorToStaging();
  ASSERT_EQ(writer->ensureTopic("imu")->id, topic.id);  // a cache hit mirrors the topic
  const auto ax = field("ax");
  const std::string field_json = sdk::MetadataDocumentBuilder().description("ax").build();
  const std::string topic_json = sdk::MetadataDocumentBuilder().description("imu").build();

  ASSERT_TRUE(writer->setFieldMetadata(ax, field_json).has_value());
  ASSERT_TRUE(writer->setTopicMetadata(topic, topic_json).has_value());

  EXPECT_EQ(metadataIn(engine, ax), field_json);
  EXPECT_EQ(metadataIn(staging, ax), field_json);
  EXPECT_EQ(topicMetadataIn(engine, topic), topic_json);
  EXPECT_EQ(topicMetadataIn(staging, topic), topic_json);

  ASSERT_TRUE(writer->setFieldMetadata(ax, "").has_value());
  EXPECT_TRUE(metadataIn(engine, ax).empty());
  EXPECT_TRUE(metadataIn(staging, ax).empty());
}

TEST_F(FieldMetadataTest, WriteDuringPauseReachesBothEngines) {
  mirrorToStaging();
  ASSERT_EQ(writer->ensureTopic("imu")->id, topic.id);
  const auto ax = field("ax");
  ASSERT_TRUE(writer->setFieldMetadata(ax, R"({"description":"before pause"})").has_value());

  host->setTarget(&staging);  // pause: writes land in staging, the live engine becomes the mirror
  ASSERT_TRUE(writer->setFieldMetadata(ax, R"({"description":"during pause"})").has_value());
  ASSERT_TRUE(writer->setTopicMetadata(topic, R"({"description":"imu"})").has_value());
  EXPECT_EQ(metadataIn(engine, ax), R"({"description":"during pause"})");
  EXPECT_EQ(metadataIn(staging, ax), R"({"description":"during pause"})");
  EXPECT_EQ(topicMetadataIn(engine, topic), R"({"description":"imu"})");

  host->setTarget(&engine);  // resume
  EXPECT_EQ(metadataIn(engine, ax), R"({"description":"during pause"})");
  EXPECT_EQ(topicMetadataIn(engine, topic), R"({"description":"imu"})");
}

TEST_F(FieldMetadataTest, MirrorWithoutTheFieldIsSkipped) {
  const auto ax = field("ax");  // created before the mirror exists
  mirrorToStaging();

  ASSERT_TRUE(writer->setFieldMetadata(ax, R"({"description":"ax"})").has_value());
  ASSERT_TRUE(writer->setTopicMetadata(topic, R"({"description":"imu"})").has_value());

  EXPECT_EQ(metadata(ax), R"({"description":"ax"})");
  EXPECT_EQ(topicMetadata(topic), R"({"description":"imu"})");
  const auto lock = staging.lockEngine();
  EXPECT_EQ(staging.getTopicStorage(topic.id), nullptr);
}

// --- reload: replaceDatasetFrom() takes the staged layout's metadata ------------------------------------------------

struct ReloadedSource {
  DataEngine staged;
  DatasetId dataset_id = 0;
  std::optional<DatastoreSourceWriteHost> host;
  std::optional<sdk::SourceWriteHostView> writer;
  sdk::TopicHandle topic{};

  ReloadedSource() {
    dataset_id = *staged.createDataset(DatasetDescriptor{.source_name = "src"});
    host.emplace(staged, PJ_data_source_handle_t{static_cast<uint32_t>(dataset_id)});
    writer.emplace(host->raw());
    topic = *writer->ensureTopic("imu");
  }
};

TEST_F(FieldMetadataTest, ReloadTakesTheStagedMetadata) {
  const auto ax = field("ax");
  ASSERT_TRUE(writer->setFieldMetadata(ax, R"({"description":"old"})").has_value());
  ASSERT_TRUE(writer->setTopicMetadata(topic, R"({"description":"old topic"})").has_value());

  ReloadedSource reload;
  const auto staged_ax = *reload.writer->ensureField(reload.topic, "ax", PrimitiveType::kFloat32);
  ASSERT_TRUE(reload.writer->setFieldMetadata(staged_ax, R"({"description":"new"})").has_value());
  ASSERT_TRUE(reload.writer->setTopicMetadata(reload.topic, R"({"description":"new topic"})").has_value());

  ASSERT_TRUE(engine.replaceDatasetFrom(reload.staged, reload.dataset_id, dataset_id).has_value());

  // The topic keeps its primary id; the field ids are the staged layout's.
  EXPECT_EQ(metadata(sdk::FieldHandle{.topic = topic, .id = staged_ax.id}), R"({"description":"new"})");
  EXPECT_EQ(topicMetadata(topic), R"({"description":"new topic"})");
}

TEST_F(FieldMetadataTest, ReloadWithoutMetadataClearsIt) {
  const auto ax = field("ax");
  ASSERT_TRUE(writer->setFieldMetadata(ax, R"({"description":"old"})").has_value());
  ASSERT_TRUE(writer->setTopicMetadata(topic, R"({"description":"old topic"})").has_value());

  ReloadedSource reload;  // the reloaded source sets no metadata
  const auto staged_ax = *reload.writer->ensureField(reload.topic, "ax", PrimitiveType::kFloat32);

  ASSERT_TRUE(engine.replaceDatasetFrom(reload.staged, reload.dataset_id, dataset_id).has_value());

  EXPECT_TRUE(metadata(sdk::FieldHandle{.topic = topic, .id = staged_ax.id}).empty());
  EXPECT_TRUE(topicMetadata(topic).empty());
}

}  // namespace
}  // namespace PJ
