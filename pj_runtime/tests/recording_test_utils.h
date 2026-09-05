#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Test support shared by the recording tests that read a produced file back:
// the `pj.recording` metadata lookup, and the QSettings/QStandardPaths
// isolation every test touching RecordingService needs.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

// No MCAP_IMPLEMENTATION here: pj_runtime carries the single implementation TU.
#include <mcap/reader.hpp>

#include "pj_runtime/McapRecordingWriter.h"
#include "pj_runtime/RecordingFormat.h"

namespace PJ::test {

/// Every `pj.recording` record in the file, in the order it was written. The
/// writer emits one at open() and one at close(), so a file with a single
/// record was never finalized. Requires a summary already read off `reader`.
inline std::vector<mcap::KeyValueMap> recordingRecords(mcap::McapReader& reader) {
  std::vector<mcap::KeyValueMap> records;
  auto scanned = forEachMcapMetadata(
      reader, kRecordingMetadataName, [&](const mcap::Metadata& metadata) { records.push_back(metadata.metadata); });
  EXPECT_TRUE(scanned.has_value()) << (scanned ? "" : scanned.error());
  return records;
}

/// The last `pj.recording` record — the one a reader takes, since close()
/// appends the final facts over the open()-time ones. Empty when the file
/// carries none.
inline mcap::KeyValueMap lastRecordingRecord(mcap::McapReader& reader) {
  std::vector<mcap::KeyValueMap> records = recordingRecords(reader);
  return records.empty() ? mcap::KeyValueMap{} : std::move(records.back());
}

/// Redirects QSettings and QStandardPaths into a temporary tree it owns, so a
/// test never reads or writes the developer's real configuration, and
/// bootstraps the QCoreApplication those lookups need when the process has
/// none. Declare it before anything that reads settings, and keep it alive for
/// the whole test: the tree dies with it.
///
/// The redirection is process-global (QSettings::setPath, test mode), so one
/// instance per test fixture is the intended use.
struct IsolatedQtSettings {
  IsolatedQtSettings(const QString& organization, const QString& application) {
    if (QCoreApplication::instance() == nullptr) {
      static int argc = 1;
      static char arg0[] = "pj_runtime_test";
      static char* argv[] = {arg0, nullptr};
      static auto app = std::make_unique<QCoreApplication>(argc, argv);
      (void)app;
    }
    QCoreApplication::setOrganizationName(organization);
    QCoreApplication::setApplicationName(application);
    QStandardPaths::setTestModeEnabled(true);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
  }

  [[nodiscard]] bool isValid() const {
    return dir.isValid();
  }

  QTemporaryDir dir;
};

}  // namespace PJ::test
