// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The dataset "Info" dialog must render provenance domain-neutrally: the
// dataset name and backing file as plain rows, and — when a SourceRecord is
// present — provider id, source identity, and the descriptor JSON as a generic
// key/value tree (nested objects become expandable nodes, integers print
// without a decimal point). Without a record the provenance section is absent.

#include <gtest/gtest.h>

#include <QApplication>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include "pj_runtime/SessionManager.h"
#include "support/gui_test_env.h"
#include "ui/DatasetInfoDialog.h"
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

// The organization name sandboxes QSettings reads away from real preferences;
// only this suite may select that settings scope.
struct QtEnvironment : ::testing::Environment {
  void SetUp() override {
    if (!pj_app_test::isTestSuiteSelected("DatasetInfoDialog")) {
      return;
    }
    QCoreApplication::setOrganizationName(u"PJ4DatasetInfoDialogTest"_s);
    QCoreApplication::setApplicationName(u"PJ4DatasetInfoDialogTest"_s);
  }
};
static auto* const kEnv = ::testing::AddGlobalTestEnvironment(new QtEnvironment);

QTreeWidgetItem* childNamed(const QTreeWidgetItem* parent, const QString& name) {
  for (int i = 0; i < parent->childCount(); ++i) {
    if (parent->child(i)->text(0) == name) {
      return parent->child(i);
    }
  }
  return nullptr;
}

QTreeWidgetItem* topLevelNamed(const QTreeWidget* tree, const QString& name) {
  for (int i = 0; i < tree->topLevelItemCount(); ++i) {
    if (tree->topLevelItem(i)->text(0) == name) {
      return tree->topLevelItem(i);
    }
  }
  return nullptr;
}

TEST(DatasetInfoDialog, RendersProvenanceAsGenericTree) {
  SourceRecord record;
  record.provider_id = u"mcap_cloud"_s;
  record.source_identity = u"30969b57c176ca6ae878660db15ea555"_s;
  record.descriptor_json =
      uR"({"kind":"mosaico.pull","v":1,"request":{"origin":"demo.example.org:6726","start_ns":1461159829761465942}})"_s;

  DatasetMetadata metadata;
  metadata.plugin_id = u"data_load_mcap"_s;
  metadata.json = uR"({"file": {"message_count": 120}})"_s;

  DatasetInfoDialog dialog(u"run42.mcap"_s, u"/cache/30969b.mcap"_s, &record, &metadata);
  const auto* tree = dialog.findChild<QTreeWidget*>(u"treeInfo"_s);
  ASSERT_NE(tree, nullptr);

  const QTreeWidgetItem* name_row = topLevelNamed(tree, u"Name"_s);
  ASSERT_NE(name_row, nullptr);
  EXPECT_EQ(name_row->text(1), u"run42.mcap"_s);
  const QTreeWidgetItem* file_row = topLevelNamed(tree, u"File"_s);
  ASSERT_NE(file_row, nullptr);
  EXPECT_EQ(file_row->text(1), u"/cache/30969b.mcap"_s);

  const QTreeWidgetItem* provenance = topLevelNamed(tree, u"Provenance"_s);
  ASSERT_NE(provenance, nullptr);
  EXPECT_EQ(childNamed(provenance, u"Provider"_s)->text(1), u"mcap_cloud"_s);
  EXPECT_EQ(childNamed(provenance, u"Identity"_s)->text(1), u"30969b57c176ca6ae878660db15ea555"_s);

  const QTreeWidgetItem* descriptor = childNamed(provenance, u"Descriptor"_s);
  ASSERT_NE(descriptor, nullptr);
  EXPECT_EQ(childNamed(descriptor, u"kind"_s)->text(1), u"mosaico.pull"_s);
  const QTreeWidgetItem* request = childNamed(descriptor, u"request"_s);
  ASSERT_NE(request, nullptr);
  EXPECT_EQ(childNamed(request, u"origin"_s)->text(1), u"demo.example.org:6726"_s);
  // Nanosecond timestamps round-trip as integers, never scientific notation.
  EXPECT_EQ(childNamed(request, u"start_ns"_s)->text(1), u"1461159829761465942"_s);

  // Loader metadata renders as its own attributed branch through the same
  // generic tree — the dialog copies everything at construction (owned
  // snapshot), so mutating the caller's struct afterwards changes nothing.
  const QTreeWidgetItem* loader = topLevelNamed(tree, u"Loader metadata (data_load_mcap)"_s);
  ASSERT_NE(loader, nullptr);
  const QTreeWidgetItem* file_facts = childNamed(loader, u"file"_s);
  ASSERT_NE(file_facts, nullptr);
  EXPECT_EQ(childNamed(file_facts, u"message_count"_s)->text(1), u"120"_s);
  metadata.json = uR"({"file": {"message_count": 999}})"_s;
  EXPECT_EQ(childNamed(file_facts, u"message_count"_s)->text(1), u"120"_s);

  // Without a record or metadata there are neither sections, only identity rows.
  const DatasetInfoDialog bare(u"plain.csv"_s, {}, nullptr, nullptr);
  const auto* bare_tree = bare.findChild<QTreeWidget*>(u"treeInfo"_s);
  ASSERT_NE(bare_tree, nullptr);
  EXPECT_EQ(topLevelNamed(bare_tree, u"Provenance"_s), nullptr);
  EXPECT_EQ(topLevelNamed(bare_tree, u"Loader metadata"_s), nullptr);
  EXPECT_EQ(topLevelNamed(bare_tree, u"File"_s)->text(1), u"—"_s);
}

}  // namespace
}  // namespace PJ
