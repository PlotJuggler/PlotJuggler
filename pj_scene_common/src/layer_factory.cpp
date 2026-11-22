// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene_common/layer_factory.h"

#include <algorithm>
#include <utility>

#include "pj_runtime/XmlStateOptions.h"

namespace PJ {

void LayerFactory::registerType(sdk::BuiltinObjectType object_type, Creator creator, PayloadValidator validator) {
  registrations_[object_type] = Registration{.creator = std::move(creator), .validator = std::move(validator)};
}

bool LayerFactory::acceptPayloadWithoutOptions(const QDomElement& /*payload*/) {
  return true;
}

bool LayerFactory::supports(sdk::BuiltinObjectType object_type) const {
  return registrations_.find(object_type) != registrations_.end();
}

std::vector<sdk::BuiltinObjectType> LayerFactory::types() const {
  std::vector<sdk::BuiltinObjectType> types;
  types.reserve(registrations_.size());
  for (const auto& [type, registration] : registrations_) {
    types.push_back(type);
  }
  std::sort(types.begin(), types.end());
  return types;
}

bool LayerFactory::validatePayload(sdk::BuiltinObjectType object_type, const QDomElement& payload) const {
  const auto it = registrations_.find(object_type);
  if (it == registrations_.end() || !it->second.validator) {
    XmlOptionsRecorder::noteRejection(payload, {}, QStringLiteral("no layer reads this payload"));
    return false;
  }
  return it->second.validator(payload);
}

std::unique_ptr<ISceneLayer> LayerFactory::create(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name) const {
  const auto it = registrations_.find(object_type);
  if (it == registrations_.end() || !it->second.creator) {
    return nullptr;
  }
  return it->second.creator(topic_id, object_type, display_name);
}

}  // namespace PJ
