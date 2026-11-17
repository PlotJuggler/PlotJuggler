#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "FileLoader.h"
#include "LayoutXml.h"

namespace PJ {

// LoadHints for replaying one layout-recorded source, shared by the desktop layout load, the browser source-bound
// replay and the import batch's stock loads: a layout that names a plugin requires it, no dialog runs and the saved
// preset is authoritative. The preset's filepath becomes the path actually loaded, so a moved layout folder replays
// the data beside it; side files resolve beside that data file unless the caller fills in their paths.
[[nodiscard]] inline LoadHints layoutReplayHints(const layout_xml::DataSourceRef& source, bool prefer_reuse) {
  const bool names_plugin = !source.plugin_manifest_id.isEmpty() || !source.plugin_id.isEmpty();
  QHash<QString, QString> side_file_paths;
  for (const layout_xml::SideFileRef& side_file : source.side_files) {
    side_file_paths.insert(side_file.filename, QString());
  }
  return LoadHints{
      .expected_manifest_id = source.plugin_manifest_id,
      .expected_plugin_id = source.plugin_id,
      .preset_config_json = source.plugin_config_json,
      .side_file_paths = std::move(side_file_paths),
      .dialog_policy = DialogPolicy::kNever,
      .prefer_reuse = prefer_reuse,
      .require_expected_plugin = names_plugin,
      .record_saved_filepath = true,
  };
}

}  // namespace PJ
