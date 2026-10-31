#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "FileLoader.h"
#include "LayoutXml.h"

namespace PJ {

// LoadHints for replaying ONE layout-recorded source — the shape the desktop
// reload (loadLayoutFromPath), the browser source-bound replay, and the
// layout-import batch's stock cache-hit loads share: both saved plugin
// identities ride their own tier fields, a layout that names a plugin
// requires it, and replay is automated (dialogs prohibited, the saved preset
// authoritative — DialogPolicy::kNever). Every replay points the preset (and
// its fan-out children) at the path it actually loads, so a moved or copied
// layout folder replays the data beside it.
[[nodiscard]] inline LoadHints layoutReplayHints(const layout_xml::DataSourceRef& source, bool prefer_reuse) {
  const bool names_plugin = !source.plugin_manifest_id.isEmpty() || !source.plugin_id.isEmpty();
  return LoadHints{
      .expected_manifest_id = source.plugin_manifest_id,
      .expected_plugin_id = source.plugin_id,
      .preset_config_json = source.plugin_config_json,
      .dialog_policy = DialogPolicy::kNever,
      .prefer_reuse = prefer_reuse,
      .require_expected_plugin = names_plugin,
      .record_saved_filepath = true,
  };
}

}  // namespace PJ
