// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_custom_widgets/WidgetBindings.h"

#include <algorithm>

#include "pj_base/builtin/builtin_object.hpp"

using namespace Qt::StringLiterals;

namespace PJ {

namespace {

QString kindName(WidgetInputKind kind) {
  switch (kind) {
    case WidgetInputKind::kNumber:
      return u"number"_s;
    case WidgetInputKind::kText:
      return u"text"_s;
    case WidgetInputKind::kObject:
      return u"object"_s;
  }
  return u"number"_s;
}

std::optional<WidgetInputKind> parseKind(const QString& name) {
  if (name == "number"_L1) {
    return WidgetInputKind::kNumber;
  }
  if (name == "text"_L1) {
    return WidgetInputKind::kText;
  }
  if (name == "object"_L1) {
    return WidgetInputKind::kObject;
  }
  return std::nullopt;
}

bool sameInput(const WidgetInput& a, const WidgetInput& b) {
  return a.name == b.name && a.label == b.label && a.unit == b.unit && a.kind == b.kind && a.multiple == b.multiple;
}

QString labelFor(const QString& topic, const QString& field) {
  return field.isEmpty() ? topic : topic + u'/' + field;
}

SeriesCapability capabilityFor(WidgetInputKind kind) {
  return kind == WidgetInputKind::kNumber ? SeriesCapability::kPlottable : SeriesCapability::kDiscrete;
}

}  // namespace

WidgetBindings::WidgetBindings(SessionManager* session, CatalogModel* catalog) : session_(session), catalog_(catalog) {}

WidgetBindings::InputsDelta WidgetBindings::setInputs(std::vector<WidgetInput> inputs) {
  InputsDelta delta;
  delta.inputs_changed = !std::ranges::equal(inputs, inputs_, sameInput);
  inputs_ = std::move(inputs);
  const std::size_t before = wires_.size();
  std::erase_if(wires_, [this](WidgetWire& wire) {
    const auto index = inputIndex(wire.input);
    if (!index) {
      return true;
    }
    const WidgetInput& input = inputs_[*index];
    wire.input_index = *index;
    return input.kind != wire.kind || (!input.multiple && wire.slot != 0);
  });
  delta.wires_changed = wires_.size() != before;
  return delta;
}

std::optional<std::size_t> WidgetBindings::inputIndex(const QString& name) const {
  const std::string utf8 = name.toStdString();
  for (std::size_t index = 0; index < inputs_.size(); ++index) {
    if (inputs_[index].name == utf8) {
      return index;
    }
  }
  return std::nullopt;
}

const WidgetWire* WidgetBindings::findWire(const QString& input, std::uint32_t slot) const {
  const auto it = std::ranges::find_if(wires_, [&](const WidgetWire& w) { return w.input == input && w.slot == slot; });
  return it == wires_.end() ? nullptr : &*it;
}

std::optional<WidgetInputKind> WidgetBindings::kindOfKey(const QString& key) const {
  if (catalog_ == nullptr) {
    return std::nullopt;
  }
  const auto item = catalog_->itemDescriptor(key);
  if (!item) {
    return std::nullopt;
  }
  if (isObjectTopic(*item)) {
    return WidgetInputKind::kObject;
  }
  if (!isScalarField(*item)) {
    return std::nullopt;
  }
  if (catalog_->isStringKey(key)) {
    return WidgetInputKind::kText;
  }
  if (catalog_->curveDescriptor(key, SeriesCapability::kPlottable)) {
    return WidgetInputKind::kNumber;
  }
  return std::nullopt;
}

bool WidgetBindings::canBind(std::size_t input_index, const QString& key) const {
  if (input_index >= inputs_.size()) {
    return false;
  }
  const auto kind = kindOfKey(key);
  return kind.has_value() && *kind == inputs_[input_index].kind;
}

std::optional<std::size_t> WidgetBindings::pickInput(WidgetInputKind kind, const std::set<std::size_t>& exclude) const {
  std::optional<std::size_t> first_multiple;
  std::optional<std::size_t> first_any;
  for (std::size_t index = 0; index < inputs_.size(); ++index) {
    const WidgetInput& input = inputs_[index];
    if (input.kind != kind || exclude.contains(index)) {
      continue;
    }
    if (!input.multiple && findWire(QString::fromStdString(input.name), 0) == nullptr) {
      return index;
    }
    if (input.multiple && !first_multiple) {
      first_multiple = index;
    }
    if (!first_any) {
      first_any = index;
    }
  }
  return first_multiple ? first_multiple : first_any;
}

void WidgetBindings::setDatasetIdentity(WidgetWire& wire, DatasetId dataset_id, const QString& fallback_name) const {
  wire.identity.dataset_id = dataset_id;
  wire.identity.dataset_source = catalog_->datasetSourceName(dataset_id).value_or(fallback_name);
  wire.identity.dataset_path = catalog_->datasetSourcePath(dataset_id);
  wire.dataset = dataset_id;
}

void WidgetBindings::fillScalar(WidgetWire& wire, const CurveDescriptor& curve) const {
  const QString field = curve.field_path.isEmpty() ? curve.field_name : curve.field_path;
  setDatasetIdentity(wire, curve.dataset_id, curve.dataset_name);
  wire.identity.topic = curve.topic_name;
  wire.identity.field = field;
  wire.key = curve.name;
  wire.target = WireTarget{};
  wire.target.kind = wire.kind;
  wire.target.topic_id = curve.topic_id;
  wire.target.column = curve.column_index;
  wire.target.offset = session_->displayOffset(curve.dataset_id);
  wire.label = labelFor(curve.topic_name, field).toStdString();
  wire.resolved = true;
}

bool WidgetBindings::fillObject(WidgetWire& wire, ObjectTopicId topic) const {
  const ObjectTopicDescriptor& descriptor = session_->objectStore().descriptor(topic);
  if (descriptor.topic_name.empty()) {
    return false;
  }
  const QString topic_name = QString::fromStdString(descriptor.topic_name);
  setDatasetIdentity(wire, descriptor.dataset_id, QString());
  wire.identity.topic = topic_name;
  wire.identity.field.clear();
  wire.key.clear();
  wire.target = WireTarget{};
  wire.target.kind = WidgetInputKind::kObject;
  wire.target.object_topic = topic;
  wire.target.object_type_name = sdk::name(objectTypeFromMetadata(descriptor.metadata_json));
  wire.target.offset = session_->displayOffset(descriptor.dataset_id);
  wire.label = topic_name.toStdString();
  wire.resolved = true;
  return true;
}

bool WidgetBindings::resolveFromIdentity(WidgetWire& wire) const {
  if (session_ == nullptr || catalog_ == nullptr) {
    return false;
  }
  const SavedSeriesPath& id = wire.identity;
  if (wire.kind == WidgetInputKind::kObject) {
    const DatasetIdentityResolution dataset =
        session_->resolveObjectDatasetIdentity(id.dataset_id, id.dataset_source, id.dataset_path, id.topic);
    if (!dataset.id) {
      return false;
    }
    const auto topic = session_->objectStore().findTopic(*dataset.id, id.topic.toStdString());
    return topic.has_value() && fillObject(wire, *topic);
  }
  const SeriesCapability capability = capabilityFor(wire.kind);
  const auto key = catalog_->resolveCurveKey(id, capability);
  if (!key) {
    return false;
  }
  if (wire.kind == WidgetInputKind::kText && !catalog_->isStringKey(*key)) {
    return false;
  }
  const auto curve = catalog_->curveDescriptor(*key, capability);
  if (!curve) {
    return false;
  }
  fillScalar(wire, *curve);
  return true;
}

void WidgetBindings::place(std::size_t input_index, std::uint32_t slot, WidgetWire wire) {
  const WidgetInput& input = inputs_[input_index];
  wire.input = QString::fromStdString(input.name);
  wire.input_index = input_index;
  wire.kind = input.kind;
  if (!input.multiple) {
    slot = 0;
  } else if (slot == kNewSlot) {
    slot = 0;
    for (const WidgetWire& other : wires_) {
      if (other.input == wire.input) {
        slot = std::max(slot, other.slot + 1);
      }
    }
  }
  wire.slot = slot;
  const auto existing =
      std::ranges::find_if(wires_, [&](const WidgetWire& w) { return w.input == wire.input && w.slot == slot; });
  if (existing != wires_.end()) {
    *existing = std::move(wire);
  } else {
    wires_.push_back(std::move(wire));
  }
}

bool WidgetBindings::bindKey(std::size_t input_index, std::uint32_t slot, const QString& key) {
  if (session_ == nullptr || !canBind(input_index, key)) {
    return false;
  }
  WidgetWire wire;
  wire.kind = inputs_[input_index].kind;
  if (wire.kind == WidgetInputKind::kObject) {
    const auto item = catalog_->itemDescriptor(key);
    const auto* payload = item ? asObjectTopic(*item) : nullptr;
    if (payload == nullptr || !fillObject(wire, payload->object_topic_id)) {
      return false;
    }
  } else {
    const auto curve = catalog_->curveDescriptor(key, capabilityFor(wire.kind));
    if (!curve) {
      return false;
    }
    fillScalar(wire, *curve);
  }
  place(input_index, slot, std::move(wire));
  return true;
}

bool WidgetBindings::bindObject(std::size_t input_index, std::uint32_t slot, ObjectTopicId topic) {
  if (session_ == nullptr || catalog_ == nullptr || input_index >= inputs_.size() ||
      inputs_[input_index].kind != WidgetInputKind::kObject) {
    return false;
  }
  WidgetWire wire;
  wire.kind = WidgetInputKind::kObject;
  if (!fillObject(wire, topic)) {
    return false;
  }
  place(input_index, slot, std::move(wire));
  return true;
}

bool WidgetBindings::unbind(const QString& input, std::uint32_t slot) {
  return std::erase_if(wires_, [&](const WidgetWire& w) { return w.input == input && w.slot == slot; }) > 0;
}

void WidgetBindings::clearWires() {
  wires_.clear();
}

bool WidgetBindings::retryPending() {
  bool changed = false;
  for (WidgetWire& wire : wires_) {
    if (!wire.resolved) {
      changed = resolveFromIdentity(wire) || changed;
    }
  }
  return changed;
}

bool WidgetBindings::revalidateWire(WidgetWire& wire) const {
  const WireTarget previous = wire.target;
  const DatasetId previous_dataset = wire.dataset;
  if (!resolveFromIdentity(wire)) {
    wire.resolved = false;
    wire.key.clear();
    return true;
  }
  return previous_dataset != wire.dataset || previous.topic_id != wire.target.topic_id ||
         previous.column != wire.target.column || previous.object_topic != wire.target.object_topic ||
         previous.object_type_name != wire.target.object_type_name;
}

bool WidgetBindings::revalidate() {
  bool changed = false;
  for (WidgetWire& wire : wires_) {
    if (wire.resolved) {
      changed = revalidateWire(wire) || changed;
    }
  }
  return changed;
}

bool WidgetBindings::refresh() {
  bool changed = false;
  for (WidgetWire& wire : wires_) {
    changed = (wire.resolved ? revalidateWire(wire) : resolveFromIdentity(wire)) || changed;
  }
  return changed;
}

std::optional<WireTarget> WidgetBindings::currentTarget(const WidgetWire& wire) const {
  if (!wire.resolved || session_ == nullptr) {
    return std::nullopt;
  }
  WireTarget target = wire.target;
  target.offset = session_->displayOffset(wire.dataset);
  return target;
}

std::vector<std::pair<DatasetId, QString>> WidgetBindings::displayedTopics() const {
  std::vector<std::pair<DatasetId, QString>> topics;
  for (const WidgetWire& wire : wires_) {
    if (!wire.resolved) {
      continue;
    }
    std::pair<DatasetId, QString> entry{wire.dataset, wire.identity.topic};
    if (std::ranges::find(topics, entry) == topics.end()) {
      topics.push_back(std::move(entry));
    }
  }
  return topics;
}

std::vector<PendingWireDemand> WidgetBindings::pendingDemands() const {
  std::vector<PendingWireDemand> demands;
  for (const WidgetWire& wire : wires_) {
    if (wire.resolved || wire.identity.topic.isEmpty()) {
      continue;
    }
    PendingWireDemand demand{wire.identity.topic, std::nullopt};
    if (catalog_ != nullptr) {
      const auto identity = catalog_->resolveDatasetIdentity(
          wire.identity.dataset_id, wire.identity.dataset_source, wire.identity.dataset_path);
      if (!identity.ambiguous) {
        demand.preferred_dataset = identity.id;
      }
    }
    if (std::ranges::find(demands, demand) == demands.end()) {
      demands.push_back(std::move(demand));
    }
  }
  return demands;
}

bool WidgetBindings::hasPendingTopic(const QString& topic) const {
  return std::ranges::any_of(
      wires_, [&topic](const WidgetWire& wire) { return !wire.resolved && wire.identity.topic == topic; });
}

bool WidgetBindings::hasPendingWires() const {
  return std::ranges::any_of(
      wires_, [](const WidgetWire& wire) { return !wire.resolved && !wire.identity.topic.isEmpty(); });
}

void WidgetBindings::save(QDomDocument& doc, QDomElement& parent) const {
  for (const WidgetWire& wire : wires_) {
    SavedSeriesPath id = wire.identity;
    if (wire.resolved && catalog_ != nullptr) {
      id.dataset_id = wire.dataset;
      id.dataset_source = catalog_->datasetSourceName(wire.dataset).value_or(id.dataset_source);
      id.dataset_path = catalog_->datasetSourcePath(wire.dataset);
    }
    QDomElement element = doc.createElement(u"input"_s);
    element.setAttribute(u"name"_s, wire.input);
    element.setAttribute(u"slot"_s, QString::number(wire.slot));
    element.setAttribute(u"kind"_s, kindName(wire.kind));
    element.setAttribute(u"dataset_id"_s, QString::number(id.dataset_id));
    element.setAttribute(u"dataset_source"_s, id.dataset_source);
    if (!id.dataset_path.isEmpty()) {
      element.setAttribute(u"dataset_path"_s, id.dataset_path);
    }
    element.setAttribute(u"topic"_s, id.topic);
    if (!id.field.isEmpty()) {
      element.setAttribute(u"field"_s, id.field);
    }
    parent.appendChild(element);
  }
}

void WidgetBindings::load(const QDomElement& element) {
  wires_.clear();
  for (QDomElement child = element.firstChildElement(u"input"_s); !child.isNull();
       child = child.nextSiblingElement(u"input"_s)) {
    const QString name = child.attribute(u"name"_s);
    const auto index = inputIndex(name);
    const auto kind = parseKind(child.attribute(u"kind"_s));
    bool slot_ok = false;
    const auto slot = child.attribute(u"slot"_s, u"0"_s).toUInt(&slot_ok);
    if (!index || !kind || !slot_ok || slot == kNewSlot || *kind != inputs_[*index].kind ||
        (!inputs_[*index].multiple && slot != 0) || findWire(name, slot) != nullptr) {
      continue;
    }
    WidgetWire wire;
    wire.input = name;
    wire.input_index = *index;
    wire.slot = slot;
    wire.kind = *kind;
    wire.identity.dataset_id = child.attribute(u"dataset_id"_s).toUInt();  // 0 when out of range or not a number
    wire.identity.dataset_source = child.attribute(u"dataset_source"_s);
    wire.identity.dataset_path = child.attribute(u"dataset_path"_s);
    wire.identity.topic = child.attribute(u"topic"_s);
    wire.identity.field = child.attribute(u"field"_s);
    wire.label = labelFor(wire.identity.topic, wire.identity.field).toStdString();
    resolveFromIdentity(wire);
    wires_.push_back(std::move(wire));
  }
}

}  // namespace PJ
