#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QStringList>
#include <functional>
#include <memory>
#include <optional>

#include "FileLoader.h"
#include "pj_widgets/Dialog.h"

namespace Ui {
class BatchLoadDialog;
}

namespace PJ {

// Asked once, before the files of a batch (several files picked or listed together) start loading: how the loaders'
// configuration dialogs behave for the batch (remembered across sessions), when one would repeat, and, when a layout
// comes with the files, which files get their own copy of its tabs (all checked by default).
class BatchLoadDialog : public Dialog {
  Q_OBJECT
 public:
  struct Answer {
    std::shared_ptr<LoadBatch> batch;  // for every file's LoadHints
    QStringList compared;              // the checked files' paths, in list order; empty without a layout
  };

  // `ask_mode` shows the dialog-mode choice; without it the batch shows every dialog. `list_files` lists `paths` to
  // choose which files get the layout's tabs.
  BatchLoadDialog(const QStringList& paths, bool ask_mode, bool list_files, QWidget* parent = nullptr);
  ~BatchLoadDialog() override;

  // `on_done` receives the answer, or nullopt on Cancel; the same desktop-modal / browser-async contract as
  // LayoutDatasetDialog::ask. An accepted answer with `ask_mode` stores the mode for the next batch.
  static void ask(
      QWidget* parent, const QStringList& paths, bool ask_mode, bool list_files,
      std::function<void(std::optional<Answer>)> on_done);

 private:
  [[nodiscard]] BatchDialogMode mode() const;
  [[nodiscard]] QStringList checkedPaths() const;

  Ui::BatchLoadDialog* ui_;
};

}  // namespace PJ
