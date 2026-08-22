#include "dataload_ulog.h"
#include <QTextStream>
#include <QFile>
#include <QMessageBox>
#include <QDebug>
#include <QWidget>
#include <QSettings>
#include <QMainWindow>
#include <QProgressDialog>
#include <QCoreApplication>

#include <memory>

#include "ulog_parser.h"
#include "ulog_parameters_dialog.h"

DataLoadULog::DataLoadULog() : _main_win(nullptr)
{
  for (QWidget* widget : qApp->topLevelWidgets())
  {
    if (widget->inherits("QMainWindow"))
    {
      _main_win = widget;
      break;
    }
  }
}

const std::vector<const char*>& DataLoadULog::compatibleFileExtensions() const
{
  static std::vector<const char*> extensions = { "ulg" };
  return extensions;
}

bool DataLoadULog::readDataFromFile(FileLoadInfo* fileload_info, PlotDataMapRef& plot_data)
{
  const auto& filename = fileload_info->filename;

  QFile file(filename);

  if (!file.open(QIODevice::ReadOnly))
  {
    throw std::runtime_error("ULog: Failed to open file");
  }
  const qint64 file_size = file.size();
  uchar* mapped = file.map(0, file_size);
  if (!mapped)
  {
    throw std::runtime_error(std::string("ULog: failed to memory-map file: ") +
                             file.errorString().toStdString());
  }
  ULogParser::DataStream datastream(reinterpret_cast<char*>(mapped),
                                    static_cast<size_t>(file_size));

  // Modal progress dialog shown while the (potentially large) file is parsed.
  QProgressDialog progress_dialog(tr("Loading ULog file..."), tr("Cancel"), 0, 100,
                                  _main_win);
  progress_dialog.setWindowTitle(tr("Importing ULog"));
  progress_dialog.setWindowModality(Qt::WindowModal);
  progress_dialog.setMinimumDuration(500);  // don't flash the dialog for small files
  progress_dialog.setAutoReset(false);
  progress_dialog.setValue(0);

  auto progress_cb = [&progress_dialog](size_t offset, size_t total) -> bool {
    progress_dialog.setValue(static_cast<int>(offset * 100 / total));
    QCoreApplication::processEvents();
    return !progress_dialog.wasCanceled();
  };

  std::unique_ptr<ULogParser> parser;
  try
  {
    // Direct-output mode: parsed points are pushed straight into plot_data,
    // which is roughly twice as fast as the intermediate-buffer path.
    parser.reset(new ULogParser(datastream, plot_data, progress_cb));
  }
  catch (const std::runtime_error&)
  {
    progress_dialog.close();
    if (progress_dialog.wasCanceled())
    {
      return false;  // user cancelled: abort the import silently
    }
    throw;
  }
  progress_dialog.close();

  const double min_msg_time = parser->getMinMessageTime();

  // store parameters as a timeseries with a single point
  for (const auto& param : parser->getParameters())
  {
    auto series = plot_data.addNumeric("_parameters/" + param.name);
    double value = (param.val_type == ULogParser::FLOAT) ? double(param.value.val_real) :
                                                           double(param.value.val_int);
    series->second.pushBack({ min_msg_time, value });
  }

  ULogParametersDialog* dialog = new ULogParametersDialog(*parser, _main_win);
  dialog->setWindowTitle(QString("ULog file %1").arg(filename));
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->restoreSettings();
  dialog->show();

  return true;
}

DataLoadULog::~DataLoadULog()
{
}

bool DataLoadULog::xmlSaveState(QDomDocument& doc, QDomElement& parent_element) const
{
  return true;
}

bool DataLoadULog::xmlLoadState(const QDomElement&)
{
  return true;
}
