// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "ArchiveExtractor.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <algorithm>
#include <limits>
#include <memory>
#include <string>

#include "Durability.hpp"
#include "pj_marketplace/sha256.hpp"

namespace PJ {
namespace {

using ArchiveMode = decltype(archive_entry_filetype(nullptr));

ArchiveEntryType archiveEntryType(ArchiveMode file_type) {
  switch (file_type) {
    case AE_IFREG:
      return ArchiveEntryType::kRegularFile;
    case AE_IFDIR:
      return ArchiveEntryType::kDirectory;
    case AE_IFLNK:
      return ArchiveEntryType::kSymbolicLink;
    case AE_IFBLK:
    case AE_IFCHR:
      return ArchiveEntryType::kDevice;
    case AE_IFIFO:
      return ArchiveEntryType::kFifo;
#ifdef AE_IFSOCK
    case AE_IFSOCK:
      return ArchiveEntryType::kSocket;
#endif
    default:
      return ArchiveEntryType::kOther;
  }
}

QString pathToQString(const std::filesystem::path& path) {
#ifdef Q_OS_WIN
  return QString::fromStdWString(path.native());
#else
  return QString::fromUtf8(path.native());
#endif
}

std::string archiveError(struct archive* archive_handle, std::string prefix) {
  const char* detail = archive_error_string(archive_handle);
  if (detail != nullptr) {
    prefix += ": ";
    prefix += detail;
  }
  return prefix;
}

}  // namespace

Expected<ArchiveExtraction, StoreRejection> ArchiveExtractor::extract(
    const std::filesystem::path& archive_path, const std::filesystem::path& destination, const StoreQuotas& quotas,
    const std::atomic<bool>* cancel_requested, const ExtractionWriteHook& write_hook) const {
  const QString archive_name = pathToQString(archive_path);
  QFile archive_file(archive_name);
  if (!archive_file.open(QIODevice::ReadOnly)) {
    return unexpected(storeRejection(
        StoreRejectionCode::kIoFailure,
        "cannot open bounded archive file: " + archive_file.errorString().toStdString()));
  }

  const QString destination_name = pathToQString(destination);
  QDir destination_directory(destination_name);
  if (!destination_directory.exists() && !destination_directory.mkpath(QStringLiteral("."))) {
    return unexpected(storeRejection(StoreRejectionCode::kIoFailure, "cannot create extraction directory"));
  }
  const QString safe_root = destination_directory.absolutePath() + QLatin1Char('/');

  auto archive_deleter = [](struct archive* archive_handle) { archive_read_free(archive_handle); };
  std::unique_ptr<struct archive, decltype(archive_deleter)> archive_handle(archive_read_new(), archive_deleter);
  if (archive_read_support_format_zip(archive_handle.get()) != ARCHIVE_OK) {
    return unexpected(storeRejection(
        StoreRejectionCode::kArchiveInvalid, archiveError(archive_handle.get(), "cannot initialize ZIP reader")));
  }
  if (archive_read_open_fd(archive_handle.get(), archive_file.handle(), 64U * 1024U) != ARCHIVE_OK) {
    return unexpected(storeRejection(
        StoreRejectionCode::kArchiveInvalid, archiveError(archive_handle.get(), "cannot open ZIP archive")));
  }

  ArchiveExtraction extraction;
  ArchiveBudgetUsage budget_usage;
  struct archive_entry* entry = nullptr;
  int next_status = ARCHIVE_OK;
  while ((next_status = archive_read_next_header(archive_handle.get(), &entry)) == ARCHIVE_OK) {
    if (cancel_requested != nullptr && cancel_requested->load(std::memory_order_relaxed)) {
      return unexpected(storeRejection(StoreRejectionCode::kCancelled, "archive extraction was cancelled"));
    }

    const char* raw_path = archive_entry_pathname(entry);
    if (raw_path == nullptr) {
      return unexpected(storeRejection(StoreRejectionCode::kUnsafePath, "archive entry has no path"));
    }
    const std::string raw_path_string(raw_path);
    const QString decoded_path = QString::fromUtf8(raw_path_string);
    if (decoded_path.toUtf8().toStdString() != raw_path_string) {
      return unexpected(storeRejection(StoreRejectionCode::kUnsafePath, "archive entry path is not valid UTF-8"));
    }
    auto normalized_path = validateArchivePath(raw_path_string, quotas.maximum_path_segments);
    if (!normalized_path) {
      return unexpected(normalized_path.error());
    }

    const ArchiveEntryType entry_type = archiveEntryType(archive_entry_filetype(entry));
    const bool has_hardlink = archive_entry_hardlink(entry) != nullptr;
    if (auto type_policy = validateArchiveEntryType(entry_type, has_hardlink); !type_policy) {
      return unexpected(type_policy.error());
    }

    const QString relative_path =
        QString::fromUtf8(normalized_path->data(), static_cast<qsizetype>(normalized_path->size()));
    const QString target_path = destination_directory.absoluteFilePath(relative_path);
    const QString normalized_target = QFileInfo(target_path).absoluteFilePath();
    if (!normalized_target.startsWith(safe_root)) {
      return unexpected(storeRejection(StoreRejectionCode::kUnsafePath, "archive entry escapes extraction root"));
    }

    uint64_t expanded_size = 0;
    if (entry_type == ArchiveEntryType::kRegularFile) {
      const la_int64_t declared_size = archive_entry_size(entry);
      if (declared_size < 0) {
        return unexpected(storeRejection(StoreRejectionCode::kArchiveInvalid, "regular archive entry has no size"));
      }
      expanded_size = static_cast<uint64_t>(declared_size);
    }
    auto next_budget = checkEntryBudget(quotas, budget_usage, expanded_size);
    if (!next_budget) {
      return unexpected(next_budget.error());
    }
    budget_usage = *next_budget;

    if (entry_type == ArchiveEntryType::kDirectory) {
      if (!destination_directory.mkpath(relative_path)) {
        return unexpected(storeRejection(StoreRejectionCode::kIoFailure, "cannot create archive directory"));
      }
      continue;
    }

    const QFileInfo target_info(normalized_target);
    if (!QDir().mkpath(target_info.absolutePath())) {
      return unexpected(storeRejection(StoreRejectionCode::kIoFailure, "cannot create archive file parent"));
    }
    QFile output(normalized_target);
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
      return unexpected(storeRejection(
          StoreRejectionCode::kIoFailure, "cannot create archive file: " + output.errorString().toStdString()));
    }

    Sha256 file_hash;
    uint64_t observed_size = 0;
    for (;;) {
      if (cancel_requested != nullptr && cancel_requested->load(std::memory_order_relaxed)) {
        return unexpected(storeRejection(StoreRejectionCode::kCancelled, "archive extraction was cancelled"));
      }
      const void* block = nullptr;
      size_t block_size = 0;
      la_int64_t block_offset = 0;
      const int read_status = archive_read_data_block(archive_handle.get(), &block, &block_size, &block_offset);
      if (read_status == ARCHIVE_EOF) {
        break;
      }
      if (read_status != ARCHIVE_OK) {
        return unexpected(storeRejection(
            StoreRejectionCode::kArchiveInvalid, archiveError(archive_handle.get(), "cannot read archive entry")));
      }
      if (block_offset < 0 || static_cast<uint64_t>(block_offset) != observed_size ||
          block_size > expanded_size - observed_size) {
        return unexpected(
            storeRejection(StoreRejectionCode::kArchiveInvalid, "archive entry size or offset is inconsistent"));
      }
      const char* bytes = static_cast<const char*>(block);
      if (write_hook && !write_hook(target_info.filesystemFilePath(), bytes, block_size)) {
        return unexpected(storeRejection(StoreRejectionCode::kIoFailure, "injected archive write failure"));
      }
      if (block_size > static_cast<size_t>(std::numeric_limits<qint64>::max()) ||
          output.write(bytes, static_cast<qint64>(block_size)) != static_cast<qint64>(block_size)) {
        return unexpected(storeRejection(
            StoreRejectionCode::kIoFailure, "cannot write archive file: " + output.errorString().toStdString()));
      }
      file_hash.update(reinterpret_cast<const uint8_t*>(bytes), block_size);
      observed_size += block_size;
    }
    if (observed_size != expanded_size) {
      return unexpected(storeRejection(StoreRejectionCode::kIoFailure, "archive file was not written completely"));
    }
    if (auto sync_result = syncFileToDisk(output); !sync_result) {
      return unexpected(storeRejection(
          StoreRejectionCode::kIoFailure, "cannot durably write archive file: " + sync_result.error().toStdString()));
    }
    output.close();
    extraction.files.push_back({*normalized_path, file_hash.finishHex(), observed_size});
  }

  if (next_status != ARCHIVE_EOF) {
    return unexpected(storeRejection(
        StoreRejectionCode::kArchiveInvalid, archiveError(archive_handle.get(), "archive iteration failed")));
  }
  extraction.total_expanded_bytes = budget_usage.total_expanded_bytes;
  std::ranges::sort(extraction.files, {}, &StoredFile::relative_path);
  return extraction;
}

}  // namespace PJ
