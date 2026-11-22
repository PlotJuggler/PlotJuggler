#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomElement>
#include <QString>
#include <QStringList>
#include <cmath>
#include <optional>

#include "pj_runtime/XmlStateOptions.h"

namespace PJ::scene2d_xml {

// Reporting readers for the Scene2D XML parsers (XmlStateOptions.h): each reports its option to the active
// XmlOptionsRecorder and a refused value as a rejection. An absent attribute takes the fallback; a false return leaves
// `value` untouched.

/// "true" or "false".
[[nodiscard]] inline bool readBool(const QDomElement& element, const QString& attribute, bool fallback, bool& value) {
  XmlOptionsRecorder::noteOption(
      element, {.attribute = attribute,
                .type = QStringLiteral("bool"),
                .fallback = fallback ? QStringLiteral("true") : QStringLiteral("false")});
  if (!element.hasAttribute(attribute)) {
    value = fallback;
    return true;
  }
  const QString text = element.attribute(attribute);
  if (text != QLatin1String("true") && text != QLatin1String("false")) {
    XmlOptionsRecorder::noteRejection(element, attribute, QStringLiteral("expected true or false"));
    return false;
  }
  value = text == QLatin1String("true");
  return true;
}

/// A finite float. A value outside `min` / `max` is refused with `enforce_bounds`; without it the value is read as is
/// (the parser clamps) and only noted as a rejection.
[[nodiscard]] inline bool readFloat(
    const QDomElement& element, const QString& attribute, float fallback, std::optional<double> min,
    std::optional<double> max, float& value, bool enforce_bounds = true) {
  XmlOptionsRecorder::noteOption(
      element, {.attribute = attribute,
                .type = QStringLiteral("float"),
                .min = min,
                .max = max,
                .fallback = QString::number(fallback, 'g', 9)});
  if (!element.hasAttribute(attribute)) {
    value = fallback;
    return true;
  }
  bool ok = false;
  const float parsed = element.attribute(attribute).toFloat(&ok);
  const QString reason = min && max ? QStringLiteral("expected a number in [%1, %2]").arg(*min).arg(*max)
                                    : QStringLiteral("expected a finite number");
  if (!ok || !std::isfinite(parsed)) {
    XmlOptionsRecorder::noteRejection(element, attribute, reason);
    return false;
  }
  if ((min && parsed < *min) || (max && parsed > *max)) {
    XmlOptionsRecorder::noteRejection(element, attribute, reason);
    if (enforce_bounds) {
      return false;
    }
  }
  value = parsed;
  return true;
}

/// An integer in [min, max]; `value` stays empty when the attribute is absent.
[[nodiscard]] inline bool readInt(
    const QDomElement& element, const QString& attribute, qlonglong min, qlonglong max,
    std::optional<qlonglong>& value) {
  XmlOptionsRecorder::noteOption(
      element, {.attribute = attribute,
                .type = QStringLiteral("int"),
                .min = static_cast<double>(min),
                .max = static_cast<double>(max)});
  value.reset();
  if (!element.hasAttribute(attribute)) {
    return true;
  }
  bool ok = false;
  const qlonglong parsed = element.attribute(attribute).toLongLong(&ok);
  if (!ok || parsed < min || parsed > max) {
    XmlOptionsRecorder::noteRejection(
        element, attribute, QStringLiteral("expected an integer in [%1, %2]").arg(min).arg(max));
    return false;
  }
  value = parsed;
  return true;
}

}  // namespace PJ::scene2d_xml
