#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <string>
#include <vector>

#include "pj_base/types.hpp"

namespace PJ {

/// One source dataset for DataEngine::mergeDatasets, with the raw shift added to each of its timestamps. The engine
/// knows nothing of display offsets: the host bakes the alignment into the shift (typically
/// `display_offset(anchor) - display_offset(source)`).
struct DatasetMergeSource {
  DatasetId dataset_id = 0;
  Timestamp raw_shift_ns = 0;
};

/// Outcome of DataEngine::mergeDatasets. All topic ids are the anchor's: its topics keep their ids so curve keys
/// survive the merge.
struct DatasetMergeReport {
  /// Anchor topics rebuilt to include source rows.
  std::vector<TopicId> modified_topics;
  /// Topics created under the anchor for a name only the sources had.
  std::vector<TopicId> added_topics;
  /// Sources emptied of their topics. The datasets stay registered; the host removes them from its catalog.
  std::vector<DatasetId> consumed_datasets;
  /// Names of topics where a source has a field of another storage kind than the columns gathered so far: that
  /// source's rows are left out of the topic.
  std::vector<std::string> skipped_topics;
};

}  // namespace PJ
