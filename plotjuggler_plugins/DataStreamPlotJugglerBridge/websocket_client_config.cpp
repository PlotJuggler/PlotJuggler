#include "websocket_client_config.h"

WebsocketClientConfig::WebsocketClientConfig() = default;

// =========================
// XML (PlotJuggler layout)
// =========================
void WebsocketClientConfig::xmlSaveState(QDomDocument& doc, QDomElement& plugin_elem) const
{
  QDomElement cfg = doc.createElement("websocket_client");
  plugin_elem.appendChild(cfg);

  cfg.setAttribute("url", url);
  cfg.setAttribute("max_array_size", max_array_size);
  cfg.setAttribute("clamp_large_arrays", int(clamp_large_arrays));
  cfg.setAttribute("use_timestamp", int(use_timestamp));

  QDomElement topics_elem = doc.createElement("topics");
  cfg.appendChild(topics_elem);

  for (const auto& topic : topics)
  {
    QDomElement t = doc.createElement("topic");
    t.setAttribute("name", topic);
    topics_elem.appendChild(t);
  }
}

void WebsocketClientConfig::xmlLoadState(const QDomElement& parent_element)
{
  QDomElement cfg = parent_element.firstChildElement("websocket_client");
  if (cfg.isNull())
  {
    return;
  }

  url = cfg.attribute("url", "ws://127.0.0.1:9090");
  max_array_size = cfg.attribute("max_array_size", "500").toUInt();
  clamp_large_arrays = bool(cfg.attribute("clamp_large_arrays", "0").toInt());
  use_timestamp = bool(cfg.attribute("use_timestamp", "0").toInt());

  topics.clear();

  QDomElement topics_elem = cfg.firstChildElement("topics");
  for (QDomElement t = topics_elem.firstChildElement("topic"); !t.isNull();
       t = t.nextSiblingElement("topic"))
  {
    QString name = t.attribute("name");
    if (!name.isEmpty())
    {
      topics.push_back(name);
    }
  }
}

// =========================
// QSettings (global defaults)
// =========================
void WebsocketClientConfig::saveToSettings(QSettings& settings, const QString& group) const
{
  settings.setValue(group + "/url", url);
  settings.setValue(group + "/topics", topics);
  settings.setValue(group + "/max_array_size", max_array_size);
  settings.setValue(group + "/clamp_large_arrays", clamp_large_arrays);
  settings.setValue(group + "/use_timestamp", use_timestamp);
}

void WebsocketClientConfig::loadFromSettings(const QSettings& settings, const QString& group)
{
  url = settings.value(group + "/url", "ws://127.0.0.1:9090").toString();
  topics = settings.value(group + "/topics").toStringList();
  max_array_size = settings.value(group + "/max_array_size", 500).toUInt();
  clamp_large_arrays = settings.value(group + "/clamp_large_arrays", false).toBool();
  use_timestamp = settings.value(group + "/use_timestamp", false).toBool();
}
