#ifndef FOXGLOVE_CLIENT_CONFIG_H
#define FOXGLOVE_CLIENT_CONFIG_H

#include <QDomDocument>
#include <QSettings>
#include <QString>
#include <QStringList>

class FoxgloveClientConfig
{
public:
  /// Persisted connection target used by the dialog and profile state.
  QString url = "ws://127.0.0.1:8765";
  QStringList topics;
  unsigned max_array_size = 500;
  bool clamp_large_arrays = false;
  bool use_timestamp = false;

  FoxgloveClientConfig();

  /// Save per-layout state inside a PlotJuggler profile.
  void xmlSaveState(QDomDocument& doc, QDomElement& plugin_elem) const;
  void xmlLoadState(const QDomElement& parent_element);

  /// Save global defaults shared across sessions.
  void saveToSettings(QSettings& settings, const QString& group) const;
  void loadFromSettings(const QSettings& settings, const QString& group);
};

#endif  // FOXGLOVE_CLIENT_CONFIG_H
