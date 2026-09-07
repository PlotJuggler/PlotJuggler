#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QList>
#include <QString>

#include "pj_base/expected.hpp"
#include "pj_marketplace/extension.hpp"

namespace PJ {

// Host facts used to turn the complete registry candidate set into one visible
// release per extension id. This value object deliberately has no network,
// filesystem, QObject, or widget dependency.
struct RegistryResolutionContext {
  QString platform;
  QString sdk_version;
};

// Validate every registry version/floor and select one candidate per id:
//   1. highest compatible release for the current platform;
//   2. otherwise highest release for the current platform (visible but blocked);
//   3. otherwise highest release overall (visible to non-filtering consumers).
// Reusing one semantic (id, version) slot is an error, including records that
// differ only by build metadata; platforms must share one immutable record.
[[nodiscard]] Expected<QList<Extension>, QString> resolveRegistryCandidates(
    const QList<Extension>& candidates, const RegistryResolutionContext& context);

}  // namespace PJ
