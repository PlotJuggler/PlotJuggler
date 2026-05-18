#include "foxglove_client.h"

#include <QDialogButtonBox>
#include <QtEndian>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMessageBox>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSettings>

#include <cstring>

namespace
{

constexpr auto kSubprotocol = "foxglove.sdk.v1";
constexpr uint8_t kMessageDataOpcode = 0x01;
constexpr int kDataReceivedIntervalMs = 20;
constexpr int kStatusLevelInfo = 0;
constexpr int kStatusLevelWarning = 1;
constexpr int kStatusLevelError = 2;

template <typename T>
bool readLE(const uint8_t*& ptr, const uint8_t* end, T& out)
{
  if (ptr + sizeof(T) > end)
  {
    return false;
  }
  std::memcpy(&out, ptr, sizeof(T));
  out = qFromLittleEndian(out);
  ptr += sizeof(T);
  return true;
}

quint64 jsonToUInt64(const QJsonValue& value, bool* ok)
{
  *ok = false;

  if (value.isDouble())
  {
    const double dbl = value.toDouble();
    if (dbl >= 0)
    {
      *ok = true;
      return quint64(dbl);
    }
  }
  if (value.isString())
  {
    return value.toString().toULongLong(ok);
  }
  if (value.isUndefined() || value.isNull())
  {
    return 0;
  }

  const QVariant variant = value.toVariant();
  bool local_ok = false;
  const quint64 result = variant.toULongLong(&local_ok);
  *ok = local_ok;
  return result;
}

}  // namespace

FoxgloveBridgeClient::FoxgloveBridgeClient()
{
  loadDefaultSettings();
  setupSettings();

  _data_received_timer.setInterval(kDataReceivedIntervalMs);
  _data_received_timer.setSingleShot(true);
  connect(&_data_received_timer, &QTimer::timeout, this, &FoxgloveBridgeClient::flushPendingData);

  connect(&_socket, &QWebSocket::connected, this, &FoxgloveBridgeClient::onConnected);
  connect(&_socket, &QWebSocket::textMessageReceived, this,
          &FoxgloveBridgeClient::onTextMessageReceived);
  connect(&_socket, &QWebSocket::binaryMessageReceived, this,
          &FoxgloveBridgeClient::onBinaryMessageReceived);
  connect(&_socket, &QWebSocket::disconnected, this, &FoxgloveBridgeClient::onDisconnected);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  connect(&_socket, &QWebSocket::errorOccurred, this, &FoxgloveBridgeClient::onError);
#else
  connect(&_socket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error), this,
          &FoxgloveBridgeClient::onError);
#endif
}

void FoxgloveBridgeClient::setupSettings()
{
  _action_pause = new QAction("Pause", this);
  connect(_action_pause, &QAction::triggered, this, &FoxgloveBridgeClient::onPauseTriggered);

  _actions = { _action_pause };
}

const std::vector<QAction*>& FoxgloveBridgeClient::availableActions()
{
  return _actions;
}

bool FoxgloveBridgeClient::start(QStringList*)
{
  if (_running)
  {
    return true;
  }

  FoxgloveDialog dialog(_config);
  _dialog = &dialog;

  connect(dialog.topicsWidget(), &QTreeWidget::itemSelectionChanged, this,
          &FoxgloveBridgeClient::updateOkButton);

  connect(dialog.connectButton(), &QPushButton::toggled, this,
          &FoxgloveBridgeClient::onDialogConnectToggled);

  connect(dialog.buttonBox(), &QDialogButtonBox::accepted, this,
          &FoxgloveBridgeClient::onDialogAccepted);

  connect(dialog.buttonBox(), &QDialogButtonBox::rejected, this,
          &FoxgloveBridgeClient::onDialogRejected);

  dialog.exec();
  _dialog = nullptr;

  if (!_running || _state != FoxgloveState::Streaming)
  {
    shutdown();
    return false;
  }
  return true;
}

void FoxgloveBridgeClient::onPauseTriggered()
{
  if (!_running || _state != FoxgloveState::Streaming)
  {
    return;
  }
  _paused = !_paused;
  _action_pause->setText(_paused ? "Resume" : "Pause");
}

bool FoxgloveBridgeClient::openDialogConnection()
{
  if (!_dialog)
  {
    return false;
  }

  const QString url_str = _dialog->url();
  const QUrl url(url_str);
  if (!url.isValid())
  {
    QMessageBox::warning(_dialog, "Foxglove Bridge", "Invalid URL", QMessageBox::Ok);
    _dialog->setConnected(false);
    return false;
  }

  _url = url;
  _config.url = url_str;
  saveDefaultSettings();

  _dialog->connectButton()->setText("Connecting...");

  QNetworkRequest request(_url);
  request.setRawHeader("Sec-WebSocket-Protocol", kSubprotocol);
  _socket.open(request);
  return true;
}

void FoxgloveBridgeClient::onDialogConnectToggled(bool checked)
{
  if (checked)
  {
    openDialogConnection();
    return;
  }

  _closing = true;
  if (_socket.state() != QAbstractSocket::UnconnectedState)
  {
    _socket.abort();
    _socket.close();
  }
  resetState();
  _running = false;

  if (_dialog)
  {
    _dialog->clearChannels();
  }
  updateOkButton();
}

void FoxgloveBridgeClient::onDialogAccepted()
{
  if (!_dialog || !_running || _state != FoxgloveState::SelectingTopics || !_dialog->hasSelection())
  {
    return;
  }
  _config.max_array_size = _dialog->maxArraySize();
  _config.clamp_large_arrays = _dialog->clampLargeArrays();
  _config.use_timestamp = _dialog->useTimestamp();

  if (subscribeSelectedChannels(_dialog->selectedChannels()))
  {
    _dialog->accept();
  }
}

void FoxgloveBridgeClient::onDialogRejected()
{
  _closing = true;
  if (_socket.state() != QAbstractSocket::UnconnectedState)
  {
    _socket.abort();
    _socket.close();
  }
  if (_dialog)
  {
    _dialog->reject();
  }
}

void FoxgloveBridgeClient::shutdown()
{
  const bool had_data = _running;

  _closing = true;
  _data_received_timer.stop();
  _data_received_pending = false;

  if (_socket.state() == QAbstractSocket::ConnectedState && _state == FoxgloveState::Streaming)
  {
    unsubscribeActiveSubscriptions();
  }

  if (_socket.state() != QAbstractSocket::UnconnectedState)
  {
    _socket.abort();
    _socket.close();
  }

  resetState();
  _running = false;
  _paused = false;
  _closing = false;

  if (_action_pause)
  {
    _action_pause->setText("Pause");
  }

#ifdef PJ_BUILD
  if (had_data)
  {
    std::lock_guard<std::mutex> lock(mutex());
    dataMap().clear();
    emit dataReceived();
  }
#endif
}

void FoxgloveBridgeClient::saveDefaultSettings()
{
  QSettings settings;
  _config.saveToSettings(settings, "FoxgloveBridgeCX");
}

void FoxgloveBridgeClient::loadDefaultSettings()
{
  QSettings settings;
  _config.loadFromSettings(settings, "FoxgloveBridgeCX");
}

void FoxgloveBridgeClient::updateOkButton()
{
  if (!_dialog)
  {
    return;
  }
  const bool enabled =
      _running && _state == FoxgloveState::SelectingTopics && _dialog->hasSelection();
  _dialog->setOkButton("Subscribe", enabled);
}

void FoxgloveBridgeClient::resetState()
{
  _state = FoxgloveState::Disconnected;
  _channels.clear();
  _subscriptions.clear();
  _subscription_by_channel.clear();
#ifdef PJ_BUILD
  _parsers_by_subscription.clear();
#endif
  _next_subscription_id = 1;
  _data_received_timer.stop();
  _data_received_pending = false;
}

bool FoxgloveBridgeClient::canUseChannel(const FoxgloveChannelInfo& channel) const
{
  return channel.encoding == "cdr" && channel.schema_encoding == "ros2msg" &&
         !channel.schema.isEmpty() && !channel.schema_name.isEmpty() && !channel.topic.isEmpty();
}

bool FoxgloveBridgeClient::subscribeSelectedChannels(
    const std::vector<FoxgloveChannelInfo>& channels)
{
  const auto factories = parserFactories();
  if (!factories)
  {
    QMessageBox::warning(nullptr, "Foxglove Bridge", "No parser factories are available",
                         QMessageBox::Ok);
    return false;
  }

  auto parser_it = factories->find("ros2msg");
  if (parser_it == factories->end())
  {
    QMessageBox::warning(nullptr, "Foxglove Bridge", "No parser available for encoding [ros2msg]",
                         QMessageBox::Ok);
    return false;
  }

  QJsonArray subscriptions_json;
  QStringList subscribed_topics;
  QStringList failures;

#ifdef PJ_BUILD
  QHash<quint32, PJ::MessageParserPtr> new_parsers;
#endif
  QHash<quint32, quint64> new_subscriptions;
  QHash<quint64, quint32> new_subscription_by_channel;

  for (const auto& channel : channels)
  {
    const quint32 subscription_id = _next_subscription_id++;
    const std::string schema_data = channel.schema.toStdString();

#ifdef PJ_BUILD
    try
    {
      auto parser = parser_it->second->createParser(
          channel.topic.toStdString(), channel.schema_name.toStdString(), schema_data, dataMap());
      if (!parser)
      {
        failures.push_back(QString("%1: parser creation returned null").arg(channel.topic));
        continue;
      }
      parser->setLargeArraysPolicy(_config.clamp_large_arrays, _config.max_array_size);
      parser->enableEmbeddedTimestamp(_config.use_timestamp);
      new_parsers.insert(subscription_id, std::move(parser));
    }
    catch (std::exception& ex)
    {
      failures.push_back(QString("%1: %2").arg(channel.topic, ex.what()));
      continue;
    }
#endif

    QJsonObject sub;
    sub["id"] = int(subscription_id);
    sub["channelId"] = double(channel.id);
    subscriptions_json.push_back(sub);

    subscribed_topics.push_back(channel.topic);
    new_subscriptions.insert(subscription_id, channel.id);
    new_subscription_by_channel.insert(channel.id, subscription_id);
  }

  if (subscriptions_json.isEmpty())
  {
    const QString msg =
        failures.isEmpty() ? "No compatible ROS 2 channels were selected" : failures.join('\n');
    QMessageBox::warning(nullptr, "Foxglove Bridge", msg, QMessageBox::Ok);
    return false;
  }

  QJsonObject message;
  message["op"] = "subscribe";
  message["subscriptions"] = subscriptions_json;
  if (!sendJsonMessage(message))
  {
    QMessageBox::warning(nullptr, "Foxglove Bridge",
                         "Failed to send subscribe request because the connection is no longer "
                         "available",
                         QMessageBox::Ok);
    return false;
  }

  _config.topics = subscribed_topics;
  saveDefaultSettings();

  for (auto it = new_subscriptions.cbegin(); it != new_subscriptions.cend(); ++it)
  {
    _subscriptions.insert(it.key(), it.value());
  }
  for (auto it = new_subscription_by_channel.cbegin(); it != new_subscription_by_channel.cend();
       ++it)
  {
    _subscription_by_channel.insert(it.key(), it.value());
  }
#ifdef PJ_BUILD
  for (auto it = new_parsers.cbegin(); it != new_parsers.cend(); ++it)
  {
    _parsers_by_subscription.insert(it.key(), it.value());
  }
#endif

  _state = FoxgloveState::Streaming;
  _paused = false;
  if (_action_pause)
  {
    _action_pause->setText("Pause");
  }

  if (!failures.isEmpty())
  {
    QMessageBox::warning(nullptr, "Foxglove Bridge", failures.join('\n'), QMessageBox::Ok);
  }

  return true;
}

void FoxgloveBridgeClient::unsubscribeActiveSubscriptions()
{
  if (_subscriptions.isEmpty() || _socket.state() != QAbstractSocket::ConnectedState)
  {
    return;
  }

  QJsonArray subscription_ids;
  for (auto it = _subscriptions.cbegin(); it != _subscriptions.cend(); ++it)
  {
    subscription_ids.push_back(int(it.key()));
  }

  QJsonObject message;
  message["op"] = "unsubscribe";
  message["subscriptionIds"] = subscription_ids;
  sendJsonMessage(message);
}

bool FoxgloveBridgeClient::sendJsonMessage(const QJsonObject& obj)
{
  if (_socket.state() != QAbstractSocket::ConnectedState)
  {
    return false;
  }
  const QJsonDocument doc(obj);
  return _socket.sendTextMessage(QString::fromUtf8(doc.toJson(QJsonDocument::Compact))) >= 0;
}

void FoxgloveBridgeClient::flushPendingData()
{
  if (!_data_received_pending)
  {
    return;
  }
  _data_received_pending = false;
  emit dataReceived();
}

void FoxgloveBridgeClient::onConnected()
{
  _running = true;
  _state = FoxgloveState::SelectingTopics;
  _paused = false;

  if (_dialog)
  {
    _dialog->setConnected(true);
    _dialog->setChannels(_channels, _config.topics);
    updateOkButton();
  }
}

void FoxgloveBridgeClient::onDisconnected()
{
  const bool was_running = _running;
  const bool was_closing = _closing;
  const auto previous_state = _state;

  if (_dialog)
  {
    _dialog->clearChannels();
    _dialog->setConnected(false);
    updateOkButton();
  }

  resetState();
  _running = false;
  _paused = false;
  _closing = false;

  if (_action_pause)
  {
    _action_pause->setText("Pause");
  }

  if (was_running && !was_closing && !_dialog && previous_state == FoxgloveState::Streaming)
  {
    QMessageBox::warning(nullptr, "Foxglove Bridge", "Server closed the connection",
                         QMessageBox::Ok);
    emit closed();
  }
}

void FoxgloveBridgeClient::onError(QAbstractSocket::SocketError)
{
  if (!_closing)
  {
    if (_dialog)
    {
      _dialog->setConnected(false);
      _dialog->clearChannels();
      updateOkButton();
    }
    QMessageBox::warning(nullptr, "Foxglove Bridge", _socket.errorString(), QMessageBox::Ok);
  }
}

void FoxgloveBridgeClient::onTextMessageReceived(const QString& message)
{
  if (!_running)
  {
    return;
  }

  QJsonParseError err;
  const auto doc = QJsonDocument::fromJson(message.toUtf8(), &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject())
  {
    return;
  }

  const auto obj = doc.object();
  const QString op = obj.value("op").toString();

  if (op == "advertise")
  {
    const auto channels = obj.value("channels").toArray();
    for (const auto& value : channels)
    {
      if (!value.isObject())
      {
        continue;
      }
      const auto channel_obj = value.toObject();
      bool ok = false;
      const quint64 id = jsonToUInt64(channel_obj.value("id"), &ok);
      if (!ok)
      {
        continue;
      }

      FoxgloveChannelInfo channel;
      channel.id = id;
      channel.topic = channel_obj.value("topic").toString();
      channel.encoding = channel_obj.value("encoding").toString();
      channel.schema_name = channel_obj.value("schemaName").toString();
      channel.schema = channel_obj.value("schema").toString();
      channel.schema_encoding = channel_obj.value("schemaEncoding").toString();

      if (canUseChannel(channel))
      {
        _channels.insert(id, channel);
      }
      else
      {
        _channels.remove(id);
      }
    }
    if (_state == FoxgloveState::SelectingTopics && _dialog)
    {
      _dialog->setChannels(_channels, _config.topics);
      updateOkButton();
    }
    return;
  }

  if (op == "unadvertise")
  {
    QStringList removed_subscribed_topics;
    const auto ids = obj.value("channelIds").toArray();
    for (const auto& value : ids)
    {
      bool ok = false;
      const quint64 channel_id = jsonToUInt64(value, &ok);
      if (!ok)
      {
        continue;
      }

      const auto channel_it = _channels.find(channel_id);
      QString channel_topic;
      if (channel_it != _channels.end())
      {
        channel_topic = channel_it->topic;
        _channels.erase(channel_it);
      }

      const auto sub_it = _subscription_by_channel.find(channel_id);
      if (sub_it != _subscription_by_channel.end())
      {
        const quint32 subscription_id = sub_it.value();
        removed_subscribed_topics.push_back(
            channel_topic.isEmpty() ? QString("channel %1").arg(channel_id) : channel_topic);
        _subscription_by_channel.erase(sub_it);
        _subscriptions.remove(subscription_id);
#ifdef PJ_BUILD
        _parsers_by_subscription.remove(subscription_id);
#endif
      }
    }

    if (_state == FoxgloveState::SelectingTopics && _dialog)
    {
      _dialog->setChannels(_channels, _config.topics);
      updateOkButton();
    }
    else if (!_dialog && !removed_subscribed_topics.isEmpty())
    {
      if (_subscriptions.isEmpty())
      {
        QMessageBox::warning(nullptr, "Foxglove Bridge",
                             QString("All subscribed channels were removed by the server:\n%1")
                                 .arg(removed_subscribed_topics.join('\n')),
                             QMessageBox::Ok);
        shutdown();
        emit closed();
        return;
      }

      QMessageBox::warning(nullptr, "Foxglove Bridge",
                           QString("The server removed subscribed channel(s):\n%1")
                               .arg(removed_subscribed_topics.join('\n')),
                           QMessageBox::Ok);
    }
    return;
  }

  if (op == "status")
  {
    const QString text = obj.value("message").toString();
    int level = -1;
    const auto level_value = obj.value("level");
    if (level_value.isDouble())
    {
      level = level_value.toInt(-1);
    }
    else
    {
      const QString level_text = level_value.toString().trimmed().toLower();
      if (level_text == "info")
      {
        level = kStatusLevelInfo;
      }
      else if (level_text == "warn" || level_text == "warning")
      {
        level = kStatusLevelWarning;
      }
      else if (level_text == "error")
      {
        level = kStatusLevelError;
      }
    }

    if (!text.isEmpty() && level >= kStatusLevelWarning)
    {
      QMessageBox::warning(nullptr, "Foxglove Bridge", text, QMessageBox::Ok);
    }
    return;
  }

  if (op == "serverInfo")
  {
    return;
  }
}

void FoxgloveBridgeClient::onBinaryMessageReceived(const QByteArray& message)
{
  if (!_running || _state != FoxgloveState::Streaming || _paused)
  {
    return;
  }

  if (message.size() < 1 + 4 + 8)
  {
    return;
  }

  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(message.constData());
  const uint8_t* end = ptr + message.size();

  const uint8_t opcode = *ptr++;
  if (opcode != kMessageDataOpcode)
  {
    return;
  }

  quint32 subscription_id = 0;
  quint64 log_time = 0;
  if (!readLE(ptr, end, subscription_id) || !readLE(ptr, end, log_time))
  {
    return;
  }

#ifdef PJ_BUILD
  auto parser_it = _parsers_by_subscription.find(subscription_id);
  if (parser_it == _parsers_by_subscription.end())
  {
    return;
  }

  double timestamp = double(log_time) * 1e-9;
  QString parse_error;
  try
  {
    std::lock_guard<std::mutex> lock(mutex());
    parser_it.value()->parseMessage(PJ::MessageRef(ptr, size_t(end - ptr)), timestamp);
  }
  catch (std::exception& ex)
  {
    parse_error = QString::fromLocal8Bit(ex.what());
  }

  if (!parse_error.isEmpty())
  {
    QMessageBox::warning(
        nullptr, tr("Foxglove Bridge"),
        tr("Problem parsing the message. Foxglove Bridge will be stopped.\n%1").arg(parse_error),
        QMessageBox::Ok);
    shutdown();
    emit closed();
    return;
  }
#endif

  _data_received_pending = true;
  if (!_data_received_timer.isActive())
  {
    _data_received_timer.start();
  }
}

bool FoxgloveBridgeClient::xmlSaveState(QDomDocument& doc, QDomElement& parent_element) const
{
  _config.xmlSaveState(doc, parent_element);
  return true;
}

bool FoxgloveBridgeClient::xmlLoadState(const QDomElement& parent_element)
{
  _config.xmlLoadState(parent_element);
  return true;
}
