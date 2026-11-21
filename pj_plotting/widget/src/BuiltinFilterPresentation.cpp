// SPDX-License-Identifier: MPL-2.0
#include "pj_plotting/BuiltinFilterPresentation.h"

#include <QCoreApplication>
#include <algorithm>
#include <iterator>
#include <string_view>

namespace PJ {
namespace {

// Luau metadata is not parsed by lupdate. Keep the English resource authoritative
// and expose its display strings here without translating ids or executable code.
constexpr const char* kTranslationSources[] = {
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "None"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Pass-through (no change)."),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Absolute"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Absolute value |y|."),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Scale"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Affine scale + offset, with a time shift."),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Offset"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Time offset"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Derivative"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Rate of change dv/dt."),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Use fixed Δt"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Fixed Δt"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Integral"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Running trapezoidal integral of y over time."),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Moving Average"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Boxcar moving average."),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Window"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Center timestamp"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Moving Root Mean Squared"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Moving RMS over a window."),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Moving Variance / Stdev"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Moving variance or standard deviation."),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Standard deviation"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Outlier Removal"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Drop single-sample spikes."),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Factor"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Samples Counter"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Count of samples in a trailing time window."),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Binary Filter"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Boolean 0/1 predicate over the value."),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Comparison"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "== a"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "< a"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "<= a"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "> a"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", ">= a"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "in [a, b]"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Threshold a"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Threshold b"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Tolerance (==)"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Time Since Previous"),
    QT_TRANSLATE_NOOP("PJ::BuiltinFilters", "Δt between consecutive samples (seconds)."),
};

constexpr std::string_view kBuiltinIds[] = {
    "none",       "absolute",        "scale",           "derivative",      "integral",      "moving_average",
    "moving_rms", "moving_variance", "outlier_removal", "samples_counter", "binary_filter", "time_since_previous",
};

/// Translate only registered display text; never guess at unknown metadata.
QString displayText(const std::string& source) {
  const auto key = std::find_if(
      std::begin(kTranslationSources), std::end(kTranslationSources), [&](const char* text) { return source == text; });
  return key == std::end(kTranslationSources) ? QString::fromStdString(source)
                                              : QCoreApplication::translate("PJ::BuiltinFilters", *key);
}

}  // namespace

BuiltinFilterPresentation builtinFilterPresentation(const scripting::FilterClass& filter) {
  BuiltinFilterPresentation display{
      QString::fromStdString(filter.name), QString::fromStdString(filter.description), filter.parameters};
  if (filter.origin != "bundled" ||
      std::find(std::begin(kBuiltinIds), std::end(kBuiltinIds), filter.id) == std::end(kBuiltinIds)) {
    return display;
  }
  display.name = displayText(filter.name);
  display.description = displayText(filter.description);
  for (scripting::ParamSpec& parameter : display.parameters) {
    parameter.label = displayText(parameter.label).toStdString();
    parameter.tooltip = displayText(parameter.tooltip).toStdString();
    for (scripting::EnumValue& choice : parameter.values) {
      choice.label = displayText(choice.label).toStdString();
    }
  }
  return display;
}

std::span<const char* const> builtinFilterTranslationSources() {
  return kTranslationSources;
}

}  // namespace PJ
