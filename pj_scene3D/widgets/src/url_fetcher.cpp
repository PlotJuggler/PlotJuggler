// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "url_fetcher.h"

#include <QFile>
#ifndef PJ_TARGET_WASM
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#endif
#include <QStandardPaths>
#include <QString>
#include <QTimer>
#include <chrono>
#include <optional>
#include <utility>

#include "pj_runtime/HttpGet.h"
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
  // Browser-local sources are MEMFS files supplied by data/layout state: bound them before the QByteArray allocation.
  // Native local files are read unbounded below.
  const qint64 size = file.size();
  const bool fits = size >= 0 && size <= UrlFetcher::kMaxFetchBytes;
  if (fits) {
    result.bytes = file.read(size);
  }
  if (!fits || !file.atEnd()) {
    result.bytes.clear();
    result.error = u"local model exceeds the %1 MiB limit"_s.arg(UrlFetcher::kMaxFetchBytes / (1024 * 1024));
    return result;
  }
#else
  result.bytes = file.readAll();
#endif
  result.ok = true;
  return result;
}

// The local filesystem path for `url` when it denotes a local file, else nullopt. Recognized: a file:// URL, a
// scheme-less bare path, and a Windows drive-letter path that QUrl mis-parses as a single-letter "scheme"
// (QUrl("C:/dir/x") -> scheme "c", path "/dir/x"). Network/other schemes (http/https/ftp/...) return nullopt.
std::optional<QString> localFilePath(const QUrl& url) {
  if (url.isLocalFile()) {
    return url.toLocalFile();
  }
  const QString scheme = url.scheme();
  if (scheme.isEmpty()) {
    return url.toString();
  }
  if (scheme.size() == 1 && scheme.at(0).isLetter()) {
    return scheme + u":"_s + url.path();
  }
  return std::nullopt;
}

}  // namespace

UrlFetcher::UrlFetcher(QObject* parent) : QObject(parent) {
  // Persist fetched remote models so re-opening the dataset (or the next session) serves them from disk.
  // QNetworkDiskCache honors HTTP Cache-Control/ETag: a fresh entry is served without the network; a stale one is
  // revalidated when online (304 -> reuse) and served from cache when the origin is unreachable. Local file reads
  // bypass the cache. The directory defaults to <AppData>/models, overridable via PJ_MODEL_CACHE_DIR (to relocate the
  // cache, and to let tests redirect it to a throwaway dir).
#ifndef PJ_TARGET_WASM
  QString cache_dir = qEnvironmentVariable("PJ_MODEL_CACHE_DIR");
  if (cache_dir.isEmpty()) {
    cache_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + u"/models"_s;
  }
  network_ = new QNetworkAccessManager(this);
  auto* cache = new QNetworkDiskCache(network_);
  cache->setCacheDirectory(cache_dir);
  cache->setMaximumCacheSize(256LL * 1024 * 1024);
  network_->setCache(cache);
#endif
}

void UrlFetcher::deliverLater(std::function<void(FetchResult)> on_done, FetchResult result) {
  QTimer::singleShot(0, this, [on_done = std::move(on_done), result = std::move(result)]() { on_done(result); });
}

void UrlFetcher::fetch(const QUrl& url, std::function<void(FetchResult)> on_done, bool allow_network) {
  // Local sources never touch the network (see localFilePath() for the drive-letter case): read straight from disk,
  // still delivered asynchronously.
  if (const std::optional<QString> local = localFilePath(url)) {
    deliverLater(std::move(on_done), readLocalFile(*local));
    return;
  }
  if (!PJ::isHttpUrl(url)) {
    FetchResult result;
    result.error = tr("unsupported URL scheme '%1'").arg(url.scheme());
    deliverLater(std::move(on_done), std::move(result));
    return;
  }
  if (!allow_network) {
    FetchResult result;
    result.error = tr("remote fetch is disabled");
    deliverLater(std::move(on_done), std::move(result));
    return;
  }
  PJ::fetchBytesWithLimit(
      url, kMaxFetchBytes, std::chrono::seconds(15), this,
      [on_done = std::move(on_done)](PJ::FetchedBytes fetched) {
        FetchResult result;
        result.ok = fetched.error.isEmpty();
        result.error = std::move(fetched.error);
        result.bytes = std::move(fetched.bytes);
        result.final_url = std::move(fetched.final_url);
        on_done(std::move(result));
      },
      network_);
}

}  // namespace pj::scene3d
