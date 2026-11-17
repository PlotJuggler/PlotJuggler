#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QString>
#include <QStringList>

class QAction;
class QMenu;
class QWidget;

namespace PJ {

// Persistent MRU list rendered as a popup menu. enabledChanged() fires only on empty/non-empty transitions and not from
// the constructor: callers seed the initial enabled state with isEmpty().
class RecentFilesMenu : public QObject {
  Q_OBJECT
 public:
  RecentFilesMenu(QString settings_key, int max_entries, QWidget* parent = nullptr);

  void record(const QString& path);

  void clear();

  void popupAt(QPoint global_pos);

  bool isEmpty() const;

 signals:
  void activated(const QString& path);
  void enabledChanged(bool enabled);

 private:
  void rebuildMenu();
  void persist() const;

  const QString kSettingsKey;
  const int kMaxEntries;
  QStringList files_;
  QMenu* menu_ = nullptr;
  QAction* clear_action_ = nullptr;
};

}  // namespace PJ
