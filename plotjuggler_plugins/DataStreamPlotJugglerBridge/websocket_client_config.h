#ifndef WEBSOCKET_CLIENT_CONFIG_H
#define WEBSOCKET_CLIENT_CONFIG_H

#include <QString>
#include <QStringList>
#include <QSettings>
#include <QDomDocument>

class WebsocketClientConfig
{
public:
  // =========================
  // Persisted fields
  // =========================
  QString url = "ws://127.0.0.1:9090";
  QStringList topics;
  unsigned max_array_size = 500;
  bool clamp_large_arrays = false;
  bool use_timestamp = false;

  WebsocketClientConfig();

  // =========================
  // XML (PlotJuggler layout)
  // =========================
  void xmlSaveState(QDomDocument& doc, QDomElement& plugin_elem) const;
  void xmlLoadState(const QDomElement& parent_element);

  // =========================
  // QSettings (global defaults)
  // =========================
  void saveToSettings(QSettings& settings, const QString& group) const;
  void loadFromSettings(const QSettings& settings, const QString& group);
};

#endif  // WEBSOCKET_CLIENT_CONFIG_H
