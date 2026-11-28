#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomElement>
#include <QString>
#include <optional>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/types.hpp"

namespace PJ {

class LayerFactory;

/// How a saved <layer> or <config_topic> names the store topic it shows; decides what its dataset_id and topic_name
/// must hold.
enum class SavedTopicRef {
  kOptional,  ///< any topic_name; a dataset_id in [0, max], 0 naming no dataset (bound by source, path or topic)
  kRequired,  ///< a non-empty topic_name and, when present, a dataset_id >= 1: a topic resolves through a real dataset
  kUnused,    ///< no store topic (a layer the dock owns): the dataset_id is never read, so a bad one is tolerated
};

/// The topic reference of a saved <layer> or <config_topic>, as SceneDockWidget::deferTopicIntent and the families'
/// xmlSaveState write it.
struct SavedTopicHeader {
  QString topic_name;
  DatasetId dataset_id = 0;  ///< 0 when absent
  QString dataset_source;
  QString dataset_path;
  /// A dataset_id, dataset_source or dataset_path attribute is present (SceneDockWidget::hasDatasetIdentity).
  bool qualified = false;
  /// A runtime intent for an advertised topic, not a saved element; only "true" sets it, any other value is tolerated.
  bool pending_intent = false;
};

/// The attributes every family's <layer> carries; a family's own (Scene2D's layer_kind, Scene3D's role and local)
/// and the payload child are read by the family.
struct SavedLayerHeader : SavedTopicHeader {
  /// nullopt when absent or not a type name; whether the family handles it is the caller's rule.
  std::optional<sdk::BuiltinObjectType> object_type;
  QString display_name;
  bool visible = true;
  /// nullopt when absent (the loaders fill it with the element's index before restoring).
  std::optional<int> order;
};

// The one reader of these attributes, for the restore paths and the parse-only xmlValidateState alike: every attribute
// is reported to the active XmlOptionsRecorder, a refused one is noted, and the result is nullopt when any is refused.
// Each attribute is read and reported even after a refusal.

[[nodiscard]] std::optional<SavedTopicHeader> parseSavedTopicHeader(const QDomElement& element, SavedTopicRef ref);

/// Also reports object_type as an enum of `factory`'s types; the type itself is never refused here.
[[nodiscard]] std::optional<SavedLayerHeader> parseSavedLayerHeader(
    const QDomElement& element, SavedTopicRef ref, const LayerFactory& factory);

/// The persisted `object_type` token of `object_type`.
[[nodiscard]] QString objectTypeName(sdk::BuiltinObjectType object_type);

/// Why a saved element of `saved` type cannot bind `topic_name`, whose data is `live`: the skip reason of a layout
/// whose element names the wrong object type.
[[nodiscard]] QString objectTypeMismatch(
    const QString& topic_name, sdk::BuiltinObjectType saved, sdk::BuiltinObjectType live);

}  // namespace PJ
