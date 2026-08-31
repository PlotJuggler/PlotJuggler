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

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

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

  // Trailing separator ensures prefix check is exact and not fooled by
  // sibling directories sharing a common prefix (e.g. /tmp/foo vs /tmp/foo_evil).
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
    // Cancel checkpoint before starting each entry. The consumer wipes the
    // whole transaction dir on cancel, so returning here with a partial file
    // (if the previous entry was mid-write) is safe.
    if (cancel_requested.load(std::memory_order_relaxed)) {
      return PJ::unexpected(u"Cancelled"_s);
    }

    const QString entry_name = QString::fromUtf8(archive_entry_pathname(entry));

    // Refuse the path's shape before it reaches the filesystem, so a name that is
    // harmless here and unusable on Windows fails on both.
    if (const auto refusal = admitEntryPath(entry_name.toStdString(), limits.maximum_path_segments)) {
      return PJ::unexpected(u"%1: %2"_s.arg(QString::fromStdString(*refusal), entry_name));
    }

    const QString target_path = dest_dir.filePath(entry_name);

    // Guard against path-traversal attacks (e.g. entries containing "../")
    if (!QFileInfo(target_path).absoluteFilePath().startsWith(safe_root)) {
      return PJ::unexpected(u"Unsafe path detected in ZIP entry: %1"_s.arg(entry_name));
    }

    // Deduced, not spelled: archive_entry_filetype() returns __LA_MODE_T, which is
    // mode_t only where POSIX provides it — unsigned short on Windows, and int from
    // libarchive 4.0 on.
    const auto entry_type = archive_entry_filetype(entry);
    const ArchiveEntryKind entry_kind = entry_type == AE_IFDIR   ? ArchiveEntryKind::kDirectory
                                        : entry_type == AE_IFREG ? ArchiveEntryKind::kRegularFile
                                                                 : ArchiveEntryKind::kOther;
    if (const auto refusal = admitEntryKind(entry_kind)) {
      return PJ::unexpected(u"%1: %2"_s.arg(QString::fromStdString(*refusal), entry_name));
    }

    // An entry does not always declare its size. A ZIP written without seeking
    // defers the sizes to a descriptor after the payload, and when the archive's
    // central directory is out of reach of the reader that would supply them, the
    // size stays unset — libarchive then reports zero. Reading that as "declares
    // nothing" would refuse a valid archive at its first data block, so the two
    // cases are handled apart: a declared size is charged before the file is
    // opened, an undeclared one is bounded while the bytes arrive.
    const bool size_declared = archive_entry_size_is_set(entry) != 0;
    const uint64_t declared_bytes = (size_declared && entry_kind == ArchiveEntryKind::kRegularFile)
                                        ? static_cast<uint64_t>(std::max<la_int64_t>(archive_entry_size(entry), 0))
                                        : 0U;

    // Always called, declared or not: it is what counts the entry against the
    // entry-count cap. Undeclared entries charge zero here and settle up at EOF.
    if (const auto refusal = admitEntry(limits, budget, declared_bytes)) {
      return PJ::unexpected(u"%1 (at \"%2\")"_s.arg(QString::fromStdString(*refusal), entry_name));
    }

    if (entry_kind == ArchiveEntryKind::kDirectory) {
      dest_dir.mkpath(entry_name);
      continue;
    }

    // Declared, the entry is held to its own declaration. Undeclared, it is held
    // to whatever the budgets still allow, which is the same ceiling the up-front
    // charge would have applied.
    //
    // Computed before the file is opened, because opening truncates: a ceiling of
    // zero has to refuse the entry while whatever is at that path is still
    // untouched. The remaining total is subtracted defensively — the budget never
    // exceeds the cap, but an unsigned wrap here would raise the ceiling instead
    // of lowering it, and a limit that fails open is worse than one that refuses
    // too much.
    const uint64_t remaining_total = limits.maximum_total_expanded_bytes > budget.total_expanded_bytes
                                         ? limits.maximum_total_expanded_bytes - budget.total_expanded_bytes
                                         : 0U;
    const uint64_t write_cap =
        size_declared ? declared_bytes : std::min(limits.maximum_entry_expanded_bytes, remaining_total);

    // Ensure the parent directory exists before writing
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
      // Cancel checkpoint per block so a single huge entry cannot pin cancel
      // response time to the whole entry's write duration.
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
      // Anything past the ceiling is bytes nobody accounted for — the shape a
      // decompression bomb takes when its header lies, or an undeclared entry that
      // outgrows what is left of the budget. Stop at the first block that crosses it.
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

    // Settle an undeclared entry against the budget now that its real size is
    // known. Capped above, so this cannot push the total past the limit.
    if (!size_declared) {
      budget.total_expanded_bytes += written_bytes;
    }
  }

  if (r != ARCHIVE_EOF) {
    return PJ::unexpected(u"Error during extraction: %1"_s.arg(QString::fromUtf8(archive_error_string(a.get()))));
  }

  return {};
}

// ---------------------------------------------------------------------------
// DownloadManager
// ---------------------------------------------------------------------------

DownloadManager::DownloadManager(QObject* parent) : QObject(parent), network_(new QNetworkAccessManager(this)) {
  connect(network_, &QNetworkAccessManager::finished, this, &DownloadManager::onReplyFinished);
}

DownloadManager::~DownloadManager() {
  // Signal cancel to every in-flight worker before draining. Extract checks
  // the flag at each archive entry and each data block, so the wait is
  // bounded by one block (~ms) plus at worst one full checksum on ~100 MB
  // (~30-100 ms) rather than the whole extract time.
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
  // Download phase: abort the reply, onReplyFinished will emit cancelled().
  if (QNetworkReply* reply = active_replies_.value(id, nullptr); reply) {
    reply->abort();
    return;
  }
  // Checksum/extract phase: flip the flag, the worker returns early and the
  // watcher's finished slot maps that outcome to cancelled(id).
  if (auto it = cancel_flags_.find(id); it != cancel_flags_.end()) {
    (*it)->store(true, std::memory_order_relaxed);
  }
}

void DownloadManager::cancelAndWait(int id) {
  cancel(id);
  // Copy the future before waiting: the map can be mutated by a nested cancel path,
  // and a QFuture handle keeps the work item alive on its own.
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

  // Checksum verification and extraction are CPU/IO-heavy for a large artifact.
  // Run them on a worker thread so the GUI thread stays responsive; the result
  // (empty QString on success, else an error message) is delivered back here via
  // the watcher's finished() on the GUI thread, where the signals are emitted.
  //
  // Lifetime: the destructor drains pending_extracts_ before returning, so
  // `this` remains valid for the whole worker + watcher-slot lifetime.
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
  auto future = QtConcurrent::run([this, id, data, expected, destination, cancel_flag]() -> QString {
    // Checksum is a single-shot ~30-100 ms hash even on ~100 MB artifacts, so
    // we do not thread the cancel flag through it. If the user cancels during
    // this window, the flag is caught at the boundary check below or inside
    // extractFromMemory; the watcher slot maps the outcome to cancelled(id).
    if (!expected.isEmpty()) {
      // A "sha256:" prefix with no digest is a malformed registry field, not a
      // real hash. Fail (a garbage checksum should not silently pass), but with
      // a message that points at the registry — "Checksum mismatch" wrongly
      // implies the downloaded artifact is corrupt/tampered and sends whoever
      // debugs it to the wrong place. (An empty/absent checksum still skips
      // verification via the emptiness gate above.)
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
    if (auto extract_result = extractFromMemory(data, destination, *cancel_flag, archive_limits_); !extract_result) {
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
  // Hex digests are case-insensitive: `toHex()` emits lowercase, but a registry
  // may list the checksum in uppercase. Compare without regard to case so a
  // correct artifact is not rejected over digit casing.
  return calculateSha256(data).compare(expected, Qt::CaseInsensitive) == 0;
}

}  // namespace PJ
