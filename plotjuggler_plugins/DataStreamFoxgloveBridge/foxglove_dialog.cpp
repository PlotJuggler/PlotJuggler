#include "foxglove_dialog.h"

#include <QRegularExpressionValidator>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QTimer>

#include "ui_foxglove_client.h"

namespace
{
constexpr auto url_regex = "^wss?:\\/\\/([a-zA-Z0-9][-a-zA-Z0-9._]+)"
                           "(:[0-9]{1,5})?"
                           "(\\/.*)?$";
}

FoxgloveDialog::FoxgloveDialog(const FoxgloveClientConfig& config)
  : QDialog(nullptr), ui(new Ui::FoxgloveDialog)
{
  ui->setupUi(this);
  setWindowTitle("Foxglove Bridge");

  ui->lineEditURL->setText(config.url);
  ui->lineEditURL->setValidator(
      new QRegularExpressionValidator(QRegularExpression(url_regex), this));

  ui->spinBoxArraySize->setValue(config.max_array_size);
  if (config.clamp_large_arrays)
  {
    ui->radioClamp->setChecked(true);
  }
  else
  {
    ui->radioSkip->setChecked(true);
  }
  ui->checkBoxUseTimestamp->setChecked(config.use_timestamp);

  if (auto ok_btn = ui->buttonBox->button(QDialogButtonBox::Ok))
  {
    ok_btn->setText("Subscribe");
    ok_btn->setEnabled(false);
  }

  ui->topicsList->setSortingEnabled(true);
  ui->topicsList->sortByColumn(0, Qt::AscendingOrder);

  connect(ui->lineEditFilter, &QLineEdit::textChanged, this, &FoxgloveDialog::applyFilter);

  QSettings settings;
  restoreGeometry(settings.value("FoxgloveBridgeCX/dialogGeometry").toByteArray());
}

FoxgloveDialog::~FoxgloveDialog()
{
  QSettings settings;
  settings.setValue("FoxgloveBridgeCX/dialogGeometry", saveGeometry());
  delete ui;
}

QString FoxgloveDialog::url() const
{
  return ui->lineEditURL->text().trimmed();
}

void FoxgloveDialog::setChannels(const QMap<quint64, FoxgloveChannelInfo>& channels,
                                 const QStringList& preselectNames)
{
  _channels = channels;

  auto* view = ui->topicsList;
  auto* vsb = view->verticalScrollBar();
  const int scroll_y = vsb ? vsb->value() : 0;

  QSet<QString> wanted(preselectNames.constBegin(), preselectNames.constEnd());
  for (auto* item : view->selectedItems())
  {
    const QString topic = item->text(0);
    if (!topic.isEmpty())
    {
      wanted.insert(topic);
    }
  }

  view->setUpdatesEnabled(false);
  view->blockSignals(true);
  view->setSortingEnabled(false);
  view->setVisible(false);
  view->clear();

  for (auto it = _channels.cbegin(); it != _channels.cend(); ++it)
  {
    const auto& channel = it.value();
    auto* item = new QTreeWidgetItem(view);
    item->setText(0, channel.topic);
    item->setText(1, channel.schema_name);
    item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(channel.id));
    item->setToolTip(0, QString("Channel ID: %1\nEncoding: %2\nSchema Encoding: %3")
                            .arg(channel.id)
                            .arg(channel.encoding, channel.schema_encoding));
    if (wanted.contains(channel.topic))
    {
      item->setSelected(true);
    }
  }

  applyFilter(ui->lineEditFilter->text());
  view->resizeColumnToContents(0);
  view->resizeColumnToContents(1);

  view->setSortingEnabled(true);
  view->setVisible(true);
  view->blockSignals(false);
  view->setUpdatesEnabled(true);

  QTimer::singleShot(0, view, [view, scroll_y]() {
    if (auto* sb = view->verticalScrollBar())
    {
      sb->setValue(scroll_y);
    }
  });
}

void FoxgloveDialog::clearChannels()
{
  _channels.clear();
  ui->topicsList->clear();
}

bool FoxgloveDialog::hasSelection() const
{
  for (auto* item : ui->topicsList->selectedItems())
  {
    if (!item->isHidden())
    {
      return true;
    }
  }
  return false;
}

QStringList FoxgloveDialog::selectedTopicNames() const
{
  QStringList names;
  for (auto* item : ui->topicsList->selectedItems())
  {
    if (item->isHidden())
    {
      continue;
    }
    const QString topic = item->text(0);
    if (!topic.isEmpty())
    {
      names.push_back(topic);
    }
  }
  return names;
}

std::vector<FoxgloveChannelInfo> FoxgloveDialog::selectedChannels() const
{
  const auto items = ui->topicsList->selectedItems();
  std::vector<FoxgloveChannelInfo> result;
  result.reserve(size_t(items.size()));

  for (auto* item : items)
  {
    if (item->isHidden())
    {
      continue;
    }
    bool ok = false;
    const quint64 id = item->data(0, Qt::UserRole).toULongLong(&ok);
    if (!ok)
    {
      continue;
    }
    const auto it = _channels.find(id);
    if (it != _channels.end())
    {
      result.push_back(it.value());
    }
  }
  return result;
}

unsigned FoxgloveDialog::maxArraySize() const
{
  return ui->spinBoxArraySize->value();
}

bool FoxgloveDialog::clampLargeArrays() const
{
  return ui->radioClamp->isChecked();
}

bool FoxgloveDialog::useTimestamp() const
{
  return ui->checkBoxUseTimestamp->isChecked();
}

void FoxgloveDialog::setOkButton(const QString& text, bool enabled)
{
  if (auto* button = ui->buttonBox->button(QDialogButtonBox::Ok))
  {
    button->setText(text);
    button->setEnabled(enabled);
  }
}

void FoxgloveDialog::setConnected(bool connected)
{
  ui->buttonConnect->blockSignals(true);
  ui->buttonConnect->setChecked(connected);
  ui->buttonConnect->setText(connected ? "Disconnect" : "Connect");
  ui->buttonConnect->blockSignals(false);

  ui->lineEditURL->setEnabled(!connected);
}

QDialogButtonBox* FoxgloveDialog::buttonBox() const
{
  return ui->buttonBox;
}

QTreeWidget* FoxgloveDialog::topicsWidget() const
{
  return ui->topicsList;
}

QPushButton* FoxgloveDialog::connectButton() const
{
  return ui->buttonConnect;
}

void FoxgloveDialog::applyFilter(const QString& filter)
{
  const QString fl = filter.trimmed().toLower();
  auto* list = ui->topicsList;

  for (int row = 0; row < list->topLevelItemCount(); ++row)
  {
    auto* item = list->topLevelItem(row);

    if (fl.isEmpty())
    {
      item->setHidden(false);
      continue;
    }

    const QString topic = item->text(0).toLower();
    const QString type = item->text(1).toLower();
    const bool match = topic.contains(fl) || type.contains(fl);
    if (!match && item->isSelected())
    {
      item->setSelected(false);
    }
    item->setHidden(!match);
  }
}
