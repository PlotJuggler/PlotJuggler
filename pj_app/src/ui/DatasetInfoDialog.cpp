// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "ui/DatasetInfoDialog.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTreeWidgetItem>

#include "ui_DatasetInfoDialog.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

QString scalarText(const QJsonValue& value) {
  switch (value.type()) {
    case QJsonValue::Bool:
      return value.toBool() ? u"true"_s : u"false"_s;
    case QJsonValue::Double: {
      // Qt's JSON parser keeps integral values as qint64, so nanosecond
      // timestamps print exactly; going through toDouble() would round past
      // 2^53. QVariant preserves the stored representation.
      const QVariant variant = value.toVariant();
      if (variant.typeId() == QMetaType::LongLong || variant.typeId() == QMetaType::ULongLong ||
          variant.typeId() == QMetaType::Int) {
        return variant.toString();
      }
      return QString::number(value.toDouble());
    }
    case QJsonValue::String:
      return value.toString();
    case QJsonValue::Null:
      return u"null"_s;
    default:
      return {};
  }
}

// Renders a JSON value under `parent` as generic key/value rows: objects and
// arrays become expandable nodes, scalars become leaf values. Deliberately
// interpretation-free — provider-specific keys are shown verbatim.
void addJsonChildren(QTreeWidgetItem* parent, const QJsonValue& value) {
  if (value.isObject()) {
    const QJsonObject object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it) {
      auto* row = new QTreeWidgetItem(parent);
      row->setText(0, it.key());
      if (it.value().isObject() || it.value().isArray()) {
        addJsonChildren(row, it.value());
      } else {
        row->setText(1, scalarText(it.value()));
      }
    }
    return;
  }
  if (value.isArray()) {
    const QJsonArray array = value.toArray();
    for (qsizetype i = 0; i < array.size(); ++i) {
      auto* row = new QTreeWidgetItem(parent);
      row->setText(0, u"[%1]"_s.arg(i));
      if (array[i].isObject() || array[i].isArray()) {
        addJsonChildren(row, array[i]);
      } else {
        row->setText(1, scalarText(array[i]));
      }
    }
  }
}

QTreeWidgetItem* addRow(QTreeWidget* tree, const QString& key, const QString& value) {
  auto* row = new QTreeWidgetItem(tree);
  row->setText(0, key);
  row->setText(1, value);
  return row;
}

// Adds a top-level section rendering `json` as a generic key/value tree, with
// the raw text as fallback when it does not parse (still information — show it).
void addJsonSection(QTreeWidget* tree, const QString& title, const QString& json) {
  auto* section = new QTreeWidgetItem(tree);
  section->setText(0, title);
  QJsonParseError parse_error{};
  const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &parse_error);
  if (parse_error.error == QJsonParseError::NoError && (doc.isObject() || doc.isArray())) {
    addJsonChildren(section, doc.isObject() ? QJsonValue(doc.object()) : QJsonValue(doc.array()));
  } else {
    section->setText(1, json);
  }
}

}  // namespace

DatasetInfoDialog::DatasetInfoDialog(
    const QString& dataset_name, const QString& source_path, const SourceRecord* record,
    const DatasetMetadata* metadata, QWidget* parent)
    : Dialog(parent), ui_(new Ui::DatasetInfoDialog) {
  setDialogTitle(tr("Dataset Info"));
  setAttribute(Qt::WA_DeleteOnClose, true);
  setMinimumSize(560, 420);

  auto* body = new QWidget;
  ui_->setupUi(body);
  contentLayout()->addWidget(body);

  QTreeWidget* tree = ui_->treeInfo;
  addRow(tree, tr("Name"), dataset_name);
  addRow(tree, tr("File"), source_path.isEmpty() ? u"—"_s : source_path);

  if (record != nullptr && !record->provider_id.isEmpty()) {
    auto* provenance = new QTreeWidgetItem(tree);
    provenance->setText(0, tr("Provenance"));
    auto add_child = [&](const QString& key, const QString& value) {
      auto* row = new QTreeWidgetItem(provenance);
      row->setText(0, key);
      row->setText(1, value);
      return row;
    };
    add_child(tr("Provider"), record->provider_id);
    add_child(tr("Identity"), record->source_identity);

    QJsonParseError parse_error{};
    const QJsonDocument doc = QJsonDocument::fromJson(record->descriptor_json.toUtf8(), &parse_error);
    if (parse_error.error == QJsonParseError::NoError && (doc.isObject() || doc.isArray())) {
      auto* descriptor = new QTreeWidgetItem(provenance);
      descriptor->setText(0, tr("Descriptor"));
      addJsonChildren(descriptor, doc.isObject() ? QJsonValue(doc.object()) : QJsonValue(doc.array()));
    } else if (!record->descriptor_json.isEmpty()) {
      // Unparseable descriptors are still provenance — show the raw bytes.
      add_child(tr("Descriptor"), record->descriptor_json);
    }
  }

  if (metadata != nullptr && !metadata->json.isEmpty()) {
    const QString title =
        metadata->plugin_id.isEmpty() ? tr("Loader metadata") : tr("Loader metadata (%1)").arg(metadata->plugin_id);
    addJsonSection(tree, title, metadata->json);
  }

  tree->expandAll();
  tree->resizeColumnToContents(0);

  if (parent != nullptr) {
    const QPoint centre = parent->mapToGlobal(parent->rect().center());
    move(centre.x() - width() / 2, centre.y() - height() / 2);
  }
}

DatasetInfoDialog::~DatasetInfoDialog() {
  delete ui_;
}

}  // namespace PJ
