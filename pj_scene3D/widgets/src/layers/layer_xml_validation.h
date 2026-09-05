// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QDomElement>
#include <QString>
#include <cmath>
#include <initializer_list>

namespace pj::scene3d::detail {

// Layer payloads are attribute-only leaves. Unknown attributes are deliberately
// ignored for forward compatibility, while unknown nested elements remain a
// structural error because an older build cannot faithfully restore them.
// `allowed_nested_tag` carves out the one exception: a layer that OWNS a
// sub-layer persists it as a single nested payload of that tag
// (PosesInFrameLayer's embedded <trail>). Every other nested element still
// fails, and so does a second copy of the allowed one.
inline bool isLeafPayload(const QDomElement& element, QLatin1String allowed_nested_tag = {}) {
  bool seen_nested = false;
  for (QDomNode child = element.firstChild(); !child.isNull(); child = child.nextSibling()) {
    if (child.isElement()) {
      if (allowed_nested_tag.isEmpty() || seen_nested || child.toElement().tagName() != allowed_nested_tag) {
        return false;
      }
      seen_nested = true;
      continue;
    }
    if ((child.isText() || child.isCDATASection()) && !child.nodeValue().trimmed().isEmpty()) {
      return false;
    }
  }
  return true;
}

inline bool parseFiniteFloat(
    const QDomElement& element, const char* attribute, float fallback, float minimum, float maximum, float& value) {
  bool ok = true;
  value = element.hasAttribute(QLatin1String(attribute)) ? element.attribute(QLatin1String(attribute)).toFloat(&ok)
                                                         : fallback;
  return ok && std::isfinite(value) && value >= minimum && value <= maximum;
}

inline bool parseFiniteDouble(
    const QDomElement& element, const char* attribute, double fallback, double minimum, double maximum, double& value) {
  bool ok = true;
  value = element.hasAttribute(QLatin1String(attribute)) ? element.attribute(QLatin1String(attribute)).toDouble(&ok)
                                                         : fallback;
  return ok && std::isfinite(value) && value >= minimum && value <= maximum;
}

inline bool parseBoundedInt(
    const QDomElement& element, const char* attribute, int fallback, int minimum, int maximum, int& value) {
  bool ok = true;
  value = element.hasAttribute(QLatin1String(attribute)) ? element.attribute(QLatin1String(attribute)).toInt(&ok)
                                                         : fallback;
  return ok && value >= minimum && value <= maximum;
}

inline bool parseTrueFalse(const QDomElement& element, const char* attribute, bool fallback, bool& value) {
  if (!element.hasAttribute(QLatin1String(attribute))) {
    value = fallback;
    return true;
  }
  const QString text = element.attribute(QLatin1String(attribute));
  if (text == QStringLiteral("true")) {
    value = true;
    return true;
  }
  if (text == QStringLiteral("false")) {
    value = false;
    return true;
  }
  return false;
}

inline bool parseZeroOne(const QDomElement& element, const char* attribute, bool fallback, bool& value) {
  if (!element.hasAttribute(QLatin1String(attribute))) {
    value = fallback;
    return true;
  }
  const QString text = element.attribute(QLatin1String(attribute));
  if (text == QStringLiteral("1")) {
    value = true;
    return true;
  }
  if (text == QStringLiteral("0")) {
    value = false;
    return true;
  }
  return false;
}

}  // namespace pj::scene3d::detail
