#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>

#include "pj_runtime/SessionManager.h"
#include "pj_widgets/Dialog.h"

namespace Ui {
class DatasetInfoDialog;
}

namespace PJ {

// A dataset's identity and provenance: display name, file path, and when present the SourceRecord and the
// loader-metadata document as generic key/value trees, shown verbatim (the host stays domain-neutral).
// WA_DeleteOnClose: callers show() and forget it. `record` and `metadata` may be null (their sections are then
// absent) and are read only in the constructor, so removing the dataset while the dialog is open leaves nothing
// dangling.
class DatasetInfoDialog : public Dialog {
  Q_OBJECT
 public:
  DatasetInfoDialog(
      const QString& dataset_name, const QString& source_path, const SourceRecord* record,
      const DatasetMetadata* metadata, QWidget* parent = nullptr);
  ~DatasetInfoDialog() override;

 private:
  Ui::DatasetInfoDialog* ui_;
};

}  // namespace PJ
