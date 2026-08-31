// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QStorageInfo>
#include <utility>

#include "pj_marketplace/store_lease.hpp"

using namespace Qt::StringLiterals;

namespace PJ {
namespace {

StoreRejection storeLockRefusal(const QLockFile& lock, const QString& lock_path) {
  switch (lock.error()) {
    case QLockFile::LockFailedError:
      // Contention with a live instance: kWriterLeaseUnavailable is the signal the
      // caller reads to advise a restart rather than a filesystem fix.
      return storeRejection(
          StoreRejectionCode::kWriterLeaseUnavailable,
          u"store writer lease is already held: %1"_s.arg(lock_path).toStdString());
    case QLockFile::PermissionError:
    case QLockFile::NoError:
    case QLockFile::UnknownError:
      break;
  }
  // Cannot create the lock file at all — a filesystem problem, not another
  // instance. kIoFailure keeps it distinct so the caller does not send the user
  // after a process that is not running.
  return storeRejection(
      StoreRejectionCode::kIoFailure, u"cannot create store writer lease file: %1"_s.arg(lock_path).toStdString());
}

}  // namespace

QString storeLockPath(const QString& canonical_store_root) {
  const QFileInfo store(canonical_store_root);
  return QDir(store.absolutePath()).absoluteFilePath(u"."_s + store.fileName() + u".lock"_s);
}

QString storeSiblingPath(const QString& canonical_store_root, const QString& suffix) {
  return canonical_store_root + suffix;
}

bool sameFilesystem(const QString& first, const QString& second) {
  const QStorageInfo first_volume(first);
  const QStorageInfo second_volume(second);
  return first_volume.isValid() && second_volume.isValid() && first_volume.device() == second_volume.device();
}

StoreWriteLease::StoreWriteLease(std::unique_ptr<QLockFile> lock) : lock_(std::move(lock)) {}

StoreWriteLease::~StoreWriteLease() = default;

StoreWriteLease::StoreWriteLease(StoreWriteLease&&) noexcept = default;

StoreWriteLease& StoreWriteLease::operator=(StoreWriteLease&&) noexcept = default;

Expected<StoreWriteLease, StoreRejection> StoreWriteLease::acquire(
    const QString& canonical_store_root, int timeout_milliseconds) {
  const QString lock_path = storeLockPath(canonical_store_root);
  auto lock = std::make_unique<QLockFile>(lock_path);
  lock->setStaleLockTime(0);
  if (!lock->tryLock(timeout_milliseconds)) {
    return unexpected(storeLockRefusal(*lock, lock_path));
  }
  return StoreWriteLease(std::move(lock));
}

}  // namespace PJ
