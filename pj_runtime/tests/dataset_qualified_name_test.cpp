// SPDX-License-Identifier: MPL-2.0
//
// The dataset-qualified name contract: "label:topic/field", the dataset label
// (DataEngine::datasetLabel) the catalog shows, accepted back as an input key. Split
// against KNOWN labels (never parsed blindly at ':'), and the marker dataset
// binding built on top of it: qualified keys decide and are normalized to bare
// form; an unqualified key duplicated across datasets is refused with the
// qualified candidates instead of landing on the first-loaded dataset.
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "pj_base/builtin/plot_markers.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_runtime/DatasetQualifiedName.h"

namespace {

using PJ::CatalogItem;
using PJ::DatasetId;
using PJ::resolveMarkerDataset;
using PJ::splitDatasetQualifier;

CatalogItem scalarItem(DatasetId ds, const char* topic, const char* field) {
  CatalogItem item;
  item.dataset_id = ds;
  item.topic_name = QString::fromUtf8(topic);
  PJ::ScalarFieldPayload payload;
  payload.field_name = QString::fromUtf8(field);
  payload.field_path = QString::fromUtf8(field);
  payload.logical_type = PJ::PrimitiveType::kFloat64;
  item.payload = payload;
  return item;
}

// An engine holding one dataset per source, ids 1, 2, ... in order.
PJ::DataEngine engineWith(const std::vector<std::string>& sources) {
  PJ::DataEngine engine;
  for (const std::string& source : sources) {
    EXPECT_TRUE(engine.createDataset(PJ::DatasetDescriptor{.source_name = source}).has_value());
  }
  return engine;
}

TEST(SplitDatasetQualifier, MatchesOnlyKnownSourceNames) {
  const std::vector<std::string> sources = {"run_a.mcap", "run_b.mcap"};
  const auto hit = splitDatasetQualifier("run_a.mcap:/speed/value", sources);
  EXPECT_TRUE(hit.qualified);
  EXPECT_EQ(hit.dataset_source, "run_a.mcap");
  EXPECT_EQ(hit.bare, "/speed/value");

  // A ':' whose prefix is no loaded source is part of the name, not a qualifier.
  const std::vector<std::string> other = {"other.mcap"};
  const auto miss = splitDatasetQualifier("run_a.mcap:/speed/value", other);
  EXPECT_FALSE(miss.qualified);
  EXPECT_EQ(miss.bare, "run_a.mcap:/speed/value");
}

TEST(SplitDatasetQualifier, StreamStyleNamesNeedNoEscaping) {
  const std::vector<std::string> sources = {"[stream] UDP Server"};
  const auto split = splitDatasetQualifier("[stream] UDP Server:/udp/data/value", sources);
  EXPECT_TRUE(split.qualified);
  EXPECT_EQ(split.bare, "/udp/data/value");
}

TEST(SplitDatasetQualifier, LongestKnownNameWins) {
  const std::vector<std::string> sources = {"a", "a:b"};
  const auto split = splitDatasetQualifier("a:b:/t/f", sources);
  EXPECT_TRUE(split.qualified);
  EXPECT_EQ(split.dataset_source, "a:b");
  EXPECT_EQ(split.bare, "/t/f");
}

TEST(ResolveMarkerDataset, QualifiedKeyPicksItsDatasetAndIsStripped) {
  const std::string key = PJ::sdk::markerSeriesKey("/speed", "value");
  const std::vector<CatalogItem> items = {scalarItem(1, "/speed", "value"), scalarItem(2, "/speed", "value")};
  std::vector<std::string> inputs = {"b:" + key};
  std::vector<std::string> outputs = {"b:" + key};

  const auto ds = resolveMarkerDataset(inputs, outputs, items, engineWith({"a", "b"}));
  ASSERT_TRUE(ds.has_value()) << ds.error();
  EXPECT_EQ(*ds, 2u);
  // The marker engine and markerSeriesKey never see the qualifier.
  EXPECT_EQ(inputs.front(), key);
  EXPECT_EQ(outputs.front(), key);
}

TEST(ResolveMarkerDataset, UnqualifiedUniqueKeyResolves) {
  const std::string key = PJ::sdk::markerSeriesKey("/speed", "value");
  const std::vector<CatalogItem> items = {scalarItem(2, "/speed", "value")};
  std::vector<std::string> inputs = {key};
  std::vector<std::string> outputs = {key};

  const auto ds = resolveMarkerDataset(inputs, outputs, items, engineWith({"a", "b"}));
  ASSERT_TRUE(ds.has_value()) << ds.error();
  EXPECT_EQ(*ds, 2u);
}

// A fallback to the first-loaded dataset would strand the generator on the wrong data; the duplicated bare key is
// refused.
TEST(ResolveMarkerDataset, DuplicatedBareKeyIsRefusedWithQualifiedCandidates) {
  const std::string key = PJ::sdk::markerSeriesKey("/speed", "value");
  const std::vector<CatalogItem> items = {scalarItem(1, "/speed", "value"), scalarItem(2, "/speed", "value")};
  std::vector<std::string> inputs = {key};
  std::vector<std::string> outputs = {key};

  const auto ds = resolveMarkerDataset(inputs, outputs, items, engineWith({"a", "b"}));
  ASSERT_FALSE(ds.has_value());
  EXPECT_NE(ds.error().find("'a:" + key + "'"), std::string::npos) << ds.error();
  EXPECT_NE(ds.error().find("'b:" + key + "'"), std::string::npos) << ds.error();
}

TEST(ResolveMarkerDataset, KeysSpanningTwoDatasetsAreRefused) {
  const std::string key1 = PJ::sdk::markerSeriesKey("/speed", "value");
  const std::string key2 = PJ::sdk::markerSeriesKey("/imu", "value");
  const std::vector<CatalogItem> items = {scalarItem(1, "/speed", "value"), scalarItem(2, "/imu", "value")};
  std::vector<std::string> inputs = {"a:" + key1, "b:" + key2};
  std::vector<std::string> outputs = {};

  const auto ds = resolveMarkerDataset(inputs, outputs, items, engineWith({"a", "b"}));
  ASSERT_FALSE(ds.has_value());
  EXPECT_NE(ds.error().find("share one dataset"), std::string::npos) << ds.error();
}

// Two files named data.csv (or two streams of one plugin): each copy is addressable by its label, and the newer copy
// keeps "data.csv (2)" once the older is removed; the stale "data.csv" then addresses nothing.
TEST(ResolveMarkerDataset, LabelOfADuplicatedSourceOutlivesTheOlderCopy) {
  const std::string key = PJ::sdk::markerSeriesKey("/speed", "value");
  PJ::DataEngine engine = engineWith({"data.csv", "data.csv"});
  engine.removeDataset(1);
  const std::vector<CatalogItem> items = {scalarItem(2, "/speed", "value")};
  std::vector<std::string> outputs;

  std::vector<std::string> newer = {"data.csv (2):" + key};
  const auto ds = resolveMarkerDataset(newer, outputs, items, engine);
  ASSERT_TRUE(ds.has_value()) << ds.error();
  EXPECT_EQ(*ds, 2u);

  std::vector<std::string> stale = {"data.csv:" + key};
  EXPECT_FALSE(resolveMarkerDataset(stale, outputs, items, engine).has_value()) << "a stale label never retargets";
}

TEST(ResolveMarkerDataset, CatalogLabelOfADuplicatedSourceIsAccepted) {
  const std::string key = PJ::sdk::markerSeriesKey("/speed", "value");
  const std::vector<CatalogItem> items = {scalarItem(1, "/speed", "value"), scalarItem(2, "/speed", "value")};
  std::vector<std::string> inputs = {"same (2):" + key};
  std::vector<std::string> outputs = {};

  const auto ds = resolveMarkerDataset(inputs, outputs, items, engineWith({"same", "same"}));
  ASSERT_TRUE(ds.has_value()) << ds.error();
  EXPECT_EQ(*ds, 2u);
  EXPECT_EQ(inputs.front(), key);

  std::vector<std::string> older = {"same:" + key};
  const auto first = resolveMarkerDataset(older, outputs, items, engineWith({"same", "same"}));
  ASSERT_TRUE(first.has_value()) << first.error();
  EXPECT_EQ(*first, 1u);

  std::vector<std::string> bare = {key};
  const auto refused = resolveMarkerDataset(bare, outputs, items, engineWith({"same", "same"}));
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().find("'same:" + key + "'"), std::string::npos) << refused.error();
  EXPECT_NE(refused.error().find("'same (2):" + key + "'"), std::string::npos) << refused.error();
}

TEST(ResolveMarkerDataset, NoMatchingInputIsAnError) {
  std::vector<std::string> inputs = {"/nope/value"};
  std::vector<std::string> outputs = {};
  const auto ds = resolveMarkerDataset(inputs, outputs, {}, engineWith({"a"}));
  EXPECT_FALSE(ds.has_value());
}

TEST(ResolveMarkerDataset, UnmatchedPrefixRequiresTheWholeBareName) {
  const PJ::DataEngine names = engineWith({"loaded"});
  std::vector<std::string> inputs = {"missing:/speed/value"};
  std::vector<std::string> outputs;
  EXPECT_FALSE(resolveMarkerDataset(inputs, outputs, {scalarItem(1, "/speed", "value")}, names));
  EXPECT_EQ(inputs.front(), "missing:/speed/value");

  const auto result = resolveMarkerDataset(inputs, outputs, {scalarItem(1, "missing:/speed", "value")}, names);
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, 1u);
  EXPECT_EQ(inputs.front(), "missing:/speed/value");
}

TEST(ResolveMarkerDataset, LongestSourcePrefixDecidesEvenWhenTheShorterFormExists) {
  std::vector<std::string> inputs = {"a:b:/t/value"};
  std::vector<std::string> outputs;
  const std::vector<CatalogItem> items = {scalarItem(1, "b:/t", "value"), scalarItem(2, "/t", "value")};
  const auto result = resolveMarkerDataset(inputs, outputs, items, engineWith({"a", "a:b"}));
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, 2u);
  EXPECT_EQ(inputs.front(), "/t/value");
}

// A recognized qualifier must not return as soon as it selects a dataset: "a:/missing/value" and the empty bare "a:"
// need a series check too.
TEST(ResolveMarkerDataset, QualifiedInputMustExistInItsDataset) {
  const PJ::DataEngine names = engineWith({"a"});
  const std::vector<CatalogItem> items = {scalarItem(1, "/speed", "value")};
  std::vector<std::string> outputs;

  std::vector<std::string> missing = {"a:/missing/value"};
  EXPECT_FALSE(resolveMarkerDataset(missing, outputs, items, names).has_value());

  std::vector<std::string> empty = {"a:"};
  EXPECT_FALSE(resolveMarkerDataset(empty, outputs, items, names).has_value());
}

}  // namespace
