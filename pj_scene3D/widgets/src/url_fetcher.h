// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Shared async URL/file fetcher for the scene layers (robot-model kUrl source, SceneEntities ModelPrimitive URLs);
// never spins a nested QEventLoop.

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>
#include <functional>

#include "pj_scene3d_core/model_budget.h"

class QNetworkAccessManager;

namespace pj::scene3d {

struct FetchResult {
  bool ok{false};
  QByteArray bytes;
  QString error;
  // Where a network fetch was served from after redirects (in the browser, the
  // requested URL: it follows redirects out of sight); empty for a local source.
  QUrl final_url;
};

// Whether UrlFetcher::fetch() requests an http(s) URL, and which redirects it follows.
enum class RemoteFetch : quint8 {
  kRefused,    // fails without a request
  kOwnOrigin,  // follows only redirects within the URL's own scheme, host and port (in the browser, none)
  kAnyOrigin,  // follows PJ::RedirectScope::kAnyOrigin redirects
};

// Asynchronous byte fetcher with a hard size cap and bounded redirects.
//
// Contract (all of it load-bearing):
//  - GUI-thread only: construct, fetch() and destroy on the thread whose event loop delivers the callbacks.
//  - The callback is ALWAYS delivered asynchronously through the event loop, even for local files and immediate errors
//    (deferred via a queued single-shot), so a caller never re-enters its own stack from fetch().
//  - Local sources (file:// URLs on this host and scheme-less bare paths) are read with QFile inside the fetcher; only
//    http(s) URLs (PJ::isHttpUrl) reach the network. A network share (local_path.h) fails with "network share
//    refused", any other scheme with "unsupported URL scheme". WASM checks the MEMFS file size against
//    kMaxFetchBytes before allocating its QByteArray.
//  - Network requests go through pj_runtime's PJ::fetchBytesWithLimit (HttpGet.h: an inactivity timeout, a bounded
//    size). Whether an http(s) URL is requested, and whether a redirect may leave its origin, is the caller's
//    RemoteFetch, passed with each fetch(); the fetcher keeps no host list of its own.
//  - Native http(s) GETs are disk-cached (QNetworkDiskCache, set up in the constructor): a fresh entry is served
//    without the network, and a previously fetched entry is served from cache when the origin is unreachable, so a
//    model fetched in a prior session renders offline. Local-file reads bypass the cache. The directory is
//    <AppData>/models, overridable via PJ_MODEL_CACHE_DIR. In WASM the browser owns HTTP caching and enforces CORS;
//    the Qt disk-cache backend is absent.
//  - The response is aborted once it exceeds kMaxFetchBytes, checked against the declared Content-Length when the
//    headers arrive and against the buffered byte count as the body streams in. An aborted response is not committed
//    to the cache (a later fetch re-hits the network).
//  - Destroying the fetcher aborts every in-flight request and drops its callback: a callback never fires after the
//    fetcher is gone. Layers rely on this (each owns its fetcher, so layer destruction cancels the fetch); an extra
//    QPointer guard in a callback is insurance only.
class UrlFetcher : public QObject {
  Q_OBJECT
 public:
  static constexpr qint64 kMaxFetchBytes = static_cast<qint64>(kSceneLimits.max_model_source_bytes);

  explicit UrlFetcher(QObject* parent = nullptr);

  // `on_done` is invoked exactly once, via the event loop, unless the fetcher is destroyed first (then never).
  void fetch(const QUrl& url, std::function<void(FetchResult)> on_done, RemoteFetch remote);

 private:
  // Queues `result` to `on_done` through the event loop (the never-synchronous delivery guarantee for the local-file /
  // immediate-error paths). Parented to `this`, so destruction drops pending deliveries too.
  void deliverLater(std::function<void(FetchResult)> on_done, FetchResult result);

  QNetworkAccessManager* network_ = nullptr;
};

}  // namespace pj::scene3d
