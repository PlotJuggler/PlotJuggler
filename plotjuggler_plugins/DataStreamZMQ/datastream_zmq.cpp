#include "datastream_zmq.h"
#include "ui_datastream_zmq.h"

#include "PlotJuggler/messageparser_base.h"
#include "PlotJuggler/dialog_utils.h"
#include <QDebug>
#include <QDialog>
#include <QIntValidator>
#include <QMetaObject>
#include <QMessageBox>
#include <QSettings>
#include <QThread>
#include <chrono>
#include <iostream>

using namespace PJ;

StreamZMQDialog::StreamZMQDialog(QWidget* parent) : QDialog(parent), ui(new Ui::DataStreamZMQ)
{
  ui->setupUi(this);
  ui->lineEditPort->setValidator(new QIntValidator());
}

StreamZMQDialog::~StreamZMQDialog()
{
  while (ui->layoutOptions->count() > 0)
  {
    auto item = ui->layoutOptions->takeAt(0);
    item->widget()->setParent(nullptr);
  }
  delete ui;
}

DataStreamZMQ::DataStreamZMQ()
  : _running(false), _zmq_context(), _zmq_socket(_zmq_context, zmq::socket_type::sub)
{
}

DataStreamZMQ::~DataStreamZMQ()
{
  shutdown();
}

bool DataStreamZMQ::start(QStringList*)
{
  if (_running)
  {
    return _running;
  }

  if (parserFactories() == nullptr || parserFactories()->empty())
  {
    QMessageBox::warning(nullptr, tr("UDP Server"), tr("No available MessageParsers"),
                         QMessageBox::Ok);
    _running = false;
    return false;
  }

  bool ok = false;

  StreamZMQDialog* dialog = new StreamZMQDialog();

  for (const auto& it : *parserFactories())
  {
    dialog->ui->comboBoxProtocol->addItem(it.first);

    if (auto widget = it.second->optionsWidget())
    {
      widget->setVisible(false);
      dialog->ui->layoutOptions->addWidget(widget);
    }
  }

  // load previous values
  QSettings settings;
  QString address = settings.value("ZMQ_Subscriber::address", "localhost").toString();
  QString protocol = settings.value("ZMQ_Subscriber::protocol", "JSON").toString();
  if (parserFactories()->find(protocol) == parserFactories()->end())
  {
    protocol = parserFactories()->begin()->first;
  }
  QString topics = settings.value("ZMQ_Subscriber::topics", "").toString();
  _is_connect = settings.value("ZMQ_Subscriber::is_connect", true).toBool();

  QString previous_address = address;

  connect(dialog->ui->radioConnect, &QRadioButton::toggled, dialog, [&](bool toggled) {
    dialog->ui->lineEditAddress->setEnabled(toggled);
    if (toggled)
    {
      dialog->ui->lineEditAddress->setText(previous_address);
    }
    else
    {
      previous_address = dialog->ui->lineEditAddress->text();
      dialog->ui->lineEditAddress->setText("0.0.0.0");
    }
  });

  if (_is_connect)
  {
    dialog->ui->radioConnect->setChecked(true);
  }
  else
  {
    dialog->ui->radioBind->setChecked(true);
  }

  int port = settings.value("ZMQ_Subscriber::port", 9872).toInt();

  dialog->ui->lineEditAddress->setText(address);
  dialog->ui->lineEditPort->setText(QString::number(port));
  dialog->ui->lineEditTopics->setText(topics);

  // IPC endpoints are filesystem paths; no port applies. Disable the port
  // field when ipc:// is selected so the user doesn't type a value that
  // would be silently ignored.
  auto updatePortEnabled = [dialog]() {
    const bool use_port = dialog->ui->comboBox->currentText() != "ipc://";
    dialog->ui->lineEditPort->setEnabled(use_port);
  };
  connect(dialog->ui->comboBox, qOverload<const QString&>(&QComboBox::currentIndexChanged), dialog,
          [updatePortEnabled](const QString&) { updatePortEnabled(); });
  updatePortEnabled();

  connect(dialog->ui->comboBoxProtocol, qOverload<const QString&>(&QComboBox::currentIndexChanged),
          this, [this, dialog](const QString& selected_protocol) {
            if (_parser_creator)
            {
              if (auto prev_widget = _parser_creator->optionsWidget())
              {
                prev_widget->setVisible(false);
              }
            }
            _parser_creator = parserFactories()->at(selected_protocol);

            showOptionsWidget(dialog, dialog->ui->boxOptions, _parser_creator->optionsWidget());
          });

  dialog->ui->comboBoxProtocol->setCurrentText(protocol);

  int res = dialog->exec();
  if (res == QDialog::Rejected)
  {
    _running = false;
    return false;
  }

  address = dialog->ui->lineEditAddress->text();
  port = dialog->ui->lineEditPort->text().toUShort(&ok);
  protocol = dialog->ui->comboBoxProtocol->currentText();
  topics = dialog->ui->lineEditTopics->text();
  _is_connect = dialog->ui->radioConnect->isChecked();

  _parser = _parser_creator->createParser({}, {}, {}, dataMap());

  // save back to service
  settings.setValue("ZMQ_Subscriber::address", address);
  settings.setValue("ZMQ_Subscriber::protocol", protocol);
  settings.setValue("ZMQ_Subscriber::port", port);
  settings.setValue("ZMQ_Subscriber::topics", topics);
  settings.setValue("ZMQ_Subscriber::is_connect", _is_connect);

  const QString endpoint_protocol = dialog->ui->comboBox->currentText();
  QString addr;
  if (endpoint_protocol == "ipc://")
  {
    // IPC endpoints are filesystem paths; no port suffix.
    addr = endpoint_protocol + address;
  }
  else
  {
    addr = endpoint_protocol + address + ":" + QString::number(port);
  }
  _socket_address = addr.toStdString();
  if (_is_connect)
  {
    _zmq_socket.connect(_socket_address.c_str());
  }
  else
  {
    _zmq_socket.bind(_socket_address.c_str());
  }

  parseTopicFilters(topics);
  subscribeTopics();

  // Add a parser for each topic
  for (const auto& topic : _topic_filters)
  {
    _parsers[topic] = _parser_creator->createParser(topic, {}, {}, dataMap());
  }

  _zmq_socket.set(zmq::sockopt::rcvtimeo, 100);

  qDebug() << "ZMQ listening on address" << QString::fromStdString(_socket_address);
  _running = true;

  _receive_thread = std::thread(&DataStreamZMQ::receiveLoop, this);

  dialog->deleteLater();
  return _running;
}

void DataStreamZMQ::shutdown()
{
  if (_running)
  {
    _running = false;

    if (_receive_thread.joinable())
    {
      _receive_thread.join();
    }

    unsubscribeTopics();

    if (_is_connect)
    {
      _zmq_socket.disconnect(_socket_address.c_str());
    }
    else
    {
      _zmq_socket.unbind(_socket_address.c_str());
    }
  }
}

void DataStreamZMQ::receiveLoop()
{
  while (_running)
  {
    zmq::message_t topic_payload_msg;
    zmq::recv_result_t result = _zmq_socket.recv(topic_payload_msg);

    // If we did not receive anything, continue
    if (topic_payload_msg.size() <= 0)
    {
      continue;
    }

    // If there are more parts, then it is the topic
    std::string topic = "";
    if (topic_payload_msg.more())
    {
      topic = std::string(reinterpret_cast<const char*>(topic_payload_msg.data()), topic_payload_msg.size());

      // Then it is the payload
      topic_payload_msg.rebuild();
      result = _zmq_socket.recv(topic_payload_msg);

      // If we did not receive anything, continue
      if (topic_payload_msg.size() <= 0)
      {
        continue;
      }
    }

    PJ::MessageRef msg{ PJ::MessageRef(reinterpret_cast<uint8_t*>(topic_payload_msg.data()),
                                       topic_payload_msg.size()) };

    // If there are more parts, then it is the timestamp
    double timestamp = 0.0;
    if (topic_payload_msg.more())
    {
      // Create new message_t object to avoid rebuilding topic_payload_msg
      zmq::message_t timestamp_extra_msg;
      result = _zmq_socket.recv(timestamp_extra_msg);

      if (timestamp_extra_msg.size() > 0)
      {
        // The timestamp is the seconds since the epoch as a string
        timestamp =
            std::stod(std::string(reinterpret_cast<const char*>(timestamp_extra_msg.data()), timestamp_extra_msg.size()));
      }
      
      // Extinguish remaining parts (if any)
      while (timestamp_extra_msg.more())
      {
        timestamp_extra_msg.rebuild();
        result = _zmq_socket.recv(timestamp_extra_msg);
      }
    }
    else
    {
      // If there are no more parts, the timestamp is the current time
      timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::high_resolution_clock::now().time_since_epoch())
                      .count() *
                  1e-6;
    }

    // Parse the message without a topic if it is empty
    if (topic.empty())
    {
      if (parseMessage(msg, timestamp))
      {
        emit this->dataReceived();
      }
    }
    // Otherwise, parse the message with the topic
    else
    {
      if (parseMessage(topic, msg, timestamp))
      {
        emit this->dataReceived();
      }
    }
  }
}

bool DataStreamZMQ::parseMessage(const PJ::MessageRef& msg, double& timestamp)
{
  try
  {
    std::lock_guard<std::mutex> lock(mutex());
    _parser->parseMessage(msg, timestamp);
    return true;
  }
  catch (...)
  {
    return false;
  }
}

PJ::MessageParserPtr DataStreamZMQ::ensureTopicParser(const std::string& topic)
{
  {
    std::lock_guard<std::mutex> lock(mutex());
    auto it = _parsers.find(topic);
    if (it != _parsers.end())
    {
      return it->second;
    }
  }

  PJ::MessageParserPtr parser;

  auto create_parser = [this, &parser, topic]() {
    std::lock_guard<std::mutex> lock(mutex());
    auto it = _parsers.find(topic);
    if (it == _parsers.end())
    {
      it = _parsers.emplace(topic, _parser_creator->createParser(topic, {}, {}, dataMap())).first;
    }
    parser = it->second;
  };

  if (QThread::currentThread() == thread())
  {
    create_parser();
  }
  else
  {
    QMetaObject::invokeMethod(this, create_parser, Qt::BlockingQueuedConnection);
  }

  return parser;
}

bool DataStreamZMQ::parseMessage(const std::string& topic, const PJ::MessageRef& msg,
                                 double& timestamp)
{
  try
  {
    auto parser = ensureTopicParser(topic);
    std::lock_guard<std::mutex> lock(mutex());
    parser->parseMessage(msg, timestamp);
    return true;
  }
  catch (...)
  {
    return false;
  }
}

void DataStreamZMQ::parseTopicFilters(const QString& topic_filters)
{
  const QRegExp regex("(,{0,1}\\s+)|(;\\s*)");

  if (topic_filters.trimmed().size() != 0)
  {
    const auto splitted = topic_filters.split(regex);

    for (const auto& topic : splitted)
    {
      _topic_filters.push_back(topic.toStdString());
    }
  }
  else
  {
    _topic_filters.push_back("");
  }
}

void DataStreamZMQ::subscribeTopics()
{
  for (const auto& topic : _topic_filters)
  {
    qDebug() << "ZMQ Subscribed topic" << QString::fromStdString(topic);

    _zmq_socket.set(zmq::sockopt::subscribe, topic);
  }
}

void DataStreamZMQ::unsubscribeTopics()
{
  for (const auto& topic : _topic_filters)
  {
    qDebug() << "ZMQ Unsubscribed topic" << QString::fromStdString(topic);

    _zmq_socket.set(zmq::sockopt::unsubscribe, topic);
  }

  _topic_filters.clear();
}
