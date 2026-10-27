// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/BrowserFetch.h"

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>

#include <QByteArray>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QObject>
#include <QTemporaryFile>
#include <QTimer>
#include <memory>
#include <optional>
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
};

// The page copies each received chunk into the wasm heap in pieces of at most this size, to hash it.
constexpr int kHashPieceBytes = 256 * 1024;

struct Transfer {
  std::function<void(BrowserFetchResult)> done;
  /// Engaged only for a transfer that reports its SHA-256; `piece` is the page's copy buffer for it.
  std::optional<QCryptographicHash> hash;
  QByteArray piece;
  qint64 max_bytes = 0;
  std::chrono::milliseconds idle_timeout{0};
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
// A `piece_bytes` of 0 skips hashing. `status` tracks the phase so an exception maps to its FetchStatus. A body-less
// answer (a 204 may come without one) is an empty file. The macro joins the body into one line: every statement ends in
// a semicolon.
// clang-format off
EM_JS(void, pj_browser_fetch_start, (unsigned id, const char* url_utf8, const char* path_utf8, double max_bytes,
                                     int idle_timeout_ms, int piece_bytes), {
  const kOk = 0;
  const kNetwork = 1;
  const kHttpError = 2;
  const kTooLarge = 3;
  const kIdleTimeout = 4;
  const kWriteFailed = 5;
  const kInsecureRedirect = 6;
  const url = UTF8ToString(url_utf8);
  const path = UTF8ToString(path_utf8);
  const controller = new AbortController();
  if (!Module.pjBrowserFetches) {
    Module.pjBrowserFetches = new Map();
  }
  Module.pjBrowserFetches.set(id, controller);
  let status = kNetwork;
  let httpStatus = 0;
  let received = 0;
  let timedOut = false;
  let timer = 0;
  const arm = () => {
    clearTimeout(timer);
    timer = setTimeout(() => { timedOut = true; controller.abort(); }, idle_timeout_ms);
  };
  (async () => {
    let stream = null;
    try {
      arm();
      const response = await fetch(url, { signal: controller.signal });
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
        for (let offset = 0; piece_bytes > 0 && offset < bytes.length; offset += piece_bytes) {
          const piece = bytes.subarray(offset, offset + piece_bytes);
          const buffer = Module._pj_browser_fetch_piece(id);
          if (!buffer) {
            return;
          }
          HEAPU8.set(piece, buffer);
          Module._pj_browser_fetch_hash(id, piece.length);
        }
        received += bytes.length;
      }
      status = kWriteFailed;
      if (received !== declared) {
        FS.ftruncate(stream.fd, received);
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
      Module._pj_browser_fetch_done(id, status, received, httpStatus);
    }
  })();
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
    std::function<void(BrowserFetchResult)> done) {
  const quint32 id = addTransfer(std::move(done), max_bytes, idle_timeout);
  if (!isHttpUrl(url)) {
    complete(id, {.error = u"only http(s) URLs can be fetched: %1"_s.arg(url.toString())});
    return id;
  }
  if (hash) {
    Transfer& transfer = transfers()[id];
    transfer.hash.emplace(QCryptographicHash::Sha256);
    transfer.piece = QByteArray(kHashPieceBytes, Qt::Uninitialized);
  }
  pj_browser_fetch_start(
      id, url.toString(QUrl::FullyEncoded).toUtf8().constData(), path.toUtf8().constData(),
      static_cast<double>(max_bytes), static_cast<int>(idle_timeout.count()), hash ? kHashPieceBytes : 0);
  return id;
}

}  // namespace

quint32 browserFetchToFile(
    const QUrl& url, const QString& path, qint64 max_bytes, std::chrono::milliseconds idle_timeout,
    std::function<void(BrowserFetchResult)> done) {
  Q_ASSERT(emscripten_is_main_browser_thread());
  return startFetch(url, path, max_bytes, idle_timeout, /*hash=*/true, std::move(done));
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

/// Where the page copies the next piece of transfer `id`'s body to hash it; null once it is cancelled.
extern "C" EMSCRIPTEN_KEEPALIVE char* pj_browser_fetch_piece(quint32 id) {
  const auto found = PJ::transfers().find(id);
  return found == PJ::transfers().end() || !found->second.hash ? nullptr : found->second.piece.data();
}

/// Adds the first `bytes` of the piece buffer to transfer `id`'s hash.
extern "C" EMSCRIPTEN_KEEPALIVE void pj_browser_fetch_hash(quint32 id, int bytes) {
  const auto found = PJ::transfers().find(id);
  if (found != PJ::transfers().end() && found->second.hash) {
    found->second.hash->addData(QByteArrayView(found->second.piece.constData(), bytes));
  }
}

/// The page's fetch for transfer `id` ended with `status` (a FetchStatus) after writing `bytes`.
extern "C" EMSCRIPTEN_KEEPALIVE void pj_browser_fetch_done(quint32 id, int status, double bytes, int http_status) {
  const auto found = PJ::transfers().find(id);
  if (found == PJ::transfers().end()) {
    return;
  }
  const PJ::Transfer& transfer = found->second;
  PJ::BrowserFetchResult result{.size = static_cast<qint64>(bytes)};
  switch (static_cast<PJ::FetchStatus>(status)) {
    case PJ::FetchStatus::kOk:
      if (transfer.hash) {
        result.sha256 = QString::fromLatin1(transfer.hash->result().toHex());
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
    case PJ::FetchStatus::kNetwork:
    default:
      // The browser hides why: an unreachable server and a CORS refusal look the same.
      result.error = u"network error, or the server does not let this page read the file"_s;
      break;
  }
  PJ::complete(id, std::move(result));
}
