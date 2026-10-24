// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Luau backend for the ScriptEngine seam. Hand-written binding (NOT sol2 — Luau
// has its own VM with C++-linkage symbols, see bench_luau.cpp). One lua_State per
// FilterInstance; the engine itself holds no per-instance state.
//
// Luau API gotchas baked in here:
//   - headers are C++ linkage, so NO `extern "C"` wrapper.
//   - lua_ref(L, idx) does NOT pop (unlike PUC luaL_ref) — we pop explicitly.
//   - lua_getref(L, ref) pushes the referenced value.

#include <algorithm>  // std::clamp
#include <cmath>      // std::isfinite
#include <cstdint>
#include <cstdlib>  // std::free
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "lua.h"
#include "luacode.h"
#include "lualib.h"
#include "luau_sandbox_state.h"
#include "pj_scripting/script_engine.h"

namespace PJ::scripting {
namespace {

using namespace detail;

// ---- small stack helpers -------------------------------------------------

// Length of the array part of the table at `idx` (counts 1..n until the first
// nil). Avoids depending on lua_objlen's exact spelling across Luau versions.
int arrayLen(lua_State* L, int idx) {
  int n = 0;
  while (true) {
    lua_rawgeti(L, idx, n + 1);
    const bool is_nil = lua_isnil(L, -1);
    lua_pop(L, 1);
    if (is_nil) {
      return n;
    }
    ++n;
  }
}

std::string stringField(lua_State* L, int table_idx, const char* key, const std::string& fallback = "") {
  lua_rawgetfield(L, table_idx, key);
  std::string out = fallback;
  if (lua_isstring(L, -1)) {
    out = lua_tostring(L, -1);
  }
  lua_pop(L, 1);
  return out;
}

std::optional<double> numberField(lua_State* L, int table_idx, const char* key) {
  lua_rawgetfield(L, table_idx, key);
  std::optional<double> out;
  if (lua_isnumber(L, -1)) {
    out = lua_tonumber(L, -1);
  }
  lua_pop(L, 1);
  return out;
}

// Top-of-stack Lua scalar -> JSON (numbers/bools/strings; else null).
nlohmann::json luaScalarToJson(lua_State* L, int idx) {
  switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN:
      return static_cast<bool>(lua_toboolean(L, idx));
    case LUA_TNUMBER:
      return lua_tonumber(L, idx);
    case LUA_TSTRING:
      return std::string(lua_tostring(L, idx));
    default:
      return nullptr;
  }
}

// Render a JSON scalar to a short label (the fallback when an enum option omits
// `label`): integers without a trailing ".0", others via dump().
std::string jsonScalarToString(const nlohmann::json& j) {
  if (j.is_string()) {
    return j.get<std::string>();
  }
  if (j.is_number_integer()) {
    return std::to_string(j.get<std::int64_t>());
  }
  if (j.is_boolean()) {
    return j.get<bool>() ? "true" : "false";
  }
  if (j.is_number()) {
    return std::to_string(j.get<double>());
  }
  return {};
}

void pushJsonScalar(lua_State* L, const nlohmann::json& j) {
  if (j.is_boolean()) {
    lua_pushboolean(L, j.get<bool>());
  } else if (j.is_number_integer()) {
    lua_pushnumber(L, static_cast<double>(j.get<std::int64_t>()));
  } else if (j.is_number()) {
    lua_pushnumber(L, j.get<double>());
  } else if (j.is_string()) {
    lua_pushstring(L, j.get<std::string>().c_str());
  } else {
    lua_pushnil(L);
  }
}

bool parseParamType(const std::string& s, ParamType& out) {
  if (s == "number" || s == "double" || s == "float") {
    out = ParamType::kNumber;
    return true;
  }
  if (s == "integer" || s == "int") {
    out = ParamType::kInteger;
    return true;
  }
  if (s == "boolean" || s == "bool") {
    out = ParamType::kBoolean;
    return true;
  }
  if (s == "enum" || s == "choice") {
    out = ParamType::kEnum;
    return true;
  }
  if (s == "string") {
    out = ParamType::kString;
    return true;
  }
  if (s == "text" || s == "multiline") {
    out = ParamType::kText;
    return true;
  }
  return false;
}

// Reads the `values` array of an enum param (entries are {value=, label=} or a
// bare string). Leaves the stack as found. `param_idx` is the param descriptor.
void readEnumValues(lua_State* L, int param_idx, ParamSpec& spec) {
  lua_rawgetfield(L, param_idx, "values");
  if (lua_istable(L, -1)) {
    const int values_idx = lua_gettop(L);
    const int count = arrayLen(L, values_idx);
    for (int i = 1; i <= count; ++i) {
      lua_rawgeti(L, values_idx, i);
      const int entry = lua_gettop(L);
      EnumValue ev;
      if (lua_istable(L, entry)) {
        lua_rawgetfield(L, entry, "value");
        ev.value = luaScalarToJson(L, lua_gettop(L));
        lua_pop(L, 1);
        ev.label = stringField(L, entry, "label", jsonScalarToString(ev.value));
      } else {  // a bare scalar entry: value == label
        ev.value = luaScalarToJson(L, entry);
        ev.label = jsonScalarToString(ev.value);
      }
      lua_pop(L, 1);  // entry
      if (!ev.value.is_null()) {
        spec.values.push_back(std::move(ev));
      }
    }
  }
  lua_pop(L, 1);  // values
}

// Parse the `parameters` array of the class table at `class_idx` into specs.
// Returns an error string on a malformed entry, empty on success.
std::string readParameters(lua_State* L, int class_idx, std::vector<ParamSpec>& out) {
  lua_rawgetfield(L, class_idx, "parameters");
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    return {};  // absent => no params
  }
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return "`parameters` must be a table";
  }
  const int params_idx = lua_gettop(L);
  const int count = arrayLen(L, params_idx);
  for (int i = 1; i <= count; ++i) {
    lua_rawgeti(L, params_idx, i);
    const int p = lua_gettop(L);
    if (!lua_istable(L, p)) {  // raw field reads below require a real table
      lua_pop(L, 2);           // entry + the `parameters` table
      return "a `parameters` entry is not a table";
    }
    ParamSpec spec;
    spec.name = stringField(L, p, "name");
    if (spec.name.empty()) {
      lua_pop(L, 2);  // param entry + the `parameters` table (leave stack as found)
      return "a parameter is missing its `name`";
    }
    const std::string type_str = stringField(L, p, "type", "number");
    if (!parseParamType(type_str, spec.type)) {
      lua_pop(L, 2);  // param entry + the `parameters` table
      return "unknown parameter type '" + type_str + "' for '" + spec.name + "'";
    }
    spec.label = stringField(L, p, "label", spec.name);
    spec.tooltip = stringField(L, p, "tooltip");
    spec.unit = stringField(L, p, "unit");
    spec.min = numberField(L, p, "min");
    spec.max = numberField(L, p, "max");
    spec.step = numberField(L, p, "step");
    if (auto dec = numberField(L, p, "decimals"); dec && std::isfinite(*dec)) {
      spec.decimals = static_cast<int>(std::clamp(*dec, 0.0, 17.0));  // guard NaN/huge → int UB
    }
    lua_rawgetfield(L, p, "default");
    spec.default_value = luaScalarToJson(L, lua_gettop(L));
    lua_pop(L, 1);
    if (spec.type == ParamType::kEnum) {
      readEnumValues(L, p, spec);
    }
    // visible_when = { param = "...", equals = <scalar> }
    lua_rawgetfield(L, p, "visible_when");
    if (lua_istable(L, -1)) {
      const int vw = lua_gettop(L);
      std::string vw_param = stringField(L, vw, "param");
      if (!vw_param.empty()) {
        spec.visible_when_param = vw_param;
        lua_rawgetfield(L, vw, "equals");
        spec.visible_when_equals = luaScalarToJson(L, lua_gettop(L));
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);  // visible_when
    out.push_back(std::move(spec));
    lua_pop(L, 1);  // param entry
  }
  lua_pop(L, 1);  // parameters
  return {};
}

// Compile + load `source` and run its main chunk, leaving the returned module
// value on top of `L`. Returns an error string (empty on success).
std::string loadModule(lua_State* L, BudgetState* budget, const std::string& source) {
  if (source.size() > kMaxSourceBytes) {  // luau_compile runs on the host heap, outside the VM cap
    return "filter source exceeds the size limit";
  }
  lua_CompileOptions opts = {};
  opts.optimizationLevel = 1;
  opts.debugLevel = 1;  // line info for error messages
  std::size_t bc_size = 0;
  char* bc = luau_compile(source.c_str(), source.size(), &opts, &bc_size);
  if (bc == nullptr) {
    return "luau_compile failed";
  }
  const int load_status = luau_load(L, "=filter", bc, bc_size, 0);
  std::free(bc);
  if (load_status != 0) {
    std::string err = lua_isstring(L, -1) ? lua_tostring(L, -1) : "luau_load failed";
    lua_pop(L, 1);
    return err;
  }
  if (lua_pcall(L, 0, 1, 0) != 0) {
    std::string err = lua_isstring(L, -1) ? lua_tostring(L, -1) : "module evaluation failed";
    lua_pop(L, 1);
    return err;
  }
  if (budget->trip != nullptr) {  // a top-level pcall may have swallowed the watchdog error
    lua_pop(L, 1);
    return "filter module exceeded its instruction budget during evaluation";
  }
  return {};
}

// Read metadata (NOT create) from the class table on top of `L` into a
// FilterClass. Leaves the stack as found. Returns an error string.
std::string readClassMetadata(lua_State* L, FilterClass& fc) {
  const int idx = lua_gettop(L);
  fc.id = stringField(L, idx, "id");
  fc.name = stringField(L, idx, "name", fc.id);
  if (fc.id.empty()) {
    return "filter class is missing its `id`";
  }
  fc.description = stringField(L, idx, "description");
  fc.version = stringField(L, idx, "version");
  fc.output_kind = stringField(L, idx, "output", "double");
  return readParameters(L, idx, fc.parameters);
}

// Bind an instance method (`calculate`/`reset`) by name WITHOUT triggering any
// metamethod: first a raw field on the instance, else a raw lookup through a
// TABLE-valued `__index` — the `setmetatable(state, Class)` class idiom. A `__index`
// *function* is deliberately never invoked (it would run unbounded Lua outside any
// pcall/fuel guard during binding), so an adversarial metatable cannot execute code
// here. Leaves the resolved value (function or nil) on the stack top. Returns true
// iff it came from the class metatable — i.e. a `:method` that must be called with
// the instance as `self`; false for a plain closure field (called without self) or
// when nothing was found.
[[nodiscard]] bool pushInstanceMethod(lua_State* L, int inst_idx, const char* name) {
  lua_rawgetfield(L, inst_idx, name);
  if (!lua_isnil(L, -1)) {
    return false;  // a plain field on the instance (closure-style) — no self
  }
  lua_pop(L, 1);
  if (lua_getmetatable(L, inst_idx) == 0) {
    lua_pushnil(L);
    return false;  // no metatable
  }
  // stack: [..., metatable]
  bool via_metatable = false;
  lua_rawgetfield(L, -1, "__index");  // [..., metatable, __index]
  if (lua_istable(L, -1)) {
    lua_rawgetfield(L, -1, name);  // [..., metatable, __index, method]
    via_metatable = lua_isfunction(L, -1);
    lua_replace(L, -2);  // [..., metatable, method]  (drop __index)
  } else {
    lua_pop(L, 1);   // a non-table __index is not a class — ignore it
    lua_pushnil(L);  // [..., metatable, nil]
  }
  lua_replace(L, -2);  // [..., method-or-nil]  (drop metatable)
  return via_metatable;
}

// ---- the live instance ---------------------------------------------------

class LuauInstance final : public FilterInstance {
 public:
  LuauInstance(
      lua_State* L, int calc_ref, int reset_ref, int self_ref, bool calc_self, bool reset_self,
      std::unique_ptr<BudgetState> budget, std::uint64_t call_fuel)
      : L_(L),
        calc_ref_(calc_ref),
        reset_ref_(reset_ref),
        self_ref_(self_ref),
        calc_self_(calc_self),
        reset_self_(reset_self),
        budget_(std::move(budget)),
        call_fuel_(call_fuel) {}

  ~LuauInstance() override {
    if (L_ != nullptr) {
      lua_close(L_);
    }
  }
  LuauInstance(const LuauInstance&) = delete;
  LuauInstance& operator=(const LuauInstance&) = delete;

  Result calculate(double t, double v) override {
    Result r;
    if (failed_ || calc_ref_ == LUA_NOREF) {
      r.suppress = true;
      return r;
    }
    const int base = lua_gettop(L_);
    lua_getref(L_, calc_ref_);  // push the calculate function
    if (!lua_isfunction(L_, -1)) {
      lua_settop(L_, base);
      fail("filter `calculate` is not a function");
      r.suppress = true;
      return r;
    }
    int nargs = 2;
    if (calc_self_) {
      lua_getref(L_, self_ref_);  // self — a class `:calculate(self, t, v)`; pushed before the args
      ++nargs;
    }
    lua_pushnumber(L_, t);
    lua_pushnumber(L_, v);
    armFuel(budget_.get(), call_fuel_);  // bound this calculate() against a runaway loop
    if (lua_pcall(L_, nargs, LUA_MULTRET, 0) != 0) {
      fail(lua_isstring(L_, -1) ? lua_tostring(L_, -1) : "calculate error");
      lua_settop(L_, base);
      r.suppress = true;
      return r;
    }
    if (budget_->trip != nullptr) {  // the script may have pcall-swallowed the watchdog — fail anyway
      lua_settop(L_, base);
      fail("calculate exceeded its instruction budget");
      r.suppress = true;
      return r;
    }
    const int nres = lua_gettop(L_) - base;
    if (nres <= 0 || lua_isnil(L_, base + 1)) {
      lua_settop(L_, base);
      r.suppress = true;
      return r;
    }
    if (nres >= 2) {
      // Two-result form MUST be (number time, number value); reject (n, nil)/(n, "x").
      if (lua_isnumber(L_, base + 1) && lua_isnumber(L_, base + 2)) {
        r.out_time = lua_tonumber(L_, base + 1);  // (t_out, value)
        r.value = lua_tonumber(L_, base + 2);
      } else {
        lua_settop(L_, base);
        fail("calculate (time, value) form must return two numbers");
        r.suppress = true;
        return r;
      }
    } else if (lua_isnumber(L_, base + 1)) {
      r.value = lua_tonumber(L_, base + 1);  // single value at the input time
    } else {
      lua_settop(L_, base);
      fail("calculate must return a number, (time, value), or nil");
      r.suppress = true;
      return r;
    }
    lua_settop(L_, base);
    return r;
  }

  MimoResult calculateMimo(double t, PJ::Span<const double> inputs) override {
    MimoResult r;
    if (failed_ || calc_ref_ == LUA_NOREF) {
      r.suppress = true;
      return r;
    }
    const int base = lua_gettop(L_);
    lua_getref(L_, calc_ref_);  // push the calculate function
    if (!lua_isfunction(L_, -1)) {
      lua_settop(L_, base);
      fail("filter `calculate` is not a function");
      r.suppress = true;
      return r;
    }
    int nargs = 1 + static_cast<int>(inputs.size());  // t + N inputs
    if (calc_self_) {
      lua_getref(L_, self_ref_);  // self — `:calculate(self, t, v, v1..)`; pushed before the args
      ++nargs;
    }
    lua_pushnumber(L_, t);
    for (const double in_value : inputs) {
      lua_pushnumber(L_, in_value);
    }
    armFuel(budget_.get(), call_fuel_);
    if (lua_pcall(L_, nargs, LUA_MULTRET, 0) != 0) {
      fail(lua_isstring(L_, -1) ? lua_tostring(L_, -1) : "calculate error");
      lua_settop(L_, base);
      r.suppress = true;
      return r;
    }
    if (budget_->trip != nullptr) {  // pcall may have swallowed the watchdog — fail anyway
      lua_settop(L_, base);
      fail("calculate exceeded its instruction budget");
      r.suppress = true;
      return r;
    }
    const int nres = lua_gettop(L_) - base;
    if (nres <= 0 || lua_isnil(L_, base + 1)) {  // nil/no first result suppresses the row
      lua_settop(L_, base);
      r.suppress = true;
      return r;
    }
    r.values.reserve(static_cast<std::size_t>(nres));
    for (int i = 0; i < nres; ++i) {  // every returned result must be a number (one per output)
      if (!lua_isnumber(L_, base + 1 + i)) {
        lua_settop(L_, base);
        fail("calculate must return a number per output");
        r.suppress = true;
        r.values.clear();
        return r;
      }
      r.values.push_back(lua_tonumber(L_, base + 1 + i));
    }
    lua_settop(L_, base);
    return r;
  }

  void reset() override {
    if (failed_ || reset_ref_ == LUA_NOREF) {
      return;
    }
    const int base = lua_gettop(L_);
    lua_getref(L_, reset_ref_);
    if (lua_isfunction(L_, -1)) {
      int nargs = 0;
      if (reset_self_) {
        lua_getref(L_, self_ref_);  // self — a class `:reset(self)`
        ++nargs;
      }
      armFuel(budget_.get(), call_fuel_);
      if (lua_pcall(L_, nargs, 0, 0) != 0) {
        fail(lua_isstring(L_, -1) ? lua_tostring(L_, -1) : "reset error");
      } else if (budget_->trip != nullptr) {
        fail("reset exceeded its instruction budget");
      }
    }
    lua_settop(L_, base);
  }

  [[nodiscard]] bool failed() const override {
    return failed_;
  }
  [[nodiscard]] const std::string& error() const override {
    return error_;
  }

 private:
  void fail(std::string msg) {
    failed_ = true;
    if (error_.empty()) {
      error_ = std::move(msg);
    }
  }

  lua_State* L_ = nullptr;
  int calc_ref_ = LUA_NOREF;
  int reset_ref_ = LUA_NOREF;
  int self_ref_ = LUA_NOREF;  // the instance table, passed as self iff a method came from the class metatable
  bool calc_self_ = false;    // calculate is a class `:method` → call with self
  bool reset_self_ = false;   // reset is a class `:method` → call with self
  std::unique_ptr<BudgetState> budget_;  // outlives L_ (destroyed after lua_close in the dtor body)
  std::uint64_t call_fuel_ = 0;
  bool failed_ = false;
  std::string error_;
};

// ---- the engine ----------------------------------------------------------

class LuauEngine final : public ScriptEngine {
 public:
  explicit LuauEngine(BudgetLimits limits) : limits_(limits) {}

  Expected<std::vector<FilterClass>> inspectModule(const std::string& source, const std::string& origin) override {
    BudgetState budget;  // declared before the Closer → outlives lua_close
    lua_State* L = newSandboxedState(&budget, limits_);
    if (L == nullptr) {
      return PJ::unexpected("failed to create Luau state");
    }
    struct Closer {
      lua_State* s;
      ~Closer() {
        lua_close(s);
      }
    } closer{L};

    if (std::string err = loadModule(L, &budget, source); !err.empty()) {
      return PJ::unexpected(err);
    }
    // Module value on top: either a single class table (has `create`) or a list.
    const int module_idx = lua_gettop(L);
    if (!lua_istable(L, module_idx)) {
      return PJ::unexpected("a filter module must return a table");
    }

    std::vector<FilterClass> classes;
    auto read_one = [&](int class_idx) -> std::string {
      FilterClass fc;
      fc.source = source;
      fc.origin = origin;
      lua_pushvalue(L, class_idx);  // metadata reader works on top-of-stack
      std::string err = readClassMetadata(L, fc);
      lua_pop(L, 1);
      if (!err.empty()) {
        return err;
      }
      classes.push_back(std::move(fc));
      return {};
    };

    lua_rawgetfield(L, module_idx, "create");
    const bool is_single_class = lua_isfunction(L, -1);
    lua_pop(L, 1);

    if (is_single_class) {
      if (std::string err = read_one(module_idx); !err.empty()) {
        return PJ::unexpected(err);
      }
    } else {
      const int count = arrayLen(L, module_idx);
      if (count == 0) {
        return PJ::unexpected("filter module returned neither a class nor a list of classes");
      }
      for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, module_idx, i);
        const int entry = lua_gettop(L);
        if (!lua_istable(L, entry)) {
          lua_pop(L, 1);
          return PJ::unexpected("filter list entry is not a table");
        }
        std::string err = read_one(entry);
        lua_pop(L, 1);
        if (!err.empty()) {
          return PJ::unexpected(err);
        }
      }
    }
    return classes;
  }

  Expected<std::unique_ptr<FilterInstance>> createInstance(
      const FilterClass& klass, const std::string& params_json) override {
    auto budget = std::make_unique<BudgetState>();
    lua_State* L = newSandboxedState(budget.get(), limits_);
    if (L == nullptr) {
      return PJ::unexpected("failed to create Luau state");
    }
    bool keep = false;
    struct Closer {
      lua_State* s;
      bool* keep;
      ~Closer() {
        if (!*keep) {
          lua_close(s);
        }
      }
    } closer{L, &keep};

    if (std::string err = loadModule(L, budget.get(), klass.source); !err.empty()) {
      return PJ::unexpected(err);
    }
    const int module_idx = lua_gettop(L);
    if (!lua_istable(L, module_idx)) {
      return PJ::unexpected("a filter module must return a table");
    }

    // Locate the class table whose `id` == klass.id (single or within a list).
    int class_idx = 0;
    lua_rawgetfield(L, module_idx, "create");
    const bool is_single = lua_isfunction(L, -1);
    lua_pop(L, 1);
    if (is_single) {
      if (stringField(L, module_idx, "id") == klass.id) {
        class_idx = module_idx;
      }
    } else {
      const int count = arrayLen(L, module_idx);
      for (int i = 1; i <= count && class_idx == 0; ++i) {
        lua_rawgeti(L, module_idx, i);  // leave candidate on stack
        if (lua_istable(L, -1) && stringField(L, lua_gettop(L), "id") == klass.id) {
          class_idx = lua_gettop(L);  // keep this one on the stack
        } else {
          lua_pop(L, 1);
        }
      }
    }
    if (class_idx == 0) {
      return PJ::unexpected("class id '" + klass.id + "' not found in module");
    }

    // Build the params table and call create(params).
    lua_rawgetfield(L, class_idx, "create");
    if (!lua_isfunction(L, -1)) {
      return PJ::unexpected("filter class '" + klass.id + "' has no create() function");
    }
    lua_newtable(L);
    const int params_idx = lua_gettop(L);
    // Seed every declared parameter with its default first, then override with
    // the caller's values — so create() always sees a COMPLETE params table even
    // when params_json only carries the non-default fields (or is "{}").
    for (const ParamSpec& spec : klass.parameters) {
      pushJsonScalar(L, spec.default_value);
      lua_setfield(L, params_idx, spec.name.c_str());
    }
    const auto params = nlohmann::json::parse(params_json, nullptr, /*allow_exceptions=*/false);
    if (params.is_object()) {
      for (auto it = params.begin(); it != params.end(); ++it) {
        pushJsonScalar(L, it.value());
        lua_setfield(L, params_idx, it.key().c_str());
      }
    }
    armFuel(budget.get(), limits_.setup_fuel);  // module-eval already spent some fuel; re-arm for create()
    if (lua_pcall(L, 1, 1, 0) != 0) {
      std::string err = lua_isstring(L, -1) ? lua_tostring(L, -1) : "create() failed";
      return PJ::unexpected(err);
    }
    if (budget->trip != nullptr) {
      return PJ::unexpected("create() exceeded its instruction budget");
    }
    if (!lua_istable(L, -1)) {
      return PJ::unexpected("create() must return an instance table");
    }
    const int inst_idx = lua_gettop(L);

    // Resolve calculate/reset through the instance: a plain field (closure) or a
    // class method reached via the metatable's __index. `*_self` records which, so
    // a class `:method` is later called with the instance bound as self.
    const bool calc_self = pushInstanceMethod(L, inst_idx, "calculate");
    if (!lua_isfunction(L, -1)) {
      return PJ::unexpected("filter '" + klass.id + "' instance has no calculate()");
    }
    const int calc_ref = lua_ref(L, -1);  // Luau lua_ref does NOT pop
    lua_pop(L, 1);

    const bool reset_self = pushInstanceMethod(L, inst_idx, "reset");
    const int reset_ref = lua_isfunction(L, -1) ? lua_ref(L, -1) : LUA_NOREF;
    lua_pop(L, 1);

    // A class method holds the per-instance state in `self` (the instance table),
    // not in closure upvalues, so the engine must keep that table alive for its
    // lifetime — otherwise the GC frees the state mid-stream. A plain-field closure
    // needs neither self nor this ref.
    int self_ref = LUA_NOREF;
    if (calc_self || reset_self) {
      lua_pushvalue(L, inst_idx);
      self_ref = lua_ref(L, -1);
      lua_pop(L, 1);
    }

    // Construct the owning instance FIRST; only then disarm the Closer. If the
    // allocation throws, `keep` is still false and the Closer closes L (no leak).
    auto instance = std::unique_ptr<FilterInstance>(new LuauInstance(
        L, calc_ref, reset_ref, self_ref, calc_self, reset_self, std::move(budget), limits_.call_fuel));
    keep = true;
    return instance;
  }

 private:
  BudgetLimits limits_;
};

}  // namespace

std::shared_ptr<ScriptEngine> makeLuauEngine(BudgetLimits limits) {
  return std::make_shared<LuauEngine>(limits);
}

}  // namespace PJ::scripting
