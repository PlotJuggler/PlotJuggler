// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene_common/saved_layer_header.h"

#include <QStringList>
#include <limits>
#include <string_view>
#include <utility>

#include "pj_runtime/XmlStateOptions.h"
#include "pj_scene_common/layer_factory.h"

using namespace Qt::StringLiterals;

namespace PJ {

namespace {

// Parsed with toULongLong, as SceneDockWidget's dataset binding (savedDatasetOf) reads it, so the check and the
// binding accept the same texts.
bool readDatasetId(const QDomElement& element, SavedTopicRef ref, DatasetId& value) {
  const QString attribute = u"dataset_id"_s;
  const XmlOption option{
      .attribute = attribute,
      .type = u"int"_s,
      .min = ref == SavedTopicRef::kRequired ? 1.0 : 0.0,
      .max = static_cast<double>(std::numeric_limits<DatasetId>::max())};
  XmlOptionsRecorder::noteOption(element, option);
  const QString text = element.attribute(attribute);
  if (!element.hasAttribute(attribute) || (ref == SavedTopicRef::kUnused && text.isEmpty())) {
    return true;
  }
  bool ok = false;
  const qulonglong raw = text.toULongLong(&ok);
  if (ok && raw >= static_cast<qulonglong>(*option.min) && raw <= std::numeric_limits<DatasetId>::max()) {
    value = static_cast<DatasetId>(raw);
    return true;
  }
  // An empty text fails every number parse, so xmlValueProblem words the bound as every reader does.
  XmlOptionsRecorder::noteRejection(element, attribute, xmlValueProblem(option, QString()));
  return ref == SavedTopicRef::kUnused;
}

bool readTopicHeader(const QDomElement& element, SavedTopicRef ref, SavedTopicHeader& header) {
  header.topic_name = readXmlText(element, u"topic_name"_s);
  bool ok = readDatasetId(element, ref, header.dataset_id);
  header.dataset_source = readXmlText(element, u"dataset_source"_s);
  header.dataset_path = readXmlText(element, u"dataset_path"_s);
  header.qualified = element.hasAttribute(u"dataset_id"_s) || element.hasAttribute(u"dataset_source"_s) ||
                     element.hasAttribute(u"dataset_path"_s);
  (void)readXmlBool(element, u"pending_intent"_s, false, header.pending_intent, XmlBadValue::kTolerate);
  if (ref == SavedTopicRef::kRequired && header.topic_name.isEmpty()) {
    XmlOptionsRecorder::noteRejection(element, u"topic_name"_s, u"required"_s);
    ok = false;
  }
  return ok;
}

}  // namespace

std::optional<SavedTopicHeader> parseSavedTopicHeader(const QDomElement& element, SavedTopicRef ref) {
  SavedTopicHeader header;
  if (!readTopicHeader(element, ref, header)) {
    return std::nullopt;
  }
  return header;
}

std::optional<SavedLayerHeader> parseSavedLayerHeader(
    const QDomElement& element, SavedTopicRef ref, const LayerFactory& factory) {
  SavedLayerHeader header;
  bool ok = readTopicHeader(element, ref, header);
  if (XmlOptionsRecorder::active()) {
    XmlOption object_type{.attribute = u"object_type"_s, .type = u"enum"_s};
    for (const sdk::BuiltinObjectType type : factory.types()) {
      const std::string_view name = sdk::name(type);
      object_type.values.append(QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size())));
    }
    XmlOptionsRecorder::noteOption(element, std::move(object_type));
  }
  header.object_type = sdk::parseBuiltinObjectType(element.attribute(u"object_type"_s).toStdString());
  header.display_name = readXmlText(element, u"display_name"_s);
  ok = readXmlBool(element, u"visible"_s, true, header.visible) && ok;
  int order = 0;
  ok = readXmlInt<int>(element, u"order"_s, std::nullopt, 0, std::numeric_limits<int>::max(), order) && ok;
  if (element.hasAttribute(u"order"_s)) {
    header.order = order;
  }
  if (!ok) {
    return std::nullopt;
  }
  return header;
}

}  // namespace PJ
