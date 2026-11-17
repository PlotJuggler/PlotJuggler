#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <optional>

#include "pj_runtime/DiagnosticHistory.h"
#include "pj_widgets/Dialog.h"

namespace Ui {
class DiagnosticsDialog;
}

namespace PJ {

// Every diagnostic in a DiagnosticHistory, with severity / source / text filters and the selected record's full
// message. Every history or filter change rebuilds the table from `history->snapshot()` (the history is capped, so
// this is cheap and mirrors eviction); while hidden it only marks itself stale. The history must outlive the dialog.
class DiagnosticsDialog : public Dialog {
  Q_OBJECT
 public:
  explicit DiagnosticsDialog(DiagnosticHistory* history, QWidget* parent = nullptr);
  ~DiagnosticsDialog() override;

  // Rows surviving the current filters.
  [[nodiscard]] int visibleRowCount() const;
  [[nodiscard]] QString paneText() const;

 protected:
  void showEvent(QShowEvent* event) override;

 private:
  void rebuild();
  void applyFilters();
  void refreshSources(const QList<DiagnosticRecord>& records);
  void onSelectionChanged();
  [[nodiscard]] std::optional<DiagnosticRecord> selectedRecord() const;

  Ui::DiagnosticsDialog* ui_;
  DiagnosticHistory* history_;
  bool stale_ = true;
  bool splitter_sized_ = false;
};

}  // namespace PJ
