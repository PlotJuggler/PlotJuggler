// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/marker_topic_metadata.hpp"

#include <array>
#include <nlohmann/json.hpp>
#include <utility>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/plot_markers.hpp"
#include "pj_base/sdk/object_topic_metadata.hpp"

namespace PJ {
namespace {

constexpr std::array<std::pair<MarkerScopeKind, std::string_view>, 3> kScopeNames{{
    {MarkerScopeKind::kSeries, "series"},
    {MarkerScopeKind::kDataset, "dataset"},
    {MarkerScopeKind::kAll, "all"},
}};

std::string_view scopeName(MarkerScopeKind scope) {
  for (const auto& [kind, name] : kScopeNames) {
    if (kind == scope) {
      return name;
    }
  }
  return {};
}

std::optional<MarkerScopeKind> parseScope(std::string_view name) {
  for (const auto& [kind, scope_name] : kScopeNames) {
    if (scope_name == name) {
      return kind;
    }
  }
  return std::nullopt;
}

std::optional<std::string> stringField(const nlohmann::json& object, std::string_view key) {
  const auto field = object.find(std::string(key));
  if (field == object.end() || !field->is_string()) {
    return std::nullopt;
  }
  return field->get<std::string>();
}

// The parsed metadata when its `pj_derived` is the string `kind`. Scans of every object
// topic reach here (catalog rebuilds, each plot paint): the overwhelming majority are
// not derived and are rejected without parsing.
std::optional<nlohmann::json> derivedMetadataOfKind(std::string_view metadata_json, std::string_view kind) {
  if (metadata_json.find(kDerivedMetadataKey) == std::string_view::npos) {
    return std::nullopt;
  }
  nlohmann::json doc = nlohmann::json::parse(metadata_json, nullptr, /*allow_exceptions=*/false);
  if (!doc.is_object() || stringField(doc, kDerivedMetadataKey) != kind) {
    return std::nullopt;
  }
  return doc;
}

}  // namespace

std::string markerTopicMetadataJson(const MarkerTopicInfo& info) {
  sdk::ObjectTopicMetadataBuilder builder;
  builder.builtinObjectType(sdk::BuiltinObjectType::kPlotMarkers)
      .string(kDerivedMetadataKey, kDerivedMarkers)
      .string(kMarkerScopeMetadataKey, scopeName(info.scope));
  if (info.scope == MarkerScopeKind::kSeries) {
    builder.string(kMarkerTargetMetadataKey, info.target);
  }
  if (!info.owner.empty()) {
    builder.string(kMarkerOwnerMetadataKey, info.owner);
  }
  const auto json = builder.build();
  return json.has_value() ? *json : std::string{};
}

std::optional<MarkerTopicInfo> markerTopicInfo(std::string_view metadata_json) {
  const std::optional<nlohmann::json> parsed = derivedMetadataOfKind(metadata_json, kDerivedMarkers);
  if (!parsed) {
    return std::nullopt;
  }
  const nlohmann::json& doc = *parsed;
  const std::optional<std::string> scope_name = stringField(doc, kMarkerScopeMetadataKey);
  const std::optional<MarkerScopeKind> scope = scope_name ? parseScope(*scope_name) : std::nullopt;
  if (!scope) {
    return std::nullopt;
  }
  MarkerTopicInfo info{*scope};
  if (*scope == MarkerScopeKind::kSeries) {
    const std::optional<std::string> target = stringField(doc, kMarkerTargetMetadataKey);
    if (!target || target->empty()) {
      return std::nullopt;
    }
    info.target = *target;
  }
  info.owner = stringField(doc, kMarkerOwnerMetadataKey).value_or(std::string{});
  return info;
}

bool isDerivedOfKind(std::string_view metadata_json, std::string_view kind) {
  return derivedMetadataOfKind(metadata_json, kind).has_value();
}

void stampLegacyMarkerMetadata(ObjectTopicDescriptor& descriptor) {
  if (!descriptor.topic_name.starts_with(sdk::kMarkerObjectTopicPrefix)) {
    return;
  }
  const std::string_view key = std::string_view(descriptor.topic_name).substr(sdk::kMarkerObjectTopicPrefix.size());
  if (key.empty()) {
    return;
  }
  // Empty, non-JSON and non-object metadata describe nothing: the stamp replaces it.
  nlohmann::json doc = nlohmann::json::parse(descriptor.metadata_json, nullptr, /*allow_exceptions=*/false);
  if (!doc.is_object()) {
    doc = nlohmann::json::object();
  }
  if (doc.contains(std::string(kDerivedMetadataKey))) {
    return;
  }
  // A declared type other than plot-markers (even a non-string one) is another kind of object.
  if (doc.contains(std::string(sdk::kBuiltinObjectTypeMetadataKey)) &&
      stringField(doc, sdk::kBuiltinObjectTypeMetadataKey) !=
          std::string(sdk::name(sdk::BuiltinObjectType::kPlotMarkers))) {
    return;
  }

  const std::string stamp = markerTopicMetadataJson(markerInfoForKey(key));
  if (stamp.empty()) {
    return;
  }
  // A key that is not UTF-8 cannot be described in JSON: the topic registers unstamped.
  const nlohmann::json fields = nlohmann::json::parse(stamp, nullptr, /*allow_exceptions=*/false);
  if (fields.is_discarded()) {
    return;
  }
  doc.update(fields);
  descriptor.metadata_json = doc.dump();
}

}  // namespace PJ
