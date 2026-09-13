// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/sdk/dataset_qualified_name.hpp"
#include "pj_runtime/CatalogModel.h"

namespace PJ {

// The SDK owns the catalog-dependent prefix convention; host resolution below
// validates the resulting dataset and series.
using sdk::DatasetQualifierSplit;
using sdk::splitDatasetQualifier;

/// The scalar payload behind `item` when it is the plottable series a marker
/// generator names `key` (markerSeriesKey naming), else nullptr. Shared by the
/// generator's series resolver and the dataset it binds to, which must agree:
/// a generator bound to a dataset whose series the resolver cannot find runs
/// against no data.
[[nodiscard]] const ScalarFieldPayload* markerSeriesField(const CatalogItem& item, const std::string& key);

/// Which loaded dataset a raw source name addresses.
struct UniqueDatasetLookup {
  std::optional<DatasetId> id;  ///< set only when exactly one dataset carries the source name
  bool ambiguous = false;       ///< the source is loaded more than once — not addressable by name
};

/// The one dataset among `datasets` whose `source_name_of` is `source` (an empty
/// source matches nothing). A source loaded twice yields no id and `ambiguous`.
[[nodiscard]] UniqueDatasetLookup uniqueDatasetForSource(
    std::string_view source, const std::vector<DatasetId>& datasets,
    const std::function<std::optional<std::string>(DatasetId)>& source_name_of);

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
/// `source_name_of` maps a dataset to its raw source name (CatalogModel's
/// datasetSourceName) — the disambiguated UI label is NOT valid here.
[[nodiscard]] Expected<DatasetId> resolveMarkerDataset(
    std::vector<std::string>& inputs, std::vector<std::string>& outputs, const std::vector<CatalogItem>& items,
    const std::vector<DatasetId>& datasets, const std::function<std::optional<std::string>(DatasetId)>& source_name_of);

}  // namespace PJ
