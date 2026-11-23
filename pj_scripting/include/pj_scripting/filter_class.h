// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace PJ::scripting {

/// Parameter widget kinds of the generated `ParameterForm`. The schema is data read off a filter class's `parameters`
/// table, so the host builds the editor without per-filter Qt. Contract: pj_scripting/docs/FILTER_CLASS.md.
enum class ParamType {
  kNumber,   ///< double  -> validated QLineEdit
  kInteger,  ///< int     -> QSpinBox (32-bit range)
  kBoolean,  ///< bool    -> PJ::ToggleSwitch
  kEnum,     ///< choice  -> PJ::ComboBox (stored value is the option's `value`, a typed JSON scalar)
  kString,   ///< text    -> QLineEdit (single line)
  kText,     ///< text    -> QPlainTextEdit (multi-line, e.g. a custom expression)
};

/// One enum option: the value persisted in params JSON plus its UI label. The value is a typed JSON scalar so an
/// int-keyed enum (binary_filter's `binary_op`) round-trips as a number, not the string "3".
struct EnumValue {
  nlohmann::json value;
  std::string label;
};

/// One parameter, read from a class's `parameters` entry without instantiating the filter; `name` is also the JSON key
/// in `saveParams()` / `loadParams()`.
struct ParamSpec {
  std::string name;  ///< params-table key and JSON key
  ParamType type = ParamType::kNumber;
  std::string label;  ///< UI label; falls back to `name` when empty
  std::string tooltip;
  nlohmann::json default_value;          ///< typed default (number/int/bool/string)
  std::optional<double> min, max, step;  ///< bounds; `step` only for an integer's spin box
  std::optional<int> decimals;           ///< decimals a number accepts
  std::string unit;                      ///< e.g. "s"; the generated form does not display it
  std::vector<EnumValue> values;         ///< enum options (kEnum only)

  /// The row shows only while parameter `visible_when_param` equals `visible_when_equals`.
  std::optional<std::string> visible_when_param;
  nlohmann::json visible_when_equals;
};

/// A filter class read off a module without running `create()` / `calculate()`. `source` is the full module text, so an
/// instance can be built later and a layout can embed it.
struct FilterClass {
  std::string id;    ///< stable catalogue key (e.g. "derivative")
  std::string name;  ///< human type name
  std::string description;
  std::string version;
  std::string output_kind = "double";  ///< "double" | "int64" | "same" | "string"
  std::vector<ParamSpec> parameters;
  std::string source;  ///< full module text the class was parsed from
  std::string origin;  ///< provenance label (e.g. "bundled")

  /// The class returns text per sample, written to a string column.
  [[nodiscard]] bool textOutput() const {
    return output_kind == "string";
  }
};

}  // namespace PJ::scripting
