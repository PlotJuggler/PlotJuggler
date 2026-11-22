#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomElement>
#include <QString>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_scene_common/scene_layer.h"

namespace PJ {

/// Per-scene-family registry that maps object types to layer constructors and to the parse-only check of the
/// payload each layer saves.
class LayerFactory {
 public:
  /// Creates an unattached layer for the requested object topic.
  using Creator = std::function<std::unique_ptr<ISceneLayer>(
      ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name)>;

  /// True when the layer's xmlLoadState would accept `payload` (the saved layer element's child, so its parent is
  /// that element), reporting what it reads to the active XmlOptionsRecorder. Parse only: no layer is touched.
  using PayloadValidator = std::function<bool(const QDomElement& payload)>;

  /// Registers or replaces the creator and payload validator of an object type. Without a validator every payload of
  /// the type is refused (noted as "no layer reads this payload"); a layer whose payload carries no options passes
  /// acceptPayloadWithoutOptions.
  void registerType(sdk::BuiltinObjectType object_type, Creator creator, PayloadValidator validator = {});

  /// The validator of a layer that saves no options: accepts any payload and reports nothing, so an attribute in
  /// it stays unreported (`unreportedXmlAttributes`) and widget_state refuses it as unknown.
  static bool acceptPayloadWithoutOptions(const QDomElement& payload);

  [[nodiscard]] bool supports(sdk::BuiltinObjectType object_type) const;

  /// The registered types, in ascending enum order.
  [[nodiscard]] std::vector<sdk::BuiltinObjectType> types() const;

  /// Runs the type's validator on `payload`; false, with a rejection noted, for a type without one.
  [[nodiscard]] bool validatePayload(sdk::BuiltinObjectType object_type, const QDomElement& payload) const;

  /// Returns nullptr when the object type is unsupported or has no creator.
  [[nodiscard]] std::unique_ptr<ISceneLayer> create(
      ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name) const;

 private:
  struct Registration {
    Creator creator;
    PayloadValidator validator;
  };
  std::unordered_map<sdk::BuiltinObjectType, Registration> registrations_;
};

}  // namespace PJ
