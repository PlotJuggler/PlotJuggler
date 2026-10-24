// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <pj_plugins/dialog_protocol.h>

#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <vector>

#include "host_info_capture.hpp"

/// State shared between the test and the MockPanelPlugin instance.
/// The dialog plugin ABI has no per-instance test parameterization, so the
/// plugin reads/writes from this single global, and tests reset it before
/// each scenario via resetMockPanelState().
struct MockPanelState {
  std::string text;
  std::string label = "Hello";
  bool chart_enabled = false;

  bool close_on_next_tick = false;
  std::string close_reason;

  bool sub_dialog_on_next_tick = false;
  std::string sub_dialog_ui;

  bool sub_panel_on_next_tick = false;
  std::string sub_panel_ui;

  std::vector<std::string> events_seen;

  // Number of widget_data() polls the host has made — lets tests pin how many
  // full document builds a given interaction costs.
  int widget_data_calls = 0;

  // The `scenePreview` frame's scene_view request; no key while `kind` is empty.
  PJ::SceneView scene_view;
  // Every scene_view_error received, as "<frame>|<kind>|<topic>|<dataset>|<reason>".
  std::vector<std::string> scene_view_errors;
  // Answers to a scene_view_error: a one-shot close request ("scene error"), and a
  // label reading "errors:<count received>".
  bool close_on_scene_error = false;
  bool label_counts_scene_errors = false;

  HostInfoCapture host_info;
};

MockPanelState& mockPanelState();
void resetMockPanelState();
