// SPDX-License-Identifier: MPL-2.0
#include "pj_runtime/DatasetQualifiedName.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "pj_base/builtin/plot_markers.hpp"
#include "pj_datastore/engine.hpp"

namespace PJ {

const ScalarFieldPayload* markerSeriesField(const CatalogItem& item, const std::string& key, bool strings_too) {
  const ScalarFieldPayload* scalar = asScalarField(item);
  if (scalar == nullptr || !(isPlottablePrimitive(scalar->logical_type) ||
                             (strings_too && scalar->logical_type == PrimitiveType::kString))) {
    return nullptr;
  }
  const std::string topic = item.topic_name.toStdString();
  if (sdk::markerSeriesKey(topic, scalar->field_path.toStdString()) != key &&
      sdk::markerSeriesKey(topic, scalar->field_name.toStdString()) != key) {
    return nullptr;
  }
  return scalar;
}

Expected<DatasetId> resolveMarkerDataset(
    std::vector<std::string>& inputs, std::vector<std::string>& outputs, const std::vector<CatalogItem>& items,
    const DataEngine& engine) {
  const std::vector<std::string> labels = engine.datasetLabels();

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
      const DatasetQualifierSplit split = splitDatasetQualifier(key, labels);
      if (!split.qualified) {
        continue;
      }
      const std::optional<DatasetId> target = engine.datasetByLabel(split.dataset_source);
      if (!target.has_value()) {
        return unexpected("unknown dataset label: " + split.dataset_source);
      }
      const std::string original = std::move(key);
      key = split.bare;
      if (auto err = choose(*target, original)) {
        return unexpected(std::move(*err));
      }
    }
  }
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
        const std::string label = engine.datasetLabel(item.dataset_id);
        const std::string qualified = sdk::qualifiedSeriesName(label, key);
        // A label shadowed by a longer loaded prefix cannot be addressed at all.
        if (label.empty() || splitDatasetQualifier(qualified, labels).dataset_source != label ||
            std::find(candidates.begin(), candidates.end(), qualified) != candidates.end()) {
          continue;
        }
        candidates.push_back(qualified);
      }
      std::string msg = "input '" + key + "' exists in several datasets";
      if (candidates.empty()) {
        msg += " and none of them can be addressed by dataset label";
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
      return unexpected("input '" + key + "' does not exist in dataset '" + engine.datasetLabel(*chosen) + "'");
    }
  }
  return *chosen;
}

}  // namespace PJ
