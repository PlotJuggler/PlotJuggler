// SPDX-License-Identifier: MPL-2.0
#include "pj_runtime/DatasetQualifiedName.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "pj_base/builtin/plot_markers.hpp"

namespace PJ {

const ScalarFieldPayload* markerSeriesField(const CatalogItem& item, const std::string& key) {
  const ScalarFieldPayload* scalar = asScalarField(item);
  if (scalar == nullptr || !isPlottablePrimitive(scalar->logical_type)) {
    return nullptr;
  }
  const std::string topic = item.topic_name.toStdString();
  if (sdk::markerSeriesKey(topic, scalar->field_path.toStdString()) != key &&
      sdk::markerSeriesKey(topic, scalar->field_name.toStdString()) != key) {
    return nullptr;
  }
  return scalar;
}

UniqueDatasetLookup uniqueDatasetForSource(
    std::string_view source, const std::vector<DatasetId>& datasets,
    const std::function<std::optional<std::string>(DatasetId)>& source_name_of) {
  UniqueDatasetLookup found;
  if (source.empty() || !source_name_of) {
    return found;
  }
  for (const DatasetId ds : datasets) {
    const std::optional<std::string> name = source_name_of(ds);
    if (!name.has_value() || *name != source) {
      continue;
    }
    if (found.id.has_value()) {
      return UniqueDatasetLookup{.id = std::nullopt, .ambiguous = true};
    }
    found.id = ds;
  }
  return found;
}

Expected<DatasetId> resolveMarkerDataset(
    std::vector<std::string>& inputs, std::vector<std::string>& outputs, const std::vector<CatalogItem>& items,
    const std::vector<DatasetId>& datasets,
    const std::function<std::optional<std::string>(DatasetId)>& source_name_of) {
  std::vector<std::string> source_names;
  source_names.reserve(datasets.size());
  for (const DatasetId ds : datasets) {
    const std::optional<std::string> src = source_name_of ? source_name_of(ds) : std::nullopt;
    source_names.push_back(src.value_or(std::string{}));
  }

  std::optional<DatasetId> chosen;
  const auto choose = [&](DatasetId ds, const std::string& key) -> std::optional<std::string> {
    if (chosen.has_value() && *chosen != ds) {
      return "marker inputs must share one dataset, but '" + key + "' names a different one";
    }
    chosen = ds;
    return std::nullopt;
  };

  // Pass 1: qualified keys decide the dataset, and are normalized in place.
  for (std::vector<std::string>* keys : {&inputs, &outputs}) {
    for (std::string& key : *keys) {
      const DatasetQualifierSplit split = splitDatasetQualifier(key, source_names);
      if (!split.qualified) {
        continue;
      }
      const UniqueDatasetLookup target = uniqueDatasetForSource(split.dataset_source, datasets, source_name_of);
      if (target.ambiguous) {
        return unexpected("dataset source is ambiguous (loaded more than once): " + split.dataset_source);
      }
      const std::string original = std::move(key);
      key = split.bare;
      if (auto err = choose(*target.id, original)) {
        return unexpected(std::move(*err));
      }
    }
  }
  const auto source_of = [&](DatasetId ds) -> std::string {
    for (std::size_t i = 0; i < datasets.size(); ++i) {
      if (datasets[i] == ds) {
        return source_names[i];
      }
    }
    return {};
  };
  const auto exists_in = [&](DatasetId ds, const std::string& key) {
    return std::any_of(items.begin(), items.end(), [&](const CatalogItem& item) {
      return item.dataset_id == ds && markerSeriesField(item, key) != nullptr;
    });
  };

  // Pass 2 (nothing qualified): an unqualified input that lives in exactly one
  // dataset votes for it; one that lives in several is refused with the qualified
  // forms that resolve back to that dataset.
  static const std::vector<std::string> kNone;
  for (const std::string& key : chosen.has_value() ? kNone : inputs) {
    std::optional<DatasetId> found;
    bool ambiguous = false;
    for (const CatalogItem& item : items) {
      if (markerSeriesField(item, key) == nullptr) {
        continue;
      }
      if (found.has_value() && *found != item.dataset_id) {
        ambiguous = true;
        break;
      }
      found = item.dataset_id;
    }
    if (ambiguous) {
      std::vector<std::string> candidates;
      for (const CatalogItem& item : items) {
        if (markerSeriesField(item, key) == nullptr) {
          continue;
        }
        const std::string src = source_of(item.dataset_id);
        const std::string qualified = sdk::qualifiedSeriesName(src, key);
        // A source loaded twice, or one shadowed by a longer loaded prefix, cannot be
        // addressed by name at all.
        if (uniqueDatasetForSource(src, datasets, source_name_of).id != item.dataset_id ||
            splitDatasetQualifier(qualified, source_names).dataset_source != src ||
            std::find(candidates.begin(), candidates.end(), qualified) != candidates.end()) {
          continue;
        }
        candidates.push_back(qualified);
      }
      std::string msg = "input '" + key + "' exists in several datasets";
      if (candidates.empty()) {
        msg += " and none of them can be addressed unambiguously by source name";
      } else {
        msg += "; qualify it as ";
        for (std::size_t i = 0; i < candidates.size(); ++i) {
          msg += (i == 0 ? "'" : " or '") + candidates[i] + "'";
        }
      }
      return unexpected(std::move(msg));
    }
    if (found.has_value()) {
      if (auto err = choose(*found, key)) {
        return unexpected(std::move(*err));
      }
    }
  }
  if (!chosen.has_value()) {
    return unexpected("none of the generator's inputs matches a loaded series");
  }
  // An unknown whole name is an error, qualified or not (the engine would otherwise
  // run the script against silently-absent data).
  for (const std::string& key : inputs) {
    if (key.empty()) {
      return unexpected("empty input name (a dataset qualifier needs a series after the ':')");
    }
    if (!exists_in(*chosen, key)) {
      return unexpected("input '" + key + "' does not exist in dataset '" + source_of(*chosen) + "'");
    }
  }
  return *chosen;
}

}  // namespace PJ
