// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "BrowserFileStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <memory>
#include <utility>
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

struct StagedFile {
  QString path;
  QString leaf_directory;
  QString session_directory;

  ~StagedFile() {
    QFile::remove(path);
    QDir().rmdir(leaf_directory);
    QDir().rmdir(session_directory);  // succeeds only after the final lease.
  }
};

// Fixed staging chunk. Large enough that the per-chunk event-loop yield adds
// negligible overhead to a normal file, small enough that a single write of one
// chunk never stalls the browser main thread perceptibly.
constexpr qint64 kStageChunkBytes = 16 * 1024 * 1024;

}  // namespace

BrowserFileStore::BrowserFileStore(QString root)
    : root_(QDir::cleanPath(std::move(root))), session_token_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {}

QString BrowserFileStore::sanitizedBasename(const QString& browser_name) {
  // Browsers normally return a basename, but defensively strip both POSIX and
  // Windows separators before creating anything in MEMFS.
  QString name = browser_name;
  const qsizetype slash = std::max(name.lastIndexOf('/'), name.lastIndexOf('\\'));
  if (slash >= 0) {
    name = name.sliced(slash + 1);
  }
  name.replace(QRegularExpression(u"[^A-Za-z0-9._-]+"_s), u"_"_s);
  while (name.startsWith('.')) {
    name.remove(0, 1);
  }
  if (name.isEmpty() || name == "."_L1 || name == ".."_L1) {
    return u"upload.bin"_s;
  }
  return name.left(240);
}

QString BrowserFileStore::displayNameForIdentity(const QString& identity) {
  if (!isBrowserUploadIdentity(identity)) {
    return {};
  }
  const QString payload = identity.sliced(static_cast<qsizetype>(QLatin1StringView(kBrowserUploadScheme).size()));
  const QStringList parts = payload.split(QLatin1Char('/'));
  if (parts.size() != 3 || parts[0].isEmpty() || parts[2].isEmpty()) {
    return {};
  }
  const QUuid session = QUuid::fromString(parts[0]);
  bool id_ok = false;
  const qulonglong id = parts[1].toULongLong(&id_ok);
  if (session.isNull() || session.toString(QUuid::WithoutBraces) != parts[0] || !id_ok || id == 0) {
    return {};
  }
  const QString decoded = QUrl::fromPercentEncoding(parts[2].toLatin1());
  if (decoded.isEmpty() || QString::fromLatin1(QUrl::toPercentEncoding(decoded)) != parts[2]) {
    return {};
  }
  return decoded;
}

std::optional<BrowserFileStore::StagePlacement> BrowserFileStore::preparePlacement(
    const QString& browser_name, QString& error_out) {
  if (browser_name.isEmpty()) {
    error_out = u"The browser did not provide a filename."_s;
    return std::nullopt;
  }

  const quint64 id = next_id_.fetch_add(1, std::memory_order_relaxed);
  const QString session_dir = root_ + QLatin1Char('/') + session_token_;
  const QString leaf_dir = session_dir + QLatin1Char('/') + QString::number(id);
  if (!QDir().mkpath(leaf_dir)) {
    error_out = u"Could not create browser upload directory %1."_s.arg(leaf_dir);
    return std::nullopt;
  }

  const QString basename = sanitizedBasename(browser_name);
  const QString path = leaf_dir + QLatin1Char('/') + basename;
  const QString encoded_name = QString::fromLatin1(QUrl::toPercentEncoding(browser_name));
  const QString identity =
      u"%1%2/%3/%4"_s.arg(QLatin1StringView(kBrowserUploadScheme), session_token_).arg(id).arg(encoded_name);
  return StagePlacement{
      .session_directory = session_dir,
      .leaf_directory = leaf_dir,
      .path = path,
      .identity = identity,
      .display_name = browser_name,
      .content_sha256 = {},
  };
}

BrowserFileStore::StageResult BrowserFileStore::makeSuccess(const StagePlacement& placement) {
  auto lease = std::make_shared<StagedFile>();
  lease->path = placement.path;
  lease->leaf_directory = placement.leaf_directory;
  lease->session_directory = placement.session_directory;
  return {
      .input =
          LoadInput{
              .display_name = placement.display_name,
              .backing_path = placement.path,
              .source_identity = placement.identity,
              .content_sha256 = placement.content_sha256,
              .lease = std::static_pointer_cast<void>(lease),
          },
      .error = {},
  };
}

BrowserFileStore::StageResult BrowserFileStore::reserve(const QString& browser_name) {
  QString error;
  const std::optional<StagePlacement> placement = preparePlacement(browser_name, error);
  if (!placement.has_value()) {
    return {.input = std::nullopt, .error = std::move(error)};
  }
  return makeSuccess(*placement);
}

BrowserFileStore::StageResult BrowserFileStore::stage(const QString& browser_name, QByteArray bytes) {
  QString error;
  std::optional<StagePlacement> placement = preparePlacement(browser_name, error);
  if (!placement.has_value()) {
    return {.input = std::nullopt, .error = std::move(error)};
  }

  QFile output(placement->path);
  if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    QDir().rmdir(placement->leaf_directory);
    return {
        .input = std::nullopt,
        .error = u"Could not stage %1: %2"_s.arg(browser_name, output.errorString()),
    };
  }
  const qint64 expected = bytes.size();
  placement->content_sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
  const qint64 written = output.write(bytes);
  const bool flushed = output.flush();
  output.close();
  bytes.clear();  // Release the source copy the moment its bytes are on disk.
  if (written != expected || !flushed) {
    QFile::remove(placement->path);
    QDir().rmdir(placement->leaf_directory);
    return {
        .input = std::nullopt,
        .error = u"Short write while staging %1 (%2 of %3 bytes)."_s.arg(browser_name).arg(written).arg(expected),
    };
  }

  QFile verify(placement->path);
  if (!verify.open(QIODevice::ReadOnly) || verify.size() != expected) {
    const QString detail = verify.errorString();
    verify.close();
    QFile::remove(placement->path);
    QDir().rmdir(placement->leaf_directory);
    return {
        .input = std::nullopt,
        .error = u"Could not reopen staged upload %1: %2"_s.arg(browser_name, detail),
    };
  }
  verify.close();

  return makeSuccess(*placement);
}

void BrowserFileStore::stageAsync(const QString& browser_name, QByteArray bytes, StageCompletion completion) {
  // Heap state of one stageAsync run, decoupled from the store after kickoff
  // (the store may be destroyed mid-staging). Lifetime: every scheduled lambda
  // owns only a shared_ptr to the job — the job never stores a callable that
  // captures itself — so the last pending event releases it. The completion
  // stays a member until then, making it a faithful destruction witness for
  // tests. Local struct: keeps the enclosing method's access to the private
  // StagePlacement/makeSuccess.
  struct AsyncStageJob {
    StagePlacement placement;
    QByteArray source;  // Cleared on every terminal path, before completion.
    QCryptographicHash hash{QCryptographicHash::Sha256};
    qint64 offset = 0;  // Bytes already written.
    qint64 total = 0;   // == source.size() captured before the writes start.
    std::unique_ptr<QFile> file;
    StageCompletion completion;

    // Always hops the event loop once before invoking completion so it is
    // never re-entrant with stageAsync().
    static void finish(std::shared_ptr<AsyncStageJob> job, StageResult result) {
      job->source.clear();  // Release the upload copy on failure paths too.
      QTimer::singleShot(
          0, [job = std::move(job), result = std::move(result)]() mutable { job->completion(std::move(result)); });
    }

    // One step: write up to one chunk, then re-arm on the event loop until the
    // whole buffer is staged.
    static void step(const std::shared_ptr<AsyncStageJob>& job) {
      const qint64 remaining = job->total - job->offset;
      const qint64 span = std::min<qint64>(remaining, kStageChunkBytes);
      const qint64 written = span > 0 ? job->file->write(job->source.constData() + job->offset, span) : 0;
      if (written != span) {
        const QString detail = job->file->errorString();
        job->file->close();
        QFile::remove(job->placement.path);
        QDir().rmdir(job->placement.leaf_directory);
        finish(
            job, {.input = std::nullopt,
                  .error = u"Short write while staging %1 (%2 of %3 bytes): %4"_s.arg(job->placement.display_name)
                               .arg(job->offset + std::max<qint64>(written, 0))
                               .arg(job->total)
                               .arg(detail)});
        return;
      }
      if (span > 0) {
        job->hash.addData(
            QByteArrayView(job->source).sliced(static_cast<qsizetype>(job->offset), static_cast<qsizetype>(span)));
      }
      job->offset += span;
      if (job->offset < job->total) {
        QTimer::singleShot(0, [job]() { step(job); });
        return;
      }

      // Last chunk written: flush, close, and release the source buffer BEFORE
      // completion so the load prologue never overlaps the staging copy.
      const bool flushed = job->file->flush();
      job->file->close();
      job->file.reset();
      job->placement.content_sha256 = QString::fromLatin1(job->hash.result().toHex());
      if (!flushed) {
        QFile::remove(job->placement.path);
        QDir().rmdir(job->placement.leaf_directory);
        finish(
            job, {.input = std::nullopt,
                  .error = u"Short write while staging %1 (flush failed)."_s.arg(job->placement.display_name)});
        return;
      }

      QFile verify(job->placement.path);
      if (!verify.open(QIODevice::ReadOnly) || verify.size() != job->total) {
        const QString detail = verify.errorString();
        verify.close();
        QFile::remove(job->placement.path);
        QDir().rmdir(job->placement.leaf_directory);
        finish(
            job, {.input = std::nullopt,
                  .error = u"Could not reopen staged upload %1: %2"_s.arg(job->placement.display_name, detail)});
        return;
      }
      verify.close();
      finish(job, makeSuccess(job->placement));
    }
  };

  QString error;
  std::optional<StagePlacement> placement = preparePlacement(browser_name, error);
  if (!placement.has_value()) {
    // Report the failure through the same event-loop hop as the success path so
    // the caller never sees a re-entrant completion.
    StageResult result{.input = std::nullopt, .error = std::move(error)};
    QTimer::singleShot(0, [completion = std::move(completion), result = std::move(result)]() mutable {
      completion(std::move(result));
    });
    return;
  }

  auto job = std::make_shared<AsyncStageJob>();
  job->placement = std::move(*placement);
  job->source = std::move(bytes);
  job->total = job->source.size();
  job->file = std::make_unique<QFile>(job->placement.path);
  job->completion = std::move(completion);

  // Sized once up front: MEMFS would otherwise regrow, and copy, the file's backing array as chunks extend it.
  if (!job->file->open(QIODevice::WriteOnly | QIODevice::Truncate) || !job->file->resize(job->total)) {
    const QString detail = job->file->errorString();
    job->file->close();
    QFile::remove(job->placement.path);
    QDir().rmdir(job->placement.leaf_directory);
    AsyncStageJob::finish(
        job, {.input = std::nullopt, .error = u"Could not stage %1: %2"_s.arg(job->placement.display_name, detail)});
    return;
  }

  // Kick off on the event loop, never re-entrantly from this call.
  QTimer::singleShot(0, [job]() { AsyncStageJob::step(job); });
}

}  // namespace PJ
