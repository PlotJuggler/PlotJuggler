// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Shared async URL/file fetcher for the scene layers (robot-model kUrl source,
// SceneEntities ModelPrimitive URLs). Replaces the nested-QEventLoop blocking
// fetches those layers used to spin from attach()/render() paths.

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>
#include <functional>

#include "pj_scene3d_core/model_budget.h"

namespace pj::scene3d {

// Outcome of one UrlFetcher::fetch. `bytes` is meaningful only when `ok`.
struct FetchResult {
  bool ok{false};
  QByteArray bytes;
  QString error;
};

// Asynchronous byte fetcher with a hard size cap and bounded redirects.
//
// Contract (all of it load-bearing):
//  - GUI-thread only: construct, fetch(), and destroy on the thread whose event
//    loop delivers the callbacks.
//  - The callback is ALWAYS delivered asynchronously through the event loop —
//    even for local files and immediate errors (deferred via a queued
//    single-shot) — so a caller never re-enters its own stack from fetch().
//  - Local sources (file:// URLs and scheme-less bare paths) are read with
//    QFile inside the fetcher; only http/https ever reach the network; any
//    other scheme fails with "unsupported URL scheme". WASM checks the MEMFS
//    file size against kMaxFetchBytes before allocating its QByteArray; native
//    local-file behavior remains unchanged.
//  - Network requests use QNetworkRequest::setTransferTimeout (15 s) and
//    NoLessSafeRedirectPolicy capped at 4 redirects — bounded, with no
//    https->http downgrade. There is no host allowlist; the egress bound is the
//    scheme/redirect/size/timeout envelope itself, which a redirect cannot
//    escalate beyond. (WHETHER a data-supplied URL is fetched at all is the
//    caller's policy decision — see SceneEntitiesLayer's remote-fetch gate.)
//  - Native http(s) GETs are disk-cached (QNetworkDiskCache, set up in the constructor):
//    a fresh entry is served without the network, and a previously-fetched entry
//    is served from cache when the origin is unreachable, so a model fetched in a
//    prior session still renders offline. Local-file reads bypass the cache. The
//    cache directory is <AppData>/models, overridable via the PJ_MODEL_CACHE_DIR
//    environment variable. In WASM, the browser owns HTTP caching and enforces
//    CORS; the Qt disk-cache backend is deliberately absent.
//  - The response is aborted once it exceeds kMaxFetchBytes — checked against
//    the declared Content-Length as soon as the headers arrive, and against
//    the buffered byte count as the body streams in. An aborted (capped) response
//    is not committed to the cache (a later fetch re-hits the network).
//  - Destroying the fetcher aborts every in-flight request and drops its
//    callback: a callback can never fire after the fetcher is gone. Layers
//    rely on this — each owns its fetcher, so layer destruction cancels the
//    fetch; any extra QPointer guard in a callback is insurance only.
class UrlFetcher : public QObject {
  Q_OBJECT
 public:
  // Hard cap on accepted response bytes: the scene's model-source limit with the
  // QRhi renderer; the OpenGL renderer keeps its 64 MiB contract.
#ifdef PJ_SCENE3D_RHI
  static constexpr qint64 kMaxFetchBytes = static_cast<qint64>(kSceneLimits.max_model_source_bytes);
#else
  static constexpr qint64 kMaxFetchBytes = 64LL * 1024 * 1024;
#endif

  explicit UrlFetcher(QObject* parent = nullptr);

  // Start one fetch. `on_done` is invoked exactly once, via the event loop,
  // unless the fetcher is destroyed first (then never).
  void fetch(const QUrl& url, std::function<void(FetchResult)> on_done);

 private:
  // Queue `result` to `on_done` through the event loop — the never-synchronous
  // delivery guarantee for the local-file / immediate-error paths. Parented to
  // `this`, so destruction drops pending deliveries too.
  void deliverLater(std::function<void(FetchResult)> on_done, FetchResult result);

  QNetworkAccessManager manager_;
};

}  // namespace pj::scene3d
