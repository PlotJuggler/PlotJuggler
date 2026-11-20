// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "ui/BatchLoadDialog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
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

BatchLoadDialog::BatchLoadDialog(const QStringList& paths, bool ask_mode, bool list_files, QWidget* parent)
    : Dialog(parent), ui_(new Ui::BatchLoadDialog) {
  setDialogTitle(tr("Load Data"));
  auto* body = new QWidget;
  ui_->setupUi(body);
  contentLayout()->addWidget(body);

  ui_->labelPrompt->setText(tr("Loading %1 files.").arg(paths.size()));
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
  const bool known = saved >= 0 && saved <= static_cast<int>(BatchDialogMode::kSkip);
  ui_->dialogMode->setSelectedIndex(known ? saved : static_cast<int>(BatchDialogMode::kShowFirst));
  show_hint(ui_->dialogMode->selectedIndex());
  ui_->labelDialogMode->setVisible(ask_mode);
  ui_->dialogMode->setVisible(ask_mode);
  ui_->labelDialogModeHint->setVisible(ask_mode);
  if (!ask_mode) {
    ui_->dialogMode->setSelectedIndex(static_cast<int>(BatchDialogMode::kShowAll));
  }

  ui_->labelCompare->setVisible(list_files);
  ui_->listFiles->setVisible(list_files);
  if (list_files) {
    QStringList names;
    for (const QString& path : paths) {
      names.push_back(QFileInfo(path).fileName());
    }
    for (qsizetype index = 0; index < paths.size(); ++index) {
      // Same-named files (one run per folder) are told apart by their folder.
      const QString label =
          names.count(names[index]) > 1 ? QFileInfo(paths[index]).dir().dirName() + u'/' + names[index] : names[index];
      auto* item = new QListWidgetItem(label, ui_->listFiles);
      item->setData(Qt::UserRole, paths[index]);
      item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
      item->setCheckState(Qt::Checked);
    }
  }
  QPushButton* ok = ui_->buttonBox->button(QDialogButtonBox::Ok);
  connect(ui_->listFiles, &QListWidget::itemChanged, this, [this, ok]() { ok->setEnabled(!checkedPaths().isEmpty()); });
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

BatchDialogMode BatchLoadDialog::mode() const {
  return static_cast<BatchDialogMode>(ui_->dialogMode->selectedIndex());
}

QStringList BatchLoadDialog::checkedPaths() const {
  QStringList paths;
  for (int row = 0; row < ui_->listFiles->count(); ++row) {
    if (ui_->listFiles->item(row)->checkState() == Qt::Checked) {
      paths.push_back(ui_->listFiles->item(row)->data(Qt::UserRole).toString());
    }
  }
  return paths;
}

void BatchLoadDialog::ask(
    QWidget* parent, const QStringList& paths, bool ask_mode, bool list_files,
    std::function<void(std::optional<Answer>)> on_done) {
  auto* dialog = new BatchLoadDialog(paths, ask_mode, list_files, parent);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  connect(dialog, &QDialog::finished, dialog, [dialog, ask_mode, on_done = std::move(on_done)](int result) {
    if (result != QDialog::Accepted) {
      on_done(std::nullopt);
      return;
    }
    const BatchDialogMode mode = dialog->mode();
    if (ask_mode) {
      QSettings().setValue(kModeKey, static_cast<int>(mode));
    }
    on_done(
        Answer{
            .batch = std::make_shared<LoadBatch>(LoadBatch{.mode = mode, .prompted = {}, .cancelled = {}}),
            .compared = dialog->checkedPaths(),
        });
  });
#ifdef PJ_TARGET_WASM
  dialog->open();
#else
  dialog->exec();
#endif
}

}  // namespace PJ
