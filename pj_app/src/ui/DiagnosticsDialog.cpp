// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "ui/DiagnosticsDialog.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QHeaderView>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QShowEvent>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <algorithm>

#include "pj_widgets/CheckButton.h"
#include "pj_widgets/ComboBox.h"
#include "pj_widgets/HeaderResizePolicy.h"
#include "pj_widgets/Search.h"
#include "pj_widgets/SvgUtil.h"
#include "ui_DiagnosticsDialog.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

constexpr int kColTime = 0;
constexpr int kColSeverity = 1;
constexpr int kColSource = 2;
constexpr int kColMessage = 3;

// The whole DiagnosticRecord, stored on the Time item: the table is the model.
constexpr int kRecordRole = Qt::UserRole;
// Machine sort key (QDateTime for Time, int level for Severity) — the display
// text loses ordering within a second and "Error" < "Info" alphabetically.
constexpr int kSortKeyRole = Qt::UserRole + 1;

class KeyedItem : public QTableWidgetItem {
 public:
  using QTableWidgetItem::QTableWidgetItem;
  [[nodiscard]] bool operator<(const QTableWidgetItem& other) const override {
    return QVariant::compare(data(kSortKeyRole), other.data(kSortKeyRole)) == QPartialOrdering::Less;
  }
};

struct LevelInfo {
  QString label;
  QString icon_path;
};

LevelInfo levelInfo(DiagnosticLevel level) {
  switch (level) {
    case DiagnosticLevel::kError:
      return {DiagnosticsDialog::tr("Error"), u":/resources/svg/diag_error.svg"_s};
    case DiagnosticLevel::kWarning:
      return {DiagnosticsDialog::tr("Warning"), u":/resources/svg/diag_warning.svg"_s};
    case DiagnosticLevel::kInfo:
    default:
      return {DiagnosticsDialog::tr("Info"), u":/resources/svg/diag_info.svg"_s};
  }
}

DiagnosticRecord recordAt(const QTableWidget* table, int row) {
  return table->item(row, kColTime)->data(kRecordRole).value<DiagnosticRecord>();
}

}  // namespace

DiagnosticsDialog::DiagnosticsDialog(DiagnosticHistory* history, QWidget* parent)
    : Dialog(parent), ui_(new Ui::DiagnosticsDialog), history_(history) {
  setDialogTitle(tr("Diagnostics"));
  resize(960, 560);

  auto* body = new QWidget;
  ui_->setupUi(body);
  contentLayout()->addWidget(body);

  for (CheckButton* toggle : {ui_->toggleInfo, ui_->toggleWarning, ui_->toggleError}) {
    toggle->setChecked(true);
    connect(toggle, &CheckButton::toggled, this, &DiagnosticsDialog::applyFilters);
  }
  ui_->searchBox->setVariant(Search::Variant::kStandalone);
  connect(ui_->searchBox, &Search::textChanged, this, &DiagnosticsDialog::applyFilters);
  connect(ui_->comboSource, &QComboBox::currentIndexChanged, this, &DiagnosticsDialog::applyFilters);
  connect(ui_->buttonClear, &QPushButton::clicked, history_, &DiagnosticHistory::clear);

  ui_->buttonCopy->setIcon(loadSvg(":/resources/svg/copy.svg", currentTheme()));
  connect(ui_->buttonCopy, &QPushButton::clicked, this, [this]() {
    if (const auto record = selectedRecord()) {
      QGuiApplication::clipboard()->setText(record->message);
    }
  });

  auto* table = ui_->table;
  table->sortByColumn(kColTime, Qt::DescendingOrder);
  connect(table, &QTableWidget::itemSelectionChanged, this, &DiagnosticsDialog::onSelectionChanged);
  auto* policy = HeaderResizePolicy::install(table->horizontalHeader(), kColMessage);
  policy->setSectionWidth(kColTime, 150);
  policy->setSectionWidth(kColSeverity, 90);
  policy->setSectionWidth(kColSource, 180);
  // The table takes every extra pixel; the pane keeps a few lines unless dragged.
  ui_->splitter->setStretchFactor(0, 1);
  ui_->splitter->setStretchFactor(1, 0);
  ui_->messagePane->setMinimumHeight(ui_->messagePane->fontMetrics().lineSpacing() * 3);

  connect(history_, &DiagnosticHistory::recorded, this, &DiagnosticsDialog::rebuild);
  connect(history_, &DiagnosticHistory::cleared, this, &DiagnosticsDialog::rebuild);

  if (parent != nullptr) {
    const QPoint centre = parent->mapToGlobal(parent->rect().center());
    move(centre.x() - width() / 2, centre.y() - height() / 2);
  }
}

DiagnosticsDialog::~DiagnosticsDialog() {
  delete ui_;
}

int DiagnosticsDialog::visibleRowCount() const {
  int visible = 0;
  for (int row = 0; row < ui_->table->rowCount(); ++row) {
    visible += ui_->table->isRowHidden(row) ? 0 : 1;
  }
  return visible;
}

QString DiagnosticsDialog::paneText() const {
  return ui_->messagePane->toPlainText();
}

void DiagnosticsDialog::showEvent(QShowEvent* event) {
  Dialog::showEvent(event);
  if (stale_) {
    rebuild();
  }
  // Absolute splitter sizes only stick once the splitter has real geometry.
  if (!splitter_sized_) {
    splitter_sized_ = true;
    ui_->splitter->setSizes({height(), ui_->messagePane->minimumHeight()});
  }
}

void DiagnosticsDialog::rebuild() {
  if (!isVisible()) {
    stale_ = true;
    return;
  }
  stale_ = false;
  const std::optional<DiagnosticRecord> previous = selectedRecord();
  const QList<DiagnosticRecord> records = history_->snapshot();
  refreshSources(records);

  auto* table = ui_->table;
  table->setSortingEnabled(false);
  table->setRowCount(0);
  table->setRowCount(static_cast<int>(records.size()));

  const QString theme = currentTheme();
  QTableWidgetItem* reselect = nullptr;
  for (int row = 0; row < records.size(); ++row) {
    const DiagnosticRecord& record = records[row];
    const LevelInfo level = levelInfo(record.level);
    auto* time_item = new KeyedItem(record.timestamp.toString(u"yyyy-MM-dd HH:mm:ss"_s));
    time_item->setData(kSortKeyRole, record.timestamp);
    time_item->setData(kRecordRole, QVariant::fromValue(record));
    auto* level_item = new KeyedItem(loadSvg(level.icon_path, theme), level.label);
    level_item->setData(kSortKeyRole, static_cast<int>(record.level));
    auto* message_item = new QTableWidgetItem(record.message.section(u'\n', 0, 0));
    message_item->setToolTip(record.message);
    table->setItem(row, kColTime, time_item);
    table->setItem(row, kColSeverity, level_item);
    table->setItem(row, kColSource, new QTableWidgetItem(record.source));
    table->setItem(row, kColMessage, message_item);
    if (reselect == nullptr && previous == record) {
      reselect = time_item;
    }
  }
  // Re-enabling sorting re-sorts on the header's current indicator.
  table->setSortingEnabled(true);
  if (reselect != nullptr) {
    table->selectRow(reselect->row());
  }
  applyFilters();
}

void DiagnosticsDialog::refreshSources(const QList<DiagnosticRecord>& records) {
  QSet<QString> sources;
  for (const DiagnosticRecord& record : records) {
    if (!record.source.isEmpty()) {
      sources.insert(record.source);
    }
  }
  QStringList sorted(sources.begin(), sources.end());
  sorted.sort(Qt::CaseInsensitive);

  auto* combo = ui_->comboSource;
  const QString current = combo->currentData().toString();
  const QSignalBlocker blocker(combo);
  combo->clear();
  combo->addItem(tr("All sources"), QString());
  for (const QString& source : sorted) {
    combo->addItem(source, source);
  }
  combo->setCurrentIndex(std::max(0, combo->findData(current)));
}

void DiagnosticsDialog::applyFilters() {
  const bool show_info = ui_->toggleInfo->isChecked();
  const bool show_warning = ui_->toggleWarning->isChecked();
  const bool show_error = ui_->toggleError->isChecked();
  const QString source = ui_->comboSource->currentData().toString();
  const QString needle = ui_->searchBox->text().trimmed();

  const auto passes = [&](const DiagnosticRecord& record) {
    const bool level_on = (record.level == DiagnosticLevel::kError && show_error) ||
                          (record.level == DiagnosticLevel::kWarning && show_warning) ||
                          (record.level == DiagnosticLevel::kInfo && show_info);
    if (!level_on || (!source.isEmpty() && record.source != source)) {
      return false;
    }
    return needle.isEmpty() || record.message.contains(needle, Qt::CaseInsensitive) ||
           record.source.contains(needle, Qt::CaseInsensitive) || record.id.contains(needle, Qt::CaseInsensitive);
  };

  auto* table = ui_->table;
  for (int row = 0; row < table->rowCount(); ++row) {
    const bool hidden = !passes(recordAt(table, row));
    table->setRowHidden(row, hidden);
    if (hidden && table->item(row, kColTime)->isSelected()) {
      table->clearSelection();
    }
  }
  onSelectionChanged();
}

std::optional<DiagnosticRecord> DiagnosticsDialog::selectedRecord() const {
  const QList<QTableWidgetItem*> selected = ui_->table->selectedItems();
  if (selected.isEmpty()) {
    return std::nullopt;
  }
  return recordAt(ui_->table, selected.first()->row());
}

void DiagnosticsDialog::onSelectionChanged() {
  const std::optional<DiagnosticRecord> record = selectedRecord();
  ui_->buttonCopy->setEnabled(record.has_value());
  if (!record) {
    ui_->messagePane->clear();
    return;
  }
  QStringList header;
  header << record->timestamp.toString(u"ddd d MMM yyyy HH:mm:ss"_s) << levelInfo(record->level).label;
  if (!record->source.isEmpty()) {
    header << record->source;
  }
  if (!record->id.isEmpty()) {
    header << record->id;
  }
  ui_->messagePane->setPlainText(header.join(u" · "_s) + u"\n\n"_s + record->message);
}

}  // namespace PJ
