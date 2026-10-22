// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The Luau object-script engine (see object_script.h): one sandboxed state per thread and a fresh thread on it per
// run, the `pj` table, and the binding of the returned value to outputs. Objects and their methods are
// object_binder.cpp's.

#include "pj_scripting/object_script.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>

#include "lua.h"
#include "luacode.h"
#include "lualib.h"
#include "luau_sandbox_state.h"
#include "object_binder.h"
#include "pj_base/builtin/field_table_registry.hpp"
#include "pj_scripting/memory_budget.h"
#include "script_contract.h"
#include "script_media.h"

namespace PJ::scripting {
namespace {

// `pj` chains stay lazily indexed: with safeenv on, Luau would otherwise fold `pj.tf.now` into a constant
// resolved once at load, observing the clock whether or not the branch that reads it runs.
constexpr const char* const kMutableGlobals[] = {"pj", nullptr};

std::string_view trim(std::string_view text) {
  const size_t begin = text.find_first_not_of(" \t");
  if (begin == std::string_view::npos) {
    return {};
  }
  return text.substr(begin, text.find_last_not_of(" \t") - begin + 1);
}

// ---- pj and pj.tf ---------------------------------------------------------------

/// What one run's `pj.tf` reads and records; the closures reach it through a light userdata upvalue.
struct Run {
  const RunOptions* options = nullptr;
  bool observed_query_clock = false;
  bool observed_tf = false;
};

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
    return pushFailure(L, detail::tfLookupErrorText(found.error()));
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
  lua_createtable(L, 0, 6);
  detail::setFunctions(L, {{"unavailable", lUnavailable}});
  detail::installBuilders(L);
  lua_createtable(L, 0, 2);
  detail::setFunctions(L, {{"lookup", lTfLookup}, {"frames", lTfFrames}}, &run);
  lua_createtable(L, 0, 1);
  detail::setFunctions(L, {{"__index", lTfIndex}}, &run);
  lua_setmetatable(L, -2);
  lua_setfield(L, -2, "tf");
  lua_setglobal(L, "pj");
  // luaopen_math seeds math.random from the clock and an address, and the state's earlier runs moved it on; a result
  // must depend on its inputs only.
  lua_getglobal(L, "math");
  lua_getfield(L, -1, "randomseed");
  lua_pushinteger(L, 0);
  lua_call(L, 1, 0);
  lua_pop(L, 1);
}

// ---- one state per thread (F8) ----------------------------------------------------

/// A thread's sandboxed state: the frozen stdlib and the binder, made on its first run. Every run gets a thread of
/// its own on it, with its own globals, and leaves nothing behind (`EndRun`).
struct ThreadState {
  detail::BudgetState budget;  ///< the state's allocator and watchdog; re-armed by every run
  lua_State* L = nullptr;
  bool busy = false;  ///< a run is on it: a nested run (none today) takes a state of its own

  ThreadState() = default;
  ThreadState(const ThreadState&) = delete;
  ThreadState& operator=(const ThreadState&) = delete;
  ~ThreadState() {
    close();
  }

  void close() {
    if (L != nullptr) {
      lua_close(L);
      L = nullptr;
    }
  }
};

/// Ends a run on its `ThreadState`: drops the run's thread and collects in full, so the objects the run left in the VM
/// release their inputs, frames and native charges now, and the next run's memory cap starts from the bare state. A
/// run the allocator failed inside (`drop`) closes the state instead; the next run makes a new one.
struct EndRun {
  ThreadState& state;
  bool drop = false;

  ~EndRun() {
    state.budget.max_bytes = std::numeric_limits<std::size_t>::max();
    state.budget.cancelled = nullptr;  // the run's check dies with it
    if (drop) {
      state.close();
    } else {
      lua_settop(state.L, 0);
      lua_gc(state.L, LUA_GCCOLLECT, 0);
    }
    state.busy = false;
  }
};

// ---- inputs ---------------------------------------------------------------------

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

/// Pushes a scalar or object input, and notes an image or video input in `media`, keeping it in `held`; the caller has
/// rejected unavailable inputs.
void pushInput(lua_State* L, const NamedValue& input, ScriptMedia& media) {
  if (const auto* number = std::get_if<double>(&input.value)) {
    lua_pushnumber(L, *number);
  } else if (const auto* text = std::get_if<std::string>(&input.value)) {
    lua_pushlstring(L, text->data(), text->size());
  } else {
    sdk::BuiltinObject object = detail::withStampShifted(std::get<sdk::BuiltinObject>(input.value), input.stamp_shift);
    media.noteInput(object, input);
    detail::pushObject(L, std::move(object));
  }
}

// ---- binding the returned value -------------------------------------------------

/// Copies the value at `index` into `item`. A string leaving the VM is charged to `charge` first, so a
/// script cannot multiply one interned string into gigabytes of copies.
Status readValue(lua_State* L, int index, native::MemoryReservation& charge, detail::Returned& item) {
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
    item.keepalive = detail::toCharge(L, index);
  }
  return okStatus();
}

/// The returned value at `result`, one item per output candidate: a non-table is one item; a table is
/// either keyed or an array starting at 1.
Expected<std::vector<detail::Returned>> collectReturned(lua_State* L, int result, native::MemoryReservation& charge) {
  std::vector<detail::Returned> returned(1);
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
    detail::Returned item;
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
  std::ranges::sort(returned, {}, &detail::Returned::position);
  for (size_t index = 0; positional && index < returned.size(); ++index) {
    if (returned[index].position != static_cast<int>(index) + 1) {
      return unexpected(std::string("positional values must be an array starting at 1, without holes"));
    }
  }
  return returned;
}

}  // namespace

std::string outputTypeName(const OutputSpec& spec) {
  switch (spec.type) {
    case OutputType::kNumber:
      return "number";
    case OutputType::kString:
      return "string";
    case OutputType::kObject:
      break;
  }
  return std::string(sdk::name(spec.object_type));
}

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
    if (!detail::producible(*object_type)) {
      return unexpected(quoted + " names " + std::string(type) + ", a type a script cannot produce");
    }
    out.type = OutputType::kObject;
    out.object_type = *object_type;
  }
  return out;
}

std::shared_ptr<native::MemoryBudget> makeNativeBudget(std::size_t limit_bytes) {
  return std::make_shared<native::MemoryBudget>(limit_bytes);
}

Expected<CompiledScript> compileOnDemandScript(std::string_view source) {
  if (source.size() > kMaxSourceBytes) {
    return unexpected("script larger than " + native::describeBytes(kMaxSourceBytes));
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
    const bool over_budget = detail::isOverBudget(message);
    return unexpected(ScriptError{std::move(message), run.observed_query_clock, run.observed_tf, over_budget});
  };
  // Nothing below may raise a Lua error outside the protected call: reject what would, first.
  for (const NamedValue& input : inputs) {
    if (std::string problem = detail::inputProblem(input); !problem.empty()) {
      return fail(std::move(problem));
    }
  }
  const Expected<nlohmann::json> params = detail::parseParams(params_json);
  if (!params) {
    return fail(params.error());
  }

  const native::NativeLimits native_limits{detail::nativeBudgetOf(options), options.cancelled};
  ScriptMedia media(options.image_decoder, native_limits, options.result_stamp, options.video_source);
  thread_local ThreadState worker_state;
  ThreadState nested_state;
  ThreadState& state = worker_state.busy ? nested_state : worker_state;
  if (state.L == nullptr) {
    state.L = detail::newFrozenState(&state.budget, options.limits.setup_fuel);
    if (state.L == nullptr) {
      return fail("failed to create a Luau state");
    }
    detail::registerBinder(state.L);
  }
  detail::BudgetState& budget = state.budget;
  budget.max_bytes = std::numeric_limits<std::size_t>::max();  // the host's own setup is not capped
  budget.cancelled = options.cancelled ? &options.cancelled : nullptr;
  state.busy = true;
  EndRun end_run{state};
  // The run's thread, anchored on the state's stack until `end_run`: globals of its own over the frozen stdlib.
  lua_State* L = lua_newthread(state.L);
  luaL_sandboxthread(L);
  detail::setRunMedia(L, media);
  installPj(L, run);
  if (luau_load(L, "=script", script.bytecode.data(), script.bytecode.size(), 0) != 0) {
    return fail(lua_isstring(L, -1) ? lua_tostring(L, -1) : "luau_load failed");
  }
  lua_createtable(L, 0, static_cast<int>(inputs.size()));
  for (const NamedValue& input : inputs) {
    pushInput(L, input, media);
    lua_setfield(L, -2, input.name.c_str());
  }
  pushJson(L, *params);

  budget.max_bytes = options.limits.mem_bytes;
  detail::armFuel(&budget, options.limits.setup_fuel);
  const int status = lua_pcall(L, 2, 1, 0);
  budget.max_bytes = std::numeric_limits<std::size_t>::max();  // reading the result back is host work again
  if (budget.trip != nullptr) {                                // even when the script pcall-swallowed it
    return fail(budget.trip);
  }
  if (status == LUA_ERRMEM) {
    end_run.drop = true;  // an allocation failed somewhere inside the VM: do not run on that state again
    return fail(detail::vmLimitMessage(options.limits.mem_bytes));
  }
  if (status != LUA_OK) {
    return fail(lua_isstring(L, -1) ? lua_tostring(L, -1) : "script error");
  }
  native::MemoryReservation strings(native_limits.budget);
  Expected<std::vector<detail::Returned>> returned = collectReturned(L, lua_gettop(L), strings);
  if (!returned) {
    return fail(returned.error());
  }
  Expected<ScriptBundle> bundle = detail::bindReturned(*returned, outputs);
  if (!bundle) {
    return fail(bundle.error());
  }
  bundle->observed_query_clock = run.observed_query_clock;
  bundle->observed_tf = run.observed_tf;
  return std::move(*bundle);
}

}  // namespace PJ::scripting
