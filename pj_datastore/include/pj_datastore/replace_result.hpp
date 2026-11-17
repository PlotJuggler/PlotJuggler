#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <vector>

#include "pj_base/types.hpp"

namespace PJ {

/// Outcome of DataEngine::replaceDatasetFrom(). All ids are the primary engine's, and stay stable across a replace so
/// curve keys do not change.
struct DatasetReplaceResult {
  /// Primary topics that matched a staged topic by name; their chunks were replaced.
  std::vector<TopicId> replaced_topics;
  /// Staged topics with no primary match; created in the primary.
  std::vector<TopicId> added_topics;
  /// Primary topics with no staged match: chunks cleared and the id retired. listTopics() hides them, while
  /// getTopicStorage() still returns an empty storage, so an in-flight reader does not dangle. Informational: callers
  /// need not act on it.
  std::vector<TopicId> retired_topics;
};

}  // namespace PJ
