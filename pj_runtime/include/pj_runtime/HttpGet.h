#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

/**
 * @file HttpGet.h
 * @brief One-shot async HTTP GET/POST with a transfer timeout, and a size-capped GET.
 *
 * The shared core of every "hit a URL once and react to the outcome" caller
 * (UpdateChecker's release check, the Preferences registry-URL reachability
 * probe, TelemetryPing's launch beacon): issue the request, deliver the
 * finished reply to a callback on the caller's thread, and own the reply's
 * cleanup. fetchBytesWithLimit() is the one size-capped GET that also runs in
 * the browser.
 */

#include <QByteArray>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>
#include <QtGlobal>
#include <chrono>
#include <functional>

#include "pj_runtime/HttpUrl.h"

class QNetworkAccessManager;
class QNetworkReply;
class QObject;

namespace PJ {

// Fires one GET for `request` (caller-configured: URL, headers, redirect
// policy) with the given transfer timeout, and invokes on_finished with the
// finished reply on `context`'s thread — including on error, abort, and
// timeout. The reply is deleteLater'd right after the callback returns, so the
// callback must not retain it. Returns the in-flight reply (non-owning; it may
// be ignored) so callers that supersede checks can abort() it.
QNetworkReply* httpGetWithTimeout(
    QNetworkAccessManager& network, QNetworkRequest request, std::chrono::milliseconds timeout, QObject* context,
    std::function<void(QNetworkReply&)> on_finished);

// POST sibling of httpGetWithTimeout with the identical callback/lifetime
// contract: `body` is sent with the caller-configured request (set the
// Content-Type header yourself), on_finished fires on `context`'s thread for
// every outcome, and the reply is deleteLater'd right after it returns.
QNetworkReply* httpPostWithTimeout(
    QNetworkAccessManager& network, QNetworkRequest request, const QByteArray& body, std::chrono::milliseconds timeout,
    QObject* context, std::function<void(QNetworkReply&)> on_finished);

// GET sibling of httpGetWithTimeout for untrusted bodies of bounded size: the transfer is
// aborted, from the event loop right after the reply signal that saw it, once the declared Content-Length (headers) or
// the bytes buffered so far exceed `max_bytes`. `finished` receives the finished reply and whether the cap ended it, in
// which case the reply carries OperationCanceledError (or, if the body completed first, its body) and must be treated
// as unusable; the final buffered size is checked too, so the flag is the one place a caller learns the body was over
// the cap.
//
// Same callback contract as httpGetWithTimeout (`context`'s thread, every outcome including a
// caller's `abort()`, reply deleteLater'd after the callback), plus: destroying `context`
// aborts the transfer and deletes the reply WITHOUT calling `finished`, so a callback capturing
// the context's state is never invoked afterwards. `context` must not be null. Redirect policy
// is the caller's, set on `request`. Desktop only: Qt's browser backend never frees a reply's
// body, so browser code goes through fetchBytesWithLimit().
QNetworkReply* httpGetWithLimit(
    QNetworkAccessManager& network, QNetworkRequest request, qint64 max_bytes, std::chrono::milliseconds timeout,
    QObject* context, std::function<void(QNetworkReply&, bool limit_exceeded)> finished);

/// How a fetchBytesWithLimit() transfer ended.
struct FetchedBytes {
  QByteArray bytes;  ///< The body; empty on failure.
  /// Where the body was served from after redirects. The browser follows redirects out of sight, so there it is
  /// the requested URL.
  QUrl final_url;
  QString error;                ///< Empty on success.
  bool limit_exceeded = false;  ///< The body was larger than `max_bytes`; `error` is downloadLimitMessage().
};

/// A transfer's bytes received so far and its total: the declared Content-Length, or -1 when unknown.
using FetchProgress = std::function<void(qint64 received, qint64 total)>;

/// Drops a fetchBytesWithLimit() transfer: its `done` never runs afterwards. Calling it again, or after `done`
/// ran, does nothing.
using CancelFetch = std::function<void()>;

/**
 * Size-capped GET of an untrusted body into memory, on desktop and in the browser. Only http(s) URLs are fetched
 * (anything else fails: native Qt would read file://). Redirects follow at most 4 hops and never from https to
 * http; in the browser the hop limit is the browser's. `timeout` is an inactivity timeout, not wall clock.
 *
 * `done` runs exactly once, later, on `context`'s thread, unless the returned CancelFetch runs or `context` is
 * destroyed first. Desktop: the GET goes through `network` (e.g. one with a disk cache), or, when null, through a
 * manager `context` owns and reuses across calls. The browser bypasses Qt's network backend (BrowserFetch.h) and
 * ignores `network`. `progress`, when set, runs as the desktop body arrives (never after `done`); the browser's
 * in-memory fetch reports none (browserFetchToFile() does). GUI thread only; `context` must not be null.
 */
CancelFetch fetchBytesWithLimit(
    const QUrl& url, qint64 max_bytes, std::chrono::milliseconds timeout, QObject* context,
    std::function<void(FetchedBytes)> done, QNetworkAccessManager* network = nullptr, FetchProgress progress = {});

/// The error of a download over `max_bytes`, worded in MiB.
QString downloadLimitMessage(qint64 max_bytes);

}  // namespace PJ
