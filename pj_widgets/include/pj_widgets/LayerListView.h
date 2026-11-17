#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QListWidgetItem>
#include <QString>
#include <QWidget>
#include <QtGlobal>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

class QListWidget;

namespace PJ {

struct LayerRow {
  qint64 id;
  QString name;
  bool visible = true;
  // The warning state travels with the row, so setRows() is authoritative (it also survives drag-reorder);
  // setRowWarning() mutates it live.
  bool warn = false;
  QString warning_reason = {};
  /// A short state shown after the name, never a fault (e.g. "behind 0.4 s"); empty for none.
  QString status = {};
};

class LayerListView : public QWidget {
  Q_OBJECT
 public:
  explicit LayerListView(QWidget* parent = nullptr);

  void setRows(const std::vector<LayerRow>& rows);
  void addRow(const LayerRow& row);
  void removeRow(qint64 id);
  /// Re-sequences existing rows to an order the owner decided, keeping each row's warning and the selection (none stays
  /// none). Unknown ids are skipped. Does not emit reordered(), which reports user drags only.
  void setOrder(const std::vector<qint64>& ordered_ids);
  void clearRows();
  void setRowVisible(qint64 id, bool visible);
  void setRowName(qint64 id, const QString& name);
  void setRowWarning(qint64 id, bool warn, const QString& reason = {});
  void setRowStatus(qint64 id, const QString& status);
  void setCurrentId(qint64 id);
  [[nodiscard]] std::optional<qint64> currentId() const;
  [[nodiscard]] std::vector<qint64> order() const;
  void setTheme(const QString& theme);

 signals:
  void selectionChanged();
  void visibilityToggled(qint64 id, bool visible);
  void removeRequested(qint64 id);
  void reordered(const std::vector<qint64>& ordered_ids);

 private:
  void installRowWidget(QListWidgetItem* item, const LayerRow& row);
  void rebuildFromOrder(const std::vector<qint64>& ordered_ids, qint64 select_id);
  void onRowMoved(int from, int to);
  // The height tracks the row count: a 4-row floor growing to a 6-row cap (scrollbar beyond). Called by every row
  // mutation.
  void updateMinimumHeight();

  QListWidget* list_ = nullptr;
  std::unordered_map<qint64, QListWidgetItem*> items_;
  QString current_theme_;
};

namespace detail {

// The reorder arithmetic shared by LayerListView::onRowMoved and its tests: moves the element at `from` to the drop
// position `to` (interpreted against the pre-removal list, as the drop indicator is). `ids` unchanged when `from` is
// out of range.
[[nodiscard]] std::vector<qint64> reorderIds(std::vector<qint64> ids, int from, int to);

}  // namespace detail

}  // namespace PJ
