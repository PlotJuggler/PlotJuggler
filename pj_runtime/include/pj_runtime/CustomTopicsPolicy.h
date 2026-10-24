// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QString>
#include <optional>
#include <string>

#include "pj_base/types.hpp"

namespace PJ {

class BatchEngine;
class DataProcessorService;
class RecipeGraph;

/// Why the Custom Topics panel may not delete a row. The editor (the toolbox declaring `custom_topics_editor`,
/// `ExtensionCatalogService::customTopicsEditorId`) deletes only its own recipes, and a delete never reaches another
/// plugin's work. The shell words the refusal; this owns the rule.
struct CustomTopicDeleteRefusal {
  enum class Reason {
    kGone,           ///< no recipe writes the row any more: clearing the row is all that is left
    kReadOnly,       ///< `owner_plugin` made `user_id`, not the editor (an empty owner is never the editor)
    kForeignReader,  ///< `reader`, made by `owner_plugin`, reads the output and the removal would cascade into it
    kExemptReader,  ///< the history-exempt outputs `reader` (", "-joined) read what it removes: a redo would be refused
  };
  Reason reason = Reason::kGone;
  std::string user_id;
  std::string owner_plugin;
  std::string reader;
};

/// nullopt when the editor `editor_id` may delete the transform writing `output_topic`. The removal cascades into
/// every transform reading it, so a reader with an owner other than the editor refuses it; a reader with no owner
/// goes with it. The delete is an undo step, so a history-exempt on-demand recipe (`graph`) or marker generator
/// (`markers`) reading an output the cascade removes refuses it too: its redo would strand that reader.
[[nodiscard]] std::optional<CustomTopicDeleteRefusal> transformDeleteRefusal(
    const DataProcessorService& processors, const RecipeGraph& graph, const BatchEngine& markers, TopicId output_topic,
    const QString& editor_id);

/// nullopt when the editor `editor_id` may delete the persistent on-demand recipe `key`. Its ordinary readers stay
/// and report a missing input; a history-exempt one refuses it, since the delete is an undo step whose redo would
/// strand that reader (`DerivedObjects::refusal`).
[[nodiscard]] std::optional<CustomTopicDeleteRefusal> onDemandDeleteRefusal(
    const RecipeGraph& graph, const std::string& key, const QString& editor_id);

}  // namespace PJ
