// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "url_fetcher.h"

#include <QFile>
#ifndef PJ_TARGET_WASM
#include <QNetworkDiskCache>
#endif
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QString>
#include <QTimer>
#include <chrono>
#include <optional>
#include <utility>

#include "pj_runtime/HttpGet.h"
#ifdef PJ_TARGET_WASM
#include "pj_runtime/BrowserFetch.h"
#endif
using namespace Qt::StringLiterals;

namespace pj::scene3d {
namespace {

FetchResult readLocalFile(const QString& path) {
  FetchResult result;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    result.error = file.errorString();
    return result;
  }
#ifdef PJ_TARGET_WASM
  // Browser-local sources are MEMFS files supplied by data/layout state. Bound
  // them before QByteArray allocation; native retains its existing unbounded
  // local-file behavior below.
  const qint64 size = file.size();
  if (size < 0 || size > UrlFetcher::kMaxFetchBytes) {
    result.error = QString(u"local model exceeds the %1 MiB limit"_s).arg(UrlFetcher::kMaxFetchBytes / (1024 * 1024));
    return result;
  }
  result.bytes = file.read(size);
  if (!file.atEnd()) {
    result.bytes.clear();
    result.error = QString(u"local model exceeds the %1 MiB limit"_s).arg(UrlFetcher::kMaxFetchBytes / (1024 * 1024));
    return result;
  }
#else
  result.bytes = file.readAll();
#endif
  result.ok = true;
  return result;
}

// The local filesystem path for `url` when it denotes a local file, else
// nullopt. Three local forms are recognized: a file:// URL, a scheme-less bare
// path, and a Windows drive-letter path that QUrl mis-parses as a single-letter
// "scheme" (e.g. QUrl("C:/dir/x") -> scheme "c", path "/dir/x"). Genuine
// network/other schemes (http/https/ftp/...) return nullopt.
std::optional<QString> localFilePath(const QUrl& url) {
  if (url.isLocalFile()) {
    return url.toLocalFile();
  }
  const QString scheme = url.scheme();
  if (scheme.isEmpty()) {
    return url.toString();
  }
  if (scheme.size() == 1 && scheme.at(0).isLetter()) {
    // A one-letter "scheme" is a Windows drive letter, not a URL scheme:
    // rebuild "<drive>:<path>" so QFile can open it.
    return scheme + u":"_s + url.path();
  }
  return std::nullopt;
}

}  // namespace

UrlFetcher::UrlFetcher(QObject* parent) : QObject(parent), manager_(this) {
  // Persist fetched remote models so re-opening the dataset (or the next session)
  // serves them from disk instead of re-downloading. QNetworkDiskCache honors HTTP
  // Cache-Control/ETag: a fresh entry is served without touching the network; a
  // stale one is revalidated when online (304 → reuse) and served from cache when
  // the origin is unreachable (so the model still renders offline, possibly from
  // an older copy). Local file reads bypass the cache.
  //
  // The directory defaults to <AppData>/models, overridable via the
  // PJ_MODEL_CACHE_DIR environment variable — both to relocate the cache and to
  // let tests redirect it to a throwaway dir without touching the real home.
#ifndef PJ_TARGET_WASM
  QString cache_dir = qEnvironmentVariable("PJ_MODEL_CACHE_DIR");
  if (cache_dir.isEmpty()) {
    cache_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + u"/models"_s;
  }
  auto* cache = new QNetworkDiskCache(this);
  cache->setCacheDirectory(cache_dir);
  cache->setMaximumCacheSize(256LL * 1024 * 1024);
  manager_.setCache(cache);
#endif
}

UrlFetcher::~UrlFetcher() {
  for (QNetworkReply* reply : manager_.findChildren<QNetworkReply*>()) {
    reply->disconnect(this);
    if (!reply->isFinished()) {
      reply->abort();
    }
  }
}

void UrlFetcher::deliverLater(std::function<void(FetchResult)> on_done, FetchResult result) {
  QTimer::singleShot(0, this, [on_done = std::move(on_done), result = std::move(result)]() { on_done(result); });
}

void UrlFetcher::fetch(const QUrl& url, std::function<void(FetchResult)> on_done) {
  // Local sources never touch the network: file:// URLs, scheme-less bare paths,
  // and Windows drive-letter paths read straight from disk (still delivered
  // asynchronously). See localFilePath() for why the drive-letter case matters.
  if (const std::optional<QString> local = localFilePath(url)) {
    deliverLater(std::move(on_done), readLocalFile(*local));
    return;
  }
  if (!isNetworkUrl(url)) {
    FetchResult result;
    result.error = tr("unsupported URL scheme '%1'").arg(url.scheme());
    deliverLater(std::move(on_done), std::move(result));
    return;
  }

#ifdef PJ_TARGET_WASM
  PJ::browserFetchToBytes(
      url, kMaxFetchBytes, std::chrono::seconds(15), this,
      [url, on_done = std::move(on_done)](QByteArray bytes, QString error, bool) {
        FetchResult result;
        result.ok = error.isEmpty();
        result.error = std::move(error);
        result.bytes = std::move(bytes);
        result.final_url = url;  // the browser follows redirects out of sight
        on_done(std::move(result));
      });
#else
  QNetworkRequest request(url);
  // Bounded redirects with no https->http downgrade (see the class contract for
  // why this is the chosen policy).
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  request.setMaximumRedirectsAllowed(4);
  // `this` as context: destroying the fetcher aborts the reply and drops the callback.
  PJ::httpGetWithLimit(
      manager_, request, kMaxFetchBytes, std::chrono::seconds(15), this,
      [on_done = std::move(on_done)](QNetworkReply& reply, bool limit_exceeded) {
        FetchResult result;
        if (limit_exceeded) {
          result.error = tr("response exceeds the %1 MiB limit").arg(kMaxFetchBytes / (1024 * 1024));
        } else if (reply.error() != QNetworkReply::NoError) {
          result.error = reply.errorString();
        } else {
          result.bytes = reply.readAll();
          result.final_url = reply.url();
          result.ok = true;
        }
        on_done(std::move(result));
      });
#endif
}

}  // namespace pj::scene3d
