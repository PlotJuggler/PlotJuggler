#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

namespace PJ {

/// `kFull` captures the complete layout-file state. `kHistory` omits every
/// history-exempt processor and plot tab, leaving undo/redo without authority
/// to recreate or remove them.
enum class SnapshotScope { kFull, kHistory };

/// What authority a restore has over history-exempt workspace state. `kReplace`
/// reconciles the full layout; `kHistory` preserves live exempt processors and
/// plot tabs, omits them from capture, and rejects a processor restore that
/// would strand an exempt dependent. A rollback inherits the intent of the
/// transaction it reverses.
enum class RestoreIntent { kReplace, kHistory };

}  // namespace PJ
