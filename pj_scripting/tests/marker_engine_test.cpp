// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Phase-1 parity spike: the Luau marker engine must produce the same markers a
// sol2 anomaly rule would, for the same series. We assert exact markers for known
// synthetic inputs (the cheapest way to prove the language port is faithful).

#include "pj_scripting/marker_engine.h"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <string>
#include <vector>

namespace {

using PJ::scripting::runMarkerScript;
using PJ::scripting::SeriesProvider;
using PJ::scripting::SeriesView;

// A provider exposing one named series.
SeriesProvider oneSeries(const std::string& name, const SeriesView& sv) {
  SeriesProvider p;
  p.names = {name};
  p.get = [name, &sv](const std::string& n) -> const SeriesView* { return n == name ? &sv : nullptr; };
  return p;
}

constexpr double kSec = 1e9;  // one second in nanoseconds

TEST(MarkerEngine, SpikeRuleMatchesExpectedMarkers) {
  SeriesView sv;
  sv.timestamps = {0, 1 * kSec, 2 * kSec, 3 * kSec, 4 * kSec, 5 * kSec};
  sv.values = {0.0, 0.1, 0.2, 3.0, 0.3, 0.4};  // jumps >0.8 at i=3 (->3.0) and i=4 (->0.3)

  const std::string spike = R"(
    local s = series("v")
    local JUMP = 0.8
    for i = 1, s:size() - 1 do
      local a = s:at(i - 1)
      local b = s:at(i)
      if math.abs(b.v - a.v) > JUMP then
        createPointMarker(b.t, b.v, {label="spike", severity="error"})
      end
    end
  )";

  std::string err;
  const auto markers = runMarkerScript(spike, oneSeries("v", sv), &err);
  ASSERT_TRUE(err.empty()) << err;
  ASSERT_EQ(markers.size(), 2u);

  EXPECT_EQ(markers[0].kind, PJ::sdk::MarkerKind::kEvent);
  EXPECT_TRUE(markers[0].has_value);
  EXPECT_EQ(markers[0].t_start, static_cast<PJ::Timestamp>(3 * kSec));
  EXPECT_DOUBLE_EQ(markers[0].value_low, 3.0);
  EXPECT_EQ(markers[0].severity, PJ::sdk::MarkerSeverity::kError);
  EXPECT_EQ(markers[0].label, "spike");

  EXPECT_EQ(markers[1].t_start, static_cast<PJ::Timestamp>(4 * kSec));
  EXPECT_DOUBLE_EQ(markers[1].value_low, 0.3);
}

TEST(MarkerEngine, RegionAndValueBand) {
  SeriesView sv;
  sv.timestamps = {0, 1 * kSec, 2 * kSec, 3 * kSec};
  sv.values = {0.0, 1.0, 2.0, 3.0};

  const std::string rule = R"(
    local s = series("v")
    startMarker(s:at(1).t)
    closeMarker(s:at(3).t, {label="r", severity="warning"})
    createBandMarker(-0.5, 0.5, {label="band"})
  )";

  std::string err;
  const auto markers = runMarkerScript(rule, oneSeries("v", sv), &err);
  ASSERT_TRUE(err.empty()) << err;
  ASSERT_EQ(markers.size(), 2u);

  EXPECT_EQ(markers[0].kind, PJ::sdk::MarkerKind::kRegion);
  EXPECT_EQ(markers[0].t_start, static_cast<PJ::Timestamp>(1 * kSec));
  EXPECT_EQ(markers[0].t_end, static_cast<PJ::Timestamp>(3 * kSec));
  EXPECT_EQ(markers[0].severity, PJ::sdk::MarkerSeverity::kWarning);
  EXPECT_EQ(markers[0].label, "r");

  EXPECT_EQ(markers[1].kind, PJ::sdk::MarkerKind::kValueBand);
  EXPECT_DOUBLE_EQ(markers[1].value_low, -0.5);
  EXPECT_DOUBLE_EQ(markers[1].value_high, 0.5);
  EXPECT_EQ(markers[1].severity, PJ::sdk::MarkerSeverity::kInfo);
}

// The explicit shortcuts emit markers identical to the matching createMarker case:
// createVerticalMarker(x) == createMarker(x); createHorizontalMarker(y) ==
// createMarker(nil, y); createPointMarker(x, y) == createMarker(x, y).
TEST(MarkerEngine, ShortcutsMatchCreateMarker) {
  SeriesView sv;
  sv.timestamps = {0, 1 * kSec};
  sv.values = {0.0, 1.0};

  const std::string rule = R"(
    createVerticalMarker(1, {label="v"})
    createHorizontalMarker(0.5, {label="h"})
    createPointMarker(2, 0.25, {label="p"})
  )";

  std::string err;
  const auto markers = runMarkerScript(rule, oneSeries("v", sv), &err);
  ASSERT_TRUE(err.empty()) << err;
  ASSERT_EQ(markers.size(), 3u);

  // vertical line: an event with no value, severity error.
  EXPECT_EQ(markers[0].kind, PJ::sdk::MarkerKind::kEvent);
  EXPECT_FALSE(markers[0].has_value);
  EXPECT_EQ(markers[0].t_start, static_cast<PJ::Timestamp>(1));
  EXPECT_EQ(markers[0].severity, PJ::sdk::MarkerSeverity::kError);
  EXPECT_EQ(markers[0].label, "v");

  // horizontal line: a degenerate value band [y, y], severity warning.
  EXPECT_EQ(markers[1].kind, PJ::sdk::MarkerKind::kValueBand);
  EXPECT_DOUBLE_EQ(markers[1].value_low, 0.5);
  EXPECT_DOUBLE_EQ(markers[1].value_high, 0.5);
  EXPECT_EQ(markers[1].severity, PJ::sdk::MarkerSeverity::kWarning);
  EXPECT_EQ(markers[1].label, "h");

  // point: an event carrying a value.
  EXPECT_EQ(markers[2].kind, PJ::sdk::MarkerKind::kEvent);
  EXPECT_TRUE(markers[2].has_value);
  EXPECT_EQ(markers[2].t_start, static_cast<PJ::Timestamp>(2));
  EXPECT_DOUBLE_EQ(markers[2].value_low, 0.25);
  EXPECT_EQ(markers[2].label, "p");
}

TEST(MarkerEngine, BandPowerDetectsSinusoid) {
  // 1 Hz sine sampled at 100 Hz for 2 s; band [0.5,1.5] Hz should hold real power.
  SeriesView sv;
  for (int i = 0; i < 200; ++i) {
    sv.timestamps.push_back(static_cast<double>(i) * (kSec / 100.0));
    sv.values.push_back(std::sin(2.0 * std::numbers::pi * 1.0 * (static_cast<double>(i) / 100.0)));
  }
  const std::string rule = R"(
    local s = series("sine")
    local p = bandPower(s, 0.5, 1.5)
    if p > 0 then createMarker(0, p, {label="vibration"}) end
  )";

  std::string err;
  const auto markers = runMarkerScript(rule, oneSeries("sine", sv), &err);
  ASSERT_TRUE(err.empty()) << err;
  ASSERT_EQ(markers.size(), 1u);
  EXPECT_GT(markers[0].value_low, 0.0);
}

TEST(MarkerEngine, CompileErrorReportsAndEmitsNothing) {
  SeriesView sv;
  sv.timestamps = {0, 1 * kSec};
  sv.values = {0.0, 1.0};
  std::string err;
  const auto markers = runMarkerScript("this is not valid lua (((", oneSeries("v", sv), &err);
  EXPECT_TRUE(markers.empty());
  EXPECT_FALSE(err.empty());
}

// A non-finite marker time (math.huge / 0/0) must be rejected, not cast to int64
// (undefined behavior). The script errors cleanly and emits nothing.
TEST(MarkerEngine, NonFiniteMarkerTimeErrorsInsteadOfUB) {
  SeriesView sv;
  sv.timestamps = {0, 1 * kSec};
  sv.values = {0.0, 1.0};
  for (const char* expr :
       {"createVerticalMarker(math.huge)", "createPointMarker(0/0, 1)", "startMarker(-math.huge); closeMarker(0)"}) {
    std::string err;
    const auto markers = runMarkerScript(expr, oneSeries("v", sv), &err);
    EXPECT_TRUE(markers.empty()) << expr;
    EXPECT_FALSE(err.empty()) << expr;
  }
}

// A non-finite argument to series:atTime must not read out of bounds (a NaN query
// used to collapse lower_bound to begin() and underflow the low index to SIZE_MAX).
TEST(MarkerEngine, AtTimeNonFiniteDoesNotReadOutOfBounds) {
  SeriesView sv;
  sv.timestamps = {0, 1 * kSec, 2 * kSec};
  sv.values = {10.0, 20.0, 30.0};
  std::string err;
  // Emits a point using the atTime(NaN) result — must run without crashing (ASAN).
  const auto markers =
      runMarkerScript("createPointMarker(1000000000, series(\"v\"):atTime(0/0))", oneSeries("v", sv), &err);
  ASSERT_TRUE(err.empty()) << err;
  ASSERT_EQ(markers.size(), 1u);
  EXPECT_EQ(markers[0].value_low, 0.0);  // atTime(NaN) → 0.0 sentinel
}

// A loop that never calls a native function is stopped by the interpreter's own
// interrupt: a set cancel ends it with the cancelled error, not the fuel one.
TEST(MarkerEngine, LuauLoopHonoursTheCancelToken) {
  std::string err;
  const auto markers = runMarkerScript("while true do end", SeriesProvider{}, &err, {}, [] { return true; });
  EXPECT_TRUE(markers.empty());
  EXPECT_NE(err.find("cancelled"), std::string::npos) << err;
  EXPECT_EQ(err.find("instruction budget"), std::string::npos) << err;
}

// Markers and labels live outside the VM's memory cap, so they have their own ceiling.
TEST(MarkerEngine, NativeOutputByteCeilingRejectsManyMarkersAndLargeLabels) {
  std::string err;
  const auto many = runMarkerScript("for i = 1, 1000 do createMarker(i) end", SeriesProvider{}, &err, {}, {}, 1024);
  EXPECT_TRUE(many.empty());
  EXPECT_NE(err.find("byte ceiling"), std::string::npos) << err;
  const auto label =
      runMarkerScript("createMarker(0, 1, {label = string.rep('x', 2048)})", SeriesProvider{}, &err, {}, {}, 1024);
  EXPECT_TRUE(label.empty());
  EXPECT_NE(err.find("byte ceiling"), std::string::npos) << err;
}

// A pcall inside the script catches the ceiling error, but the run still fails as a whole:
// a truncated marker set must never replace the previous one.
TEST(MarkerEngine, NativeOutputByteCeilingSurvivesAScriptPcall) {
  std::string err;
  const auto markers = runMarkerScript(
      "pcall(function() for i = 1, 1000 do createMarker(i) end end)", SeriesProvider{}, &err, {}, {}, 1024);
  EXPECT_TRUE(markers.empty());
  EXPECT_NE(err.find("byte ceiling"), std::string::npos) << err;
}

// A cancel the script swallows with pcall still fails the run, with or without an error out-parameter.
TEST(MarkerEngine, ACancelSwallowedByPcallStillFailsTheRun) {
  const std::string script = "pcall(function() while true do end end)\ncreateMarker(0)\n";
  int polls = 0;
  // True once, at a poll deep inside the pcall'd loop, then false: the script goes on to emit a marker.
  const auto once = [&polls] { return ++polls == 100; };
  std::string err;
  EXPECT_TRUE(runMarkerScript(script, SeriesProvider{}, &err, {}, once).empty());
  EXPECT_NE(err.find("cancelled"), std::string::npos) << err;
  polls = 0;
  EXPECT_TRUE(runMarkerScript(script, SeriesProvider{}, nullptr, {}, once).empty());
}

}  // namespace
