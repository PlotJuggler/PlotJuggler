// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDir>
#include <QFileInfo>
#include <utility>

#include "pj_marketplace/platform_utils.hpp"
#include "pj_marketplace/store_lease.hpp"
#include "pj_marketplace/store_session.hpp"

using namespace Qt::StringLiterals;

namespace PJ {
namespace {

void sweepOperationDirectories(const QString& extraction_root) {
  QDir root(extraction_root);
  for (const QFileInfo& entry : root.entryInfoList(QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
    QDir(entry.absoluteFilePath()).removeRecursively();
  }
}

}  // namespace

StoreLayout::StoreLayout(QString store_root, QString artifacts_root, QString extraction_root, QString profiles_root)
    : store_root_(std::move(store_root)),
      artifacts_root_(std::move(artifacts_root)),
      extraction_root_(std::move(extraction_root)),
      profiles_root_(std::move(profiles_root)) {}

Expected<StoreLayout, StoreRejection> StoreLayout::create(const QString& store_root) {
  if (!QDir().mkpath(store_root)) {
    return unexpected(storeRejection(
        StoreRejectionCode::kIoFailure, u"cannot create marketplace store root: %1"_s.arg(store_root).toStdString()));
  }
  const QString canonical_root = PlatformUtils::canonicalStoreRoot(store_root);
  const QString artifacts_root = storeSiblingPath(canonical_root, u".artifacts"_s);
  const QString extraction_root = storeSiblingPath(canonical_root, u".extraction"_s);
  const QString profiles_root = storeSiblingPath(canonical_root, u".profiles"_s);

  for (const QString& path : {artifacts_root, extraction_root, profiles_root}) {
    if (!QDir().mkpath(path)) {
      return unexpected(storeRejection(
          StoreRejectionCode::kIoFailure, u"cannot create marketplace store sibling: %1"_s.arg(path).toStdString()));
    }
    if (!sameFilesystem(canonical_root, path)) {
      return unexpected(storeRejection(
          StoreRejectionCode::kFilesystemMismatch,
          u"marketplace lifecycle state is not on the store filesystem: %1"_s.arg(path).toStdString()));
    }
  }
  return StoreLayout(canonical_root, artifacts_root, extraction_root, profiles_root);
}

const QString& StoreLayout::storeRoot() const noexcept {
  return store_root_;
}

const QString& StoreLayout::artifactsRoot() const noexcept {
  return artifacts_root_;
}

const QString& StoreLayout::extractionRoot() const noexcept {
  return extraction_root_;
}

const QString& StoreLayout::profilesRoot() const noexcept {
  return profiles_root_;
}

StoreSession::StoreSession(
    StoreLayout layout, std::unique_ptr<StoreWriteLease> lease, std::optional<StoreRejection> refusal)
    : layout_(std::move(layout)), lease_(std::move(lease)), refusal_(std::move(refusal)) {}

Expected<StoreSession, StoreRejection> StoreSession::open(const QString& store_root) {
  auto layout = StoreLayout::create(store_root);
  if (!layout) {
    return unexpected(layout.error());
  }
  auto lease = StoreWriteLease::acquire(layout->storeRoot());
  if (!lease) {
    return StoreSession(std::move(*layout), nullptr, lease.error());
  }
  // The legacy ExtensionManager is still the only production session caller in
  // PR 5, so writer-open must retain its crash-residue sweep independently of
  // ProfileStore's narrower, lifecycle-aware garbage collection.
  sweepOperationDirectories(layout->extractionRoot());
  return StoreSession(std::move(*layout), std::make_unique<StoreWriteLease>(std::move(*lease)), std::nullopt);
}

StoreSession::~StoreSession() = default;
StoreSession::StoreSession(StoreSession&&) noexcept = default;
StoreSession& StoreSession::operator=(StoreSession&&) noexcept = default;

const StoreLayout& StoreSession::layout() const noexcept {
  return layout_;
}

bool StoreSession::hasStoreWriteAccess() const noexcept {
  return lease_ != nullptr;
}

const StoreRejection* StoreSession::writeRefusal() const noexcept {
  return refusal_ ? &*refusal_ : nullptr;
}

}  // namespace PJ
