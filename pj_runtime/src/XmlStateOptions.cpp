// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/XmlStateOptions.h"

#include <QDomNamedNodeMap>
#include <algorithm>

namespace PJ {

namespace {
thread_local XmlOptionsRecorder* t_active = nullptr;
}  // namespace

XmlOptionsRecorder::XmlOptionsRecorder() : previous_(t_active) {
  t_active = this;
}

XmlOptionsRecorder::~XmlOptionsRecorder() {
  t_active = previous_;
  if (previous_ == nullptr) {
    return;
  }
  for (XmlOption& option : options_) {
    const auto known = std::find_if(previous_->options_.begin(), previous_->options_.end(), [&](const XmlOption& o) {
      return o.element == option.element && o.attribute == option.attribute;
    });
    if (known == previous_->options_.end()) {
      previous_->options_.push_back(std::move(option));
    }
  }
  if (previous_->first_rejection_.isEmpty()) {
    previous_->first_rejection_ = first_rejection_;
  }
}

void XmlOptionsRecorder::noteOption(const QDomElement& element, XmlOption option) {
  if (t_active == nullptr) {
    return;
  }
  option.element = element.tagName();
  auto& options = t_active->options_;
  const bool known = std::any_of(options.begin(), options.end(), [&](const XmlOption& o) {
    return o.element == option.element && o.attribute == option.attribute;
  });
  if (!known) {
    options.push_back(std::move(option));
  }
}

void XmlOptionsRecorder::noteRejection(const QDomElement& element, const QString& attribute, const QString& reason) {
  if (t_active == nullptr || !t_active->first_rejection_.isEmpty()) {
    return;
  }
  QString where = u'<' + element.tagName() + u'>';
  if (!attribute.isEmpty()) {
    where += u' ' + attribute + u"=\"" + element.attribute(attribute) + u'"';
  }
  t_active->first_rejection_ = where + u": " + reason;
}

bool readXmlEnum(
    const QDomElement& element, const QString& attribute, const QStringList& tokens, const QString& fallback,
    QString& value) {
  XmlOptionsRecorder::noteOption(
      element, {.attribute = attribute, .type = QStringLiteral("enum"), .values = tokens, .fallback = fallback});
  if (!element.hasAttribute(attribute)) {
    value = fallback;
    return true;
  }
  const QString text = element.attribute(attribute);
  if (!tokens.contains(text)) {
    XmlOptionsRecorder::noteRejection(element, attribute, u"expected one of " + tokens.join(u", "));
    return false;
  }
  value = text;
  return true;
}

QString readXmlText(const QDomElement& element, const QString& attribute, const QString& fallback) {
  XmlOptionsRecorder::noteOption(
      element, {.attribute = attribute, .type = QStringLiteral("text"), .fallback = fallback});
  return element.attribute(attribute, fallback);
}

QStringList unreportedXmlAttributes(const QDomElement& root, const std::vector<XmlOption>& options) {
  QStringList missing;
  const auto visit = [&](const auto& self, const QDomElement& element) -> void {
    const QDomNamedNodeMap attributes = element.attributes();
    for (int index = 0; index < attributes.count(); ++index) {
      const QString name = attributes.item(index).nodeName();
      const bool reported = std::any_of(options.begin(), options.end(), [&](const XmlOption& option) {
        return option.element == element.tagName() && option.attribute == name;
      });
      const QString entry = element.tagName() + u'.' + name;
      if (!reported && !missing.contains(entry)) {
        missing.push_back(entry);
      }
    }
    for (QDomElement child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
      self(self, child);
    }
  };
  visit(visit, root);
  return missing;
}

}  // namespace PJ
