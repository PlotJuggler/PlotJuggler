// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Luau backend of the whole-series -> markers path (marker_engine.h). A script runs once with a read accessor over the
// whole series and the marker primitives bound as read-only globals; emitted markers collect in a per-run context
// (`lua_upvalueindex`). Timestamps stay in nanoseconds as doubles, with no rebasing. Globals must be bound before
// `luaL_sandbox` freezes them.

#include "pj_scripting/marker_engine.h"

#include <pocketfft_hdronly.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lua.h"
#include "luacode.h"
#include "lualib.h"
#include "luau_sandbox_state.h"

namespace PJ::scripting {

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
  // A NaN query makes the front/back guards below false, lower_bound returns begin(), and `lo = 0 - 1` wraps and reads
  // out of bounds.
  if (!std::isfinite(t)) {
    return 0.0;
  }
  if (t <= timestamps.front()) {
    return values.front();
  }
  if (t >= timestamps.back()) {
    return values.back();
  }
  // t > timestamps.front(), so lower_bound lands at index >= 1 and `lo` cannot underflow.
  const auto it = std::lower_bound(timestamps.begin(), timestamps.end(), t);
  const std::size_t hi = static_cast<std::size_t>(it - timestamps.begin());
  const std::size_t lo = hi - 1;
  const double span = timestamps[hi] - timestamps[lo];
  const double frac = (span > 0.0) ? (t - timestamps[lo]) / span : 0.0;
  return values[lo] + frac * (values[hi] - values[lo]);
}

namespace {

struct NamedColor {
  std::string_view name;
  std::uint8_t r, g, b, a;
};

constexpr NamedColor kNamedColors[] = {
#include "marker_color_names.inc"
};

}  // namespace

std::optional<PJ::sdk::ColorRGBA> parseMarkerColor(std::string_view text) {
  if (!text.empty() && text[0] == '#') {
    const auto hx = [](char c) -> int {
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
    const std::string_view digits = text.substr(1);
    if (digits.size() != 3 && digits.size() != 6) {
      return std::nullopt;
    }
    int v[6];
    for (std::size_t i = 0; i < digits.size(); ++i) {
      v[i] = hx(digits[i]);
      if (v[i] < 0) {
        return std::nullopt;
      }
    }
    const auto channel = [&](std::size_t i) {
      return static_cast<std::uint8_t>(digits.size() == 3 ? v[i] * 17 : v[2 * i] * 16 + v[2 * i + 1]);
    };
    return PJ::sdk::ColorRGBA{channel(0), channel(1), channel(2), 255};
  }
  // Spaces dropped, as QColor does ("light blue"); ASCII fold, since std::tolower follows the C locale (a Turkish one
  // maps 'I' to a dotless i).
  std::string lower;
  for (const char c : text) {
    if (c != ' ' && c != '\t') {
      lower.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
  }
  const auto* it = std::ranges::lower_bound(kNamedColors, std::string_view(lower), {}, &NamedColor::name);
  if (it == std::end(kNamedColors) || it->name != lower) {
    return std::nullopt;
  }
  return PJ::sdk::ColorRGBA{it->r, it->g, it->b, it->a};
}

namespace {

using namespace detail;

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

// Every emitted marker goes through here, so the native result is capped apart from the VM's memory cap. A marker
// counts its sizeof plus its strings; vector slack is not counted.
void appendMarker(lua_State* L, RunContext* ctx, PJ::sdk::PlotMarker&& marker) {
  const std::size_t bytes =
      sizeof(PJ::sdk::PlotMarker) + marker.label.size() + marker.description.size() + marker.category.size();
  if (bytes > ctx->max_output_bytes - ctx->output_bytes) {
    tripRun(L, *ctx->budget, "marker output exceeds the byte ceiling");
  }
  ctx->output_bytes += bytes;
  ctx->emitted.push_back(std::move(marker));
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

std::optional<std::string> optString(lua_State* L, int idx, const char* key) {
  lua_getfield(L, idx, key);
  std::optional<std::string> out;
  if (lua_isstring(L, -1)) {
    out = lua_tostring(L, -1);
  }
  lua_pop(L, 1);
  return out;
}

// Applies the opts table, the last argument when it is a table, to `m`.
void applyOpts(lua_State* L, int opts_idx, PJ::sdk::PlotMarker& m) {
  if (!lua_istable(L, opts_idx)) {
    return;
  }
  // The prelude's same() compares exactly these keys: keep the two in step.
  if (auto v = optString(L, opts_idx, "label")) {
    m.label = *v;
  }
  if (auto v = optString(L, opts_idx, "description")) {
    m.description = *v;
  }
  if (auto v = optString(L, opts_idx, "category")) {
    m.category = *v;
  }
  lua_getfield(L, opts_idx, "color");
  const int color_type = lua_type(L, -1);
  lua_pop(L, 1);
  if (color_type != LUA_TNIL && color_type != LUA_TSTRING) {
    luaL_error(
        L, "color must be a string (\"#rrggbb\", \"#rgb\" or a Qt color name), not a %s", lua_typename(L, color_type));
  }
  if (auto v = optString(L, opts_idx, "color")) {
    const auto rgba = parseMarkerColor(*v);
    if (!rgba) {
      luaL_error(L, "unknown color '%s' (use \"#rrggbb\", \"#rgb\" or a Qt color name)", v->c_str());
    }
    m.color = *rgba;
  }
  if (auto v = optString(L, opts_idx, "severity")) {
    m.severity = parseSeverity(*v);
  }
  if (auto v = optString(L, opts_idx, "status")) {
    m.status = parseStatus(*v);
  }
}

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

// A Luau number as an absolute-ns timestamp. A non-finite value or one outside the int64 range (~292 years) is an
// undefined double->int64 cast, so it raises a Luau error (a longjmp: every marker script runs under a protected
// call).
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

void emitRegion(lua_State* L, RunContext* ctx, std::int64_t a, std::int64_t b, int opts_idx) {
  PJ::sdk::PlotMarker m;
  m.kind = PJ::sdk::MarkerKind::kRegion;
  m.t_start = std::min(a, b);
  m.t_end = std::max(a, b);
  m.severity = PJ::sdk::MarkerSeverity::kWarning;
  m.category = "anomaly";
  applyOpts(L, opts_idx, m);
  appendMarker(L, ctx, std::move(m));
}

int l_closeMarker(lua_State* L) {
  RunContext* ctx = ctxFromUpvalue(L);
  if (!ctx->open_region) {
    return 0;
  }
  // Reset only after the region is emitted: a closeMarker that raises (caught by a pcall) leaves the region open.
  emitRegion(L, ctx, *ctx->open_region, finiteToNs(L, lua_tonumber(L, 1)), 2);
  ctx->open_region.reset();
  return 0;
}

// (t_start, t_end, opts): a whole region in one call, for the prelude's colorRegions. It leaves the rule's own
// startMarker/closeMarker region alone, so the two can interleave.
int l_region(lua_State* L) {
  emitRegion(L, ctxFromUpvalue(L), finiteToNs(L, lua_tonumber(L, 1)), finiteToNs(L, lua_tonumber(L, 2)), 3);
  return 0;
}

// (series, i) -> t, v for the prelude's scan: s:at(i) without its per-sample table, the bulk of a scan's time.
int l_sample(lua_State* L) {
  const SeriesView* sv = viewFromArg(L, 1);
  const auto p = (sv != nullptr) ? sv->at(static_cast<std::size_t>(lua_tointeger(L, 2))) : std::nullopt;
  if (!p) {
    return 0;
  }
  lua_pushnumber(L, p->t);
  lua_pushnumber(L, p->v);
  return 2;
}

// Rule helpers written in Luau over the primitives above, run before luaL_sandbox so its globals freeze with the rest.
// The chunk receives l_region and l_sample as its arguments, so no rule can reach them. Every Luau call and loop
// iteration per sample costs the rule's fuel: the colorRegions loop keeps both to what a hand-written loop pays.
constexpr const char* kPrelude = R"LUAU(
local region, sample = ...

-- Two mapping results draw the same region when equal, or when both are tables with the same opts (a mapping may
-- build a new table per sample). The fields are the ones applyOpts reads: compared without a loop, since every loop
-- iteration costs the rule's fuel on every sample.
local function same(a, b)
  if type(a) ~= "table" or type(b) ~= "table" then
    return a == b
  end
  return a.color == b.color and a.label == b.label and a.severity == b.severity and a.status == b.status
    and a.category == b.category and a.description == b.description
end

-- The run's own copy of a table result, so a mapping that mutates and returns one shared table cannot recolor it.
local function snapshot(r)
  if type(r) ~= "table" then
    return r
  end
  return { color = r.color, label = r.label, severity = r.severity, status = r.status, category = r.category,
    description = r.description }
end

local function regionOpts(r)
  local opts = { severity = "info", category = "color" }
  if type(r) == "table" then
    for k, v in r do
      opts[k] = v
    end
  else
    opts.color = r
  end
  return opts
end

inf = math.huge

-- ranges({min, max, color}, ...): a value takes the color of the first range with min <= v < max (v <= inf when max
-- is inf), else none.
function ranges(...)
  local list = table.pack(...)
  for i = 1, list.n do
    local r = list[i]
    if type(r) ~= "table" or type(r[1]) ~= "number" or type(r[2]) ~= "number" or r[3] == nil then
      error("ranges: argument " .. i .. " must be {min, max, color}", 2)
    end
    if not (r[1] < r[2]) then
      error("ranges: argument " .. i .. " has min >= max", 2)
    end
  end
  return function(v)
    for i = 1, list.n do
      local r = list[i]
      if v >= r[1] and (v < r[2] or r[2] == math.huge) then
        return r[3]
      end
    end
    return nil
  end
end

local function checkThreshold(name, limit, color)
  if type(limit) ~= "number" or color == nil then
    error(name .. "(limit, color): limit must be a number and color given", 3)
  end
end

function lessThan(limit, color)
  checkThreshold("lessThan", limit, color)
  return function(v)
    if v < limit then
      return color
    end
  end
end

function greaterThan(limit, color)
  checkThreshold("greaterThan", limit, color)
  return function(v)
    if v > limit then
      return color
    end
  end
end

function colorRegions(source, mapping)
  local s = source
  if type(source) == "string" then
    s = series(source)
    if s == nil then
      error("colorRegions: no series named '" .. source .. "'", 2)
    end
  elseif type(source) ~= "table" or source.__sv == nil then
    error("colorRegions: the first argument must be a series name or series(...)", 2)
  end
  local is_table = type(mapping) == "table"
  if not is_table and type(mapping) ~= "function" then
    error("colorRegions: the mapping must be a table (value -> color) or a function (v -> color)", 2)
  end
  local cur, cur_start, t = nil, 0, nil
  local function flush(t_end)
    if cur ~= nil and t_end > cur_start then
      region(cur_start, t_end, regionOpts(cur))
    end
  end
  for i = 0, s:size() - 1 do
    local v
    t, v = sample(s, i)
    local r = (if is_table then mapping[v] else mapping(v)) or nil
    if r ~= cur and not same(r, cur) then
      flush(t)
      cur, cur_start = snapshot(r), t
    end
  end
  if t ~= nil then
    flush(t)
  end
end
)LUAU";

// The three marker shapes shared by `createMarker` and the explicit shortcuts, so both emit identical markers.
// `opts_idx` is the stack slot of the opts table: arg 3 for createMarker, arg 2 for the shortcuts.

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

// createMarker(x?, y?, opts?): only x -> vertical line, only y -> horizontal line, both -> point; the three
// create*Marker functions below are shortcuts for each case.
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

int l_createVerticalMarker(lua_State* L) {
  emitVline(ctxFromUpvalue(L), lua_tonumber(L, 1), 2, L);
  return 0;
}

int l_createHorizontalMarker(lua_State* L) {
  emitHline(ctxFromUpvalue(L), lua_tonumber(L, 1), 2, L);
  return 0;
}

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

// bandPower(series, fLo, fHi): summed FFT power in [fLo, fHi] Hz, DC removed.
//
// Above this budget the series is truncated to the largest even 11-smooth length, the rule of toolbox_fft's fftLength
// (pj-official-plugins), so a rule matches the plugin. pj_scripting/docs/TECHNICAL_NOTES.md "`bandPower` memory".
#ifdef __EMSCRIPTEN__
constexpr std::size_t kBandPowerMemoryBudgetBytes = 350'000'000;
#else
constexpr std::size_t kBandPowerMemoryBudgetBytes = 700'000'000;
#endif

std::size_t bandPowerLength(std::size_t count) {
  std::size_t n = count & ~std::size_t{1};
  if (n <= kBandPowerMemoryBudgetBytes / 80) {
    return n;
  }
  const auto smooth = [](std::size_t m) {
    for (const std::size_t p : {2U, 3U, 5U, 7U, 11U}) {
      while (m % p == 0) {
        m /= p;
      }
    }
    return m == 1;
  };
  while (!smooth(n)) {
    n -= 2;
  }
  return n;
}

double computeBandPower(const SeriesView& sv, double fLo, double fHi) {
  if (sv.values.size() < 4 || sv.timestamps.size() != sv.values.size()) {
    return 0.0;
  }
  const std::size_t n = bandPowerLength(sv.values.size());
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

  // Float: half the memory of double on pocketfft's Bluestein path, at ~1e-7 relative accuracy.
  std::vector<float> data(n);
  for (std::size_t i = 0; i < n; ++i) {
    data[i] = static_cast<float>(sv.values[i] - mean);
  }
  try {
    // In place; the output is FFTPACK half-complex: re0, re1, im1, re2, im2, ..., re(n/2) (n even).
    pocketfft::detail::pocketfft_r<float>(n).exec(data.data(), 1.0f, true);
  } catch (const std::bad_alloc&) {
    return 0.0;
  }

  const double nd = static_cast<double>(n);
  double power = 0.0;
  for (std::size_t i = 0; i < n / 2; ++i) {
    const double f = static_cast<double>(i) * (1.0 / dt_seconds) / nd;
    if (f >= fLo && f <= fHi) {
      const double amp =
          static_cast<double>(i == 0 ? std::abs(data[0]) : std::hypot(data[2 * i - 1], data[2 * i])) / nd;
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

void bindGlobal(lua_State* L, RunContext* ctx, const char* name, lua_CFunction fn) {
  lua_pushlightuserdata(L, ctx);
  lua_pushcclosure(L, fn, name, 1);
  lua_setglobal(L, name);
}

// Bytecode for `source`, empty only when luau_compile cannot allocate: a syntax error is encoded in the bytecode and
// raised by luau_load.
std::string compileChunk(std::string_view source) {
  lua_CompileOptions opts = {};
  opts.optimizationLevel = 1;
  opts.debugLevel = 1;  // line info for error messages
  std::size_t size = 0;
  char* bc = luau_compile(source.data(), source.size(), &opts, &size);
  std::string out = (bc != nullptr) ? std::string(bc, size) : std::string();
  std::free(bc);
  return out;
}

// Pushes the chunk's function. False sets `*error` (when non-null).
bool loadChunk(lua_State* L, const std::string& bytecode, const char* chunkname, std::string* error) {
  if (bytecode.empty()) {
    if (error != nullptr) {
      *error = "luau_compile failed";
    }
    return false;
  }
  if (luau_load(L, chunkname, bytecode.data(), bytecode.size(), 0) != 0) {
    if (error != nullptr) {
      *error = lua_isstring(L, -1) ? lua_tostring(L, -1) : "luau_load failed";
    }
    return false;
  }
  return true;
}

bool runPrelude(lua_State* L, RunContext* ctx, std::string* error) {
  static const std::string bytecode = compileChunk(kPrelude);
  if (!loadChunk(L, bytecode, "=colorRegions", error)) {
    return false;
  }
  lua_pushlightuserdata(L, ctx);
  lua_pushcclosure(L, l_region, "region", 1);
  lua_pushcfunction(L, l_sample, "sample");
  if (lua_pcall(L, 2, 0, 0) != 0) {
    if (error != nullptr) {
      *error = std::string("marker prelude failed: ") + (lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
    }
    return false;
  }
  return true;
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
  // Before luaL_sandbox freezes the globals.
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
  if (!runPrelude(L, &ctx, error)) {
    lua_close(L);
    return {};
  }
  luaL_sandbox(L);

  lua_callbacks(L)->userdata = &budget;
  lua_callbacks(L)->interrupt = budgetInterrupt;
  budget.max_bytes = limits.mem_bytes;
  // One run scans a whole series, so it gets the module-evaluation fuel, not a per-sample calculate's.
  budget.fuel = static_cast<std::int64_t>(limits.setup_fuel);

  if (!loadChunk(L, compileChunk(code), "=rule", error)) {
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
  // A fresh VM only to host luau_load: no primitives, no pcall, no inputs.
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
  const bool ok = loadChunk(L, compileChunk(code), "=rule", error);
  lua_close(L);
  return ok;
}

}  // namespace PJ::scripting
