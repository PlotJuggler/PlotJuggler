#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QApplication>
#include <QDataStream>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QWidget>
#include <cstdint>
#include <functional>
#include <pj_plugins/host/widget_event_builder.hpp>
#include <pj_plugins/host_qt/widget_binding.hpp>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace PJ {

/// Single event filter installed on the dialog root that handles drag-and-drop
/// of PJ fields. Registered widgets are tracked by objectName. When a drop
/// lands on (or inside) a registered widget, the callback fires an itemsDropped event.
class DropEventFilter : public QObject {
  Q_OBJECT

 public:
  DropEventFilter(QWidget* dialog_root, WidgetEventCallback callback)
      : QObject(dialog_root), root_(dialog_root), callback_(std::move(callback)) {
    root_->setAcceptDrops(true);
    root_->installEventFilter(this);
  }

  void addTarget(const std::string& widget_name) {
    targets_.insert(widget_name);
  }

  /// Optional: map each dragged catalog key to a topic or series name before it is
  /// delivered to the plugin. Keys that resolve to empty are kept verbatim.
  void setKeyResolver(std::function<std::string(const std::string&)> resolver) {
    key_resolver_ = std::move(resolver);
  }

  /// Optional: map each dragged catalog key to its data source id, which the plugin
  /// reads as DroppedItem::data_source_id (0 = unknown). Names alone cannot tell two
  /// datasets' same-named series apart.
  void setKeyDataSourceResolver(std::function<uint32_t(const std::string&)> resolver) {
    key_data_source_resolver_ = std::move(resolver);
  }

 protected:
  bool eventFilter(QObject* /*obj*/, QEvent* event) override {
    if (event->type() == QEvent::DragEnter) {
      auto* e = static_cast<QDragEnterEvent*>(event);
      if (e->mimeData()->hasFormat(kCatalogItemsMime)) {
        e->acceptProposedAction();
        return true;
      }
    } else if (event->type() == QEvent::DragMove) {
      auto* e = static_cast<QDragMoveEvent*>(event);
      if (e->mimeData()->hasFormat(kCatalogItemsMime)) {
        // Only accept if the cursor is over a registered target.
        if (findTargetAt(e->position().toPoint())) {
          e->acceptProposedAction();
        } else {
          e->ignore();
        }
        return true;
      }
    } else if (event->type() == QEvent::Drop) {
      auto* e = static_cast<QDropEvent*>(event);
      auto* target_name = findTargetAt(e->position().toPoint());
      if (target_name && e->mimeData()->hasFormat(kCatalogItemsMime)) {
        auto labels = parseMime(e->mimeData()->data(kCatalogItemsMime));
        std::vector<uint32_t> data_sources;
        if (key_data_source_resolver_) {
          data_sources.reserve(labels.size());
          for (const auto& label : labels) {
            data_sources.push_back(key_data_source_resolver_(label));
          }
        }
        // The PJ4 curve tree drags opaque catalog keys; resolve each to a name
        // the plugin can interpret. Unresolved keys pass through verbatim.
        if (key_resolver_) {
          for (auto& label : labels) {
            if (std::string name = key_resolver_(label); !name.empty()) {
              label = std::move(name);
            }
          }
        }
        if (!labels.empty()) {
          callback_(
              *target_name, key_data_source_resolver_ ? WidgetEventBuilder::itemsDropped(labels, data_sources)
                                                      : WidgetEventBuilder::itemsDropped(labels));
          e->acceptProposedAction();
          return true;
        }
      }
    }
    return false;
  }

 private:
  // The PJ4 curve tree publishes dragged catalog entries under this MIME type
  // (CurveTreeView::catalogItemsMimeType): a QDataStream sequence of the selected
  // catalog keys (curve/field paths), with no count prefix.
  static constexpr const char* kCatalogItemsMime = "plotjuggler/catalog-items";

  QWidget* root_;
  WidgetEventCallback callback_;
  std::set<std::string> targets_;
  std::function<std::string(const std::string&)> key_resolver_;
  std::function<uint32_t(const std::string&)> key_data_source_resolver_;

  /// Walk up from the widget at pos to find a registered drop target.
  const std::string* findTargetAt(QPoint pos) const {
    auto* w = root_->childAt(pos);
    while (w && w != root_) {
      auto name = w->objectName().toStdString();
      auto it = targets_.find(name);
      if (it != targets_.end()) {
        return &(*it);
      }
      w = w->parentWidget();
    }
    return nullptr;
  }

  static std::vector<std::string> parseMime(const QByteArray& data) {
    // Matches CurveTreeView::encodeCatalogKeys: a bare sequence of QString keys.
    QDataStream stream(data);
    std::vector<std::string> labels;
    while (!stream.atEnd()) {
      QString key;
      stream >> key;
      if (stream.status() != QDataStream::Ok) {
        break;
      }
      if (!key.isEmpty()) {
        labels.push_back(key.toStdString());
      }
    }
    return labels;
  }
};

}  // namespace PJ
