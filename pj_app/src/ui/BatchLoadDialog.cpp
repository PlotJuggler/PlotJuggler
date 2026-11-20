// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "ui/BatchLoadDialog.h"

#include <QDialogButtonBox>
#include <QLayout>
#include <QListWidgetItem>
#include <QPushButton>
#include <QSettings>

#include "pj_widgets/DualOptionsWidget.h"
#include "ui_BatchLoadDialog.h"

namespace PJ {
namespace {

// Persisted name: the mode the last accepted batch used.
constexpr auto kModeKey = "BatchLoad/dialog_mode";

}  // namespace

BatchLoadDialog::BatchLoadDialog(int file_count, const QStringList& layout_files, QWidget* parent)
    : Dialog(parent), ui_(new Ui::BatchLoadDialog) {
  setDialogTitle(tr("Load Data"));
  auto* body = new QWidget;
  ui_->setupUi(body);
  contentLayout()->addWidget(body);

  ui_->labelPrompt->setText(tr("Loading %1 files.").arg(file_count));
  // Segment order is BatchDialogMode's order.
  ui_->dialogMode->setOptions({tr("Show all"), tr("Show first"), tr("Skip")});
  const auto show_hint = [this](int index) {
    static const char* const kHints[] = {
        QT_TR_NOOP("Every file shows its loader's dialog."),
        QT_TR_NOOP("The first file of each format shows its dialog; the others reuse its settings."),
        QT_TR_NOOP("Every file uses its format's last settings; a format never configured still asks."),
    };
    ui_->labelDialogModeHint->setText(tr(kHints[index]));
  };
  connect(ui_->dialogMode, &DualOptionsWidget::selectionChanged, this, show_hint);
  const int saved = QSettings().value(kModeKey, static_cast<int>(BatchDialogMode::kShowFirst)).toInt();
  ui_->dialogMode->setSelectedIndex(saved >= 0 && saved <= 2 ? saved : static_cast<int>(BatchDialogMode::kShowFirst));
  show_hint(ui_->dialogMode->selectedIndex());

  const bool with_layout = !layout_files.isEmpty();
  ui_->labelCompare->setVisible(with_layout);
  ui_->listFiles->setVisible(with_layout);
  for (const QString& name : layout_files) {
    auto* item = new QListWidgetItem(name, ui_->listFiles);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Checked);
  }
  QPushButton* ok = ui_->buttonBox->button(QDialogButtonBox::Ok);
  connect(ui_->listFiles, &QListWidget::itemChanged, this, [this, ok, with_layout]() {
    ok->setEnabled(!with_layout || !answer().compared.isEmpty());
  });
  connect(ui_->buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(ui_->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
  ok->setFocus();

  // The frameless Dialog chrome otherwise opens at its base minimum and clips the content.
  layout()->activate();
  resize(sizeHint());
}

BatchLoadDialog::~BatchLoadDialog() {
  delete ui_;
}

BatchLoadDialog::Answer BatchLoadDialog::answer() const {
  Answer result;
  result.mode = static_cast<BatchDialogMode>(ui_->dialogMode->selectedIndex());
  for (int row = 0; row < ui_->listFiles->count(); ++row) {
    if (ui_->listFiles->item(row)->checkState() == Qt::Checked) {
      result.compared.push_back(row);
    }
  }
  return result;
}

void BatchLoadDialog::ask(
    QWidget* parent, int file_count, const QStringList& layout_files,
    std::function<void(std::optional<Answer>)> on_done) {
  auto* dialog = new BatchLoadDialog(file_count, layout_files, parent);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  connect(dialog, &QDialog::finished, dialog, [dialog, on_done = std::move(on_done)](int result) {
    if (result != QDialog::Accepted) {
      on_done(std::nullopt);
      return;
    }
    const Answer answer = dialog->answer();
    QSettings().setValue(kModeKey, static_cast<int>(answer.mode));
    on_done(answer);
  });
#ifdef PJ_TARGET_WASM
  dialog->open();
#else
  dialog->exec();
#endif
}

}  // namespace PJ
