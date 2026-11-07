// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "script_contract.h"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <utility>

#include "pj_base/builtin/field_table_registry.hpp"
#include "pj_datastore/saturating_time.hpp"
#include "pj_scripting/memory_budget.h"

namespace PJ::scripting::detail {
namespace {

/// Calls `visit` with the typed struct behind `object`, for each type with an SDK field table.
template <class... Types, class Visit>
bool visitAs(const sdk::BuiltinObject& object, Visit&& visit) {
  return ((object.get<Types>() != nullptr && (visit(*object.get<Types>()), true)) || ...);
}
template <class Visit>
bool visitDescribed(const sdk::BuiltinObject& object, Visit&& visit) {
  return visitAs<
      sdk::PointCloud, sdk::SceneEntities, sdk::ImageAnnotations, sdk::Image, sdk::DepthImage, sdk::CameraInfo,
      sdk::VideoFrame, sdk::FrameTransforms, sdk::VoxelGrid>(object, visit);
}

const sdk::FieldDescriptor* stampField(sdk::BuiltinObjectType type) {
  const sdk::FieldTableView* table = sdk::describe(type);
  if (table != nullptr) {
    for (const sdk::FieldDescriptor& field : table->fields) {
      if (field.kind == sdk::FieldKind::kInt64 && (field.name == "timestamp_ns" || field.name == "timestamp")) {
        return &field;
      }
    }
  }
  return nullptr;
}

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
      case sdk::BuiltinObjectType::kVoxelGrid:
        return "voxels";
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
    bundle.outputs.push_back(
        NamedValue{.name = std::move(name), .keepalive = std::move(item.keepalive), .value = std::move(*item.value)});
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
    bundle.outputs.push_back(
        NamedValue{
            .name = spec.name,
            .keepalive = std::move(returned[static_cast<size_t>(found - names.begin())].keepalive),
            .value = std::move(*value)});
  }
  return bundle;
}

}  // namespace

const void* structPointer(const sdk::BuiltinObject& object) {
  const void* pointer = nullptr;
  visitDescribed(object, [&](const auto& value) { pointer = &value; });
  return pointer;
}

bool canEnterScript(const sdk::BuiltinObject& object) {
  return sdk::describe(object.type()) != nullptr && structPointer(object) != nullptr;
}

std::string inputProblem(const NamedValue& input) {
  if (std::holds_alternative<Unavailable>(input.value)) {
    return "input '" + input.name + "' is unavailable";
  }
  const auto* object = std::get_if<sdk::BuiltinObject>(&input.value);
  if (object != nullptr && !canEnterScript(*object)) {
    return "input '" + input.name + "' is a " + std::string(sdk::name(object->type())) + ", which scripts cannot read";
  }
  return {};
}

std::optional<Timestamp> ownStamp(const sdk::BuiltinObject& object) {
  const sdk::FieldDescriptor* field = stampField(object.type());
  const void* pointer = structPointer(object);
  return field != nullptr && pointer != nullptr ? std::optional<Timestamp>(field->get_int64(pointer)) : std::nullopt;
}

sdk::BuiltinObject withStampShifted(const sdk::BuiltinObject& object, Timestamp shift) {
  const sdk::FieldDescriptor* field = stampField(object.type());
  if (shift == 0 || field == nullptr) {
    return object;
  }
  // On an image or a video packet 0 is a stamp, which annotations and frames pair with; elsewhere it means unstamped.
  const bool zero_is_a_stamp = object.type() == sdk::BuiltinObjectType::kImage ||
                               object.type() == sdk::BuiltinObjectType::kDepthImage ||
                               object.type() == sdk::BuiltinObjectType::kVideoFrame;
  sdk::BuiltinObject shifted = object;
  visitDescribed(object, [&](const auto& value) {
    const Timestamp stamp = field->get_int64(&value);
    if (stamp == 0 && !zero_is_a_stamp) {
      return;
    }
    auto copy = value;                                     // shares the payload bytes through their anchor
    field->set_int64(&copy, saturatingAdd(stamp, shift));  // a stamp near the limits must not wrap around
    shifted = sdk::BuiltinObject(std::move(copy));
  });
  return shifted;
}

std::optional<native::CompareOp> parseComparison(std::string_view op, std::string_view not_equal) {
  using Comparison = std::pair<std::string_view, native::CompareOp>;
  const std::array<Comparison, 6> comparisons{{
      {"<", native::CompareOp::kLt},
      {"<=", native::CompareOp::kLe},
      {">", native::CompareOp::kGt},
      {">=", native::CompareOp::kGe},
      {"==", native::CompareOp::kEq},
      {not_equal, native::CompareOp::kNe},
  }};
  const auto comparison = std::ranges::find(comparisons, op, &Comparison::first);
  return comparison != comparisons.end() ? std::optional(comparison->second) : std::nullopt;
}

std::optional<native::Extreme> parseExtreme(std::string_view which) {
  if (which == "max") {
    return native::Extreme::kMax;
  }
  return which == "min" ? std::optional(native::Extreme::kMin) : std::nullopt;
}

Expected<uint32_t> thresholdChannel(std::string_view name) {
  const std::size_t found = std::string_view("rgba").find(name);
  if (name.size() != 1 || found == std::string_view::npos) {
    return unexpected("threshold: unknown channel '" + std::string(name) + "' (expected r, g, b, a or 0-3)");
  }
  return static_cast<uint32_t>(found);
}

const char* tfLookupErrorText(pj::tf::LookupError error) {
  switch (error) {
    case pj::tf::LookupError::kUnknownSource:
      return "unknown source frame";
    case pj::tf::LookupError::kUnknownTarget:
      return "unknown target frame";
    case pj::tf::LookupError::kDisconnected:
      return "frames disconnected";
    case pj::tf::LookupError::kNoSampleAtTime:
      return "no sample at time";
  }
  return "tf lookup failed";
}

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
