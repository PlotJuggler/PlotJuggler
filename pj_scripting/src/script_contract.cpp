// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "script_contract.h"

#include <algorithm>
#include <map>
#include <memory>
#include <utility>

#include "pj_base/builtin/field_table_registry.hpp"
#include "pj_scripting/memory_budget.h"

namespace PJ::scripting::detail {
namespace {

/// How the error of a run past its own memory cap (`BudgetLimits::mem_bytes`) starts.
constexpr std::string_view kVmLimitError = "script memory limit";

/// True when `value` nests objects or arrays deeper than `levels`.
bool nestedDeeperThan(const nlohmann::json& value, int levels) {
  if (!value.is_structured()) {
    return false;
  }
  return levels == 0 || std::any_of(value.begin(), value.end(), [&](const nlohmann::json& element) {
           return nestedDeeperThan(element, levels - 1);
         });
}

std::string positionalName(const ScriptValue& value) {
  if (const auto* object = std::get_if<sdk::BuiltinObject>(&value)) {
    switch (object->type()) {
      case sdk::BuiltinObjectType::kPointCloud:
        return "cloud";
      case sdk::BuiltinObjectType::kSceneEntities:
        return "scene";
      case sdk::BuiltinObjectType::kImageAnnotations:
        return "annotations";
      case sdk::BuiltinObjectType::kImage:
        return "image";
      default:
        return "object";
    }
  }
  return std::holds_alternative<std::string>(value) ? "text" : "value";
}

bool hasType(const OutputSpec& spec, const ScriptValue& value) {
  switch (spec.type) {
    case OutputType::kNumber:
      return std::holds_alternative<double>(value);
    case OutputType::kString:
      return std::holds_alternative<std::string>(value);
    case OutputType::kObject: {
      const auto* object = std::get_if<sdk::BuiltinObject>(&value);
      return object != nullptr && object->type() == spec.object_type;
    }
  }
  return false;
}

std::string typeText(const OutputSpec& spec) {
  switch (spec.type) {
    case OutputType::kNumber:
      return "a number";
    case OutputType::kString:
      return "a string";
    case OutputType::kObject:
      break;
  }
  return "a " + std::string(sdk::name(spec.object_type));
}

Expected<ScriptBundle> inferOutputs(std::vector<Returned>& returned) {
  if (returned.empty()) {
    return unexpected(std::string("the script returned no values"));
  }
  ScriptBundle bundle;
  std::map<std::string, int> seen;
  for (Returned& item : returned) {
    std::string name = item.key.empty() ? "value" : item.key;
    if (item.position > 0) {
      if (item.value && std::holds_alternative<Unavailable>(*item.value)) {
        return unexpected(
            "output #" + std::to_string(item.position) + " is unavailable: name the output to mark it unavailable");
      }
      const std::string base = item.value ? positionalName(*item.value) : "value";
      const int count = ++seen[base];
      name = count == 1 ? base : base + "_" + std::to_string(count);
    }
    if (!item.value) {
      return unexpected("output '" + name + "' has an unsupported type: " + item.type_name);
    }
    if (const auto* object = std::get_if<sdk::BuiltinObject>(&*item.value); object && !producible(object->type())) {
      return unexpected(
          "output '" + name + "' is a " + std::string(sdk::name(object->type())) + ", a type a script cannot produce");
    }
    bundle.outputs.push_back(NamedValue{std::move(name), std::move(*item.value)});
  }
  if (!returned.front().key.empty()) {  // keyed: by name; positional: in order
    std::ranges::sort(bundle.outputs, {}, &NamedValue::name);
  }
  return bundle;
}

Expected<ScriptBundle> bindDeclared(std::vector<Returned>& returned, Span<const OutputSpec> declared) {
  std::vector<std::string> names;
  for (size_t index = 0; index < returned.size(); ++index) {
    const Returned& item = returned[index];
    std::string name = item.key;
    if (item.position > 0) {
      name = index < declared.size() ? declared[index].name : "#" + std::to_string(item.position);
    } else if (item.key.empty()) {
      name = declared.size() == 1 ? declared.front().name : "value";
    }
    if (std::ranges::find(declared, name, &OutputSpec::name) == declared.end()) {
      return unexpected("unexpected output '" + name + "'");
    }
    names.push_back(std::move(name));
  }
  ScriptBundle bundle;
  for (const OutputSpec& spec : declared) {
    const auto found = std::ranges::find(names, spec.name);
    if (found == names.end()) {
      return unexpected("output '" + spec.name + "' missing");
    }
    std::optional<ScriptValue>& value = returned[static_cast<size_t>(found - names.begin())].value;
    if (!value || (!std::holds_alternative<Unavailable>(*value) && !hasType(spec, *value))) {
      return unexpected("output '" + spec.name + "' must be " + typeText(spec));
    }
    bundle.outputs.push_back(NamedValue{spec.name, std::move(*value)});
  }
  return bundle;
}

}  // namespace

std::string vmLimitMessage(std::size_t limit_bytes) {
  return std::string(kVmLimitError) + " (" + native::describeBytes(limit_bytes) + ") exceeded";
}

bool isOverBudget(std::string_view message) {
  return message.find(native::kNativeLimitError) != std::string_view::npos || message.starts_with(kVmLimitError);
}

bool producible(sdk::BuiltinObjectType type) {
  return sdk::describe(type) != nullptr && type != sdk::BuiltinObjectType::kVideoFrame &&
         type != sdk::BuiltinObjectType::kFrameTransforms;
}

Expected<nlohmann::json> parseParams(std::string_view params_json) {
  nlohmann::json params =
      nlohmann::json::parse(params_json.empty() ? std::string_view("{}") : params_json, nullptr, false);
  if (!params.is_object()) {
    return unexpected(std::string("params_json is not a JSON object"));
  }
  if (nestedDeeperThan(params, kMaxParamsDepth)) {
    return unexpected("params_json is nested deeper than " + std::to_string(kMaxParamsDepth) + " levels");
  }
  return params;
}

std::shared_ptr<native::MemoryBudget> nativeBudgetOf(const RunOptions& options) {
  return options.native_budget ? options.native_budget
                               : std::make_shared<native::MemoryBudget>(options.native_budget_bytes);
}

Expected<ScriptBundle> bindReturned(std::vector<Returned>& returned, Span<const OutputSpec> declared) {
  return declared.empty() ? inferOutputs(returned) : bindDeclared(returned, declared);
}

}  // namespace PJ::scripting::detail
