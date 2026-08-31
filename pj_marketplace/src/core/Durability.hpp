#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QFileDevice>
#include <QString>

#include "pj_base/expected.hpp"

namespace PJ {

/// Flush Qt buffers and force one open file's contents to persistent storage.
[[nodiscard]] Expected<void, QString> syncFileToDisk(QFileDevice& file);

/// Force directory-entry changes in @p directory to persistent storage.
[[nodiscard]] Expected<void, QString> syncDirectoryToDisk(const QString& directory);

}  // namespace PJ
