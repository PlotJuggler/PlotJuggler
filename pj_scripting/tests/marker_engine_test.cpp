// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The Luau marker engine over known synthetic series: exact markers, and the failure paths (non-finite input, byte
// ceiling, cancellation).

#include "pj_scripting/marker_engine.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
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

// A non-finite argument to series:atTime must not read out of bounds: a NaN query would collapse lower_bound to begin()
// and wrap the low index.
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

// mode: 0 0 1 1 2 2 1 at t = 0..6 s.
SeriesView modeSeries() {
  SeriesView sv;
  sv.timestamps = {0, 1 * kSec, 2 * kSec, 3 * kSec, 4 * kSec, 5 * kSec, 6 * kSec};
  sv.values = {0, 0, 1, 1, 2, 2, 1};
  return sv;
}

PJ::sdk::ColorRGBA rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
  return {r, g, b, 255};
}

void expectRegion(const PJ::sdk::PlotMarker& m, double t0, double t1, PJ::sdk::ColorRGBA color) {
  EXPECT_EQ(m.kind, PJ::sdk::MarkerKind::kRegion);
  EXPECT_EQ(m.t_start, static_cast<PJ::Timestamp>(t0));
  EXPECT_EQ(m.t_end, static_cast<PJ::Timestamp>(t1));
  EXPECT_EQ(m.color, color);
}

// Without a condition, a table maps values to colors: one region per run of equal values, an unmapped value draws
// nothing, and the last run ends at the last sample.
TEST(MarkerEngine, WhenBackgroundColorTableMergesRunsAndSkipsUnmapped) {
  const SeriesView sv = modeSeries();
  std::string err;
  const auto markers = runMarkerScript(
      R"(when("mode"):backgroundColor({ [1] = "green", [2] = "#ff0000" }))", oneSeries("mode", sv), &err);
  ASSERT_TRUE(err.empty()) << err;
  // The final run is a single sample at the last timestamp: zero width, so nothing.
  ASSERT_EQ(markers.size(), 2u);
  expectRegion(markers[0], 2 * kSec, 4 * kSec, rgb(0, 128, 0));
  expectRegion(markers[1], 4 * kSec, 6 * kSec, rgb(255, 0, 0));
  EXPECT_EQ(markers[1].severity, PJ::sdk::MarkerSeverity::kInfo);
}

// A color function returning option tables: equal tables built per sample still merge, and their label lands.
TEST(MarkerEngine, WhenBackgroundColorFunctionReturnsOptions) {
  const SeriesView sv = modeSeries();
  std::string err;
  const auto markers = runMarkerScript(
      R"(
    local function modeColor(v)
      if v >= 1 then
        return { color = "Orange", label = "on" }
      end
    end
    when(series("mode")):backgroundColor(modeColor)
  )",
      oneSeries("mode", sv), &err);
  ASSERT_TRUE(err.empty()) << err;
  ASSERT_EQ(markers.size(), 1u);
  expectRegion(markers[0], 2 * kSec, 6 * kSec, rgb(255, 165, 0));
  EXPECT_EQ(markers[0].label, "on");
}

// A color function that mutates and returns one shared table: each region keeps the options of its own run.
TEST(MarkerEngine, WhenBackgroundColorSnapshotsAReusedResultTable) {
  const SeriesView sv = modeSeries();  // 0 0 1 1 2 2 1
  std::string err;
  const auto markers = runMarkerScript(
      "local t = {}\n"
      "when('mode'):backgroundColor(function(v) t.color = (v == 1) and 'red' or 'blue'; return t end)\n",
      oneSeries("mode", sv), &err);
  ASSERT_TRUE(err.empty()) << err;
  ASSERT_EQ(markers.size(), 3u);
  expectRegion(markers[0], 0, 2 * kSec, rgb(0, 0, 255));
  expectRegion(markers[1], 2 * kSec, 4 * kSec, rgb(255, 0, 0));
  expectRegion(markers[2], 4 * kSec, 6 * kSec, rgb(0, 0, 255));
}

// Level conditions: below and above are strict, between and outside include their limits' complement correctly, and
// one region spans each stretch where the condition holds.
TEST(MarkerEngine, WhenConditionsShadeWhileTheyHold) {
  SeriesView sv;
  sv.timestamps = {0, 1 * kSec, 2 * kSec, 3 * kSec, 4 * kSec, 5 * kSec};
  sv.values = {0.5, 1.0, 1.5, 2.0, 2.5, 2.5};
  std::string err;
  const auto markers = runMarkerScript(
      R"(
    when("v", below(1.0)):backgroundColor("green")
    when("v", between(1.0, 2.0)):backgroundColor("yellow", { label = "mid" })
    when("v", above(2.0)):backgroundColor("red")
    when("v", outside(1.0, 2.0)):backgroundColor("blue")
  )",
      oneSeries("v", sv), &err);
  ASSERT_TRUE(err.empty()) << err;
  ASSERT_EQ(markers.size(), 5u);
  expectRegion(markers[0], 0, 1 * kSec, rgb(0, 128, 0));
  expectRegion(markers[1], 1 * kSec, 4 * kSec, rgb(255, 255, 0));
  EXPECT_EQ(markers[1].label, "mid");
  expectRegion(markers[2], 4 * kSec, 5 * kSec, rgb(255, 0, 0));
  expectRegion(markers[3], 0, 1 * kSec, rgb(0, 0, 255));
  expectRegion(markers[4], 4 * kSec, 5 * kSec, rgb(0, 0, 255));
}

// minDuration drops a stretch shorter than it; a condition function sees (v, t, prev, prev_t) and may read another
// series at t; no color draws the severity color (alpha 0).
TEST(MarkerEngine, WhenMinDurationAndConditionFunctions) {
  SeriesView flag;
  flag.timestamps = {0, 1 * kSec, 2 * kSec, 3 * kSec, 4 * kSec, 7 * kSec, 8 * kSec};
  flag.values = {0, 1, 0, 0, 1, 1, 0};  // raised 1 s, then 4 s
  SeriesProvider p;
  p.names = {"flag"};
  p.get = [&](const std::string& n) -> const SeriesView* { return n == "flag" ? &flag : nullptr; };
  std::string err;
  const auto markers = runMarkerScript(
      R"(
    when("flag", isTrue()):backgroundColor("red", { minDuration = 2 })
    local other = series("flag")
    when("flag", function(v, t, prev, prev_t)
      return prev ~= nil and t - prev_t > 2e9 and other:atTime(t) == 1
    end):backgroundColor(nil, { severity = "error" })
  )",
      p, &err);
  ASSERT_TRUE(err.empty()) << err;
  ASSERT_EQ(markers.size(), 2u);
  expectRegion(markers[0], 4 * kSec, 8 * kSec, rgb(255, 0, 0));
  expectRegion(markers[1], 7 * kSec, 8 * kSec, PJ::sdk::ColorRGBA{0, 0, 0, 0});
  EXPECT_EQ(markers[1].severity, PJ::sdk::MarkerSeverity::kError);
}

// event and verticalLine fire once where the condition starts to hold (every sample with `every`), minGap skips a
// mark too close to the previous one, changes() fires at every change with a label built from (v, prev).
TEST(MarkerEngine, WhenEventsFireOncePerStretch) {
  const SeriesView sv = modeSeries();  // 0 0 1 1 2 2 1 at 0..6 s
  std::string err;
  const auto markers = runMarkerScript(
      R"(
    when("mode", above(0)):event({ label = "up" })
    when("mode", above(0)):verticalLine({ every = true })
    when("mode", changes()):verticalLine({ label = function(v, prev) return prev .. ">" .. v end })
    when("mode", changes()):event({ minGap = 3 })
  )",
      oneSeries("mode", sv), &err);
  ASSERT_TRUE(err.empty()) << err;
  ASSERT_EQ(markers.size(), 1u + 5u + 3u + 2u);
  // above(0) holds from 2 s to the end: one event.
  EXPECT_EQ(markers[0].kind, PJ::sdk::MarkerKind::kEvent);
  EXPECT_TRUE(markers[0].has_value);
  EXPECT_EQ(markers[0].t_start, static_cast<PJ::Timestamp>(2 * kSec));
  EXPECT_EQ(markers[0].label, "up");
  // every = true: a line at each of the five samples where it holds.
  EXPECT_EQ(markers[1].t_start, static_cast<PJ::Timestamp>(2 * kSec));
  EXPECT_FALSE(markers[1].has_value);
  EXPECT_EQ(markers[5].t_start, static_cast<PJ::Timestamp>(6 * kSec));
  // changes at 2, 4, 6 s.
  EXPECT_EQ(markers[6].label, "0>1");
  EXPECT_EQ(markers[7].label, "1>2");
  EXPECT_EQ(markers[8].label, "2>1");
  EXPECT_EQ(markers[8].t_start, static_cast<PJ::Timestamp>(6 * kSec));
  // minGap = 3 s: 2 s fires, 4 s is skipped, 6 s fires.
  EXPECT_EQ(markers[9].t_start, static_cast<PJ::Timestamp>(2 * kSec));
  EXPECT_EQ(markers[10].t_start, static_cast<PJ::Timestamp>(6 * kSec));
}

// Each bad call fails the run with its own message, not a later generic Luau error.
TEST(MarkerEngine, WhenRejectsBadArguments) {
  const SeriesView sv = modeSeries();
  const std::pair<const char*, const char*> cases[] = {
      {R"(when("missing", above(1)):event())", "no series named 'missing'"},
      {R"(when({}, above(1)):event())", "the first argument must be"},
      {R"(when("mode", 3):event())", "the condition must be"},
      {R"(when("mode", { test = function() return true end }):event())", "the condition must be"},
      {R"(when("mode", above("1")):event())", "the limit must be a number"},
      {R"(when("mode", equals("1")):event())", "the limit must be a number"},
      {R"(when("mode", between(5, 1)):event())", "the low limit is above the high limit"},
      {R"(when("mode", outside(5, 1)):event())", "the low limit is above the high limit"},
      {R"(when("mode", above(0)).event())", "call event with ':'"},
      {R"(when("mode", above(0)).backgroundColor("red"))", "call backgroundColor with ':'"},
      {R"(when("mode", above(0)):event({ minGap = 0/0 }))", "minGap must be a number"},
      {R"(when("mode"):event())", "event needs a condition"},
      {R"(when("mode", changes()):backgroundColor("red"))", "changes() marks moments"},
      {R"(when("mode", above(0)):backgroundColor({ "red" }))", "the color is a string"},
      {R"(when("mode"):backgroundColor("red"))", "without a condition, pass a table"},
      {R"(when("mode", above(0)):backgroundColor("red", { minDuration = "1" }))", "minDuration must be a number"},
      {R"(when("mode"):backgroundColor({ [0] = "notacolor" }))", "unknown color 'notacolor'"},
      {R"(when("mode"):backgroundColor({ [0] = "transparent" }))", "unknown color 'transparent'"},
      {R"(when("mode"):backgroundColor(function(v) return v > 0 end))", "color must be a string"},
      {R"(createVerticalMarker(0, { color = { 1, 0, 0 } }))", "color must be a string"},
  };
  for (const auto& [script, message] : cases) {
    std::string err;
    EXPECT_TRUE(runMarkerScript(script, oneSeries("mode", sv), &err).empty()) << script;
    EXPECT_NE(err.find(message), std::string::npos) << script << " -> " << err;
    // An argument error names the rule's own line, not the prelude's. Colors are checked by the natives where the
    // prelude emits, so their errors name the prelude.
    if (std::string_view(message).find("color") == std::string_view::npos) {
      EXPECT_EQ(err.rfind("rule:1:", 0), 0u) << script << " -> " << err;
    }
  }
}

// changes() does not mark every sample of a NaN run (NaN ~= NaN).
TEST(MarkerEngine, WhenChangesIgnoresNaNRuns) {
  SeriesView sv;
  sv.timestamps = {0, 1 * kSec, 2 * kSec, 3 * kSec};
  sv.values = {0, std::nan(""), std::nan(""), 0};
  std::string err;
  const auto markers = runMarkerScript(R"(when("v", changes()):verticalLine())", oneSeries("v", sv), &err);
  ASSERT_TRUE(err.empty()) << err;
  ASSERT_EQ(markers.size(), 2u);
  EXPECT_EQ(markers[0].t_start, static_cast<PJ::Timestamp>(1 * kSec));
  EXPECT_EQ(markers[1].t_start, static_cast<PJ::Timestamp>(3 * kSec));
}

// The rules create_markers' template form writes run. A built-in condition costs what a hand-written loop does (the
// loop and the sample read per sample), where a condition function trips the same budget; each mark costs a few calls
// more, so the per-sample line style runs under the default budget.
TEST(MarkerEngine, ThresholdTemplateRulesRunWithinALoopsFuel) {
  SeriesView sv;
  for (int i = 0; i < 1000; ++i) {
    sv.timestamps.push_back(i * 1e6);
    sv.values.push_back(i % 100 < 50 ? 0.0 : 5.0);
  }
  PJ::scripting::BudgetLimits limits;
  limits.setup_fuel = 3000;
  const char* rules[] = {
      R"(when("v", above(2.5)):backgroundColor(nil, {label="exceeds 2.5", severity="warning"}))",
      R"(when("v", between(2.5, math.huge)):backgroundColor(nil, {label="exceeds 2.5", severity="warning"}))",
  };
  for (const char* rule : rules) {
    std::string err;
    EXPECT_FALSE(runMarkerScript(rule, oneSeries("v", sv), &err, limits).empty()) << rule;
    EXPECT_TRUE(err.empty()) << rule << " -> " << err;
  }
  std::string err;
  EXPECT_EQ(
      runMarkerScript(
          R"(when("v", between(-math.huge, 2.5)):verticalLine({label="exceeds 2.5", severity="warning", every=true}))",
          oneSeries("v", sv), &err)
          .size(),
      500u);
  EXPECT_TRUE(err.empty()) << err;
  EXPECT_TRUE(runMarkerScript(
                  R"(when("v", function(v) return v >= 2.5 end):backgroundColor())", oneSeries("v", sv), &err, limits)
                  .empty());
  EXPECT_NE(err.find("instruction budget"), std::string::npos) << err;
}

// A closeMarker that fails (caught by the rule) leaves the region open for the next closeMarker.
TEST(MarkerEngine, AFailedCloseMarkerKeepsTheRegionOpen) {
  std::string err;
  const auto markers =
      runMarkerScript("startMarker(1e9)\npcall(closeMarker, 0/0)\ncloseMarker(3e9)\n", SeriesProvider{}, &err);
  ASSERT_TRUE(err.empty()) << err;
  ASSERT_EQ(markers.size(), 1u);
  EXPECT_EQ(markers[0].t_start, static_cast<PJ::Timestamp>(1 * kSec));
}

// when(...):backgroundColor leaves a region the rule opened with startMarker alone.
TEST(MarkerEngine, WhenDoesNotCloseTheRulesOpenRegion) {
  const SeriesView sv = modeSeries();
  std::string err;
  const auto markers = runMarkerScript(
      "startMarker(0)\nwhen('mode', equals(2)):backgroundColor('red')\ncloseMarker(1e9, { label = 'mine' })\n",
      oneSeries("mode", sv), &err);
  ASSERT_TRUE(err.empty()) << err;
  ASSERT_EQ(markers.size(), 2u);
  EXPECT_EQ(markers[1].label, "mine");
  EXPECT_EQ(markers[1].t_end, static_cast<PJ::Timestamp>(1 * kSec));
}

TEST(MarkerEngine, ParseMarkerColor) {
  using PJ::scripting::parseMarkerColor;
  ASSERT_TRUE(parseMarkerColor("#0a0B0c"));
  EXPECT_EQ(parseMarkerColor("#0a0B0c")->b, 0x0c);
  ASSERT_TRUE(parseMarkerColor("#f80"));
  EXPECT_EQ(parseMarkerColor("#f80")->g, 0x88);
  ASSERT_TRUE(parseMarkerColor("SteelBlue"));
  EXPECT_EQ(parseMarkerColor("SteelBlue")->g, 130);
  ASSERT_TRUE(parseMarkerColor("Light Blue"));  // Qt ignores spaces in names
  EXPECT_EQ(parseMarkerColor("Light Blue")->b, 230);
  // Alpha 0 is PlotMarker's "use the severity color", so Qt's "transparent" would paint the default color.
  EXPECT_FALSE(parseMarkerColor("transparent"));
  EXPECT_FALSE(parseMarkerColor(""));
  EXPECT_FALSE(parseMarkerColor("#12345"));
  EXPECT_FALSE(parseMarkerColor("#gg0000"));
  EXPECT_FALSE(parseMarkerColor("reddish"));
}

}  // namespace
