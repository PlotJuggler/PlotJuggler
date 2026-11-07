// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include "pj_scripting/marker_engine.h"
#include "pj_scripting/script_engine.h"

using namespace PJ::scripting;

// A filter that returns 1 only if the dangerous globals are all absent.
TEST(LuauSandbox, DangerousGlobalsAreAbsent) {
  constexpr const char* kProbe = R"LUAU(
    return { id="probe", name="P",
      create = function(p) return { calculate = function(t, v)
        if io == nil and os == nil and loadstring == nil and dofile == nil and require == nil
          and setfenv == nil and getfenv == nil then
          return 1
        else
          return 0
        end
      end } end }
  )LUAU";
  auto engine = makeLuauEngine();
  auto classes = engine->inspectModule(kProbe, "test");
  ASSERT_TRUE(classes.has_value()) << (classes.has_value() ? "" : classes.error());
  auto inst = engine->createInstance(classes->front(), "{}");
  ASSERT_TRUE(inst.has_value()) << (inst.has_value() ? "" : inst.error());
  EXPECT_DOUBLE_EQ((*inst)->calculate(0.0, 0.0).value, 1.0);
}

// The stdlib is frozen read-only: a filter cannot monkey-patch math.
TEST(LuauSandbox, StdlibMutationIsRejected) {
  constexpr const char* kEvil = R"LUAU(
    return { id="evil", name="E",
      create = function(p) math.floor = nil; return { calculate = function(t, v) return v end } end }
  )LUAU";
  auto engine = makeLuauEngine();
  auto classes = engine->inspectModule(kEvil, "test");
  ASSERT_TRUE(classes.has_value());  // inspect never runs create
  auto inst = engine->createInstance(classes->front(), "{}");
  EXPECT_FALSE(inst.has_value());  // create() writes a read-only stdlib table → error
}

// Each sandboxed VM owns a writable global table whose __index is the frozen
// stdlib, so a module may define a new global. The write lands in that VM's
// table only — it cannot leak across filter VMs or into the host — while the
// stdlib stays read-only (see StdlibMutationIsRejected).
TEST(LuauSandbox, GlobalWriteInModuleIsAllowedAndIsolated) {
  constexpr const char* kGlobalWrite = R"LUAU(
    sneaky = 42
    return { id="x", name="X", create = function(p) return { calculate = function(t, v) return v end } end }
  )LUAU";
  auto engine = makeLuauEngine();
  auto classes = engine->inspectModule(kGlobalWrite, "test");
  ASSERT_TRUE(classes.has_value()) << (classes.has_value() ? "" : classes.error());
  auto inst = engine->createInstance(classes->front(), "{}");
  ASSERT_TRUE(inst.has_value()) << (inst.has_value() ? "" : inst.error());
  EXPECT_DOUBLE_EQ((*inst)->calculate(0.0, 7.0).value, 7.0);
}

// A malicious __index metamethod must NOT run during host-side inspection (raw
// field reads): otherwise a looping __index would hang/crash the app outside a
// protected call. Raw reads see no `id` → a clean error, no hang.
TEST(LuauSandbox, MetatableIndexCannotHangInspection) {
  constexpr const char* kEvil = R"LUAU(
    return setmetatable({}, { __index = function() while true do end end })
  )LUAU";
  auto engine = makeLuauEngine();
  EXPECT_FALSE(engine->inspectModule(kEvil, "test").has_value());  // must return, never hang
}

// A legitimate filter (locals + returned tables, no globals) works under the sandbox.
TEST(LuauSandbox, LegitimateFilterStillWorks) {
  constexpr const char* kScale = R"LUAU(
    return { id="scale", name="S",
      parameters = { { name="k", type="number", default=3.0 } },
      create = function(p) local k = p.k; return { calculate = function(t, v) return v * k end } end }
  )LUAU";
  auto engine = makeLuauEngine();
  auto classes = engine->inspectModule(kScale, "test");
  ASSERT_TRUE(classes.has_value());
  auto inst = engine->createInstance(classes->front(), "{}");
  ASSERT_TRUE(inst.has_value());
  EXPECT_DOUBLE_EQ((*inst)->calculate(0.0, 4.0).value, 12.0);
}

// A call that writes a global or reaches for the string metatable leaves nothing the next call can see:
// both engines build a fresh VM per call, and this pins it so VM reuse cannot regress it silently.
TEST(LuauSandbox, LuauCallCannotLeakSharedState) {
  // Filter engine: instance 1 writes a global (allowed: its own per-VM table) and tries the string
  // metatable; instance 2 must see neither.
  constexpr const char* kWriter = R"LUAU(
    leaked = {1}
    pcall(function() getmetatable("").__index.leaked = true end)
    return { id="writer", name="W", create = function(p) return { calculate = function(t, v)
      return leaked ~= nil and 1 or 0
    end } end }
  )LUAU";
  constexpr const char* kReader = R"LUAU(
    return { id="reader", name="R", create = function(p) return { calculate = function(t, v)
      return (leaked ~= nil or string.leaked ~= nil or getmetatable("").__index.leaked ~= nil) and 1 or 0
    end } end }
  )LUAU";
  auto engine = makeLuauEngine();
  // -1 when the module does not load or the call fails, so the checks below fail.
  auto calculate = [&](const char* source) {
    auto classes = engine->inspectModule(source, "test");
    if (!classes.has_value()) {
      return -1.0;
    }
    auto inst = engine->createInstance(classes->front(), "{}");
    if (!inst.has_value()) {
      return -1.0;
    }
    const auto result = (*inst)->calculate(0.0, 0.0);
    return (result.suppress || (*inst)->failed()) ? -1.0 : result.value;
  };
  EXPECT_DOUBLE_EQ(calculate(kWriter), 1.0);  // the write landed in the writer's own VM
  EXPECT_DOUBLE_EQ(calculate(kReader), 0.0);

  // Marker engine: globals and the string table are read-only there, so the writes are rejected;
  // the second run must still see nothing.
  constexpr const char* kMarkerWriter = R"LUAU(
    pcall(function() leaked = {1} end)
    pcall(function() getmetatable("").__index.leaked = true end)
  )LUAU";
  constexpr const char* kMarkerReader = R"LUAU(
    if leaked ~= nil or string.leaked ~= nil or getmetatable("").__index.leaked ~= nil then
      createVerticalMarker(0, {label="leaked"})
    end
  )LUAU";
  const SeriesProvider no_series;
  std::string error;
  EXPECT_TRUE(runMarkerScript(kMarkerWriter, no_series, &error).empty());
  ASSERT_TRUE(error.empty()) << error;
  const auto markers = runMarkerScript(kMarkerReader, no_series, &error);
  ASSERT_TRUE(error.empty()) << error;
  EXPECT_TRUE(markers.empty());
}
