#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QByteArray>
#include <QString>

namespace PJ::telemetry {

// The raw per-machine identity both telemetry senders hash (never sent as-is): the OS machine id, or a
// QSettings-persisted random UUID where Qt cannot read one. Only as durable as that settings backend.
[[nodiscard]] QByteArray machineIdentity();

// SHA-256 hex of machineIdentity() and `salt`. A distinct salt per purpose keeps the ids unlinkable.
[[nodiscard]] QString saltedMachineHash(const QByteArray& salt);

// Whether a build with this PJ_INSTALLATION stamp may report at all: "source" (the CMake default,
// i.e. any self-built tree; trimmed, case-insensitive) and an empty stamp never do. Every other
// stamp does, so a downstream packager's own channel still shows up.
[[nodiscard]] bool isReportingInstallation(const QString& installation);

}  // namespace PJ::telemetry
