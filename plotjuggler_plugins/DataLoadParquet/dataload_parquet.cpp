#include "dataload_parquet.h"
#include <arrow/type_fwd.h>
#include <arrow/util/config.h>
#include <QTextStream>
#include <QFile>
#include <QMessageBox>
#include <QDebug>
#include <QSettings>
#include <QProgressDialog>
#include <QDateTime>
#include <QInputDialog>
#include <QListWidget>
#include <QSet>
#include <QTimeZone>
#include <cmath>
#include <algorithm>

namespace
{
QStringList prioritizedColumns(const std::vector<QString>& column_names, const QStringList& history)
{
  QStringList ordered;
  QSet<QString> added;

  for (const auto& name : history)
  {
    if (std::find(column_names.begin(), column_names.end(), name) != column_names.end() &&
        !added.contains(name))
    {
      ordered.push_back(name);
      added.insert(name);
    }
  }

  for (const auto& name : column_names)
  {
    if (!added.contains(name))
    {
      ordered.push_back(name);
      added.insert(name);
    }
  }

  return ordered;
}

QStringList updateColumnHistory(QStringList history, const QString& selected)
{
  if (selected.isEmpty())
  {
    return history;
  }

  history.removeAll(selected);
  history.push_front(selected);

  constexpr int kMaxHistorySize = 50;
  while (history.size() > kMaxHistorySize)
  {
    history.removeLast();
  }

  return history;
}
}  // namespace

DataLoadParquet::DataLoadParquet()
{
  ui = new Ui::DialogParquet();
  _dialog = new QDialog();

  ui->setupUi(_dialog);

  connect(ui->checkBoxDateFormat, &QCheckBox::toggled, this,
          [this](bool checked) { ui->lineEditDateFormat->setEnabled(checked); });

  connect(ui->listWidgetSeries, &QListWidget::currentTextChanged, this,
          [this](QString text) { ui->buttonBox->setEnabled(!text.isEmpty()); });

  connect(ui->listWidgetSeries, &QListWidget::doubleClicked, this,
          [this](const QModelIndex&) { _dialog->accept(); });

  connect(ui->radioButtonIndex, &QRadioButton::toggled, this, [this](bool checked) {
    ui->buttonBox->setEnabled(checked);
    ui->listWidgetSeries->setEnabled(!checked);
  });

  QSettings settings;

  bool radio_index_checked = settings.value("DataLoadParquet::radioIndexChecked", false).toBool();
  ui->radioButtonIndex->setChecked(radio_index_checked);

  bool parse_date_time = settings.value("DataLoadParquet::parseDateTime", false).toBool();
  ui->checkBoxDateFormat->setChecked(parse_date_time);

  QString date_format = settings.value("DataLoadParquet::dateFromat", "").toString();
  if (date_format.isEmpty() == false)
  {
    ui->lineEditDateFormat->setText(date_format);
  }
}

DataLoadParquet::~DataLoadParquet()
{
  delete ui;
}

const std::vector<const char*>& DataLoadParquet::compatibleFileExtensions() const
{
  static std::vector<const char*> extensions = { "parquet" };
  return extensions;
}

template <typename ArrowType>
double get_arrow_value(const std::shared_ptr<arrow::Array>& array, int64_t index)
{
  auto typed_array = std::static_pointer_cast<ArrowType>(array);
  if (typed_array->IsNull(index))
  {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return static_cast<double>(typed_array->Value(index));
}

double get_arrow_value(const std::shared_ptr<arrow::Array>& array, int64_t index,
                       arrow::Type::type arrow_type)
{
  switch (arrow_type)
  {
    case arrow::Type::BOOL:
      return get_arrow_value<arrow::BooleanArray>(array, index);
    case arrow::Type::INT8:
      return get_arrow_value<arrow::Int8Array>(array, index);
    case arrow::Type::INT16:
      return get_arrow_value<arrow::Int16Array>(array, index);
    case arrow::Type::INT32:
      return get_arrow_value<arrow::Int32Array>(array, index);
    case arrow::Type::INT64:
      return get_arrow_value<arrow::Int64Array>(array, index);
    case arrow::Type::UINT8:
      return get_arrow_value<arrow::UInt8Array>(array, index);
    case arrow::Type::UINT16:
      return get_arrow_value<arrow::UInt16Array>(array, index);
    case arrow::Type::UINT32:
      return get_arrow_value<arrow::UInt32Array>(array, index);
    case arrow::Type::UINT64:
      return get_arrow_value<arrow::UInt64Array>(array, index);
    case arrow::Type::FLOAT:
      return get_arrow_value<arrow::FloatArray>(array, index);
    case arrow::Type::DOUBLE:
      return get_arrow_value<arrow::DoubleArray>(array, index);
    case arrow::Type::TIMESTAMP: {
      auto timestamp_array = std::static_pointer_cast<arrow::TimestampArray>(array);
      if (timestamp_array->IsNull(index))
      {
        return std::numeric_limits<double>::quiet_NaN();
      }
      const auto timestamp_type =
          std::static_pointer_cast<arrow::TimestampType>(timestamp_array->type());
      const int64_t value = timestamp_array->Value(index);
      double seconds = 0;
      switch (timestamp_type->unit())
      {
        case arrow::TimeUnit::SECOND:
          seconds = static_cast<double>(value);
          break;
        case arrow::TimeUnit::MILLI:
          seconds = static_cast<double>(value) / 1000.0;
          break;
        case arrow::TimeUnit::MICRO:
          seconds = static_cast<double>(value) / 1000000.0;
          break;
        case arrow::TimeUnit::NANO:
          seconds = static_cast<double>(value) / 1000000000.0;
          break;
      }
      const std::string& timezone_str = timestamp_type->timezone();
      if (!timezone_str.empty() && timezone_str != "UTC")
      {
        QTimeZone tz(QByteArray::fromStdString(timezone_str));
        if (tz.isValid())
        {
          QDateTime utc_dt = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(seconds));
          seconds -= tz.offsetFromUtc(utc_dt);
        }
      }
      return seconds;
    }
    default:
      break;
  }
  return std::numeric_limits<double>::quiet_NaN();
}

bool DataLoadParquet::readDataFromFile(FileLoadInfo* info, PlotDataMapRef& plot_data)
{
  ui->listWidgetSeries->clear();

  // Open the file using Arrow IO
  std::shared_ptr<arrow::io::ReadableFile> infile;
  auto result = arrow::io::ReadableFile::Open(info->filename.toStdString());
  if (!result.ok())
  {
    return false;
  }

  ui->listWidgetSeries->clear();

  infile = result.ValueOrDie();

  // Create Arrow FileReader
  auto arrow_reader_result = parquet::arrow::OpenFile(infile, arrow::default_memory_pool());
  if (!arrow_reader_result.ok())
  {
    throw std::runtime_error("Failed to open Parquet file");
  }
  std::unique_ptr<parquet::arrow::FileReader> arrow_file_reader =
      std::move(arrow_reader_result.ValueOrDie());

  // Get metadata
  std::shared_ptr<parquet::FileMetaData> file_metadata =
      arrow_file_reader->parquet_reader()->metadata();
  const auto schema = file_metadata->schema();
  const int64_t total_rows = file_metadata->num_rows();

  struct ColumnInfo
  {
    std::string name;
    arrow::Type::type arrow_type;
    PlotData* numeric_data = nullptr;
    StringSeries* string_data = nullptr;
    size_t column_index = 0;
  };

  // Get Arrow schema
  std::shared_ptr<arrow::Schema> arrow_schema;
  auto status = arrow_file_reader->GetSchema(&arrow_schema);
  if (!status.ok())
  {
    throw std::runtime_error("Failed to get Arrow schema");
  }

  std::vector<ColumnInfo> columns_info;
  std::vector<QString> selectable_columns;

  for (size_t col = 0; col < file_metadata->num_columns(); col++)
  {
    const auto field = arrow_schema->field(col);
    ColumnInfo info;
    info.name = field->name();
    info.arrow_type = field->type()->id();
    info.column_index = col;

    // Numeric columns can be plotted directly and are offered as timestamp candidates.
    const bool is_numeric =
        (info.arrow_type == arrow::Type::BOOL || info.arrow_type == arrow::Type::INT8 ||
         info.arrow_type == arrow::Type::INT16 || info.arrow_type == arrow::Type::INT32 ||
         info.arrow_type == arrow::Type::INT64 || info.arrow_type == arrow::Type::UINT8 ||
         info.arrow_type == arrow::Type::UINT16 || info.arrow_type == arrow::Type::UINT32 ||
         info.arrow_type == arrow::Type::UINT64 || info.arrow_type == arrow::Type::FLOAT ||
         info.arrow_type == arrow::Type::TIMESTAMP || info.arrow_type == arrow::Type::DOUBLE);

    // Parquet BYTE_ARRAY columns (string/binary) land in Arrow as one of these.
    const bool is_string =
        (info.arrow_type == arrow::Type::STRING || info.arrow_type == arrow::Type::LARGE_STRING ||
         info.arrow_type == arrow::Type::BINARY);

    if (is_numeric)
    {
      info.numeric_data = &plot_data.getOrCreateNumeric(info.name, nullptr);
      columns_info.push_back(info);
      selectable_columns.push_back(QString::fromStdString(info.name));
    }
    else if (is_string)
    {
      info.string_data = &plot_data.getOrCreateStringSeries(info.name, nullptr);
      columns_info.push_back(info);
      // String columns are loaded but not offered as a timestamp axis.
    }
  }

  {
    QSettings settings;
    const auto ordered_columns = prioritizedColumns(
        selectable_columns, settings.value("DataLoadParquet::timeHistory").toStringList());
    for (const auto& name : ordered_columns)
    {
      ui->listWidgetSeries->addItem(name);
    }
  }

  {
    QSettings settings;
    if (_default_time_axis.isEmpty())
    {
      _default_time_axis = settings.value("DataLoadParquet::prevTimestamp", {}).toString();
    }

    if (_default_time_axis.isEmpty() == false)
    {
      auto items = ui->listWidgetSeries->findItems(_default_time_axis, Qt::MatchExactly);
      if (items.size() > 0)
      {
        ui->listWidgetSeries->setCurrentItem(items.front());
      }
    }
  }

  int ret = _dialog->exec();
  if (ret != QDialog::Accepted)
  {
    return false;
  }

  QString selected_stamp;

  if (ui->radioButtonSelect->isChecked())
  {
    auto selected = ui->listWidgetSeries->selectedItems();
    if (selected.size() == 1)
    {
      selected_stamp = selected.front()->text();
    }
  }

  QSettings settings;
  settings.setValue("DataLoadParquet::prevTimestamp", selected_stamp);
  settings.setValue(
      "DataLoadParquet::timeHistory",
      updateColumnHistory(settings.value("DataLoadParquet::timeHistory").toStringList(),
                          selected_stamp));
  settings.setValue("DataLoadParquet::radioIndexChecked", ui->radioButtonIndex->isChecked());
  settings.setValue("DataLoadParquet::parseDateTime", ui->checkBoxDateFormat->isChecked());
  settings.setValue("DataLoadParquet::dateFromat", ui->lineEditDateFormat->text());

  //-----------------------------
  // Time to parse
  int timestamp_column = -1;
  auto timestamp_arrow_type = arrow::Type::NA;

  for (const auto& info : columns_info)
  {
    if (info.name == selected_stamp.toStdString())
    {
      timestamp_column = info.column_index;
      timestamp_arrow_type = info.arrow_type;
      break;
    }
  }

  QProgressDialog progress_dialog;
  progress_dialog.setWindowTitle("Loading the Parquet file");
  progress_dialog.setLabelText("Loading... please wait");
  progress_dialog.setWindowModality(Qt::ApplicationModal);
  progress_dialog.setRange(0, columns_info.size());
  progress_dialog.setAutoClose(true);
  progress_dialog.setAutoReset(true);
  progress_dialog.show();

  // Create RecordBatchReader for efficient batch processing
  std::shared_ptr<arrow::RecordBatchReader> batch_reader;
#if ARROW_VERSION_MAJOR >= 19
  // The out-parameter overloads were deprecated in Arrow 21 and later removed.
  auto batch_reader_result = arrow_file_reader->GetRecordBatchReader();
  status = batch_reader_result.status();
  if (status.ok())
  {
    batch_reader = std::move(*batch_reader_result);
  }
#else
  status = arrow_file_reader->GetRecordBatchReader(&batch_reader);
#endif
  if (!status.ok())
  {
    throw std::runtime_error("Failed to create RecordBatchReader");
  }

  int64_t rows_processed = 0;

  // Process data in batches
  std::shared_ptr<arrow::RecordBatch> batch;
  while (batch_reader->ReadNext(&batch).ok() && batch)
  {
    const int64_t batch_rows = batch->num_rows();

    std::vector<std::pair<double, size_t>> timestamp_to_row_index(batch_rows);

    if (timestamp_column >= 0)
    {
      auto timestamp_array = batch->column(timestamp_column);
      for (int64_t row = 0; row < batch_rows; row++)
      {
        const auto ts = get_arrow_value(timestamp_array, row, timestamp_arrow_type);
        timestamp_to_row_index[row] = { ts, row };
      }
    }
    // order the timestamps for correct insertion
    std::sort(timestamp_to_row_index.begin(), timestamp_to_row_index.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    int column = 0;

    for (const auto& info : columns_info)
    {
      const auto values_array = batch->column(info.column_index);

      for (int64_t row = 0; row < batch_rows; row++)
      {
        size_t ordered_row = row;
        double timestamp = static_cast<double>(rows_processed + row);
        if (timestamp_column >= 0)
        {
          timestamp = timestamp_to_row_index[row].first;
          ordered_row = timestamp_to_row_index[row].second;
        }

        if (info.numeric_data)
        {
          double value = get_arrow_value(values_array, ordered_row, info.arrow_type);
          if (!std::isnan(value))
          {
            info.numeric_data->pushBack({ timestamp, value });
          }
        }
        else if (info.string_data)
        {
          if (values_array->IsNull(ordered_row))
          {
            continue;
          }
          std::string_view view;
          switch (info.arrow_type)
          {
            case arrow::Type::STRING:
              view =
                  std::static_pointer_cast<arrow::StringArray>(values_array)->GetView(ordered_row);
              break;
            case arrow::Type::LARGE_STRING:
              view = std::static_pointer_cast<arrow::LargeStringArray>(values_array)
                         ->GetView(ordered_row);
              break;
            case arrow::Type::BINARY:
              view =
                  std::static_pointer_cast<arrow::BinaryArray>(values_array)->GetView(ordered_row);
              break;
            default:
              break;
          }
          if (!view.empty())
          {
            info.string_data->pushBack({ timestamp, StringRef(view) });
          }
        }
      }

      if (column++ % 10 == 0)
      {
        progress_dialog.setValue(column);
        QApplication::processEvents();
        if (progress_dialog.wasCanceled())
        {
          break;
        }
      }
    }
    rows_processed += batch_rows;
  }

  return true;
}

bool DataLoadParquet::xmlSaveState(QDomDocument& doc, QDomElement& parent_element) const
{
  QDomElement elem = doc.createElement("default");
  elem.setAttribute("prevTimestamp", _default_time_axis);
  elem.setAttribute("radioIndexChecked", ui->radioButtonIndex->isChecked());
  elem.setAttribute("parseDateTime", ui->checkBoxDateFormat->isChecked());
  elem.setAttribute("dateFromat", ui->lineEditDateFormat->text());

  parent_element.appendChild(elem);
  return true;
}

bool DataLoadParquet::xmlLoadState(const QDomElement& parent_element)
{
  QDomElement elem = parent_element.firstChildElement("default");
  if (!elem.isNull())
  {
    if (elem.hasAttribute("prevTimestamp"))
    {
      _default_time_axis = elem.attribute("prevTimestamp");
    }
    if (elem.hasAttribute("radioIndexChecked"))
    {
      bool checked = elem.attribute("radioIndexChecked").toInt();
      ui->radioButtonIndex->setChecked(checked);
    }
    if (elem.hasAttribute("parseDateTime"))
    {
      bool checked = elem.attribute("parseDateTime").toInt();
      ui->checkBoxDateFormat->setChecked(checked);
    }
    if (elem.hasAttribute("dateFromat"))
    {
      ui->lineEditDateFormat->setText(elem.attribute("dateFromat"));
    }
  }

  return true;
}
