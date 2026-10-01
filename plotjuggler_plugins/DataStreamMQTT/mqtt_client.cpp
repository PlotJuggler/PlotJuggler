#include "mqtt_client.h"
#include <QDebug>
#include <QMessageBox>
#include <QString>
#include <QtGlobal>
#include <QLoggingCategory>

#ifdef WIN32
#include <windows.h>
#include <strsafe.h>
#endif

#define MQTT_DEBUG 0
#define debug() qCDebug(category)

static const QLoggingCategory category("MQTTClient");

void connect_callback(struct mosquitto* mosq, void* context, int result, int,
                      const mosquitto_property*)
{
  MQTTClient* self = static_cast<MQTTClient*>(context);

  if (!result)
  {
    for (const auto& topic : self->config().topics)
    {
      mosquitto_subscribe(mosq, nullptr, topic.c_str(), self->config().qos);
    }
  }
  else
  {
    QMessageBox::warning(nullptr, "MQTT Client",
                         QString("Connection error: %1").arg(mosquitto_reason_string(result)),
                         QMessageBox::Ok);
  }
  self->_connected = true;
}

void disconnect_callback(struct mosquitto* mosq, void* context, int result)
{
  MQTTClient* self = static_cast<MQTTClient*>(context);

  if (self->isConnected() && result == MOSQ_ERR_CONN_LOST)
  {
    emit self->disconnected();
  }
}

void message_callback(struct mosquitto* mosq, void* context,
                      const struct mosquitto_message* message, const mosquitto_property*)
{
  MQTTClient* self = static_cast<MQTTClient*>(context);
  self->onMessageReceived(message);
}

void log_callback(struct mosquitto* mosq, void* context, int log_level, const char* msg)
{
  const std::pair<int, const char*> log_level_map[] = {
    { MOSQ_LOG_INFO, "MOSQ_LOG_INFO" },
    { MOSQ_LOG_NOTICE, "MOSQ_LOG_NOTICE" },
    { MOSQ_LOG_WARNING, "MOSQ_LOG_WARNING" },
    { MOSQ_LOG_ERR, "MOSQ_LOG_ERR			" },
    { MOSQ_LOG_DEBUG, "MOSQ_LOG_DEBUG" },
    { MOSQ_LOG_SUBSCRIBE, "MOSQ_LOG_SUBSCRIBE" },
    { MOSQ_LOG_UNSUBSCRIBE, "MOSQ_LOG_UNSUBSCRIBE" },
    { MOSQ_LOG_WEBSOCKETS, "MOSQ_LOG_WEBSOCKETS" },
  };

  const auto it = std::find_if(std::begin(log_level_map), std::end(log_level_map),
                               [log_level](const auto& pair) { return log_level == pair.first; });
  if (it == std::end(log_level_map))
  {
    return;
  }

  debug() << it->second << ": " << msg;
}

//----------------------------

MQTTClient::MQTTClient()
{
  mosquitto_lib_init();

#if MQTT_DEBUG
  int major = 0, minor = 0, revision = 0;
  mosquitto_lib_version(&major, &minor, &revision);
  debug() << "mosquitto version: " << major << "." << minor << "." << revision;
#endif  // MQTT_DEBUG
}

MQTTClient::~MQTTClient()
{
  if (_connected)
  {
    disconnect();
  }
  mosquitto_lib_cleanup();
}

bool MQTTClient::connect(const MosquittoConfig& config)
{
  if (_connected)
  {
    disconnect();
  }

  // Start with a fresh mosquitto instance.
  Q_ASSERT(_mosq == nullptr);
  _mosq = mosquitto_new(nullptr, true, this);

  _config = config;
  bool success = configureMosquitto(config);
  if (!success)
  {
    mosquitto_destroy(_mosq);
    _mosq = nullptr;
    return false;
  }

  _connected = true;
  return true;
}

bool MQTTClient::configureMosquitto(const MosquittoConfig& config)
{
  mosquitto_connect_v5_callback_set(_mosq, connect_callback);
  mosquitto_disconnect_callback_set(_mosq, disconnect_callback);
  mosquitto_message_v5_callback_set(_mosq, message_callback);
#if MQTT_DEBUG
  mosquitto_log_callback_set(_mosq, log_callback);
#endif

  int rc = mosquitto_int_option(_mosq, MOSQ_OPT_PROTOCOL_VERSION, config.protocol_version);
  if (rc != MOSQ_ERR_SUCCESS)
  {
    QMessageBox::warning(nullptr, "MQTT Client", QString("MQTT initialization failed."),
                         QMessageBox::Ok);
    debug() << "MQTT initialization failed:" << mosquitto_strerror(rc);
    return false;
  }

  if ((!config.username.empty() || !config.password.empty()))
  {
    rc = mosquitto_username_pw_set(_mosq, config.username.c_str(), config.password.c_str());
    if (rc != MOSQ_ERR_SUCCESS)
    {
      QMessageBox::warning(nullptr, "MQTT Client",
                           QString("MQTT initialization failed. Double check username "
                                   "and password."),
                           QMessageBox::Ok);
      debug() << "MQTT username or password error:" << mosquitto_strerror(rc);
      return false;
    }
  }

  if (config.cafile.empty() == false)
  {
    const char* cafile = config.cafile.c_str();
    const char* certfile = config.certfile.empty() ? nullptr : config.certfile.c_str();
    const char* keyfile = config.keyfile.empty() ? nullptr : config.keyfile.c_str();
    rc = mosquitto_tls_set(_mosq, cafile, nullptr, certfile, keyfile, nullptr);
    if (rc != MOSQ_ERR_SUCCESS)
    {
      QMessageBox::warning(nullptr, "MQTT Client",
                           QString("MQTT initialization failed. Double check "
                                   "certificates."),
                           QMessageBox::Ok);
      debug() << "MQTT certificate error:" << mosquitto_strerror(rc);
      return false;
    }
  }

  rc = mosquitto_max_inflight_messages_set(_mosq, config.max_inflight);
  if (rc != MOSQ_ERR_SUCCESS)
  {
    QMessageBox::warning(nullptr, "MQTT Client", QString("MQTT initialization failed."),
                         QMessageBox::Ok);
    debug() << "MQTT setting max inflight messages failed:" << mosquitto_strerror(rc);
    return false;
  }

  const mosquitto_property* properties = nullptr;  // todo

  rc = mosquitto_connect_bind_v5(_mosq, config.host.c_str(), config.port, config.keepalive, nullptr,
                                 properties);
  // TODO bind
  if (rc > 0)
  {
    if (rc == MOSQ_ERR_ERRNO)
    {
      char err[1024];
#ifndef WIN32
      auto ret = strerror_r(errno, err, 1024);
#else
      FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, errno, 0, (LPTSTR)&err, 1024, NULL);
#endif
      QMessageBox::warning(nullptr, "MQTT Client", QString("Error: %1").arg(err), QMessageBox::Ok);
    }
    else
    {
      QMessageBox::warning(nullptr, "MQTT Client",
                           QString("Unable to connect (%1)").arg(mosquitto_strerror(rc)),
                           QMessageBox::Ok);
    }
    debug() << "MQTT connect failed:" << mosquitto_strerror(rc);
    return false;
  }

  rc = mosquitto_loop_start(_mosq);
  if (rc == MOSQ_ERR_NOT_SUPPORTED)
  {
    // Threaded mode may not be supported on windows (libmosquitto < 2.1).
    // See https://github.com/eclipse/mosquitto/issues/2707
    _thread = std::thread([this]() {
      int rc = mosquitto_loop_forever(this->_mosq, -1, 1);
      if (rc != MOSQ_ERR_SUCCESS)
      {
        debug() << "MQTT loop forever failed:" << mosquitto_strerror(rc);
      }
    });
  }
  else if (rc != MOSQ_ERR_SUCCESS)
  {
    QMessageBox::warning(nullptr, "MQTT Client", QString("Failed to start MQTT client"),
                         QMessageBox::Ok);
    debug() << "MQTT start loop failed:" << mosquitto_strerror(rc);
    return false;
  }
  return true;
}

void MQTTClient::disconnect()
{
  if (_connected)
  {
    mosquitto_disconnect(_mosq);
    mosquitto_loop_stop(_mosq, true);
    if (_thread.joinable())
    {
      _thread.join();
    }
    mosquitto_destroy(_mosq);
    _mosq = nullptr;
  }
  _connected = false;
  _topics_set.clear();
  _message_callbacks.clear();
}

bool MQTTClient::isConnected() const
{
  return _connected;
}

void MQTTClient::addMessageCallback(const std::string& topic, MQTTClient::TopicCallback callback)
{
  std::unique_lock<std::mutex> lk(_mutex);
  _message_callbacks[topic] = callback;
}

void MQTTClient::onMessageReceived(const mosquitto_message* message)
{
  std::unique_lock<std::mutex> lk(_mutex);

  _topics_set.insert(message->topic);

  auto it = _message_callbacks.find(message->topic);
  if (it != _message_callbacks.end())
  {
    it->second(message);
  }
}

const MosquittoConfig& MQTTClient::config() const
{
  return _config;
}

std::unordered_set<std::string> MQTTClient::getTopicList()
{
  std::unique_lock<std::mutex> lk(_mutex);
  return _topics_set;
}

void MQTTClient::subscribe(const std::string& topic, int qos)
{
  if (_connected)
  {
    mosquitto_subscribe(_mosq, nullptr, topic.c_str(), qos);
  }
}

void MQTTClient::unsubscribe(const std::string& topic)
{
  if (_connected)
  {
    mosquitto_unsubscribe(_mosq, nullptr, topic.c_str());
  }
}
