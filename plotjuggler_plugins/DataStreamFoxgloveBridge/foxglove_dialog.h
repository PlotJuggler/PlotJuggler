#pragma once

#include <QDialog>
#include <QDialogButtonBox>
#include <QMap>
#include <QPushButton>
#include <QTreeWidget>

#include "foxglove_client_config.h"

struct FoxgloveChannelInfo
{
  quint64 id = 0;
  QString topic;
  QString encoding;
  /// ROS interface name such as sensor_msgs/msg/Imu.
  QString schema_name;
  /// Raw schema payload as advertised by the bridge.
  QString schema;
  QString schema_encoding;
};

namespace Ui
{
class FoxgloveDialog;
}

class FoxgloveDialog : public QDialog
{
  Q_OBJECT

public:
  explicit FoxgloveDialog(const FoxgloveClientConfig& config);
  ~FoxgloveDialog();

  /// Current connection target entered by the user.
  QString url() const;

  /// Replace the visible channel list while preserving current selections when possible.
  void setChannels(const QMap<quint64, FoxgloveChannelInfo>& channels,
                   const QStringList& preselectNames);
  void clearChannels();

  bool hasSelection() const;
  QStringList selectedTopicNames() const;

  /// Return the selected Foxglove channel descriptors, including channel IDs.
  std::vector<FoxgloveChannelInfo> selectedChannels() const;

  void setOkButton(const QString& text, bool enabled);
  /// Toggle UI elements to reflect socket connection state.
  void setConnected(bool connected);

  unsigned maxArraySize() const;
  bool clampLargeArrays() const;
  bool useTimestamp() const;

  QDialogButtonBox* buttonBox() const;
  QTreeWidget* topicsWidget() const;
  QPushButton* connectButton() const;

private slots:
  /// Hide rows that do not match the current text filter.
  void applyFilter(const QString& filter);

private:
  Ui::FoxgloveDialog* ui;
  QMap<quint64, FoxgloveChannelInfo> _channels;
};
