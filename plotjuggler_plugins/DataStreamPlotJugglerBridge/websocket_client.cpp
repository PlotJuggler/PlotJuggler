#include "websocket_client.h"
#include "websocket_dialog.h"

#include <QJsonArray>
#include <QJsonParseError>
#include <QMessageBox>

#include <QtEndian>
#include <cstring>
#include <zstd.h>
#include <set>

// =======================
// WebsocketClient
// =======================
WebsocketClient::WebsocketClient()
  : _running(false), _paused(false), _closing(false), _dialog(nullptr)
{
  loadDefaultSettings();
  setupSettings();

  // Initial state
  _state.mode = WsState::Mode::Close;
  _state.req_in_flight = false;

  // Pending request tracking
  _pending_request_id.clear();
  _pending_mode = WsState::Mode::Close;

  // Timer used to periodically request topics (only while selecting topics)
  _topics_timer.setInterval(1000);
  connect(&_topics_timer, &QTimer::timeout, this, &WebsocketClient::requestTopics);

  // Heartbeat timer (used in Data mode)
  _heartbeat_timer.setInterval(1000);
  connect(&_heartbeat_timer, &QTimer::timeout, this, &WebsocketClient::sendHeartBeat);

  // Debug stats timer (5s interval)
  //  _stats_timer.setInterval(5000);
  //  connect(&_stats_timer, &QTimer::timeout, this, &WebsocketClient::printStats);

  // WebSocket signals
  connect(&_socket, &QWebSocket::connected, this, &WebsocketClient::onConnected);
  connect(&_socket, &QWebSocket::textMessageReceived, this,
          &WebsocketClient::onTextMessageReceived);
  connect(&_socket, &QWebSocket::binaryMessageReceived, this,
          &WebsocketClient::onBinaryMessageReceived);
  connect(&_socket, &QWebSocket::disconnected, this, &WebsocketClient::onDisconnected);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  connect(&_socket, &QWebSocket::errorOccurred, this, &WebsocketClient::onError);
#else
  connect(&_socket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error), this,
          &WebsocketClient::onError);
#endif
}

void WebsocketClient::setupSettings()
{
  // Action shown in PlotJuggler "Settings"
  _action_settings = new QAction("Pause", this);

  // Initial state
  _action_settings->setText("Pause");

  // Toggle pause / resume
  connect(_action_settings, &QAction::triggered, this, [this]() {
    // Not running
    if (!_running)
    {
      return;
    }

    // Request in flight
    if (_state.req_in_flight)
    {
      return;
    }

    // If paused -> resume
    if (_paused)
    {
      if (resume())
      {
        _paused = false;
        _action_settings->setText("Pause");
      }
    }
    else
    {
      // If running -> pause
      if (pause())
      {
        _paused = true;
        _action_settings->setText("Resume");
      }
    }
  });

  // Expose action to PlotJuggler
  _actions = { _action_settings };
}

// =======================
// PlotJuggler actions
// =======================
const std::vector<QAction*>& WebsocketClient::availableActions()
{
  return _actions;
}

// =======================
// Refresh OK button text/state
// =======================
void WebsocketClient::updateOkButton()
{
  if (!_dialog)
  {
    return;
  }

  bool enabled = _running && _state.mode == WsState::Mode::GetTopics && !_state.req_in_flight &&
                 _dialog->hasSelection();
  _dialog->setOkButton("Subscribe", enabled);
}

// =======================
// Start client
// =======================
bool WebsocketClient::start(QStringList*)
{
  if (_running)
  {
    return true;
  }

  WebsocketDialog dialog(_config);
  _dialog = &dialog;

  // Refresh OK button when topic selection changes
  connect(dialog.topicsWidget(), &QTreeWidget::itemSelectionChanged, this,
          &WebsocketClient::updateOkButton);

  // Connect button: toggle websocket connection
  connect(dialog.connectButton(), &QPushButton::toggled, this, [&](bool checked) {
    if (checked)
    {
      // Connect
      const QString url_str = _dialog->url();
      const QUrl url(url_str);
      if (!url.isValid())
      {
        QMessageBox::warning(&dialog, "WebSocket Client", "Invalid URL", QMessageBox::Ok);
        dialog.setConnected(false);
        return;
      }

      _url = url;
      _config.url = url_str;
      saveDefaultSettings();

      _socket.open(_url);
    }
    else
    {
      // Disconnect
      resetState();
      _socket.abort();
      _socket.close();
      _running = false;
      dialog.clearTopics();
      updateOkButton();
    }
  });

  // OK button: subscribe to selected topics
  connect(dialog.buttonBox(), &QDialogButtonBox::accepted, this, [&]() {
    if (!_running || _state.mode != WsState::Mode::GetTopics || _state.req_in_flight ||
        !dialog.hasSelection())
    {
      return;
    }

    _topics = dialog.selectedTopics();
    _config.topics = dialog.selectedTopicNames();
    _config.max_array_size = dialog.maxArraySize();
    _config.clamp_large_arrays = dialog.clampLargeArrays();
    _config.use_timestamp = dialog.useTimestamp();
    saveDefaultSettings();

    // Build JSON array
    QJsonArray arr;
    for (const auto& name : _config.topics)
    {
      arr.append(name);
    }

    _state.mode = WsState::Mode::Subscribe;
    _state.req_in_flight = true;

    QJsonObject cmd;
    cmd["command"] = "subscribe";
    cmd["topics"] = arr;

    _pending_mode = WsState::Mode::Subscribe;
    _pending_request_id = sendCommand(cmd);

    dialog.setOkButton("Subscribe", false);
    dialog.reject();
  });

  // Cancel button
  connect(dialog.buttonBox(), &QDialogButtonBox::rejected, this, [&]() {
    shutdown();
    dialog.reject();
  });

  dialog.exec();
  _dialog = nullptr;

  if (!_running)
  {
    shutdown();
    return false;
  }

  return true;
}

void WebsocketClient::shutdown()
{
  if (!_running)
  {
    return;
  }
  _running = false;
  _paused = false;

  // Reset the text of the Plotjuggler settings
  if (_action_settings)
  {
    _action_settings->setText("Pause");
  }

  resetState();

  // Close dialog if still open
  if (_dialog)
  {
    _dialog->reject();
  }
  _dialog = nullptr;

#ifdef PJ_BUILD
  // Clean data
  dataMap().clear();
  emit dataReceived();
#endif

  // Close socket
  _closing = true;
  _socket.abort();
  _socket.close();
}

bool WebsocketClient::pause()
{
  // Pause streaming on server side
  if (!_running || _state.req_in_flight)
  {
    return false;
  }

  QJsonObject cmd;
  cmd["command"] = "pause";

  return !sendCommand(cmd).isEmpty();
}

bool WebsocketClient::resume()
{
  // Resume streaming on server side
  if (!_running || _state.req_in_flight)
  {
    return false;
  }

  QJsonObject cmd;
  cmd["command"] = "resume";

  return !sendCommand(cmd).isEmpty();
}

void WebsocketClient::resetState()
{
  // Stop periodic timers
  _topics_timer.stop();
  _heartbeat_timer.stop();
  _stats_timer.stop();
  _ws_msg_count = 0;
  _topic_msg_count.clear();

  // Reset state machine
  _state.mode = WsState::Mode::Close;
  _state.req_in_flight = false;

  // Reset pending request tracking
  _pending_request_id.clear();
  _pending_mode = WsState::Mode::Close;

  // Clear topics cache
  _topics.clear();

#ifdef PJ_BUILD
  // Drop created parsers
  _parsers_topic.clear();
#endif
}

void WebsocketClient::onConnected()
{
  _running = true;

  if (_dialog)
  {
    _dialog->setConnected(true);
  }

  // First step after connect: request topics
  _state.mode = WsState::Mode::GetTopics;
  _state.req_in_flight = true;

  QJsonObject cmd;
  cmd["command"] = "get_topics";

  // Track expected response
  _pending_mode = WsState::Mode::GetTopics;
  _pending_request_id = sendCommand(cmd);

  // Start periodic topic refresh
  _topics_timer.start();
}

void WebsocketClient::onDisconnected()
{
  if (_dialog)
  {
    _dialog->clearTopics();
    _dialog->setConnected(false);
    updateOkButton();
  }

  if (!_running)
  {
    return;
  }

  if (!_dialog && !_closing)
  {
    QMessageBox::warning(nullptr, "WebSocket Client", "Server closed the connection",
                         QMessageBox::Ok);
  }

  if (_closing)
  {
    _closing = false;
  }

  resetState();

  _running = false;
}

void WebsocketClient::onError(QAbstractSocket::SocketError)
{
  // Show Qt socket error string
  QMessageBox::warning(nullptr, "WebSocket Client", _socket.errorString(), QMessageBox::Ok);
  onDisconnected();
}

void WebsocketClient::onTextMessageReceived(const QString& message)
{
  if (!_running)
  {
    return;
  }

  // Parse JSON message
  QJsonParseError err;
  const auto doc = QJsonDocument::fromJson(message.toUtf8(), &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject())
  {
    return;
  }

  const auto obj = doc.object();

  // Validate protocol version
  if (!obj.contains("protocol_version") || obj.value("protocol_version").toInt() != 1)
  {
    return;
  }

  const auto status = obj.value("status").toString();
  const auto id = obj.value("id").toString();

  // If a request is in-flight, only accept matching response "id"
  if (_state.req_in_flight)
  {
    if (_pending_request_id.isEmpty() || id != _pending_request_id)
    {
      return;
    }
  }

  // Error response from server
  if (status == "error")
  {
    _state.req_in_flight = false;

    // Reset pending request
    _pending_request_id.clear();
    _pending_mode = WsState::Mode::Close;

    const auto msg = obj.value("message").toString("Unknown error");
    QMessageBox::warning(nullptr, "WebSocket Client", msg, QMessageBox::Ok);
    return;
  }

  // Only handle successful responses
  if (status != "success")
  {
    return;
  }

  // Request completed successfully
  _state.req_in_flight = false;

  // Save mode locally, then clear pending (avoid re-entrancy issues)
  const auto handledMode = _pending_mode;
  _pending_request_id.clear();
  _pending_mode = WsState::Mode::Close;

  switch (handledMode)
  {
    case WsState::Mode::GetTopics: {
      if (!_dialog || !obj.contains("topics") || !obj.value("topics").isArray())
      {
        break;
      }

      _dialog->setTopics(obj.value("topics").toArray(), _config.topics);
      _config.topics.clear();
      updateOkButton();
      break;
    }

    case WsState::Mode::Subscribe: {
      // The server must return schemas for the accepted topics.
      // Expected format:
      // "schemas": {
      //   "/topic_a": { "name":"pkg/msg/Type", "encoding":"cdr", "definition":"..." },
      //   "/topic_b": { "name":"...", "encoding":"...", "definition":"..." }
      // }
      if (!obj.contains("schemas") || !obj.value("schemas").isObject())
      {
        _topics.clear();
#ifdef PJ_BUILD
        _parsers_topic.clear();
#endif
        break;
      }

      const auto schemas = obj.value("schemas").toObject();

      // Keep only topics that the server confirmed
      _topics.erase(std::remove_if(_topics.begin(), _topics.end(),
                                   [&](const TopicInfo& t) { return !schemas.contains(t.name); }),
                    _topics.end());

      // Fill schema fields per topic
      for (auto& t : _topics)
      {
        const auto s = schemas.value(t.name).toObject();
        t.schema_name = s.value("name").toString(t.type);
        t.schema_encoding = s.value("encoding").toString();
        t.schema_definition = s.value("definition").toString();
      }

      // Create parsers for accepted topics (PJ build only)
      createParsersForTopics();

      // Move to Data mode and start heartbeat
      _state.mode = WsState::Mode::Data;
      _topics_timer.stop();
      _heartbeat_timer.start();

      // Start debug stats collection
      _ws_msg_count = 0;
      _topic_msg_count.clear();
      _stats_elapsed.start();
      _stats_timer.start();

      break;
    }

    case WsState::Mode::Data: {
      // Text messages in data mode currently ignored
      break;
    }

    case WsState::Mode::Close:
      break;
    default:
      qWarning() << "Unhandled mode:" << int(handledMode);
      break;
  }
}

// =======================
// Binary helpers
// =======================
template <typename T>
static bool readLE(const uint8_t*& p, const uint8_t* end, T& out)
{
  // Read POD type from buffer as little-endian
  if (p + sizeof(T) > end)
  {
    return false;
  }
  std::memcpy(&out, p, sizeof(T));
  out = qFromLittleEndian(out);
  p += sizeof(T);
  return true;
}

bool WebsocketClient::parseDecompressedPayload(const QByteArray& decompressed,
                                               uint32_t expected_count)
{
  // Payload format: repeated blocks
  // [u16 topic_name_len][bytes topic_name][u64 ts_ns][u32 cdr_len][bytes cdr]
  const uint8_t* q = reinterpret_cast<const uint8_t*>(decompressed.constData());
  const uint8_t* qend = q + decompressed.size();

  uint32_t parsed = 0;

  std::lock_guard<std::mutex> lock(mutex());

  // Parse until end of payload
  while (q < qend)
  {
    uint16_t name_len = 0;
    if (!readLE(q, qend, name_len) || (q + name_len > qend))
    {
      return false;
    }

    QString topic = QString::fromUtf8(reinterpret_cast<const char*>(q), name_len);
    q += name_len;

    uint64_t ts_ns = 0;
    if (!readLE(q, qend, ts_ns))
    {
      return false;
    }
    double ts_sec = double(ts_ns) * 1e-9;

    uint32_t data_len = 0;
    if (!readLE(q, qend, data_len) || (q + data_len > qend))
    {
      return false;
    }

    // CDR buffer points inside decompressed payload
    const uint8_t* cdr = q;
    q += data_len;

    // Push message into parser / PlotJuggler
    onRos2CdrMessage(topic, ts_sec, cdr, data_len);
    parsed++;
  }

  // Header message_count must match parsed messages
  if (parsed != expected_count)
  {
    qWarning() << "Parsed messages mismatch. header=" << expected_count << "parsed=" << parsed
               << "decompressed=" << decompressed.size();
    return false;
  }

  return true;
}

void WebsocketClient::onBinaryMessageReceived(const QByteArray& message)
{
  if (!_running)
  {
    return;
  }

  // Frame header must be at least 16 bytes
  if (message.size() < 16)
  {
    return;
  }

  // Frame header fields (little-endian)
  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(message.constData());
  const uint8_t* end = ptr + message.size();

  uint32_t magic = 0;
  uint32_t message_count = 0;
  uint32_t uncompressed_size = 0;
  uint32_t flags = 0;

  if (!readLE(ptr, end, magic) || !readLE(ptr, end, message_count) ||
      !readLE(ptr, end, uncompressed_size) || !readLE(ptr, end, flags))
  {
    return;
  }

  // Validate magic and flags
  if (magic != 0x42524A50)
  {  // "PJRB"
    qWarning() << "Bad magic:" << Qt::hex << magic;
    return;
  }
  if (flags != 0)
  {
    qWarning() << "Bad flag:" << flags;
    return;
  }

  // Compressed payload starts after 16-byte header
  const size_t header_size = 16;
  const size_t compressed_size = message.size() - header_size;
  if (compressed_size == 0)
  {
    return;
  }

  // ZSTD decompress
  QByteArray decompressed;
  decompressed.resize(static_cast<int>(uncompressed_size));

  size_t res = ZSTD_decompress(decompressed.data(), size_t(decompressed.size()),
                               message.constData() + header_size, compressed_size);

  if (ZSTD_isError(res))
  {
    qWarning() << "ZSTD_decompress error:" << ZSTD_getErrorName(res);
    return;
  }

  // Resize to actual decompressed bytes
  decompressed.resize(int(res));

  // Parse messages inside payload
  parseDecompressedPayload(decompressed, message_count);
  _ws_msg_count++;

  // Notify PlotJuggler that new data is available (once per binary frame)
  emit dataReceived();
}

// =======================
// Commands / requests
// =======================
QString WebsocketClient::sendCommand(QJsonObject obj)
{
  if (_socket.state() != QAbstractSocket::ConnectedState)
  {
    return QString();
  }

  // Every command must have a "command" field
  if (!obj.contains("command"))
  {
    return QString();
  }

  // Generate unique ID if missing
  if (!obj.contains("id"))
  {
    obj["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
  }

  // Addprotocol version if missing
  if (!obj.contains("protocol_version"))
  {
    obj["protocol_version"] = 1;
  }

  // Serialize and send JSON
  QJsonDocument doc(obj);
  _socket.sendTextMessage(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));

  return obj["id"].toString();
}

void WebsocketClient::requestTopics()
{
  if (_socket.state() != QAbstractSocket::ConnectedState)
  {
    return;
  }

  // Only poll when connected and idle
  if (!_running || (_state.mode != WsState::Mode::GetTopics) || _state.req_in_flight)
  {
    return;
  }

  _state.req_in_flight = true;

  QJsonObject cmd;
  cmd["command"] = "get_topics";

  // Track expected response
  _pending_mode = WsState::Mode::GetTopics;
  _pending_request_id = sendCommand(cmd);
}

void WebsocketClient::sendHeartBeat()
{
  if (_socket.state() != QAbstractSocket::ConnectedState || !_running ||
      (_state.mode != WsState::Mode::Data))
  {
    return;
  }

  // Keep-alive / watchdog on server side
  QJsonObject cmd;
  cmd["command"] = "heartbeat";
  sendCommand(cmd);
}

// =======================
// PlotJuggler integration
// =======================
void WebsocketClient::createParsersForTopics()
{
#ifdef PJ_BUILD
  // Create one parser per subscribed topic using PlotJuggler factories
  for (const auto& t : _topics)
  {
    // Already created
    if (_parsers_topic.contains(t.name))
    {
      continue;
    }

    // Find parser factory by encoding
    auto factories = parserFactories();
    auto it = factories->find(t.schema_encoding);
    if (it == factories->end())
    {
      // Warn only once per encoding
      static std::set<QString> warned;
      if (warned.insert(t.schema_encoding).second)
      {
        QMessageBox::warning(
            nullptr, "Encoding problem",
            QString("No parser available for encoding [%0]").arg(t.schema_encoding));
      }
      continue;
    }

    // Create parser instance
    PJ::MessageParserPtr parser =
        it->second->createParser(t.name.toStdString(), t.schema_name.toStdString(),
                                 t.schema_definition.toStdString(), dataMap());

    if (!parser)
    {
      continue;
    }

    parser->setLargeArraysPolicy(_config.clamp_large_arrays, _config.max_array_size);
    parser->enableEmbeddedTimestamp(_config.use_timestamp);
    _parsers_topic.insert(t.name, std::move(parser));
  }
#endif
}

void WebsocketClient::onRos2CdrMessage(const QString& topic, double ts_sec, const uint8_t* cdr,
                                       uint32_t len)
{
#ifdef PJ_BUILD
  // Route CDR blob to the parser created for this topic
  auto it = _parsers_topic.find(topic);
  if (it == _parsers_topic.end())
  {
    return;
  }

  PJ::MessageRef msg_ref(cdr, len);
  try
  {
    it.value()->parseMessage(msg_ref, ts_sec);
    _topic_msg_count[topic]++;
  }
  catch (std::exception& err)
  {
    QMessageBox::warning(nullptr, tr("WebSocket Client"),
                         tr("Problem parsing the message. WebSocket Client will be "
                            "stopped.\n%1")
                             .arg(err.what()),
                         QMessageBox::Ok);
    shutdown();
    emit closed();
    return;
  }
#else
  // Debug build: just log reception
  Q_UNUSED(cdr);
  Q_UNUSED(len);
  qDebug() << "RX msg topic=" << topic << "ts=" << ts_sec << "cdr=" << len;
#endif
}

// =======================
// Debug stats
// =======================
void WebsocketClient::printStats()
{
  double elapsed_sec = _stats_elapsed.elapsed() / 1000.0;
  if (elapsed_sec < 0.001)
  {
    return;
  }

  qDebug() << "=== WS Bridge Stats ===";
  qDebug() << "  WS frames received:" << _ws_msg_count << "(" << (_ws_msg_count / elapsed_sec)
           << "/sec)";

  for (auto it = _topic_msg_count.constBegin(); it != _topic_msg_count.constEnd(); ++it)
  {
    qDebug() << "  " << it.key() << ":" << it.value() << "(" << (it.value() / elapsed_sec)
             << "/sec)";
  }

  _ws_msg_count = 0;
  _topic_msg_count.clear();
  _stats_elapsed.restart();
}

// =======================
// PlotJuggler profiles
// =======================
void WebsocketClient::saveDefaultSettings()
{
  QSettings s;
  _config.saveToSettings(s, "WebsocketClient");
}

void WebsocketClient::loadDefaultSettings()
{
  QSettings s;
  _config.loadFromSettings(s, "WebsocketClient");
}

bool WebsocketClient::xmlSaveState(QDomDocument& doc, QDomElement& parent) const
{
  _config.xmlSaveState(doc, parent);
  return true;
}

bool WebsocketClient::xmlLoadState(const QDomElement& parent)
{
  _config.xmlLoadState(parent);
  return true;
}
