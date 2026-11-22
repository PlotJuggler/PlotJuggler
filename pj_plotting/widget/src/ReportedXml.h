#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QDomElement>
#include <QString>
#include <utility>

#include "pj_runtime/XmlStateOptions.h"

namespace PJ::plotting_detail {

// Reports `option` to the active XmlOptionsRecorder and returns the attribute's raw value (`option.fallback` when
// absent). It never refuses: the caller maps the value as its xmlLoadState always has, unknown values included. A
// present value the caller would replace (not one of `values`, a number that does not parse or is out of bounds, an
// invalid colour) is also reported as a rejection, so widget_state refuses what a layout load tolerates.
inline QString readReported(const QDomElement& element, XmlOption option) {
  QString value = element.attribute(option.attribute, option.fallback);
  QString reason;
  if (element.hasAttribute(option.attribute) && !value.isEmpty()) {
    if (!option.values.isEmpty() && !option.values.contains(value)) {
      reason = QStringLiteral("expected one of ") + option.values.join(QStringLiteral(", "));
    } else if (option.type == QLatin1String("float") || option.type == QLatin1String("int")) {
      bool ok = false;
      const double number =
          option.type == QLatin1String("int") ? static_cast<double>(value.toLongLong(&ok)) : value.toDouble(&ok);
      if (!ok || (option.min.has_value() && number < *option.min) || (option.max.has_value() && number > *option.max)) {
        reason = option.min.has_value() && option.max.has_value()
                     ? QStringLiteral("expected a number in [%1, %2]").arg(*option.min).arg(*option.max)
                     : QStringLiteral("expected a number");
      }
    } else if (option.type == QLatin1String("color") && !QColor(value).isValid()) {
      reason = QStringLiteral("expected a colour (#rrggbb or a name)");
    }
  }
  const QString attribute = option.attribute;
  XmlOptionsRecorder::noteOption(element, std::move(option));
  if (!reason.isEmpty()) {
    XmlOptionsRecorder::noteRejection(element, attribute, reason);
  }
  return value;
}

}  // namespace PJ::plotting_detail
