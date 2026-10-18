// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The Luau object-script engine (see object_script.h): a fresh sandboxed VM per run, the `pj` table,
// and the binding of the returned value to outputs. Objects and their methods are object_binder.cpp's.

#include "pj_scripting/object_script.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>

#include "lua.h"
#include "luacode.h"
#include "lualib.h"
#include "luau_sandbox_state.h"
#include "native/memory_budget.h"
#include "object_binder.h"
#include "pj_base/builtin/field_table_registry.hpp"

namespace PJ::scripting {
namespace {

// `pj` chains stay lazily indexed: with safeenv on, Luau would otherwise fold `pj.tf.now` into a constant
// resolved once at load, observing the clock whether or not the branch that reads it runs.
constexpr const char* const kMutableGlobals[] = {"pj", nullptr};

// Each level of `params` takes Lua stack slots while it is pushed, before the protected call.
constexpr int kMaxParamsDepth = 32;

std::string_view trim(std::string_view text) {
  const size_t begin = text.find_first_not_of(" \t");
  if (begin == std::string_view::npos) {
    return {};
  }
  return text.substr(begin, text.find_last_not_of(" \t") - begin + 1);
}

/// Object types an output can declare: those with a field table, except the ones whose publication no
/// slice has designed yet (a video frame, slice 9; frame transforms, which the TF ingest tap would read).
bool producible(sdk::BuiltinObjectType type) {
  return sdk::describe(type) != nullptr && type != sdk::BuiltinObjectType::kVideoFrame &&
         type != sdk::BuiltinObjectType::kFrameTransforms;
}

// ---- pj and pj.tf ---------------------------------------------------------------

/// What one run's `pj.tf` reads and records; the closures reach it through a light userdata upvalue.
struct Run {
  const RunOptions* options = nullptr;
  bool observed_query_clock = false;
  bool observed_tf = false;
};

const char* lookupErrorText(pj::tf::LookupError error) {
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

int pushFailure(lua_State* L, const char* reason) {
  lua_pushnil(L);
  lua_pushstring(L, reason);
  return 2;
}

/// `pj.tf.lookup(target, source[, when])`: see object_script.h.
int lTfLookup(lua_State* L) {
  Run& run = detail::upvalueAs<Run>(L);
  const std::string target = luaL_checkstring(L, 1);
  const std::string source = luaL_checkstring(L, 2);
  Timestamp instant = run.options->query_time;
  pj::tf::Clock clock = pj::tf::Clock::kEntry;
  run.observed_tf = true;  // even with no snapshot: one appearing later changes the answer
  if (lua_isnoneornil(L, 3)) {
    run.observed_query_clock = true;
  } else if (const sdk::BuiltinObject* object = detail::toObject(L, 3)) {
    const std::optional<Timestamp> stamp = detail::ownStamp(*object);
    if (!stamp || *stamp == 0) {
      return pushFailure(L, "object has no stamp");
    }
    instant = *stamp;
    clock = pj::tf::Clock::kHeader;
  } else if (const std::optional<int64_t> boxed = detail::toInt64(L, 3)) {
    instant = *boxed;
  } else {
    const double ns = luaL_checknumber(L, 3);
    if (!(std::abs(ns) < 9.2e18)) {
      luaL_error(L, "pj.tf.lookup: time out of range");
    }
    instant = static_cast<Timestamp>(ns);
  }
  if (run.options->tf == nullptr) {
    return pushFailure(L, "no tf snapshot");
  }
  const auto found = run.options->tf->tryLookupTransform(target, source, PJ::fromRaw(instant), clock);
  if (!found) {
    return pushFailure(L, lookupErrorText(found.error()));
  }
  lua_createtable(L, 0, 3);
  detail::pushArray(L, {found->t.x, found->t.y, found->t.z});
  lua_setfield(L, -2, "translation");
  detail::pushArray(L, {found->q.x, found->q.y, found->q.z, found->q.w});
  lua_setfield(L, -2, "rotation");
  lua_pushlstring(L, target.data(), target.size());
  lua_setfield(L, -2, "target_frame");
  return 1;
}

int lTfFrames(lua_State* L) {
  Run& run = detail::upvalueAs<Run>(L);
  run.observed_tf = true;
  const std::vector<std::string> frames =
      run.options->tf != nullptr ? run.options->tf->getAllFrames() : std::vector<std::string>{};
  lua_createtable(L, static_cast<int>(frames.size()), 0);
  for (size_t index = 0; index < frames.size(); ++index) {
    lua_pushlstring(L, frames[index].data(), frames[index].size());
    lua_rawseti(L, -2, static_cast<int>(index) + 1);
  }
  return 1;
}

/// `pj.tf`'s `__index`: `now` is read through it, so every read is observed when it happens.
int lTfIndex(lua_State* L) {
  Run& run = detail::upvalueAs<Run>(L);
  if (lua_type(L, 2) == LUA_TSTRING && std::string_view(lua_tostring(L, 2)) == "now") {
    run.observed_query_clock = true;
    detail::pushInt64(L, run.options->query_time);
    return 1;
  }
  lua_pushnil(L);
  return 1;
}

int lUnavailable(lua_State* L) {
  std::size_t length = 0;
  const char* reason = luaL_optlstring(L, 1, "", &length);
  detail::pushUnavailable(L, std::string_view(reason, length));
  return 1;
}

void installPj(lua_State* L, Run& run) {
  lua_createtable(L, 0, 2);
  detail::setFunctions(L, {{"unavailable", lUnavailable}});
  lua_createtable(L, 0, 2);
  detail::setFunctions(L, {{"lookup", lTfLookup}, {"frames", lTfFrames}}, &run);
  lua_createtable(L, 0, 1);
  detail::setFunctions(L, {{"__index", lTfIndex}}, &run);
  lua_setmetatable(L, -2);
  lua_setfield(L, -2, "tf");
  lua_setglobal(L, "pj");
  // luaopen_math seeds math.random from the clock and an address; a result must depend on its inputs only.
  lua_getglobal(L, "math");
  lua_getfield(L, -1, "randomseed");
  lua_pushinteger(L, 0);
  lua_call(L, 1, 0);
  lua_pop(L, 1);
}

// ---- inputs ---------------------------------------------------------------------

/// True when `value` nests objects or arrays deeper than `levels`.
bool nestedDeeperThan(const nlohmann::json& value, int levels) {
  if (!value.is_structured()) {
    return false;
  }
  return levels == 0 || std::any_of(value.begin(), value.end(), [&](const nlohmann::json& element) {
           return nestedDeeperThan(element, levels - 1);
         });
}

void pushJson(lua_State* L, const nlohmann::json& value) {
  lua_checkstack(L, 2);
  if (value.is_object()) {
    lua_createtable(L, 0, static_cast<int>(value.size()));
    for (auto it = value.begin(); it != value.end(); ++it) {
      pushJson(L, it.value());
      lua_setfield(L, -2, it.key().c_str());
    }
  } else if (value.is_array()) {
    lua_createtable(L, static_cast<int>(value.size()), 0);
    int position = 1;
    for (const auto& element : value) {
      pushJson(L, element);
      lua_rawseti(L, -2, position++);
    }
  } else if (value.is_boolean()) {
    lua_pushboolean(L, value.get<bool>());
  } else if (value.is_number()) {
    lua_pushnumber(L, value.get<double>());
  } else if (value.is_string()) {
    const std::string& text = value.get_ref<const std::string&>();
    lua_pushlstring(L, text.data(), text.size());
  } else {
    lua_pushnil(L);
  }
}

/// Pushes a scalar or object input; the caller has rejected unavailable inputs.
void pushInput(lua_State* L, const NamedValue& input) {
  if (const auto* number = std::get_if<double>(&input.value)) {
    lua_pushnumber(L, *number);
  } else if (const auto* text = std::get_if<std::string>(&input.value)) {
    lua_pushlstring(L, text->data(), text->size());
  } else {
    detail::pushObject(L, detail::withStampShifted(std::get<sdk::BuiltinObject>(input.value), input.stamp_shift));
  }
}

/// Why `input` cannot enter a run, or empty.
std::string inputProblem(const NamedValue& input) {
  if (std::holds_alternative<Unavailable>(input.value)) {
    return "input '" + input.name + "' is unavailable";
  }
  const auto* object = std::get_if<sdk::BuiltinObject>(&input.value);
  if (object != nullptr && !detail::canEnterScript(*object)) {
    return "input '" + input.name + "' is a " + std::string(sdk::name(object->type())) + ", which scripts cannot read";
  }
  return {};
}

// ---- binding the returned value -------------------------------------------------

/// One returned value: its key in the returned table, or its position (1-based), or neither for the
/// returned value itself; nullopt `value` for a type no output holds.
struct Returned {
  std::string key;
  int position = 0;
  std::optional<ScriptValue> value;
  std::string type_name;
};

/// Copies the value at `index` into `item`. A string leaving the VM is charged to `charge` first, so a
/// script cannot multiply one interned string into gigabytes of copies.
Status readValue(lua_State* L, int index, native::MemoryReservation& charge, Returned& item) {
  item.type_name = luaL_typename(L, index);
  std::size_t length = 0;
  if (std::optional<std::string> reason = detail::toUnavailable(L, index)) {
    if (Status charged = charge.grow(reason->size()); !charged) {
      return charged;
    }
    item.value = Unavailable{std::move(*reason)};
  } else if (lua_type(L, index) == LUA_TNUMBER) {
    item.value = lua_tonumber(L, index);
  } else if (lua_type(L, index) == LUA_TSTRING) {
    const char* text = lua_tolstring(L, index, &length);
    if (Status charged = charge.grow(length); !charged) {
      return charged;
    }
    item.value = std::string(text, length);
  } else if (const sdk::BuiltinObject* object = detail::toObject(L, index)) {
    item.value = *object;
  }
  return okStatus();
}

/// The returned value at `result`, one item per output candidate: a non-table is one item; a table is
/// either keyed or an array starting at 1.
Expected<std::vector<Returned>> collectReturned(lua_State* L, int result, native::MemoryReservation& charge) {
  std::vector<Returned> returned(1);
  if (!lua_istable(L, result) || detail::toUnavailable(L, result)) {  // a marker is a table too
    if (Status read = readValue(L, result, charge, returned.front()); !read) {
      return unexpected(read.error());
    }
    return returned;
  }
  returned.clear();
  bool keyed = false;
  bool positional = false;
  lua_pushnil(L);
  while (lua_next(L, result) != 0) {
    Returned item;
    if (lua_type(L, -2) == LUA_TSTRING) {
      std::size_t length = 0;
      const char* key = lua_tolstring(L, -2, &length);
      item.key.assign(key, length);
      keyed = true;
    } else {
      const double position = lua_type(L, -2) == LUA_TNUMBER ? lua_tonumber(L, -2) : 0.0;
      if (!(position >= 1 && position <= INT_MAX && std::floor(position) == position)) {
        return unexpected(std::string("returned table keys must be names or array positions"));
      }
      item.position = static_cast<int>(position);
      positional = true;
    }
    if (Status read = readValue(L, -1, charge, item); !read) {
      return unexpected(read.error());
    }
    lua_pop(L, 1);  // the value; lua_next needs the key
    returned.push_back(std::move(item));
  }
  if (keyed && positional) {
    return unexpected(std::string("return either named or positional values, not both"));
  }
  std::ranges::sort(returned, {}, &Returned::position);
  for (size_t index = 0; positional && index < returned.size(); ++index) {
    if (returned[index].position != static_cast<int>(index) + 1) {
      return unexpected(std::string("positional values must be an array starting at 1, without holes"));
    }
  }
  return returned;
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

/// No declared outputs: every returned value is an output, named by the inference rule.
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

/// Declared outputs: a non-table binds to the single declaration, a position to the declaration at that
/// position, a key to the declaration of that name.
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

Expected<OutputSpec> parseOutputSpec(std::string_view spec) {
  const std::string quoted = "output spec '" + std::string(spec) + "'";
  const std::string_view trimmed = trim(spec);
  const size_t colon = trimmed.find(':');
  if (colon == std::string_view::npos) {
    return unexpected(quoted + " is missing ':type'");
  }
  if (trimmed.find(':', colon + 1) != std::string_view::npos) {
    return unexpected(quoted + " has more than one ':'");
  }
  const std::string_view name = trim(trimmed.substr(0, colon));
  const std::string_view type = trim(trimmed.substr(colon + 1));
  if (name.empty() || name.find_first_of(" \t") != std::string_view::npos) {
    return unexpected(quoted + " has an empty or invalid name");
  }
  if (type.empty()) {
    return unexpected(quoted + " has an empty type");
  }
  OutputSpec out{std::string(name)};
  if (type == "number") {
    out.type = OutputType::kNumber;
  } else if (type == "string") {
    out.type = OutputType::kString;
  } else {
    const auto object_type = sdk::parseBuiltinObjectType(type);
    if (!object_type) {
      return unexpected(quoted + " has an unknown type '" + std::string(type) + "'");
    }
    if (!producible(*object_type)) {
      return unexpected(quoted + " names " + std::string(type) + ", a type a script cannot produce");
    }
    out.type = OutputType::kObject;
    out.object_type = *object_type;
  }
  return out;
}

Expected<CompiledScript> compileOnDemandScript(std::string_view source) {
  if (source.size() > detail::kMaxSourceBytes) {
    return unexpected("script larger than " + native::describeBytes(detail::kMaxSourceBytes));
  }
  lua_CompileOptions options = {};
  options.optimizationLevel = 1;
  options.debugLevel = 1;  // line numbers in errors
  options.mutableGlobals = kMutableGlobals;
  std::size_t size = 0;
  char* bytecode = luau_compile(source.data(), source.size(), &options, &size);
  if (bytecode == nullptr) {
    return unexpected(std::string("luau_compile failed"));
  }
  CompiledScript compiled{std::vector<char>(bytecode, bytecode + size)};
  std::free(bytecode);
  // A syntax error compiles to bytecode version 0 followed by the message (":<line>: ..."), which
  // luau_load would raise on every run.
  if (!compiled.bytecode.empty() && compiled.bytecode.front() == 0) {
    return unexpected("script" + std::string(compiled.bytecode.begin() + 1, compiled.bytecode.end()));
  }
  return compiled;
}

Expected<ScriptBundle, ScriptError> runOnDemandScript(
    const CompiledScript& script, Span<const NamedValue> inputs, std::string_view params_json,
    Span<const OutputSpec> outputs, const RunOptions& options) {
  Run run{&options};
  const auto fail = [&run](std::string message) {
    return unexpected(ScriptError{std::move(message), run.observed_query_clock, run.observed_tf});
  };
  // Nothing below may raise a Lua error outside the protected call: reject what would, first.
  for (const NamedValue& input : inputs) {
    if (std::string problem = inputProblem(input); !problem.empty()) {
      return fail(std::move(problem));
    }
  }
  const nlohmann::json params =
      nlohmann::json::parse(params_json.empty() ? std::string_view("{}") : params_json, nullptr, false);
  if (!params.is_object()) {
    return fail("params_json is not a JSON object");
  }
  if (nestedDeeperThan(params, kMaxParamsDepth)) {
    return fail("params_json is nested deeper than " + std::to_string(kMaxParamsDepth) + " levels");
  }

  const native::NativeLimits native_limits{
      std::make_shared<native::MemoryBudget>(options.native_budget_bytes), options.cancelled};
  detail::BudgetState budget;
  budget.cancelled = options.cancelled ? &options.cancelled : nullptr;
  std::unique_ptr<lua_State, decltype(&lua_close)> state(
      detail::newSandboxedState(&budget, options.limits), &lua_close);
  lua_State* L = state.get();
  if (L == nullptr) {
    return fail("failed to create a Luau state");
  }
  budget.max_bytes = std::numeric_limits<std::size_t>::max();  // the host's own setup is not capped
  detail::registerBinder(L, native_limits);
  installPj(L, run);
  if (luau_load(L, "=script", script.bytecode.data(), script.bytecode.size(), 0) != 0) {
    return fail(lua_isstring(L, -1) ? lua_tostring(L, -1) : "luau_load failed");
  }
  lua_createtable(L, 0, static_cast<int>(inputs.size()));
  for (const NamedValue& input : inputs) {
    pushInput(L, input);
    lua_setfield(L, -2, input.name.c_str());
  }
  pushJson(L, params);

  budget.max_bytes = options.limits.mem_bytes;
  detail::armFuel(&budget, options.limits.setup_fuel);
  const int status = lua_pcall(L, 2, 1, 0);
  budget.max_bytes = std::numeric_limits<std::size_t>::max();  // reading the result back is host work again
  if (budget.trip != nullptr) {                                // even when the script pcall-swallowed it
    return fail(budget.trip);
  }
  if (status == LUA_ERRMEM) {
    return fail("script memory limit (" + native::describeBytes(options.limits.mem_bytes) + ") exceeded");
  }
  if (status != LUA_OK) {
    return fail(lua_isstring(L, -1) ? lua_tostring(L, -1) : "script error");
  }
  native::MemoryReservation strings(native_limits.budget);
  Expected<std::vector<Returned>> returned = collectReturned(L, lua_gettop(L), strings);
  if (!returned) {
    return fail(returned.error());
  }
  Expected<ScriptBundle> bundle = outputs.empty() ? inferOutputs(*returned) : bindDeclared(*returned, outputs);
  if (!bundle) {
    return fail(bundle.error());
  }
  bundle->observed_query_clock = run.observed_query_clock;
  bundle->observed_tf = run.observed_tf;
  return std::move(*bundle);
}

}  // namespace PJ::scripting
