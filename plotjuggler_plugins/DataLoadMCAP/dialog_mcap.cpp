#include "dialog_mcap.h"
#include "ui_dialog_mcap.h"

#include <QSettings>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QElapsedTimer>

#define MCAP_IMPLEMENTATION
#include <mcap/reader.hpp>

const QString DialogMCAP::prefix = "DialogLoadMCAP::";

DialogMCAP::DialogMCAP(const std::unordered_map<int, mcap::ChannelPtr>& channels,
                       const std::unordered_map<int, mcap::SchemaPtr>& schemas,
                       const std::unordered_map<uint16_t, uint64_t>& messages_count_by_channelID,
                       std::optional<mcap::LoadParams> default_parameters, QWidget* parent)
  : QDialog(parent)
  , ui(new Ui::dialog_mcap)
  , _select_all(QKeySequence(Qt::CTRL + Qt::Key_A), this)
  , _deselect_all(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_A), this)
{
  ui->setupUi(this);

  if (messages_count_by_channelID.empty())
  {
    ui->tableWidget->horizontalHeader()->hideSection(3);
  }

  _select_all.setContext(Qt::WindowShortcut);
  _deselect_all.setContext(Qt::WindowShortcut);

  connect(&_select_all, &QShortcut::activated, ui->tableWidget, [this]() {
    for (int row = 0; row < ui->tableWidget->rowCount(); row++)
    {
      if (!ui->tableWidget->isRowHidden(row) && !ui->tableWidget->item(row, 0)->isSelected())
      {
        ui->tableWidget->selectRow(row);
      }
    }
  });

  connect(&_deselect_all, &QShortcut::activated, ui->tableWidget,
          &QAbstractItemView::clearSelection);

  ui->tableWidget->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  ui->tableWidget->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  ui->tableWidget->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  ui->tableWidget->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

  ui->tableWidget->setRowCount(channels.size());

  QSettings settings;
  restoreGeometry(settings.value(prefix + "geometry").toByteArray());

  mcap::LoadParams params;
  if (!default_parameters)
  {
    params.selected_topics = settings.value(prefix + "selected").toStringList();
    params.clamp_large_arrays = settings.value(prefix + "clamp", true).toBool();
    params.max_array_size = settings.value(prefix + "max_array", 500).toInt();
    params.use_timestamp = settings.value(prefix + "use_timestamp", false).toBool();
    params.use_mcap_log_time = settings.value(prefix + "use_mcap_log_time", false).toBool();
    params.sorted_column = settings.value(prefix + "sorted_column", 0).toInt();
  }
  else
  {
    params = *default_parameters;
  }

  if (params.clamp_large_arrays)
  {
    ui->radioClamp->setChecked(true);
  }
  else
  {
    ui->radioSkip->setChecked(true);
  }
  ui->spinBox->setValue(params.max_array_size);
  ui->spinBox->setFocusPolicy(Qt::ClickFocus);
  ui->checkBoxUseTimestamp->setChecked(params.use_timestamp);
  if (params.use_mcap_log_time)
  {
    ui->radioLogTime->setChecked(true);
  }
  else
  {
    ui->radioPubTime->setChecked(true);
  }

  int row = 0;
  ui->tableWidget->setFocusPolicy(Qt::NoFocus);
  const int columns_count = ui->tableWidget->columnCount();

  for (const auto& [id, channel] : channels)
  {
    auto topic = QString::fromStdString(channel->topic);
    auto const& schema = schemas.at(channel->schemaId);

    ui->tableWidget->setItem(row, 0, new QTableWidgetItem(topic));
    ui->tableWidget->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(schema->name)));
    ui->tableWidget->setItem(row, 2,
                             new QTableWidgetItem(QString::fromStdString(schema->encoding)));

    auto count_it = messages_count_by_channelID.find(id);
    int message_count = (count_it != messages_count_by_channelID.end()) ? count_it->second : 0;
    ui->tableWidget->setItem(row, 3, new QTableWidgetItem(QString::number(message_count)));

    for (int col = 0; col < columns_count; ++col)
    {
      QTableWidgetItem* it = ui->tableWidget->item(row, col);
      if (message_count == 0)
      {
        it->setFlags(it->flags() & ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable));
        it->setForeground(QBrush(Qt::gray));
      }
    }
    if (message_count != 0 && params.selected_topics.contains(topic))
    {
      ui->tableWidget->selectRow(row);
    }
    row++;
  }
  auto sort_order = (params.sorted_column >= columns_count) ? Qt::SortOrder::DescendingOrder :
                                                              Qt::SortOrder::AscendingOrder;
  auto sort_count = params.sorted_column % columns_count;
  ui->tableWidget->sortByColumn(sort_count, sort_order);

  // Load legacy CDR alignment setting
  bool use_legacy_cdr = false;
  if (default_parameters)
  {
    use_legacy_cdr = default_parameters->use_legacy_cdr_alignment;
  }
  else
  {
    use_legacy_cdr = settings.value("use_legacy_cdr", false).toBool();
  }
  ui->checkBoxUseLegacyCdr->setChecked(use_legacy_cdr);

  // Connect topic filter QLineEdit to filtering logic
  connect(ui->lineEditFilter, &QLineEdit::textChanged, this,
          &DialogMCAP::on_lineEditFilter_textChanged);
}

DialogMCAP::~DialogMCAP()
{
  delete ui;
}

mcap::LoadParams DialogMCAP::getParams() const
{
  mcap::LoadParams params;
  params.max_array_size = ui->spinBox->value();
  params.clamp_large_arrays = ui->radioClamp->isChecked();
  params.use_timestamp = ui->checkBoxUseTimestamp->isChecked();
  params.use_mcap_log_time = ui->radioLogTime->isChecked();
  params.use_legacy_cdr_alignment = ui->checkBoxUseLegacyCdr->isChecked();

  QItemSelectionModel* select = ui->tableWidget->selectionModel();
  QStringList selected_topics;
  for (QModelIndex index : select->selectedRows())
  {
    params.selected_topics.push_back(ui->tableWidget->item(index.row(), 0)->text());
  }
  return params;
}

void DialogMCAP::on_tableWidget_itemSelectionChanged()
{
  bool enabled = !ui->tableWidget->selectionModel()->selectedRows().empty();
  ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(enabled);
}

void DialogMCAP::accept()
{
  QSettings settings;
  settings.setValue(prefix + "geometry", saveGeometry());

  bool clamp_checked = ui->radioClamp->isChecked();
  int max_array = ui->spinBox->value();
  bool use_timestamp = ui->checkBoxUseTimestamp->isChecked();
  bool use_mcap_log_time = ui->radioLogTime->isChecked();
  int sort_column = ui->tableWidget->horizontalHeader()->sortIndicatorSection();
  Qt::SortOrder sortOrder = ui->tableWidget->horizontalHeader()->sortIndicatorOrder();
  // apply an offsert to descending order
  sort_column += (sortOrder == Qt::DescendingOrder) ? ui->tableWidget->columnCount() : 0;

  settings.setValue(prefix + "clamp", clamp_checked);
  settings.setValue(prefix + "max_array", max_array);
  settings.setValue(prefix + "use_timestamp", use_timestamp);
  settings.setValue(prefix + "use_mcap_log_time", use_mcap_log_time);
  settings.setValue(prefix + "use_legacy_cdr", ui->checkBoxUseLegacyCdr->isChecked());
  settings.setValue(prefix + "sorted_column", sort_column);

  QItemSelectionModel* select = ui->tableWidget->selectionModel();
  QStringList selected_topics;
  for (QModelIndex index : select->selectedRows())
  {
    selected_topics.push_back(ui->tableWidget->item(index.row(), 0)->text());
  }
  settings.setValue(prefix + "selected", selected_topics);

  QDialog::accept();
}

void DialogMCAP::on_lineEditFilter_textChanged(const QString& search_string)
{
  QStringList spaced_items = search_string.split(' ');
  for (int row = 0; row < ui->tableWidget->rowCount(); row++)
  {
    auto item = ui->tableWidget->item(row, 0);
    QString name = item ? item->text() : "";
    bool toHide = false;
    for (const auto& filter : spaced_items)
    {
      if (!name.contains(filter))
      {
        toHide = true;
        break;
      }
    }
    ui->tableWidget->setRowHidden(row, toHide);
  }
}
