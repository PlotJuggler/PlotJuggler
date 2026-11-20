// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QString>
#include <span>
#include <vector>

#include "pj_scripting/filter_class.h"

namespace PJ {

/// UI-only copy of a filter's metadata. Canonical classes, scripts and parameter
/// values remain untranslated so language changes cannot alter saved filters.
struct BuiltinFilterPresentation {
  QString name;
  QString description;
  std::vector<scripting::ParamSpec> parameters;
};

/// Translate known bundled metadata only. Embedded/custom classes and unknown
/// strings retain their source text; the generic ParameterForm stays language-neutral.
[[nodiscard]] BuiltinFilterPresentation builtinFilterPresentation(const scripting::FilterClass& filter);

/// Extractable display keys for the shipped Luau resource. Resource-coverage
/// tests compare its actual metadata with this registry to catch new omissions.
[[nodiscard]] std::span<const char* const> builtinFilterTranslationSources();

}  // namespace PJ
