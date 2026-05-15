#include "mqtt_dialog.h"
#include <QCoreApplication>
#include <QFileDialog>
#include "PlotJuggler/svg_util.h"

void MQTT_Dialog::onConnectionClosed()
{
  _client->disconnect();
  changeConnectionState(false);
  ui->listWidget->clear();
  _topic_list.clear();
  ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
  _topic_list_timer->stop();
}

MQTT_Dialog::MQTT_Dialog(MQTTClient::Ptr mosq_client)
  : QDialog(nullptr), ui(new Ui::DataStreamMQTT), _client(mosq_client)
{
  ui->setupUi(this);
  ui->lineEditPort->setValidator(new QIntValidator);

  ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);

  static QString uuid = QString("Plotjuggler-") + QString::number(rand());

  QSettings settings;
  restoreGeometry(settings.value("MosquittoMQTT::geometry").toByteArray());

  QString host = settings.value("MosquittoMQTT::host").toString();
  ui->lineEditHost->setText(host);

  int port = settings.value("MosquittoMQTT::port", 1883).toInt();
  ui->lineEditPort->setText(QString::number(port));

  int qos = settings.value("MosquittoMQTT::qos", 0).toInt();
  ui->comboBoxQoS->setCurrentIndex(qos);

  int protocol_mqtt = settings.value("MosquittoMQTT::protocol_version").toInt();
  ui->comboBoxVersion->setCurrentIndex(protocol_mqtt);

  QString topic_filter = settings.value("MosquittoMQTT::filter").toString();
  ui->lineEditTopicFilter->setText(topic_filter);

  QString username = settings.value("MosquittoMQTT::username", "").toString();
  ui->lineEditUsername->setText(username);

  QString password = settings.value("MosquittoMQTT::password", "").toString();
  ui->lineEditPassword->setText(password);

  _server_certificate_file = settings.value("MosquittoMQTT::server_certificate", "").toString();
  if (!_server_certificate_file.isEmpty())
  {
    ui->labelServerCertificate->setText(QFileInfo(_server_certificate_file).fileName());
    ui->buttonEraseServerCertificate->setEnabled(true);
  }

  _client_certificate_file = settings.value("MosquittoMQTT::client_certificate", "").toString();
  if (!_client_certificate_file.isEmpty())
  {
    ui->labelClientCertificate->setText(QFileInfo(_client_certificate_file).fileName());
    ui->buttonEraseClientCertificate->setEnabled(true);
  }

  _private_key_file = settings.value("MosquittoMQTT::private_key", "").toString();
  if (!_private_key_file.isEmpty())
  {
    ui->labelPrivateKey->setText(QFileInfo(_private_key_file).fileName());
    ui->buttonErasePrivateKey->setEnabled(true);
  }

  bool enable_tls = settings.value("MosquittoMQTT::enable_tls", false).toBool();
  ui->checkBoxSecurity->setChecked(enable_tls);

  _topic_list_timer = new QTimer(this);

  QString theme = settings.value("StyleSheet::theme", "light").toString();

  const QPixmap& icon_pixmap = LoadSvg(":/resources/svg/import.svg", theme);
  ui->buttonLoadServerCertificate->setIcon(icon_pixmap);
  ui->buttonLoadClientCertificate->setIcon(icon_pixmap);
  ui->buttonLoadPrivateKey->setIcon(icon_pixmap);

  connect(ui->buttonConnect, &QPushButton::clicked, this, &MQTT_Dialog::onButtonConnect);

  connect(_topic_list_timer, &QTimer::timeout, this, &MQTT_Dialog::onUpdateTopicList);

  connect(ui->listWidget, &QListWidget::itemSelectionChanged, this,
          &MQTT_Dialog::onSelectionChanged);

  connect(ui->buttonLoadServerCertificate, &QPushButton::clicked, this,
          &MQTT_Dialog::onLoadServerCertificate);

  connect(ui->buttonLoadClientCertificate, &QPushButton::clicked, this,
          &MQTT_Dialog::onLoadClientCertificate);

  connect(ui->buttonLoadPrivateKey, &QPushButton::clicked, this, &MQTT_Dialog::onLoadPrivateKey);

  connect(ui->buttonEraseServerCertificate, &QPushButton::clicked, this, [this]() {
    ui->buttonEraseServerCertificate->setEnabled(false);
    ui->labelServerCertificate->setText("");
    _server_certificate_file.clear();
  });

  connect(ui->buttonEraseClientCertificate, &QPushButton::clicked, this, [this]() {
    ui->buttonEraseClientCertificate->setEnabled(false);
    ui->labelClientCertificate->setText("");
    _client_certificate_file.clear();
  });

  connect(ui->buttonErasePrivateKey, &QPushButton::clicked, this, [this]() {
    ui->buttonErasePrivateKey->setEnabled(false);
    ui->labelPrivateKey->setText("");
    _private_key_file.clear();
  });

  connect(_client.get(), &MQTTClient::disconnected, this, [this]() {
    onConnectionClosed();
    QMessageBox::warning(this, "Connection Lost",
                         "Client disconnected. Maybe a problem with authentication?");
  });
}

void MQTT_Dialog::saveSettings()
{
  QSettings settings;
  settings.setValue("MosquittoMQTT::geometry", this->saveGeometry());

  // save back to service
  settings.setValue("MosquittoMQTT::host", ui->lineEditHost->text());
  settings.setValue("MosquittoMQTT::port", ui->lineEditPort->text().toInt());
  settings.setValue("MosquittoMQTT::filter", ui->lineEditTopicFilter->text());
  settings.setValue("MosquittoMQTT::password", ui->lineEditPassword->text());
  settings.setValue("MosquittoMQTT::username", ui->lineEditUsername->text());
  settings.setValue("MosquittoMQTT::protocol_version", ui->comboBoxVersion->currentIndex());
  settings.setValue("MosquittoMQTT::qos", ui->comboBoxQoS->currentIndex());
  settings.setValue("MosquittoMQTT::serialization_protocol", ui->comboBoxProtocol->currentText());
  settings.setValue("MosquittoMQTT::server_certificate", _server_certificate_file);
  settings.setValue("MosquittoMQTT::client_certificate", _client_certificate_file);
  settings.setValue("MosquittoMQTT::private_key", _private_key_file);
  settings.setValue("MosquittoMQTT::enable_tls", ui->checkBoxSecurity->isChecked());
}

MQTT_Dialog::~MQTT_Dialog()
{
  // When the destructor runs from static teardown (e.g. __cxa_finalize_ranges
  // after exit()), QtCore globals (QLocale, QSettings, QCoreApplication) are
  // already destroyed. Any Qt operation here would dereference freed memory.
  // Bail out and let the OS reclaim memory; the process is exiting anyway.
  if (!QCoreApplication::instance())
  {
    return;
  }

  // The widgets in layoutOptions are borrowed from parser factories that own
  // them. Detach them from this dialog before destruction so Qt's parent
  // ownership does not delete factory-owned widgets.
  while (ui->layoutOptions->count() > 0)
  {
    auto item = ui->layoutOptions->takeAt(0);
    item->widget()->setParent(nullptr);
  }

  saveSettings();

  delete ui;
}

void MQTT_Dialog::onButtonConnect()
{
  if (_client->isConnected())
  {
    onConnectionClosed();
    return;
  }

  MosquittoConfig config;

  config.host = ui->lineEditHost->text().toStdString();
  config.port = ui->lineEditPort->text().toInt();
  config.topics.clear();
  config.topics.push_back(ui->lineEditTopicFilter->text().toStdString());
  config.username = ui->lineEditUsername->text().toStdString();
  config.password = ui->lineEditPassword->text().toStdString();
  config.qos = ui->comboBoxQoS->currentIndex();
  config.protocol_version = MQTT_PROTOCOL_V31 + ui->comboBoxVersion->currentIndex();

  config.keepalive = 60;        // TODO
  config.bind_address = "";     // TODO
  config.max_inflight = 20;     // TODO
  config.clean_session = true;  // TODO
  config.eol = true;            // TODO

  if (ui->checkBoxSecurity->isChecked())
  {
    config.cafile = _server_certificate_file.toStdString();
    config.certfile = _client_certificate_file.toStdString();
    config.keyfile = _private_key_file.toStdString();
  }

  changeConnectionState(true);
  _client->connect(config);
  _topic_list_timer->start(2000);

  saveSettings();
}

void MQTT_Dialog::onUpdateTopicList()
{
  auto topic_list = _client->getTopicList();

  bool changed = ui->listWidget->count() != topic_list.size();
  if (!changed)
  {
    for (const auto& topic : topic_list)
    {
      if (_topic_list.count(topic) == 0)
      {
        changed = true;
        break;
      }
    }
  }

  if (changed)
  {
    ui->listWidget->clear();
    for (const auto& topic : topic_list)
    {
      ui->listWidget->addItem(QString::fromStdString(topic));
      _topic_list.insert(topic);
    }
    ui->listWidget->sortItems();
  }

  if (_client->isConnected() == false)
  {
    onConnectionClosed();
  }
}

void MQTT_Dialog::onSelectionChanged()
{
  bool selected = ui->listWidget->selectedItems().count() > 0;
  ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(selected);
}

void MQTT_Dialog::changeConnectionState(bool connected)
{
  ui->connectionFrame->setEnabled(!connected);
  ui->buttonConnect->setText(connected ? "Disconnect" : "Connect");
  ui->lineEditTopicFilter->setEnabled(!connected);
  ui->listWidget->setEnabled(connected);
}

void MQTT_Dialog::onLoadServerCertificate()
{
  QSettings settings;
  QString directory = settings.value("MQTT_Dialog.loadDirectory", QDir::currentPath()).toString();

  QString filename =
      QFileDialog::getOpenFileName(this, "Select Server Certificate", directory, tr("CRT (*.crt)"));

  if (!filename.isEmpty())
  {
    _server_certificate_file = filename;
    directory = QFileInfo(filename).absolutePath();
    settings.setValue("MQTT_Dialog.loadDirectory", directory);

    ui->labelServerCertificate->setText(QFileInfo(filename).fileName());
    ui->buttonEraseServerCertificate->setEnabled(true);
  }
}

void MQTT_Dialog::onLoadClientCertificate()
{
  QSettings settings;
  QString directory = settings.value("MQTT_Dialog.loadDirectory", QDir::currentPath()).toString();

  QString filename =
      QFileDialog::getOpenFileName(this, "Select Client Certificate", directory, tr("CRT (*.crt)"));

  if (!filename.isEmpty())
  {
    _client_certificate_file = filename;
    directory = QFileInfo(filename).absolutePath();
    settings.setValue("MQTT_Dialog.loadDirectory", directory);

    ui->labelClientCertificate->setText(QFileInfo(filename).fileName());
    ui->buttonEraseClientCertificate->setEnabled(true);
  }
}

void MQTT_Dialog::onLoadPrivateKey()
{
  QSettings settings;
  QString directory = settings.value("MQTT_Dialog.loadDirectory", QDir::currentPath()).toString();

  QString filename =
      QFileDialog::getOpenFileName(this, "Select PrivateKey", directory, tr("Key (*.key)"));

  if (!filename.isEmpty())
  {
    _private_key_file = filename;
    directory = QFileInfo(filename).absolutePath();
    settings.setValue("MQTT_Dialog.loadDirectory", directory);

    ui->labelPrivateKey->setText(QFileInfo(filename).fileName());
    ui->buttonErasePrivateKey->setEnabled(true);
  }
}
