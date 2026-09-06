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

// App-styled dialog showing a dataset's identity and provenance: display name,
// backing file path, and — when the dataset carries them — the SourceRecord
// (provider id, source identity, descriptor JSON) and the loader-metadata
// document, both rendered as generic key/value trees. The host stays
// domain-neutral: fields are shown verbatim, with no interpretation of
// provider- or loader-specific keys.
// Modeless; the constructor sets WA_DeleteOnClose so callers can `show()` and
// forget about lifetime. `record` and `metadata` may be null (their sections
// are then absent) and are only read during construction — the dialog keeps
// its own copy of everything it shows, so removing the dataset while the
// dialog is open leaves nothing dangling.
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
