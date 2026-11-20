// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "ui/LayoutDatasetDialog.h"

#include <QDialogButtonBox>
#include <QLayout>
#include <QListWidgetItem>
#include <QPushButton>
#include <QSignalBlocker>

#include "ui_LayoutDatasetDialog.h"

namespace PJ {

LayoutDatasetDialog::LayoutDatasetDialog(
    const QString& layout_name, const QStringList& labels, const QString& single_only_reason, QWidget* parent)
    : Dialog(parent), ui_(new Ui::LayoutDatasetDialog) {
  setDialogTitle(tr("Load Layout"));
  auto* body = new QWidget;
  ui_->setupUi(body);
  contentLayout()->addWidget(body);

  ui_->labelPrompt->setText(
      tr("Several loaded datasets fit the layout \"%1\". Which ones should it show? Each checked dataset gets its own "
         "copy of the layout's tabs.")
          .arg(layout_name));
  ui_->labelSingleOnly->setText(single_only_reason);
  ui_->labelSingleOnly->setVisible(!single_only_reason.isEmpty());
  for (qsizetype index = 0; index < labels.size(); ++index) {
    auto* item = new QListWidgetItem(labels[index], ui_->listDatasets);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
    item->setCheckState(index == 0 ? Qt::Checked : Qt::Unchecked);
  }
  // Space toggles the current row, so the list works from the keyboard.
  ui_->listDatasets->setCurrentRow(0);
  ui_->listDatasets->setFocus();
  QPushButton* ok = ui_->buttonBox->button(QDialogButtonBox::Ok);
  const bool single_only = !single_only_reason.isEmpty();
  connect(ui_->listDatasets, &QListWidget::itemChanged, this, [this, ok, single_only](QListWidgetItem* changed) {
    if (single_only && changed->checkState() == Qt::Checked) {
      const QSignalBlocker blocker(ui_->listDatasets);
      for (int row = 0; row < ui_->listDatasets->count(); ++row) {
        if (QListWidgetItem* item = ui_->listDatasets->item(row); item != changed) {
          item->setCheckState(Qt::Unchecked);
        }
      }
    }
    ok->setEnabled(!checkedIndexes().isEmpty());
  });
  connect(ui_->buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(ui_->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

  // The frameless Dialog chrome otherwise opens at its base minimum and clips the content.
  layout()->activate();
  resize(sizeHint());
}

LayoutDatasetDialog::~LayoutDatasetDialog() {
  delete ui_;
}

QList<int> LayoutDatasetDialog::checkedIndexes() const {
  QList<int> checked;
  for (int row = 0; row < ui_->listDatasets->count(); ++row) {
    if (ui_->listDatasets->item(row)->checkState() == Qt::Checked) {
      checked.push_back(row);
    }
  }
  return checked;
}

void LayoutDatasetDialog::ask(
    QWidget* parent, const QString& layout_name, const QStringList& labels, const QString& single_only_reason,
    std::function<void(std::optional<QList<int>>)> on_done) {
  auto* dialog = new LayoutDatasetDialog(layout_name, labels, single_only_reason, parent);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  connect(dialog, &QDialog::finished, dialog, [dialog, on_done = std::move(on_done)](int result) {
    const QList<int> checked = dialog->checkedIndexes();
    on_done(result == QDialog::Accepted && !checked.isEmpty() ? std::optional(checked) : std::nullopt);
  });
#ifdef PJ_TARGET_WASM
  dialog->open();
#else
  dialog->exec();
#endif
}

}  // namespace PJ
