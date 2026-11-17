// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <archive.h>
#include <archive_entry.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QNetworkRequest>
#include <QtConcurrent>
#include <algorithm>

#include "pj_base/expected.hpp"
#include "pj_marketplace/archive_limits.hpp"
#include "pj_marketplace/download_manager.hpp"
using namespace Qt::StringLiterals;

namespace PJ {

PJ::Expected<void, QString> DownloadManager::extractFromMemory(
    const QByteArray& data, const QString& destination_dir, const std::atomic<bool>& cancel_requested,
    const ArchiveLimits& limits) const {
  ArchiveBudget budget;

  if (static_cast<uint64_t>(data.size()) > limits.maximum_compressed_bytes) {
    return PJ::unexpected(u"Archive is larger than the %1-byte limit"_s.arg(limits.maximum_compressed_bytes));
  }

  QDir dest_dir(destination_dir);
  if (!dest_dir.exists() && !dest_dir.mkpath(u"."_s)) {
    return PJ::unexpected(u"Could not create destination directory: %1"_s.arg(destination_dir));
  }

  // The trailing separator makes the prefix check exact: /tmp/foo must not match /tmp/foo_evil.
  const QString safe_root = dest_dir.absolutePath() + QLatin1Char('/');

  auto archive_deleter = [](struct archive* a) { archive_read_free(a); };
  std::unique_ptr<struct archive, decltype(archive_deleter)> a(archive_read_new(), archive_deleter);

  archive_read_support_format_zip(a.get());

  if (archive_read_open_memory(a.get(), data.constData(), static_cast<size_t>(data.size())) != ARCHIVE_OK) {
    return PJ::unexpected(u"Could not open ZIP: %1"_s.arg(QString::fromUtf8(archive_error_string(a.get()))));
  }

  struct archive_entry* entry;
  int r;
  while ((r = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK) {
    // Cancel checkpoint per entry. The consumer wipes the whole transaction dir on cancel, so returning mid-file is
    // safe.
    if (cancel_requested.load(std::memory_order_relaxed)) {
      return PJ::unexpected(u"Cancelled"_s);
    }

    const QString entry_name = QString::fromUtf8(archive_entry_pathname(entry));

    // Refuse the path's shape before it reaches the filesystem, so a name harmless here but unusable on Windows fails
    // on both.
    if (const auto refusal = admitEntryPath(entry_name.toStdString(), limits.maximum_path_segments)) {
      return PJ::unexpected(u"%1: %2"_s.arg(QString::fromStdString(*refusal), entry_name));
    }

    const QString target_path = dest_dir.filePath(entry_name);

    // Path traversal (entries containing "../").
    if (!QFileInfo(target_path).absoluteFilePath().startsWith(safe_root)) {
      return PJ::unexpected(u"Unsafe path detected in ZIP entry: %1"_s.arg(entry_name));
    }

    // Deduced, not spelled: archive_entry_filetype() returns __LA_MODE_T, which is mode_t only where POSIX provides it
    // (unsigned short on Windows, int from libarchive 4.0).
    const auto entry_type = archive_entry_filetype(entry);
    const ArchiveEntryKind entry_kind = entry_type == AE_IFDIR   ? ArchiveEntryKind::kDirectory
                                        : entry_type == AE_IFREG ? ArchiveEntryKind::kRegularFile
                                                                 : ArchiveEntryKind::kOther;
    if (const auto refusal = admitEntryKind(entry_kind)) {
      return PJ::unexpected(u"%1: %2"_s.arg(QString::fromStdString(*refusal), entry_name));
    }

    // An entry may not declare its size: a ZIP written without seeking defers sizes to a descriptor after the payload,
    // and libarchive reports zero when it cannot reach the central directory. Reading that as "declares nothing" would
    // refuse a valid archive, so a declared size is charged before the file is opened and an undeclared one is bounded
    // while the bytes arrive.
    const bool size_declared = archive_entry_size_is_set(entry) != 0;
    const uint64_t declared_bytes = (size_declared && entry_kind == ArchiveEntryKind::kRegularFile)
                                        ? static_cast<uint64_t>(std::max<la_int64_t>(archive_entry_size(entry), 0))
                                        : 0U;

    // Always called: it counts the entry against the entry-count cap. Undeclared entries charge zero here and settle at
    // EOF.
    if (const auto refusal = admitEntry(limits, budget, declared_bytes)) {
      return PJ::unexpected(u"%1 (at \"%2\")"_s.arg(QString::fromStdString(*refusal), entry_name));
    }

    if (entry_kind == ArchiveEntryKind::kDirectory) {
      dest_dir.mkpath(entry_name);
      continue;
    }

    // Declared, the entry is held to its declaration; undeclared, to what the budgets still allow. Computed before the
    // file is opened, because opening truncates: a ceiling of zero must refuse the entry while the path is untouched.
    // The remaining total is subtracted defensively: an unsigned wrap would raise the ceiling instead of lowering it.
    const uint64_t remaining_total = limits.maximum_total_expanded_bytes > budget.total_expanded_bytes
                                         ? limits.maximum_total_expanded_bytes - budget.total_expanded_bytes
                                         : 0U;
    const uint64_t write_cap =
        size_declared ? declared_bytes : std::min(limits.maximum_entry_expanded_bytes, remaining_total);

    QFileInfo fi(target_path);
    QDir().mkpath(fi.absolutePath());

    QFile out_file(target_path);
    if (!out_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      return PJ::unexpected(u"No write permission for: %1"_s.arg(target_path));
    }

    const void* buf;
    size_t size;
    la_int64_t offset;
    uint64_t written_bytes = 0;
    for (;;) {
      // Cancel checkpoint per block, so one huge entry does not pin cancel latency to its whole write.
      if (cancel_requested.load(std::memory_order_relaxed)) {
        return PJ::unexpected(u"Cancelled"_s);
      }
      int rc = archive_read_data_block(a.get(), &buf, &size, &offset);
      if (rc == ARCHIVE_EOF) {
        break;
      }
      if (rc != ARCHIVE_OK) {
        out_file.close();
        return PJ::unexpected(
            u"Error reading ZIP entry '%1': %2"_s.arg(entry_name, QString::fromUtf8(archive_error_string(a.get()))));
      }
      // Bytes past the ceiling are unaccounted for: a decompression bomb whose header lies, or an undeclared entry
      // outgrowing what is left of the budget. Stop at the first block that crosses it.
      if (size > write_cap - written_bytes) {
        out_file.close();
        return PJ::unexpected(
            size_declared
                ? u"ZIP entry '%1' holds more bytes than it declares"_s.arg(entry_name)
                : u"ZIP entry '%1' does not declare its size and expands past the remaining budget"_s.arg(entry_name));
      }
      out_file.write(static_cast<const char*>(buf), static_cast<qint64>(size));
      written_bytes += size;
    }
    out_file.close();

    // Settle an undeclared entry against the budget now that its size is known (capped above, so the total cannot pass
    // the limit).
    if (!size_declared) {
      budget.total_expanded_bytes += written_bytes;
    }
  }

  if (r != ARCHIVE_EOF) {
    return PJ::unexpected(u"Error during extraction: %1"_s.arg(QString::fromUtf8(archive_error_string(a.get()))));
  }

  return {};
}

DownloadManager::DownloadManager(QObject* parent) : QObject(parent), network_(new QNetworkAccessManager(this)) {
  connect(network_, &QNetworkAccessManager::finished, this, &DownloadManager::onReplyFinished);
}

DownloadManager::~DownloadManager() {
  // Cancel every in-flight worker before draining: extraction checks the flag at each archive entry and data block, so
  // the wait is bounded by one block plus the checksum, which is not interruptible, not by the whole extraction.
  for (const auto& flag : cancel_flags_) {
    flag->store(true, std::memory_order_relaxed);
  }
  pending_extracts_.waitForFinished();
}

int DownloadManager::fetch(const QUrl& url, const QString& expected_checksum, const QString& destination_dir) {
  const int id = next_id_++;

  QNetworkRequest req(url);
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  QNetworkReply* reply = network_->get(req);
  reply->setProperty("operationId", id);

  active_replies_.insert(id, reply);
  operations_.insert(id, {expected_checksum, destination_dir});

  connect(reply, &QNetworkReply::downloadProgress, this, &DownloadManager::onDownloadProgress);

  emit started(id);
  return id;
}

void DownloadManager::cancel(int id) {
  // Download phase: abort the reply; onReplyFinished emits cancelled().
  if (QNetworkReply* reply = active_replies_.value(id, nullptr); reply) {
    reply->abort();
    return;
  }
  // Checksum/extract phase: flip the flag; the worker returns early and the watcher slot maps that to cancelled(id).
  if (auto it = cancel_flags_.find(id); it != cancel_flags_.end()) {
    (*it)->store(true, std::memory_order_relaxed);
  }
}

void DownloadManager::cancelAndWait(int id) {
  cancel(id);
  // Copy the future before waiting: a nested cancel path can mutate the map, and the QFuture handle keeps the work
  // item alive.
  const auto it = extract_futures_.constFind(id);
  if (it == extract_futures_.constEnd()) {
    return;  // download phase (aborted above) or nothing in flight: no worker to wait for
  }
  QFuture<QString> extract = *it;
  extract.waitForFinished();
}

void DownloadManager::onDownloadProgress(qint64 bytes_received, qint64 bytes_total) {
  auto* reply = qobject_cast<QNetworkReply*>(sender());
  if (!reply) {
    return;
  }
  emit progress(reply->property("operationId").toInt(), bytes_received, bytes_total);
}

void DownloadManager::onReplyFinished(QNetworkReply* reply) {
  const int id = reply->property("operationId").toInt();
  active_replies_.remove(id);
  const Operation op = operations_.take(id);
  if (reply->error() != QNetworkReply::NoError) {
    if (reply->error() == QNetworkReply::OperationCanceledError) {
      reply->deleteLater();
      emit cancelled(id);
      return;
    }
    const QString error = reply->errorString();
    reply->deleteLater();
    emit failed(id, error);
    return;
  }

  const QByteArray data = reply->readAll();
  reply->deleteLater();

  // Checksum and extraction are CPU/IO heavy: they run on a worker thread, and the result (empty QString on success,
  // else the error) returns through the watcher's finished() on the GUI thread, where the signals are emitted.
  //
  // Lifetime: the destructor drains pending_extracts_, so `this` stays valid for the whole worker and watcher slot.
  const QString expected = op.expected_checksum;
  const QString destination = op.destination_dir;
  auto cancel_flag = std::make_shared<std::atomic<bool>>(false);
  cancel_flags_.insert(id, cancel_flag);

  auto* watcher = new QFutureWatcher<QString>(this);
  connect(
      watcher, &QFutureWatcher<QString>::finished, this,
      [this, id, watcher, cancel_flag]() {
        const QString error = watcher->result();
        watcher->deleteLater();
        const bool was_cancelled = cancel_flag->load(std::memory_order_relaxed);
        cancel_flags_.remove(id);
        extract_futures_.remove(id);
        if (was_cancelled) {
          emit cancelled(id);
          return;
        }
        if (error.isEmpty()) {
          emit finished(id);
        } else {
          emit failed(id, error);
        }
      },
      Qt::QueuedConnection);
  // Copied for the worker: setArchiveLimits writes the member from the GUI thread
  // without a lock, so the worker must not read it after launch.
  const ArchiveLimits limits = archive_limits_;
  auto future = QtConcurrent::run([this, id, data, expected, destination, cancel_flag, limits]() -> QString {
    // The checksum is a single-shot hash, so the cancel flag is not threaded through it; a cancel during it is caught
    // at the boundary check below or inside extractFromMemory. Not measured (docs/TECHNICAL_NOTES.md "Cancel latency").
    if (!expected.isEmpty()) {
      // A "sha256:" prefix with no digest is a malformed registry field: fail with a message that points at the
      // registry, since "Checksum mismatch" would send whoever debugs it after a corrupt artifact. An empty checksum
      // skips verification (the emptiness gate above).
      QString digest = expected;
      if (digest.startsWith("sha256:"_L1, Qt::CaseInsensitive)) {
        digest = digest.mid(7);
      }
      if (digest.isEmpty()) {
        return u"Malformed checksum in registry (\"%1\" has no digest)"_s.arg(expected);
      }
      emit phaseChanged(id, WorkPhase::Verifying);
      if (!verifyChecksum(data, expected)) {
        return u"Checksum mismatch"_s;
      }
    }
    if (cancel_flag->load(std::memory_order_relaxed)) {
      return u"Cancelled"_s;
    }
    emit phaseChanged(id, WorkPhase::Extracting);
    if (auto extract_result = extractFromMemory(data, destination, *cancel_flag, limits); !extract_result) {
      return extract_result.error();
    }
    return {};  // success
  });
  watcher->setFuture(future);
  pending_extracts_.addFuture(future);
  extract_futures_.insert(id, future);
}

QString DownloadManager::calculateSha256(const QByteArray& data) const {
  return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

bool DownloadManager::verifyChecksum(const QByteArray& data, const QString& expected_checksum) const {
  QString expected = expected_checksum;
  if (expected.startsWith("sha256:"_L1, Qt::CaseInsensitive)) {
    expected = expected.mid(7);
  }
  // Hex digests are case-insensitive and toHex() emits lowercase, so compare ignoring case: a registry may list
  // uppercase.
  return calculateSha256(data).compare(expected, Qt::CaseInsensitive) == 0;
}

}  // namespace PJ
