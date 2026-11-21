#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QList>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

#include "pj_widgets/Dialog.h"

namespace Ui {
class LayoutDatasetDialog;
}

namespace PJ {

// Asks which loaded datasets a layout aimed at one dataset should show: one checkbox per candidate, all checked. A
// non-empty `single_only_reason` allows one checked dataset at a time (the first, initially) and says why.
class LayoutDatasetDialog : public Dialog {
  Q_OBJECT
 public:
  LayoutDatasetDialog(
      const QString& layout_name, const QStringList& labels, const QString& single_only_reason,
      QWidget* parent = nullptr);
  ~LayoutDatasetDialog() override;

  // Indexes into `labels`, in list order.
  [[nodiscard]] QList<int> checkedIndexes() const;

  // `on_done` receives the checked indexes, or nullopt on Cancel. The same contract as MessageBox::questionAsync: on
  // desktop the dialog is modal and `on_done` runs before this returns; in the browser, which cannot run exec()'s
  // nested event loop, it opens non-blocking and `on_done` runs once answered. The dialog is parented to `parent`, so
  // capturing `parent` is safe.
  static void ask(
      QWidget* parent, const QString& layout_name, const QStringList& labels, const QString& single_only_reason,
      std::function<void(std::optional<QList<int>>)> on_done);

 private:
  Ui::LayoutDatasetDialog* ui_;
};

}  // namespace PJ
