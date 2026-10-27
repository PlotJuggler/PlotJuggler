// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/BrowserFetch.h"

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>

#include <QByteArray>
#include <QObject>
#include <QTemporaryFile>
#include <QTimer>
#include <memory>
#include <unordered_map>
#include <utility>

#include "pj_runtime/HttpGet.h"

using namespace Qt::StringLiterals;

namespace PJ {
namespace {

// How the page's fetch ended, as pj_browser_fetch_start reports it (its script names the same numbers).
enum class FetchStatus : int {
  kOk = 0,
  kNetwork = 1,
  kHttpError = 2,
  kTooLarge = 3,
  kIdleTimeout = 4,
  kWriteFailed = 5,
  kInsecureRedirect = 6,
  kHashFailed = 7,
};

constexpr int kSha256Bytes = 32;

struct Transfer {
  std::function<void(BrowserFetchResult)> done;
  FetchProgress progress;
  /// Set for a transfer that reports its SHA-256; the page writes the digest of the finished body into `digest`.
  bool hash = false;
  QByteArray digest;
  qint64 max_bytes = 0;
  std::chrono::milliseconds idle_timeout{0};
  /// The SHA-256 a kept copy must match, its recordingCacheKey() and the file it streams into; the key is empty
  /// when the body is not kept.
  QString pinned_sha256;
  QString cache_key;
  QString path;
};

// The transfers not yet delivered, by id. Browser main thread only.
std::unordered_map<quint32, Transfer>& transfers() {
  static std::unordered_map<quint32, Transfer> instance;
  return instance;
}

// Registers a transfer and returns its id; 0 is never issued, so callers may use it for "none".
quint32 addTransfer(std::function<void(BrowserFetchResult)> done, qint64 max_bytes, std::chrono::milliseconds idle) {
  static quint32 last_id = 0;
  if (++last_id == 0) {
    ++last_id;
  }
  Transfer& transfer = transfers()[last_id];
  transfer.done = std::move(done);
  transfer.max_bytes = max_bytes;
  transfer.idle_timeout = idle;
  return last_id;
}

// Where Cache Storage keeps a body pinned to `sha256` fetched from `url`: a same-origin path no server answers. The
// loading page (packaging/web/loading.html.in) spells the same key with encodeURIComponent(), which leaves exactly
// `!*'()` unescaped beyond what toPercentEncoding() does.
QString recordingCacheKey(const QString& sha256, const QUrl& url) {
  return u"/pj-recording-cache/%1?source=%2"_s.arg(
      sha256, QString::fromLatin1(QUrl::toPercentEncoding(url.toString(QUrl::FullyEncoded), "!*'()")));
}

// Delivers `id`'s result from the event loop, never from the page's callback, unless it is cancelled by then.
void complete(quint32 id, BrowserFetchResult result) {
  QTimer::singleShot(0, [id, result = std::move(result)]() mutable {
    auto node = transfers().extract(id);
    if (!node.empty()) {
      node.mapped().done(std::move(result));
    }
  });
}

// The page side of a transfer. The body is written into `path` chunk by chunk and never kept whole: MEMFS holds
// the file outside the wasm heap. The file is sized once from Content-Length (MEMFS would otherwise regrow, and
// copy, it as chunks extend it); a compressed response declares its encoded length, which the final truncate
// corrects. The idle timer restarts with every chunk, so a large body may take as long as it keeps arriving.
// A `hash` of 0 skips hashing, otherwise the finished body is digested with WebCrypto straight from its MEMFS buffer
// (off the main thread, far faster than hashing in wasm) and the digest is handed over before the done call;
// `report_progress` reports the bytes received on the first chunk, then every
// 100 ms or so, and once at the end. Without a Content-Encoding header (CORS hides it cross-origin unless exposed) a
// body that outgrows its declared length is a decoded one, so its total becomes unknown. `status` tracks the phase so
// an exception maps to its FetchStatus. A body-less answer (a 204 may come without one) is an empty file. A response
// the loading page prefetched (globalThis.pjPrefetch, url -> {response, abort()}) is taken once instead of fetching:
// the abort reaches it, and a failed prefetch falls back to fetching. Otherwise a non-empty `cache_key` reads the
// body kept under it in the 'pj-recordings-v1' cache when there is one; any cache error counts as none. The macro
// joins the body into one line: every statement ends in a semicolon.
// clang-format off
EM_JS(void, pj_browser_fetch_start, (unsigned id, const char* url_utf8, const char* path_utf8, double max_bytes,
                                     int idle_timeout_ms, int hash, int report_progress,
                                     const char* cache_key_utf8), {
  const kOk = 0;
  const kNetwork = 1;
  const kHttpError = 2;
  const kTooLarge = 3;
  const kIdleTimeout = 4;
  const kWriteFailed = 5;
  const kInsecureRedirect = 6;
  const kHashFailed = 7;
  const url = UTF8ToString(url_utf8);
  const path = UTF8ToString(path_utf8);
  const cacheKey = UTF8ToString(cache_key_utf8);
  const controller = new AbortController();
  if (!Module.pjBrowserFetches) {
    Module.pjBrowserFetches = new Map();
  }
  Module.pjBrowserFetches.set(id, controller);
  let status = kNetwork;
  let httpStatus = 0;
  let received = 0;
  let timedOut = false;
  let fromCache = false;
  let timer = 0;
  const arm = () => {
    clearTimeout(timer);
    timer = setTimeout(() => { timedOut = true; controller.abort(); }, idle_timeout_ms);
  };
  (async () => {
    let stream = null;
    try {
      arm();
      const prefetched = globalThis.pjPrefetch ? globalThis.pjPrefetch.get(url) : undefined;
      let response;
      if (prefetched) {
        globalThis.pjPrefetch.delete(url);
        controller.signal.addEventListener('abort', () => prefetched.abort());
        response = await prefetched.response.catch(() => fetch(url, { signal: controller.signal }));
      } else {
        if (cacheKey && typeof caches !== 'undefined') {
          try {
            response = await (await caches.open('pj-recordings-v1')).match(cacheKey);
          } catch (error) {
          }
        }
        fromCache = Boolean(response);
        if (!fromCache) {
          response = await fetch(url, { signal: controller.signal });
        }
      }
      httpStatus = response.status;
      if (!response.ok) {
        status = kHttpError;
        return;
      }
      if (url.startsWith('https:') && response.url.startsWith('http:')) {
        status = kInsecureRedirect;
        return;
      }
      const declared = Number(response.headers.get('Content-Length')) || 0;
      if (declared > max_bytes) {
        status = kTooLarge;
        return;
      }
      let total = declared > 0 && !response.headers.get('Content-Encoding') ? declared : -1;
      let reported = -Infinity;
      status = kWriteFailed;
      stream = FS.open(path, 'w');
      if (declared > 0) {
        FS.ftruncate(stream.fd, declared);
      }
      const reader = response.body ? response.body.getReader() : null;
      while (reader) {
        status = kNetwork;
        const chunk = await reader.read();
        if (chunk.done) {
          break;
        }
        arm();
        const bytes = chunk.value;
        if (received + bytes.length > max_bytes) {
          status = kTooLarge;
          return;
        }
        status = kWriteFailed;
        FS.write(stream, bytes, 0, bytes.length, received);
        received += bytes.length;
        if (total >= 0 && received > total) {
          total = -1;
        }
        if (report_progress && performance.now() - reported >= 100) {
          reported = performance.now();
          Module._pj_browser_fetch_progress(id, received, total);
        }
      }
      if (report_progress) {
        Module._pj_browser_fetch_progress(id, received, total);
      }
      status = kWriteFailed;
      if (received !== declared) {
        FS.ftruncate(stream.fd, received);
      }
      if (hash) {
        clearTimeout(timer);
        status = kHashFailed;
        const node = stream.node;
        const body = node.contents ? node.contents.subarray(0, node.usedBytes) : new Uint8Array(0);
        const digest = await crypto.subtle.digest('SHA-256', body);
        const buffer = Module._pj_browser_fetch_digest_buffer(id);
        if (!buffer) {
          return;
        }
        HEAPU8.set(new Uint8Array(digest), buffer);
      }
      status = kOk;
    } catch (error) {
      if (timedOut) {
        status = kIdleTimeout;
      }
    } finally {
      clearTimeout(timer);
      if (status !== kOk) {
        controller.abort();
      }
      if (stream) {
        try {
          FS.close(stream);
        } catch (error) {
        }
      }
      Module.pjBrowserFetches.delete(id);
      Module._pj_browser_fetch_done(id, status, received, httpStatus, fromCache ? 1 : 0);
    }
  })();
});

// Keeps the MEMFS file `path` in the 'pj-recordings-v1' cache under `cache_key`, best effort and in the background.
// The file is streamed from views of its MEMFS buffer, never copied whole, and that buffer stays readable after the
// caller removes the file (MEMFS drops only the directory entry) or reopens it for writing (truncating allocates a
// new buffer).
// Before storing, the longest-kept entries go until the cache stays under its budget; then the body is stored only
// when navigator.storage.estimate() leaves room for it. Each entry records its size and when it was stored in
// X-PJ-Size / X-PJ-Stored: a hit does not refresh it (that would rewrite the whole body), so the eviction order is
// download order. Every failure leaves the file unkept.
EM_JS(void, pj_browser_cache_store, (const char* cache_key_utf8, const char* path_utf8), {
  // What the kept recordings may occupy together.
  const kBudgetBytes = 1024 * 1024 * 1024;
  const kPieceBytes = 8 * 1024 * 1024;
  const key = UTF8ToString(cache_key_utf8);
  const path = UTF8ToString(path_utf8);
  let bytes = null;
  try {
    const node = FS.lookupPath(path).node;
    if (typeof caches !== 'undefined' && node.contents instanceof Uint8Array) {
      bytes = node.contents.subarray(0, node.usedBytes);
    }
  } catch (error) {
  }
  if (!bytes) {
    return;
  }
  (async () => {
    const cache = await caches.open('pj-recordings-v1');
    const entries = [];
    for (const request of await cache.keys()) {
      const kept = await cache.match(request);
      if (kept) {
        entries.push({ request, size: Number(kept.headers.get('X-PJ-Size')) || 0,
                       stored: Number(kept.headers.get('X-PJ-Stored')) || 0 });
      }
    }
    entries.sort((a, b) => a.stored - b.stored);
    let total = entries.reduce((sum, entry) => sum + entry.size, 0);
    while (entries.length > 0 && total + bytes.length > kBudgetBytes) {
      const oldest = entries.shift();
      await cache.delete(oldest.request);
      total -= oldest.size;
    }
    if (total + bytes.length > kBudgetBytes) {
      return;
    }
    if (navigator.storage && navigator.storage.estimate) {
      const estimate = await navigator.storage.estimate();
      if (estimate.quota - estimate.usage < bytes.length) {
        return;
      }
    }
    let offset = 0;
    const body = new ReadableStream({
      pull(queue) {
        if (offset >= bytes.length) {
          queue.close();
          return;
        }
        const end = Math.min(offset + kPieceBytes, bytes.length);
        queue.enqueue(bytes.subarray(offset, end));
        offset = end;
      },
    });
    const size = String(bytes.length);
    await cache.put(key, new Response(body, {
      headers: { 'Content-Length': size, 'X-PJ-Size': size, 'X-PJ-Stored': String(Date.now()) } }));
  })().catch(() => {});
});

// Drops the entry kept under `cache_key`, best effort.
EM_JS(void, pj_browser_cache_forget, (const char* cache_key_utf8), {
  const key = UTF8ToString(cache_key_utf8);
  if (typeof caches !== 'undefined') {
    caches.open('pj-recordings-v1').then(cache => cache.delete(key)).catch(() => {});
  }
});

EM_JS(void, pj_browser_fetch_abort, (unsigned id), {
  const controller = Module.pjBrowserFetches ? Module.pjBrowserFetches.get(id) : undefined;
  if (controller) {
    Module.pjBrowserFetches.delete(id);
    controller.abort();
  }
});
// clang-format on

// browserFetchToFile(), hashing the body only when `hash` is set.
quint32 startFetch(
    const QUrl& url, const QString& path, qint64 max_bytes, std::chrono::milliseconds idle_timeout, bool hash,
    std::function<void(BrowserFetchResult)> done, FetchProgress progress = {}, const QString& pinned_sha256 = {}) {
  const quint32 id = addTransfer(std::move(done), max_bytes, idle_timeout);
  const bool report_progress = static_cast<bool>(progress);
  transfers()[id].progress = std::move(progress);
  if (!isHttpUrl(url)) {
    complete(id, {.error = u"only http(s) URLs can be fetched: %1"_s.arg(url.toString())});
    return id;
  }
  Transfer& transfer = transfers()[id];
  if (hash) {
    transfer.hash = true;
    transfer.digest = QByteArray(kSha256Bytes, '\0');
    if (!pinned_sha256.isEmpty()) {
      transfer.pinned_sha256 = pinned_sha256;
      transfer.cache_key = recordingCacheKey(pinned_sha256, url);
      transfer.path = path;
    }
  }
  pj_browser_fetch_start(
      id, url.toString(QUrl::FullyEncoded).toUtf8().constData(), path.toUtf8().constData(),
      static_cast<double>(max_bytes), static_cast<int>(idle_timeout.count()), hash ? 1 : 0, report_progress ? 1 : 0,
      transfer.cache_key.toUtf8().constData());
  return id;
}

}  // namespace

quint32 browserFetchToFile(
    const QUrl& url, const QString& path, qint64 max_bytes, std::chrono::milliseconds idle_timeout,
    std::function<void(BrowserFetchResult)> done, FetchProgress progress, const QString& pinned_sha256) {
  Q_ASSERT(emscripten_is_main_browser_thread());
  return startFetch(
      url, path, max_bytes, idle_timeout, /*hash=*/true, std::move(done), std::move(progress), pinned_sha256);
}

quint32 browserFetchToBytes(
    const QUrl& url, qint64 max_bytes, std::chrono::milliseconds idle_timeout, const QObject* context,
    std::function<void(QByteArray bytes, QString error, bool limit_exceeded)> done) {
  Q_ASSERT(emscripten_is_main_browser_thread());
  // The body lands in a scratch file, removed with the last copy of the completion below.
  auto scratch = std::make_shared<QTemporaryFile>();
  auto context_watch = std::make_shared<QMetaObject::Connection>();
  auto deliver = [scratch, context_watch, done = std::move(done)](const BrowserFetchResult& result) {
    QObject::disconnect(*context_watch);
    if (!result.error.isEmpty()) {
      done({}, result.error, result.limit_exceeded);
      return;
    }
    scratch->close();  // reopened: the page wrote through its own handle
    if (!scratch->open()) {
      done({}, u"could not read the download: %1"_s.arg(scratch->errorString()), false);
      return;
    }
    done(scratch->readAll(), QString(), false);
  };
  quint32 id = 0;
  if (scratch->open()) {
    id = startFetch(url, scratch->fileName(), max_bytes, idle_timeout, /*hash=*/false, std::move(deliver));
  } else {
    id = addTransfer(std::move(deliver), max_bytes, idle_timeout);
    complete(id, {.error = u"could not create a download file: %1"_s.arg(scratch->errorString())});
  }
  *context_watch = QObject::connect(context, &QObject::destroyed, [id]() { cancelBrowserFetch(id); });
  return id;
}

void cancelBrowserFetch(quint32 id) {
  Q_ASSERT(emscripten_is_main_browser_thread());
  if (transfers().erase(id) > 0) {
    pj_browser_fetch_abort(id);
  }
}

}  // namespace PJ

// The page calls these back from pj_browser_fetch_start's script; each ignores a cancelled transfer.

/// Where the page writes the SHA-256 (32 bytes) of transfer `id`'s finished body; null once it is cancelled or when
/// the transfer does not hash.
extern "C" EMSCRIPTEN_KEEPALIVE char* pj_browser_fetch_digest_buffer(quint32 id) {
  const auto found = PJ::transfers().find(id);
  return found == PJ::transfers().end() || !found->second.hash ? nullptr : found->second.digest.data();
}

/// Transfer `id` has received `bytes` so far, of `total` (-1: unknown). Delivered from the event loop, like `done`, so
/// a progress queued before the completion runs before it.
extern "C" EMSCRIPTEN_KEEPALIVE void pj_browser_fetch_progress(quint32 id, double bytes, double total) {
  QTimer::singleShot(0, [id, received = static_cast<qint64>(bytes), declared = static_cast<qint64>(total)]() {
    const auto found = PJ::transfers().find(id);
    if (found == PJ::transfers().end() || !found->second.progress) {
      return;
    }
    const PJ::FetchProgress progress = found->second.progress;  // a copy: the callback may cancel the transfer
    progress(received, declared);
  });
}

/// The page's fetch for transfer `id` ended with `status` (a FetchStatus) after writing `bytes`, read from the kept
/// copy when `from_cache` is set. A download that matches its pin is kept, a kept copy that does not is dropped.
extern "C" EMSCRIPTEN_KEEPALIVE void pj_browser_fetch_done(
    quint32 id, int status, double bytes, int http_status, int from_cache) {
  const auto found = PJ::transfers().find(id);
  if (found == PJ::transfers().end()) {
    return;
  }
  const PJ::Transfer& transfer = found->second;
  PJ::BrowserFetchResult result{.size = static_cast<qint64>(bytes)};
  switch (static_cast<PJ::FetchStatus>(status)) {
    case PJ::FetchStatus::kOk:
      if (transfer.hash) {
        result.sha256 = QString::fromLatin1(transfer.digest.toHex());
      }
      if (!transfer.cache_key.isEmpty()) {
        const QByteArray key = transfer.cache_key.toUtf8();
        if (from_cache != 0 && result.sha256 != transfer.pinned_sha256) {
          PJ::pj_browser_cache_forget(key.constData());
        } else if (from_cache == 0 && result.sha256 == transfer.pinned_sha256) {
          PJ::pj_browser_cache_store(key.constData(), transfer.path.toUtf8().constData());
        }
      }
      break;
    case PJ::FetchStatus::kHttpError:
      result.error = u"the server answered HTTP %1"_s.arg(http_status);
      break;
    case PJ::FetchStatus::kTooLarge:
      result.limit_exceeded = true;
      result.error = PJ::downloadLimitMessage(transfer.max_bytes);
      break;
    case PJ::FetchStatus::kIdleTimeout:
      result.error = u"no data arrived for %1 ms"_s.arg(transfer.idle_timeout.count());
      break;
    case PJ::FetchStatus::kWriteFailed:
      result.error = u"the download could not be stored in the browser's memory"_s;
      break;
    case PJ::FetchStatus::kInsecureRedirect:
      result.error = u"an https download was redirected to http"_s;
      break;
    case PJ::FetchStatus::kHashFailed:
      result.error = u"the download could not be verified: this browser cannot compute its SHA-256"_s;
      break;
    case PJ::FetchStatus::kNetwork:
    default:
      // The browser hides why: an unreachable server and a CORS refusal look the same.
      result.error = u"network error, or the server does not let this page read the file"_s;
      break;
  }
  PJ::complete(id, std::move(result));
}
