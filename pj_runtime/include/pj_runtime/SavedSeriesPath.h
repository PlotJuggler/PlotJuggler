#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Header-only and Qt-only so pj_app's DOM-only test runner can use it without
// linking pj_runtime (LayoutXml's SeriesPath derives from it).

#include <QString>
#include <cstdint>

namespace PJ {

/// A series identity as a layout persists it: topic and field plus the dataset
/// qualifiers (all optional). Resolved through `CatalogModel::resolveCurveKey`, so a
/// saved id is trusted only while the source label and path still agree.
struct SavedSeriesPath {
  QString topic;
  QString field;
  /// Exact in-session hint for undo/redo (0 = unqualified); a `DatasetId`. A resolver
  /// must reject ambiguity rather than silently pick the first dataset exposing the
  /// same topic/field.
  std::uint32_t dataset_id = 0;
  /// Portable raw source label (`DatasetInfo::source_name`), usable for layout reload
  /// after ids are reminted.
  QString dataset_source;
  /// Full file identity stamped by FileLoader at layout-file save. Unlike
  /// `dataset_source` (often a basename/display label), it distinguishes same-named
  /// files in different directories and guards a coincidentally reminted id.
  QString dataset_path;

  friend bool operator==(const SavedSeriesPath&, const SavedSeriesPath&) = default;
};

}  // namespace PJ
