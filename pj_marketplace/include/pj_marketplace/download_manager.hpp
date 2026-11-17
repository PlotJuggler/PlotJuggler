#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QFuture>
#include <QFutureSynchronizer>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QUrl>
#include <atomic>
#include <memory>

#include "pj_base/expected.hpp"
#include "pj_marketplace/archive_limits.hpp"

namespace PJ {

/// Download → checksum verification → extraction of one extension archive. The whole archive is buffered in memory and
/// the compressed-size limit is applied after the download completes. fetch() returns an integer id that identifies
/// the operation in every signal.
class DownloadManager : public QObject {
  Q_OBJECT

 public:
  /// Post-download work whose duration is not covered by byte-level `progress`.
  /// Consumers switch their UI to indeterminate/busy when a phase is announced.
  enum class WorkPhase {
    Verifying,
    Extracting,
  };
  Q_ENUM(WorkPhase)

  explicit DownloadManager(QObject* parent = nullptr);
  // Cancels every in-flight worker and drains before returning; workers check the flag at each archive entry and data
  // block.
  ~DownloadManager() override;

  /// Starts the full pipeline: download url, verify expected_checksum, extract to destination_dir.
  /// Returns a unique ID to track this operation.
  int fetch(const QUrl& url, const QString& expected_checksum, const QString& destination_dir);

  /// Replaces the archive budgets applied while unpacking (archive_limits.hpp). Production keeps the defaults; a test
  /// lowers them to drive a breach with a few kilobytes. Takes effect on the next extraction: set it before fetch().
  void setArchiveLimits(const ArchiveLimits& limits) {
    archive_limits_ = limits;
  }

  /// Cancels an in-progress operation. In the checksum/extract phase the worker exits early; the consumer rolls back
  /// the transaction directory when it receives `cancelled`.
  void cancel(int id);

  /// cancel(id), then BLOCK until that operation's worker has stopped touching its destination directory (no-op for an
  /// id with no extract in flight). For a consumer that must guarantee nothing is writing there before it releases an
  /// interprocess lock, deletes the transaction directory or tears itself down. Unlike the destructor's drain it waits
  /// for ONE operation, so an unrelated consumer sharing this downloader is not stalled. The wait does not spin an
  /// event loop, so the queued completion slot for `id` cannot run inside this call and a caller in its own destructor
  /// is not re-entered.
  void cancelAndWait(int id);

 signals:
  void started(int id);
  void progress(int id, qint64 bytes_received, qint64 bytes_total);
  void phaseChanged(int id, PJ::DownloadManager::WorkPhase phase);
  void finished(int id);
  void cancelled(int id);
  void failed(int id, const QString& error);

 private slots:
  void onReplyFinished(QNetworkReply* reply);
  void onDownloadProgress(qint64 bytes_received, qint64 bytes_total);

 private:
  struct Operation {
    QString expected_checksum;
    QString destination_dir;
  };

  QString calculateSha256(const QByteArray& data) const;
  bool verifyChecksum(const QByteArray& data, const QString& expected_checksum) const;
  PJ::Expected<void, QString> extractFromMemory(
      const QByteArray& data, const QString& destination_dir, const std::atomic<bool>& cancel_requested,
      const ArchiveLimits& limits) const;

  ArchiveLimits archive_limits_;
  QNetworkAccessManager* network_;
  QMap<int, QNetworkReply*> active_replies_;
  QMap<int, Operation> operations_;
  QMap<int, std::shared_ptr<std::atomic<bool>>> cancel_flags_;
  int next_id_ = 1;
  QFutureSynchronizer<QString> pending_extracts_;
  // Per-operation handle on the futures pending_extracts_ drains as a batch, so cancelAndWait() can wait for one.
  // Retired by the completion slot.
  QMap<int, QFuture<QString>> extract_futures_;
};

}  // namespace PJ
