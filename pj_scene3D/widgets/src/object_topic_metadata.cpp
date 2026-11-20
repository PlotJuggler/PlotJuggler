// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/object_topic_metadata.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include "pj_base/sdk/object_topic_metadata.hpp"
using namespace Qt::StringLiterals;

namespace pj::scene3d {

PJ::sdk::BuiltinObjectType builtinObjectTypeFor(const PJ::ObjectTopicDescriptor& descriptor) {
  if (descriptor.metadata_json.empty()) {
    return PJ::sdk::BuiltinObjectType::kNone;
  }
  const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(descriptor.metadata_json));
  if (!doc.isObject()) {
    return PJ::sdk::BuiltinObjectType::kNone;
  }
  const QJsonValue value = doc.object().value(u"builtin_object_type"_s);
  if (!value.isString()) {
    return PJ::sdk::BuiltinObjectType::kNone;
  }
  const auto parsed = PJ::sdk::parseBuiltinObjectType(value.toString().toStdString());
  return parsed.value_or(PJ::sdk::BuiltinObjectType::kNone);
}

bool isSnapshotStream(const PJ::ObjectTopicDescriptor& descriptor) {
  if (descriptor.metadata_json.find(PJ::sdk::kSnapshotMetadataKey) == std::string::npos) {
    return false;  // most topics: no parse
  }
  const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(descriptor.metadata_json));
  const QString key = QString::fromLatin1(PJ::sdk::kSnapshotMetadataKey.data(), PJ::sdk::kSnapshotMetadataKey.size());
  return doc.isObject() && doc.object().value(key) == u"true"_s;
}

}  // namespace pj::scene3d
