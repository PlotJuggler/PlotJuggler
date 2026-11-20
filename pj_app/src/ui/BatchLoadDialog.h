#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QList>
#include <QStringList>
#include <functional>
#include <optional>

#include "FileLoader.h"
#include "pj_widgets/Dialog.h"

namespace Ui {
class BatchLoadDialog;
}

namespace PJ {

// Asked once, before the files of a batch (several files picked or listed together) start loading: how the loaders'
// configuration dialogs behave for the batch (remembered across sessions), and, when a layout comes with the files,
// which files get their own copy of its tabs (all checked by default).
class BatchLoadDialog : public Dialog {
  Q_OBJECT
 public:
  struct Answer {
    BatchDialogMode mode = BatchDialogMode::kShowFirst;
    QList<int> compared;  // indexes into `layout_files`, in list order; empty without a layout
  };

  // An empty `layout_files` hides the list.
  BatchLoadDialog(int file_count, const QStringList& layout_files, QWidget* parent = nullptr);
  ~BatchLoadDialog() override;

  [[nodiscard]] Answer answer() const;

  // `on_done` receives the answer, or nullopt on Cancel; the same desktop-modal / browser-async contract as
  // LayoutDatasetDialog::ask. An accepted answer stores the mode for the next batch.
  static void ask(
      QWidget* parent, int file_count, const QStringList& layout_files,
      std::function<void(std::optional<Answer>)> on_done);

 private:
  Ui::BatchLoadDialog* ui_;
};

}  // namespace PJ
