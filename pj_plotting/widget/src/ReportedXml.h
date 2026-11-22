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
// absent), for a read whose layout-load mapping differs from the shared readers' "bad value reads as the fallback".
// It never refuses; a present, non-empty value the caller would replace (xmlValueProblem, or an invalid "color") is
// noted as a rejection, so widget_state refuses what a layout load tolerates.
inline QString readReported(const QDomElement& element, XmlOption option) {
  QString value = element.attribute(option.attribute, option.fallback);
  if (!XmlOptionsRecorder::active()) {
    return value;
  }
  QString reason;
  if (element.hasAttribute(option.attribute) && !value.isEmpty()) {
    if (option.type == QLatin1String("color")) {
      if (!QColor(value).isValid()) {
        reason = QStringLiteral("expected a colour (#rrggbb or a name)");
      }
    } else {
      reason = xmlValueProblem(option, value);
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
