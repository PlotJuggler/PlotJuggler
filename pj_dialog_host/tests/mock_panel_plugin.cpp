// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// MockPanelPlugin — minimal dialog plugin used to drive PanelEngine in tests.
// Distinct from mock_dialog.cpp (which is consumed by dialog_engine_test).
//
// State is held in a single global instance because the plugin ABI provides
// no per-test parameterization. Tests reset the global via the helper
// `resetMockPanelState()` declared in mock_panel_plugin.hpp.

#include "mock_panel_plugin.hpp"

#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>

namespace {

constexpr const char* kUiContent = R"(<?xml version="1.0" encoding="UTF-8"?>
<ui version="4.0">
 <class>MockPanel</class>
 <widget class="QWidget" name="MockPanel">
  <layout class="QVBoxLayout">
   <item><widget class="QLabel" name="labelHello"><property name="text"><string>Hello</string></property></widget></item>
   <item><widget class="QLineEdit" name="textBox"/></item>
   <item><widget class="QFrame" name="chartPreview"/></item>
   <item><widget class="QFrame" name="scenePreview"/></item>
   <item><widget class="QPushButton" name="buttonClose"><property name="text"><string>Close</string></property></widget></item>
   <item><widget class="QDialogButtonBox" name="buttonBox"><property name="standardButtons"><set>QDialogButtonBox::Close</set></property></widget></item>
  </layout>
 </widget>
</ui>
)";

MockPanelState g_state;

}  // namespace

MockPanelState& mockPanelState() {
  return g_state;
}

void resetMockPanelState() {
  g_state = MockPanelState{};
}

class MockPanelPlugin : public PJ::DialogPluginTyped {
 public:
  std::string manifest() const override {
    return R"({"id":"mock-panel","name":"Mock Panel","version":"0.0.1"})";
  }
  std::string ui_content() const override {
    g_state.host_info.record(hostInfo());
    return kUiContent;
  }
  std::string widget_data() override {
    ++g_state.widget_data_calls;
    PJ::WidgetData wd;
    wd.setText("textBox", g_state.text);
    wd.setLabel("labelHello", g_state.label);
    if (g_state.chart_enabled) {
      wd.setChartSeries("chartPreview", {{"input", {{0.0, 1.0}, {1.0, 2.0}, {2.0, 3.0}}, "#4c9aff", false}});
    }
    if (!g_state.scene_view.kind.empty()) {
      wd.setSceneView("scenePreview", g_state.scene_view);
    }
    if (g_state.close_on_next_tick) {
      wd.requestClose(g_state.close_reason);
      g_state.close_on_next_tick = false;
    }
    if (g_state.sub_dialog_on_next_tick) {
      wd.requestSubDialog(g_state.sub_dialog_ui);
      g_state.sub_dialog_on_next_tick = false;
    }
    if (g_state.sub_panel_on_next_tick) {
      wd.requestSubPanel(g_state.sub_panel_ui);
      g_state.sub_panel_on_next_tick = false;
    }
    return wd.toJson();
  }

  bool onTextChanged(std::string_view widget_name, std::string_view text) override {
    if (widget_name == "textBox") {
      g_state.text = std::string(text);
      g_state.events_seen.emplace_back("textBox:" + g_state.text);
      return true;
    }
    return false;
  }

  bool onSceneViewError(std::string_view widget_name, const PJ::SceneViewError& error) override {
    g_state.scene_view_errors.push_back(
        std::string(widget_name) + "|" + error.kind + "|" + error.topic + "|" + error.dataset + "|" + error.reason);
    if (g_state.close_on_scene_error) {
      g_state.close_on_next_tick = true;
      g_state.close_reason = "scene error";
    }
    if (g_state.label_counts_scene_errors) {
      g_state.label = "errors:" + std::to_string(g_state.scene_view_errors.size());
    }
    return true;
  }

  bool onClicked(std::string_view widget_name) override {
    if (widget_name == "buttonClose") {
      g_state.close_on_next_tick = true;
      g_state.close_reason = "user_back";
      g_state.events_seen.emplace_back("buttonClose");
      return true;
    }
    return false;
  }
};

PJ_DIALOG_PLUGIN(MockPanelPlugin)
