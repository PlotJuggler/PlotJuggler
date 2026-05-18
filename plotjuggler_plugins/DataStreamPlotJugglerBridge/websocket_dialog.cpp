#include "websocket_dialog.h"

#include <QRegularExpressionValidator>
#include <QScrollBar>
#include <QPushButton>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTimer>

#include "ui_websocket_client.h"

namespace
{
constexpr auto url_regex = "^wss?:\\/\\/([a-zA-Z0-9][-a-zA-Z0-9._]+)"
                           "(:[0-9]{1,5})?"
                           "(\\/.*)?$";
}

WebsocketDialog::WebsocketDialog(const WebsocketClientConfig& config)
  : QDialog(nullptr), ui(new Ui::WebSocketDialog)
{
  ui->setupUi(this);
  setWindowTitle("WebSocket Client");

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

  auto okBtn = ui->buttonBox->button(QDialogButtonBox::Ok);
  if (okBtn)
  {
    okBtn->setText("Subscribe");
    okBtn->setEnabled(false);
  }

  // Enable sorting, default by topic name ascending
  ui->topicsList->setSortingEnabled(true);
  ui->topicsList->sortByColumn(0, Qt::AscendingOrder);

  // Wire up internal filter
  connect(ui->lineEditFilter, &QLineEdit::textChanged, this, &WebsocketDialog::applyFilter);

  // Restore saved geometry
  QSettings s;
  restoreGeometry(s.value("WebsocketClient/dialogGeometry").toByteArray());
}

WebsocketDialog::~WebsocketDialog()
{
  QSettings s;
  s.setValue("WebsocketClient/dialogGeometry", saveGeometry());
  delete ui;
}

// --- Connection URL ---

QString WebsocketDialog::url() const
{
  return ui->lineEditURL->text().trimmed();
}

// --- Topic list management ---

void WebsocketDialog::setTopics(const QJsonArray& topics, const QStringList& preselectNames)
{
  auto* view = ui->topicsList;

  // Save current scroll position
  auto* vsb = view->verticalScrollBar();
  const int scroll_y = vsb ? vsb->value() : 0;

  // Merge persisted + current selection for restore
  QStringList wanted = preselectNames;
  for (auto* it : view->selectedItems())
  {
    const auto n = it->text(0);
    if (!wanted.contains(n))
    {
      wanted << n;
    }
  }

  // Batch-update without triggering signals
  view->setUpdatesEnabled(false);
  view->blockSignals(true);
  view->setSortingEnabled(false);
  view->setVisible(false);
  view->clear();

  for (const auto& v : topics)
  {
    if (!v.isObject())
    {
      continue;
    }
    const auto t = v.toObject();
    const auto name = t.value("name").toString();
    const auto type = t.value("type").toString();
    if (name.isEmpty())
    {
      continue;
    }

    auto* item = new QTreeWidgetItem(view);
    item->setText(0, name);
    item->setText(1, type);

    if (wanted.contains(name))
    {
      item->setSelected(true);
    }
  }

  applyFilter(ui->lineEditFilter->text());
  view->resizeColumnToContents(0);

  view->setSortingEnabled(true);
  view->setVisible(true);
  view->blockSignals(false);
  view->setUpdatesEnabled(true);

  // Restore scroll position after layout update
  QTimer::singleShot(0, view, [view, scroll_y]() {
    if (auto* sb = view->verticalScrollBar())
    {
      sb->setValue(scroll_y);
    }
  });
}

bool WebsocketDialog::hasSelection() const
{
  return !ui->topicsList->selectedItems().isEmpty();
}

QStringList WebsocketDialog::selectedTopicNames() const
{
  QStringList names;
  for (auto* it : ui->topicsList->selectedItems())
  {
    const auto name = it->text(0);
    if (!name.isEmpty())
    {
      names << name;
    }
  }
  return names;
}

std::vector<TopicInfo> WebsocketDialog::selectedTopics() const
{
  std::vector<TopicInfo> result;
  for (auto* it : ui->topicsList->selectedItems())
  {
    const auto name = it->text(0);
    if (name.isEmpty())
    {
      continue;
    }
    TopicInfo info;
    info.name = name;
    info.type = it->text(1);
    result.push_back(std::move(info));
  }
  return result;
}

void WebsocketDialog::clearTopics()
{
  ui->topicsList->clear();
}

// --- Parser options ---

unsigned WebsocketDialog::maxArraySize() const
{
  return ui->spinBoxArraySize->value();
}

bool WebsocketDialog::clampLargeArrays() const
{
  return ui->radioClamp->isChecked();
}

bool WebsocketDialog::useTimestamp() const
{
  return ui->checkBoxUseTimestamp->isChecked();
}

// --- OK button ---

void WebsocketDialog::setOkButton(const QString& text, bool enabled)
{
  auto b = ui->buttonBox->button(QDialogButtonBox::Ok);
  if (b)
  {
    b->setText(text);
    b->setEnabled(enabled);
  }
}

// Signal access for external connections
QDialogButtonBox* WebsocketDialog::buttonBox() const
{
  return ui->buttonBox;
}

QTreeWidget* WebsocketDialog::topicsWidget() const
{
  return ui->topicsList;
}

QPushButton* WebsocketDialog::connectButton() const
{
  return ui->buttonConnect;
}

void WebsocketDialog::setConnected(bool connected)
{
  ui->buttonConnect->blockSignals(true);
  ui->buttonConnect->setChecked(connected);
  ui->buttonConnect->setText(connected ? "Connected" : "Connect");
  ui->buttonConnect->blockSignals(false);

  ui->lineEditURL->setEnabled(!connected);
}

void WebsocketDialog::applyFilter(const QString& filter)
{
  auto* list = ui->topicsList;
  const QString fl = filter.trimmed().toLower();

  for (int i = 0; i < list->topLevelItemCount(); i++)
  {
    auto* it = list->topLevelItem(i);
    const bool selected = it->isSelected();

    if (fl.isEmpty())
    {
      it->setHidden(false);
      continue;
    }

    const QString name = it->text(0).toLower();
    const QString type = it->text(1).toLower();
    const bool match = name.contains(fl) || type.contains(fl);
    it->setHidden(!(match || selected));
  }
}
