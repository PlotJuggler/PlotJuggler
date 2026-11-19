// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/sdk/dataset_qualified_name.hpp"
#include "pj_runtime/CatalogModel.h"

namespace PJ {

class DataEngine;

// The SDK owns the catalog-dependent prefix convention; host resolution below
// validates the resulting dataset and series.
using sdk::DatasetQualifierSplit;
using sdk::splitDatasetQualifier;

/// The scalar payload behind `item` when it is the plottable series a marker
/// generator names `key` (markerSeriesKey naming), else nullptr; with
/// `strings_too`, a string field matches too (an on-demand input). Shared by the
/// generator's series resolver and the dataset it binds to, which must agree:
/// a generator bound to a dataset whose series the resolver cannot find runs
/// against no data.
[[nodiscard]] const ScalarFieldPayload* markerSeriesField(
    const CatalogItem& item, const std::string& key, bool strings_too = false);

/// Pick the dataset a marker generator binds to, from its declared input (and
/// output) keys, and normalize dataset-qualified keys to their bare form in
/// place — the marker engine's `series(key)` and `markerSeriesKey` never see a
/// qualifier; the dataset choice travels in the recipe instead.
///
/// Rules, in order: qualified keys decide (all must agree on one dataset); an
/// unqualified input that exists in exactly one dataset votes for it; one that
/// exists in several is refused with the qualified candidates — landing on the
/// first-loaded dataset silently is precisely the failure this exists to stop.
/// Finally every input must exist in the chosen dataset: an unknown (or empty)
/// name is an error, never a silently absent series.
///
/// Qualifiers name datasets by label (DataEngine::datasetLabel), split against `engine`'s labels.
[[nodiscard]] Expected<DatasetId> resolveMarkerDataset(
    std::vector<std::string>& inputs, std::vector<std::string>& outputs, const std::vector<CatalogItem>& items,
    const DataEngine& engine);

}  // namespace PJ
