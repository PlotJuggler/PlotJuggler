// SPDX-License-Identifier: MPL-2.0
//
// The dataset-qualified name contract: "dataset_source:topic/field" — the form
// SeriesPath::display() prints — accepted back as an input key. Split against
// KNOWN source names (never parsed blindly at ':'), and the marker dataset
// binding built on top of it: qualified keys decide and are normalized to bare
// form; an unqualified key duplicated across datasets is refused with the
// qualified candidates instead of landing on the first-loaded dataset.
#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/builtin/plot_markers.hpp"
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

std::function<std::optional<std::string>(DatasetId)> namesOf(std::map<DatasetId, std::string> names) {
  return [names = std::move(names)](DatasetId id) -> std::optional<std::string> {
    const auto it = names.find(id);
    return it == names.end() ? std::nullopt : std::optional<std::string>(it->second);
  };
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

  const auto ds = resolveMarkerDataset(inputs, outputs, items, {1, 2}, namesOf({{1, "a"}, {2, "b"}}));
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

  const auto ds = resolveMarkerDataset(inputs, outputs, items, {1, 2}, namesOf({{1, "a"}, {2, "b"}}));
  ASSERT_TRUE(ds.has_value()) << ds.error();
  EXPECT_EQ(*ds, 2u);
}

// The failure this whole contract exists to stop: the old behaviour fell back
// to the first-loaded dataset, stranding the generator on the wrong data.
TEST(ResolveMarkerDataset, DuplicatedBareKeyIsRefusedWithQualifiedCandidates) {
  const std::string key = PJ::sdk::markerSeriesKey("/speed", "value");
  const std::vector<CatalogItem> items = {scalarItem(1, "/speed", "value"), scalarItem(2, "/speed", "value")};
  std::vector<std::string> inputs = {key};
  std::vector<std::string> outputs = {key};

  const auto ds = resolveMarkerDataset(inputs, outputs, items, {1, 2}, namesOf({{1, "a"}, {2, "b"}}));
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

  const auto ds = resolveMarkerDataset(inputs, outputs, items, {1, 2}, namesOf({{1, "a"}, {2, "b"}}));
  ASSERT_FALSE(ds.has_value());
  EXPECT_NE(ds.error().find("share one dataset"), std::string::npos) << ds.error();
}

TEST(ResolveMarkerDataset, DuplicatedSourceNameIsRefused) {
  const std::string key = PJ::sdk::markerSeriesKey("/speed", "value");
  const std::vector<CatalogItem> items = {scalarItem(1, "/speed", "value"), scalarItem(2, "/speed", "value")};
  std::vector<std::string> inputs = {"same:" + key};
  std::vector<std::string> outputs = {};

  const auto ds = resolveMarkerDataset(inputs, outputs, items, {1, 2}, namesOf({{1, "same"}, {2, "same"}}));
  ASSERT_FALSE(ds.has_value());
  EXPECT_NE(ds.error().find("ambiguous"), std::string::npos) << ds.error();
}

TEST(ResolveMarkerDataset, NoMatchingInputIsAnError) {
  std::vector<std::string> inputs = {"/nope/value"};
  std::vector<std::string> outputs = {};
  const auto ds = resolveMarkerDataset(inputs, outputs, {}, {1}, namesOf({{1, "a"}}));
  EXPECT_FALSE(ds.has_value());
}

TEST(ResolveMarkerDataset, UnmatchedPrefixRequiresTheWholeBareName) {
  const auto names = namesOf({{1, "loaded"}});
  std::vector<std::string> inputs = {"missing:/speed/value"};
  std::vector<std::string> outputs;
  EXPECT_FALSE(resolveMarkerDataset(inputs, outputs, {scalarItem(1, "/speed", "value")}, {1}, names));
  EXPECT_EQ(inputs.front(), "missing:/speed/value");

  const auto result = resolveMarkerDataset(inputs, outputs, {scalarItem(1, "missing:/speed", "value")}, {1}, names);
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, 1u);
  EXPECT_EQ(inputs.front(), "missing:/speed/value");
}

TEST(ResolveMarkerDataset, LongestSourcePrefixDecidesEvenWhenTheShorterFormExists) {
  std::vector<std::string> inputs = {"a:b:/t/value"};
  std::vector<std::string> outputs;
  const std::vector<CatalogItem> items = {scalarItem(1, "b:/t", "value"), scalarItem(2, "/t", "value")};
  const auto result = resolveMarkerDataset(inputs, outputs, items, {1, 2}, namesOf({{1, "a"}, {2, "a:b"}}));
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, 2u);
  EXPECT_EQ(inputs.front(), "/t/value");
}

// Bug (PR #619 #6): a recognized qualifier returns as soon as it selects a dataset,
// so "a:/missing/value" and the empty bare "a:" pass without any series check.
TEST(ResolveMarkerDataset, QualifiedInputMustExistInItsDataset) {
  const auto names = namesOf({{1, "a"}});
  const std::vector<CatalogItem> items = {scalarItem(1, "/speed", "value")};
  std::vector<std::string> outputs;

  std::vector<std::string> missing = {"a:/missing/value"};
  EXPECT_FALSE(resolveMarkerDataset(missing, outputs, items, {1}, names).has_value());

  std::vector<std::string> empty = {"a:"};
  EXPECT_FALSE(resolveMarkerDataset(empty, outputs, items, {1}, names).has_value());
}

}  // namespace
