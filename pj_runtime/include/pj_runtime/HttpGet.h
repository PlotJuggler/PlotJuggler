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
 * cleanup.
 */

#include <QByteArray>
#include <QNetworkRequest>
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
// aborted, from the event loop right after the reply signal that saw it, once the declared
// Content-Length (headers) or the bytes buffered so far exceed `max_bytes`. `finished` receives
// the finished reply and whether the cap ended it, in which case the reply carries
// OperationCanceledError (or, if the body completed first, its body) and must be treated as
// unusable; the final buffered size is checked too, so the flag is the one place a caller learns
// the body was over the cap. In the browser, Qt buffers the whole body before readyRead, so the
// cap bounds what the app keeps, not what the browser downloaded.
//
// Same callback contract as httpGetWithTimeout (`context`'s thread, every outcome including a
// caller's `abort()`, reply deleteLater'd after the callback), plus: destroying `context`
// aborts the transfer and deletes the reply WITHOUT calling `finished`, so a callback capturing
// the context's state is never invoked afterwards. `context` must not be null. Redirect policy
// is the caller's, set on `request`.
//
// A `context` that owns `network` must cancel its replies in its own destructor body: the
// manager deletes them before `context`'s destroyed fires, and a browser reply deleted while
// running aborts and emits finished, which would reach the callback mid-destruction.
QNetworkReply* httpGetWithLimit(
    QNetworkAccessManager& network, QNetworkRequest request, qint64 max_bytes, std::chrono::milliseconds timeout,
    QObject* context, std::function<void(QNetworkReply&, bool limit_exceeded)> finished);

}  // namespace PJ
