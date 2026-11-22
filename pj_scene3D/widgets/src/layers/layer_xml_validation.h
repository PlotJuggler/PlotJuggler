// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QColor>
#include <QDomElement>
#include <QString>
#include <QStringList>
#include <optional>

#include "pj_runtime/XmlStateOptions.h"
#include "pj_widgets/Colormap.h"

namespace pj::scene3d::detail {

using namespace Qt::StringLiterals;

// Layer payloads are attribute-only leaves. Unknown attributes are ignored (forward compatibility); unknown nested
// elements are a structural error because an older build cannot restore them faithfully. `allowed_nested_tag` is the
// one exception: a layer that OWNS a sub-layer persists it as a single nested payload of that tag (PosesInFrameLayer's
// embedded <trail>); every other nested element fails, and so does a second copy of the allowed one.
inline bool isLeafPayload(const QDomElement& element, QLatin1String allowed_nested_tag = {}) {
  bool seen_nested = false;
  for (QDomNode child = element.firstChild(); !child.isNull(); child = child.nextSibling()) {
    if (child.isElement()) {
      if (allowed_nested_tag.isEmpty() || seen_nested || child.toElement().tagName() != allowed_nested_tag) {
        PJ::XmlOptionsRecorder::noteRejection(
            element, {}, u"unexpected child <"_s + child.toElement().tagName() + u'>');
        return false;
      }
      seen_nested = true;
      continue;
    }
    if ((child.isText() || child.isCDATASection()) && !child.nodeValue().trimmed().isEmpty()) {
      PJ::XmlOptionsRecorder::noteRejection(element, {}, u"unexpected text content"_s);
      return false;
    }
  }
  return true;
}

// A "1"/"0" boolean, the spelling most layer payloads save.
inline bool readZeroOne(const QDomElement& element, const QString& attribute, bool fallback, bool& value) {
  return PJ::readXmlBool(element, attribute, fallback, value, PJ::XmlBadValue::kRefuse, u"1"_s, u"0"_s);
}

// A colour attribute (any QColor name, "#rrggbb" as saved), reported to the active PJ::XmlOptionsRecorder. Absent:
// true with `value` untouched; `fallback` is only what is reported. Invalid: false.
inline bool parseColor(
    const QDomElement& element, const char* attribute, const QString& fallback, std::optional<QColor>& value) {
  const QString name = QLatin1String(attribute);
  PJ::XmlOptionsRecorder::noteOption(element, {.attribute = name, .type = u"color"_s, .fallback = fallback});
  if (!element.hasAttribute(name)) {
    return true;
  }
  const QColor color(element.attribute(name));
  if (!color.isValid()) {
    PJ::XmlOptionsRecorder::noteRejection(element, name, u"expected a colour"_s);
    return false;
  }
  value = color;
  return true;
}

// A colormap saved by token ("turbo", ...), Turbo when absent.
inline bool parseColormapToken(const QDomElement& element, const char* attribute, PJ::Colormap& value) {
  QString token;
  if (!PJ::readXmlEnum(
          element, QLatin1String(attribute), PJ::colormapTokens(), PJ::colormapToken(PJ::Colormap::kTurbo), token)) {
    return false;
  }
  value = *PJ::parseColormap(token);
  return true;
}

// A cross-attribute constraint the per-attribute readers cannot see (range_min <= range_max, ...).
inline bool requireConstraint(bool holds, const QDomElement& element, const char* attribute, const char* reason) {
  if (!holds) {
    PJ::XmlOptionsRecorder::noteRejection(element, QLatin1String(attribute), QLatin1String(reason));
  }
  return holds;
}

}  // namespace pj::scene3d::detail
