// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Luau backend for the whole-series → markers path (see marker_engine.h). A
// freeform script runs once with a read accessor over the WHOLE series plus the
// marker-emitting primitives bound as read-only globals; emitted markers are
// collected into a per-run context. Mirrors the semantics of the Anomaly
// Detector's sol2 engine exactly (timestamps stay in nanoseconds as doubles, no
// rebasing) so a rule produces identical markers under either backend.
//
// Luau API gotchas: headers are C++ linkage (no extern "C"); globals must be bound
// BEFORE luaL_sandbox freezes them; lua_upvalueindex carries the per-run context.

#include "pj_scripting/marker_engine.h"

#include <kiss_fftr.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "lua.h"
#include "luacode.h"
#include "lualib.h"
#include "luau_sandbox_state.h"

namespace PJ::scripting {

// ---- SeriesView accessors --------------------------------------------------

std::optional<MarkerSample> SeriesView::at(std::size_t index) const {
  if (index >= values.size() || index >= timestamps.size()) {
    return std::nullopt;
  }
  return MarkerSample{timestamps[index], values[index]};
}

double SeriesView::atTime(double t) const {
  const std::size_t n = values.size();
  if (n == 0 || timestamps.size() != n) {
    return 0.0;
  }
  // A non-finite query (NaN/inf from a rule) makes every comparison below false,
  // so the front/back guards would fall through and lower_bound would return
  // begin(), making lo = 0 - 1 wrap to SIZE_MAX and read out of bounds. Reject it.
  if (!std::isfinite(t)) {
    return 0.0;
  }
  if (t <= timestamps.front()) {
    return values.front();
  }
  if (t >= timestamps.back()) {
    return values.back();
  }
  // t is finite and strictly greater than timestamps.front() (the guards above),
  // so lower_bound lands at index >= 1 and lo never underflows.
  const auto it = std::lower_bound(timestamps.begin(), timestamps.end(), t);
  const std::size_t hi = static_cast<std::size_t>(it - timestamps.begin());
  const std::size_t lo = hi - 1;
  const double span = timestamps[hi] - timestamps[lo];
  const double frac = (span > 0.0) ? (t - timestamps[lo]) / span : 0.0;
  return values[lo] + frac * (values[hi] - values[lo]);
}

namespace {

using namespace detail;

// ---- per-run context (carried into the bound primitives as an upvalue) -----

struct RunContext {
  const SeriesProvider* provider = nullptr;
  BudgetState* budget = nullptr;
  std::vector<PJ::sdk::PlotMarker> emitted;
  std::size_t max_output_bytes = 0;
  std::size_t output_bytes = 0;
  std::optional<std::int64_t> open_region;  // startMarker -> closeMarker
};

RunContext* ctxFromUpvalue(lua_State* L) {
  return static_cast<RunContext*>(lua_tolightuserdata(L, lua_upvalueindex(1)));
}

// Every emitted marker goes through here, so the native result is bounded independently of the
// VM's memory cap. A marker counts its sizeof plus its strings; vector slack is not counted.
void appendMarker(lua_State* L, RunContext* ctx, PJ::sdk::PlotMarker&& marker) {
  const std::size_t bytes =
      sizeof(PJ::sdk::PlotMarker) + marker.label.size() + marker.description.size() + marker.category.size();
  if (bytes > ctx->max_output_bytes - ctx->output_bytes) {
    tripRun(L, *ctx->budget, "marker output exceeds the byte ceiling");
  }
  ctx->output_bytes += bytes;
  ctx->emitted.push_back(std::move(marker));
}

// ---- opts parsing (label/color/severity/status/category/description) -------

std::optional<PJ::sdk::ColorRGBA> parseHexColor(const std::string& hex) {
  if (hex.size() != 7 || hex[0] != '#') {
    return std::nullopt;
  }
  auto hx = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  };
  int v[6];
  for (int i = 0; i < 6; ++i) {
    v[i] = hx(hex[i + 1]);
    if (v[i] < 0) {
      return std::nullopt;
    }
  }
  return PJ::sdk::ColorRGBA{
      static_cast<std::uint8_t>(v[0] * 16 + v[1]), static_cast<std::uint8_t>(v[2] * 16 + v[3]),
      static_cast<std::uint8_t>(v[4] * 16 + v[5]), 255};
}

PJ::sdk::MarkerSeverity parseSeverity(const std::string& s) {
  if (s == "warning") {
    return PJ::sdk::MarkerSeverity::kWarning;
  }
  if (s == "error") {
    return PJ::sdk::MarkerSeverity::kError;
  }
  if (s == "critical") {
    return PJ::sdk::MarkerSeverity::kCritical;
  }
  return PJ::sdk::MarkerSeverity::kInfo;
}

PJ::sdk::MarkerStatus parseStatus(const std::string& s) {
  if (s == "pass") {
    return PJ::sdk::MarkerStatus::kPass;
  }
  if (s == "fail") {
    return PJ::sdk::MarkerStatus::kFail;
  }
  return PJ::sdk::MarkerStatus::kNone;
}

// Read string field `key` from the table at `idx`, or nullopt if absent/non-string.
std::optional<std::string> optString(lua_State* L, int idx, const char* key) {
  lua_getfield(L, idx, key);
  std::optional<std::string> out;
  if (lua_isstring(L, -1)) {
    out = lua_tostring(L, -1);
  }
  lua_pop(L, 1);
  return out;
}

// Apply the optional opts table (the LAST argument if it is a table) to `m`.
void applyOpts(lua_State* L, int opts_idx, PJ::sdk::PlotMarker& m) {
  if (!lua_istable(L, opts_idx)) {
    return;
  }
  if (auto v = optString(L, opts_idx, "label")) {
    m.label = *v;
  }
  if (auto v = optString(L, opts_idx, "description")) {
    m.description = *v;
  }
  if (auto v = optString(L, opts_idx, "category")) {
    m.category = *v;
  }
  if (auto v = optString(L, opts_idx, "color")) {
    if (auto rgba = parseHexColor(*v)) {
      m.color = *rgba;
    }
  }
  if (auto v = optString(L, opts_idx, "severity")) {
    m.severity = parseSeverity(*v);
  }
  if (auto v = optString(L, opts_idx, "status")) {
    m.status = parseStatus(*v);
  }
}

// ---- series accessor (a Lua table with closures over a SeriesView*) --------

const SeriesView* viewFromArg(lua_State* L, int idx) {
  if (!lua_istable(L, idx)) {
    return nullptr;
  }
  lua_getfield(L, idx, "__sv");
  const SeriesView* sv = static_cast<const SeriesView*>(lua_tolightuserdata(L, -1));
  lua_pop(L, 1);
  return sv;
}

int l_series_size(lua_State* L) {
  const auto* sv = static_cast<const SeriesView*>(lua_tolightuserdata(L, lua_upvalueindex(1)));
  lua_pushnumber(L, sv != nullptr ? static_cast<double>(sv->size()) : 0.0);
  return 1;
}

int l_series_at(lua_State* L) {
  const auto* sv = static_cast<const SeriesView*>(lua_tolightuserdata(L, lua_upvalueindex(1)));
  const auto i = static_cast<std::size_t>(lua_tointeger(L, 2));  // arg1 = self table, arg2 = index
  if (sv == nullptr) {
    lua_pushnil(L);
    return 1;
  }
  const auto p = sv->at(i);
  if (!p) {
    lua_pushnil(L);
    return 1;
  }
  lua_newtable(L);
  lua_pushnumber(L, p->t);
  lua_setfield(L, -2, "t");
  lua_pushnumber(L, p->v);
  lua_setfield(L, -2, "v");
  return 1;
}

int l_series_atTime(lua_State* L) {
  const auto* sv = static_cast<const SeriesView*>(lua_tolightuserdata(L, lua_upvalueindex(1)));
  lua_pushnumber(L, sv != nullptr ? sv->atTime(lua_tonumber(L, 2)) : 0.0);
  return 1;
}

// series("name") -> a table {__sv=ptr, size=, at=, atTime=} or nil.
int l_series(lua_State* L) {
  RunContext* ctx = ctxFromUpvalue(L);
  const char* name = lua_tostring(L, 1);
  const SeriesView* sv =
      (ctx != nullptr && ctx->provider != nullptr && name != nullptr) ? ctx->provider->get(name) : nullptr;
  if (sv == nullptr) {
    lua_pushnil(L);
    return 1;
  }
  lua_newtable(L);
  lua_pushlightuserdata(L, const_cast<SeriesView*>(sv));
  lua_setfield(L, -2, "__sv");
  lua_pushlightuserdata(L, const_cast<SeriesView*>(sv));
  lua_pushcclosure(L, l_series_size, "size", 1);
  lua_setfield(L, -2, "size");
  lua_pushlightuserdata(L, const_cast<SeriesView*>(sv));
  lua_pushcclosure(L, l_series_at, "at", 1);
  lua_setfield(L, -2, "at");
  lua_pushlightuserdata(L, const_cast<SeriesView*>(sv));
  lua_pushcclosure(L, l_series_atTime, "atTime", 1);
  lua_setfield(L, -2, "atTime");
  return 1;
}

int l_getSeriesNames(lua_State* L) {
  RunContext* ctx = ctxFromUpvalue(L);
  lua_newtable(L);
  if (ctx != nullptr && ctx->provider != nullptr) {
    int i = 1;
    for (const auto& n : ctx->provider->names) {
      lua_pushstring(L, n.c_str());
      lua_rawseti(L, -2, i++);
    }
  }
  return 1;
}

// ---- marker-emitting primitives --------------------------------------------

// Convert a Luau number to an absolute-ns timestamp, rejecting anything a
// double->int64 cast cannot represent: non-finite (math.huge / 0/0) AND finite
// values outside the int64 range (e.g. 1e19) — both are undefined behavior in the
// cast. ±9.22e18 ns is ~292 years, so no real marker time is excluded. Raises a
// Luau error (longjmp; luaL_error is noreturn) — every marker script runs under a
// protected call.
std::int64_t finiteToNs(lua_State* L, double x) {
  constexpr double kInt64Max = 9.223372036854775e18;  // just inside INT64_MAX as a double
  if (!std::isfinite(x) || x < -kInt64Max || x > kInt64Max) {
    luaL_error(L, "marker time must be a finite number within the int64 range");
  }
  return static_cast<std::int64_t>(x);
}

int l_startMarker(lua_State* L) {
  RunContext* ctx = ctxFromUpvalue(L);
  ctx->open_region = finiteToNs(L, lua_tonumber(L, 1));
  return 0;
}

int l_closeMarker(lua_State* L) {
  RunContext* ctx = ctxFromUpvalue(L);
  if (!ctx->open_region) {
    return 0;
  }
  const auto a = *ctx->open_region;
  const auto b = finiteToNs(L, lua_tonumber(L, 1));
  PJ::sdk::PlotMarker m;
  m.kind = PJ::sdk::MarkerKind::kRegion;
  m.t_start = std::min(a, b);
  m.t_end = std::max(a, b);
  m.severity = PJ::sdk::MarkerSeverity::kWarning;
  m.category = "anomaly";
  applyOpts(L, 2, m);
  appendMarker(L, ctx, std::move(m));
  ctx->open_region.reset();
  return 0;
}

// The three marker shapes, factored so the flexible `createMarker` and the explicit
// shortcuts emit byte-identical markers. `opts_idx` is the stack slot of the opts
// table (it differs: createMarker passes it as arg 3, the shortcuts as arg 2).

// A vertical line at x: an event carrying no value.
void emitVline(RunContext* ctx, double x, int opts_idx, lua_State* L) {
  PJ::sdk::PlotMarker m;
  m.category = "anomaly";
  m.kind = PJ::sdk::MarkerKind::kEvent;
  m.t_start = finiteToNs(L, x);
  m.severity = PJ::sdk::MarkerSeverity::kError;
  applyOpts(L, opts_idx, m);
  appendMarker(L, ctx, std::move(m));
}

// A horizontal line at y: a degenerate value band [y, y].
void emitHline(RunContext* ctx, double y, int opts_idx, lua_State* L) {
  PJ::sdk::PlotMarker m;
  m.category = "anomaly";
  m.kind = PJ::sdk::MarkerKind::kValueBand;
  m.value_low = y;
  m.value_high = y;
  m.severity = PJ::sdk::MarkerSeverity::kWarning;
  applyOpts(L, opts_idx, m);
  appendMarker(L, ctx, std::move(m));
}

// A point at (x, y): an event carrying a value.
void emitPoint(RunContext* ctx, double x, double y, int opts_idx, lua_State* L) {
  PJ::sdk::PlotMarker m;
  m.category = "anomaly";
  m.kind = PJ::sdk::MarkerKind::kEvent;
  m.t_start = finiteToNs(L, x);
  m.value_low = y;
  m.has_value = true;
  m.severity = PJ::sdk::MarkerSeverity::kError;
  applyOpts(L, opts_idx, m);
  appendMarker(L, ctx, std::move(m));
}

// createMarker(x?, y?, opts?): only x -> vertical line | only y -> horizontal line |
// both -> point. The do-everything primitive; the three create*Marker functions below
// are readable shortcuts for each single case (use whichever reads best, or mix them).
int l_createMarker(lua_State* L) {
  RunContext* ctx = ctxFromUpvalue(L);
  const bool has_x = !lua_isnoneornil(L, 1);
  const bool has_y = !lua_isnoneornil(L, 2);
  if (has_x && has_y) {
    emitPoint(ctx, lua_tonumber(L, 1), lua_tonumber(L, 2), 3, L);
  } else if (has_x) {
    emitVline(ctx, lua_tonumber(L, 1), 3, L);
  } else if (has_y) {
    emitHline(ctx, lua_tonumber(L, 2), 3, L);
  }
  return 0;
}

// createVerticalMarker(x, opts?): a vertical line at x.
int l_createVerticalMarker(lua_State* L) {
  emitVline(ctxFromUpvalue(L), lua_tonumber(L, 1), 2, L);
  return 0;
}

// createHorizontalMarker(y, opts?): a horizontal line at y.
int l_createHorizontalMarker(lua_State* L) {
  emitHline(ctxFromUpvalue(L), lua_tonumber(L, 1), 2, L);
  return 0;
}

// createPointMarker(x, y, opts?): a point at (x, y).
int l_createPointMarker(lua_State* L) {
  emitPoint(ctxFromUpvalue(L), lua_tonumber(L, 1), lua_tonumber(L, 2), 3, L);
  return 0;
}

int l_createBandMarker(lua_State* L) {
  RunContext* ctx = ctxFromUpvalue(L);
  PJ::sdk::PlotMarker m;
  m.kind = PJ::sdk::MarkerKind::kValueBand;
  m.value_low = lua_tonumber(L, 1);
  m.value_high = lua_tonumber(L, 2);
  m.severity = PJ::sdk::MarkerSeverity::kInfo;
  m.category = "anomaly";
  applyOpts(L, 3, m);
  appendMarker(L, ctx, std::move(m));
  return 0;
}

// ---- bandPower(series, fLo, fHi): summed FFT power in [fLo,fHi] Hz, DC-removed --

double computeBandPower(const SeriesView& sv, double fLo, double fHi) {
  std::size_t n = sv.values.size();
  if (n < 4 || sv.timestamps.size() != n) {
    return 0.0;
  }
  if (n % 2 != 0) {
    --n;
  }
  const double dt_seconds =
      static_cast<double>(sv.timestamps[n - 1] - sv.timestamps[0]) / (static_cast<double>(n - 1) * 1e9);
  if (dt_seconds <= 0.0) {
    return 0.0;
  }
  double mean = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    mean += sv.values[i];
  }
  mean /= static_cast<double>(n);

  std::vector<kiss_fft_scalar> input(n);
  for (std::size_t i = 0; i < n; ++i) {
    input[i] = static_cast<kiss_fft_scalar>(sv.values[i] - mean);
  }
  std::vector<kiss_fft_cpx> out(n / 2 + 1);
  kiss_fftr_cfg cfg = kiss_fftr_alloc(static_cast<int>(n), 0, nullptr, nullptr);
  if (cfg == nullptr) {
    return 0.0;
  }
  kiss_fftr(cfg, input.data(), out.data());
  KISS_FFT_FREE(cfg);

  const double nd = static_cast<double>(n);
  double power = 0.0;
  for (std::size_t i = 0; i < n / 2; ++i) {
    const double f = static_cast<double>(i) * (1.0 / dt_seconds) / nd;
    if (f >= fLo && f <= fHi) {
      const double amp = std::hypot(static_cast<double>(out[i].r), static_cast<double>(out[i].i)) / nd;
      power += amp * amp;
    }
  }
  return power;
}

int l_bandPower(lua_State* L) {
  const SeriesView* sv = viewFromArg(L, 1);
  const double fLo = lua_tonumber(L, 2);
  const double fHi = lua_tonumber(L, 3);
  lua_pushnumber(L, sv != nullptr ? computeBandPower(*sv, fLo, fHi) : 0.0);
  return 1;
}

// ---- bind a context-carrying C closure as a read-only global ---------------

void bindGlobal(lua_State* L, RunContext* ctx, const char* name, lua_CFunction fn) {
  lua_pushlightuserdata(L, ctx);
  lua_pushcclosure(L, fn, name, 1);
  lua_setglobal(L, name);
}

}  // namespace

std::vector<PJ::sdk::PlotMarker> runMarkerScript(
    const std::string& code, const SeriesProvider& provider, std::string* error, BudgetLimits limits,
    const std::function<bool()>& cancelled, std::size_t max_output_bytes, std::size_t* output_bytes) {
  if (error != nullptr) {
    error->clear();
  }
  if (code.size() > kMaxSourceBytes) {
    if (error != nullptr) {
      *error = "script too large";
    }
    return {};
  }

  RunContext ctx;
  ctx.provider = &provider;
  ctx.max_output_bytes = max_output_bytes;

  BudgetState budget;
  budget.max_bytes = std::numeric_limits<std::size_t>::max();  // open during trusted setup
  budget.fuel = std::numeric_limits<std::int64_t>::max();
  budget.cancelled = cancelled ? &cancelled : nullptr;
  ctx.budget = &budget;
  lua_State* L = lua_newstate(budgetAlloc, &budget);
  if (L == nullptr) {
    if (error != nullptr) {
      *error = "out of memory";
    }
    return {};
  }
  luaL_openlibs(L);
  for (const char* lib : {"os", "coroutine", "debug", "setfenv", "getfenv"}) {
    lua_pushnil(L);
    lua_setglobal(L, lib);
  }
  // Bind the primitives BEFORE luaL_sandbox freezes the globals read-only.
  bindGlobal(L, &ctx, "series", l_series);
  bindGlobal(L, &ctx, "GetSeriesNames", l_getSeriesNames);
  bindGlobal(L, &ctx, "startMarker", l_startMarker);
  bindGlobal(L, &ctx, "closeMarker", l_closeMarker);
  bindGlobal(L, &ctx, "createMarker", l_createMarker);
  bindGlobal(L, &ctx, "createVerticalMarker", l_createVerticalMarker);
  bindGlobal(L, &ctx, "createHorizontalMarker", l_createHorizontalMarker);
  bindGlobal(L, &ctx, "createPointMarker", l_createPointMarker);
  bindGlobal(L, &ctx, "createBandMarker", l_createBandMarker);
  bindGlobal(L, &ctx, "bandPower", l_bandPower);
  luaL_sandbox(L);

  lua_callbacks(L)->userdata = &budget;
  lua_callbacks(L)->interrupt = budgetInterrupt;
  budget.max_bytes = limits.mem_bytes;
  // A whole-series run is one execution that scans the series; budget it like
  // module-eval (setup_fuel), not a single per-sample calculate (call_fuel).
  budget.fuel = static_cast<std::int64_t>(limits.setup_fuel);

  size_t bc_size = 0;
  lua_CompileOptions opts = {};
  opts.optimizationLevel = 1;
  opts.debugLevel = 1;  // line info for error messages
  char* bc = luau_compile(code.c_str(), code.size(), &opts, &bc_size);
  if (bc == nullptr) {
    if (error != nullptr) {
      *error = "luau_compile failed";
    }
    lua_close(L);
    return {};
  }
  const int load_status = luau_load(L, "=rule", bc, bc_size, 0);
  std::free(bc);
  if (load_status != 0) {
    if (error != nullptr) {
      *error = lua_isstring(L, -1) ? lua_tostring(L, -1) : "luau_load failed";
    }
    lua_close(L);
    return {};
  }
  if (lua_pcall(L, 0, 0, 0) != 0) {
    if (error != nullptr) {
      *error = lua_isstring(L, -1) ? lua_tostring(L, -1) : "script error";
    }
    lua_close(L);
    return {};
  }
  if (budget.trip != nullptr) {
    if (error != nullptr && error->empty()) {
      *error = budget.trip;
    }
    lua_close(L);
    return {};
  }

  std::vector<PJ::sdk::PlotMarker> markers = std::move(ctx.emitted);
  lua_close(L);
  if (output_bytes != nullptr) {
    *output_bytes = ctx.output_bytes;
  }
  return markers;
}

bool validateScript(const std::string& code, std::string* error) {
  if (error != nullptr) {
    error->clear();
  }
  if (code.size() > kMaxSourceBytes) {
    if (error != nullptr) {
      *error = "script too large";
    }
    return false;
  }
  // Compile-only: a fresh VM just to host luau_load; no primitives bound, no pcall,
  // no inputs — so this is cheap and side-effect-free (drives the editor semaphore).
  BudgetState budget;
  budget.max_bytes = std::numeric_limits<std::size_t>::max();
  budget.fuel = std::numeric_limits<std::int64_t>::max();
  lua_State* L = lua_newstate(budgetAlloc, &budget);
  if (L == nullptr) {
    if (error != nullptr) {
      *error = "out of memory";
    }
    return false;
  }
  size_t bc_size = 0;
  lua_CompileOptions opts = {};
  opts.optimizationLevel = 1;
  opts.debugLevel = 1;
  char* bc = luau_compile(code.c_str(), code.size(), &opts, &bc_size);
  if (bc == nullptr) {
    if (error != nullptr) {
      *error = "luau_compile failed";
    }
    lua_close(L);
    return false;
  }
  const int load_status = luau_load(L, "=rule", bc, bc_size, 0);
  std::free(bc);
  const bool ok = (load_status == 0);
  if (!ok && error != nullptr) {
    *error = lua_isstring(L, -1) ? lua_tostring(L, -1) : "luau_load failed";
  }
  lua_close(L);
  return ok;
}

}  // namespace PJ::scripting
