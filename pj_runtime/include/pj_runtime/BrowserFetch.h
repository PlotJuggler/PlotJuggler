#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

/**
 * @file BrowserFetch.h
 * @brief Browser-only (`PJ_TARGET_WASM`) HTTP GET that streams the body into a MEMFS file.
 *
 * Exists because Qt's browser `QNetworkReply` never frees its fetch buffer, so every body read through
 * `QNetworkAccessManager` stays in the wasm heap for the session (docs/TECHNICAL_NOTES.md). Here the page's
 * `fetch()` writes each received chunk straight into the file, which MEMFS keeps outside the wasm heap.
 */

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QtGlobal>
#include <chrono>
#include <functional>

#include "pj_runtime/HttpGet.h"

class QObject;

namespace PJ {

/// How a browserFetchToFile() transfer ended.
struct BrowserFetchResult {
  qint64 size = 0;              ///< Body bytes written to the file.
  QString sha256;               ///< Lowercase hex SHA-256 of those bytes; empty on failure.
  QString error;                ///< Empty on success.
  bool limit_exceeded = false;  ///< The body was larger than `max_bytes`; `error` words it in MiB.
};

/**
 * Fetches `url` (http/https only; anything else fails) into the MEMFS file `path`, created or truncated (its
 * directory must exist). The file is sized from Content-Length up front so MEMFS allocates it once, and the
 * SHA-256 is computed once the body is complete, by WebCrypto over the MEMFS buffer; a browser without it fails the
 * transfer. The transfer fails once the declared length or the bytes received
 * exceed `max_bytes`, when no chunk arrives for `idle_timeout`, on an HTTP error status, and when an https URL
 * is redirected to http. Other redirects follow the browser's own policy and limit (not the desktop's
 * `NoLessSafeRedirectPolicy` with 4 hops), out of the caller's sight: there is no final URL to report.
 * A response the loading page prefetched for `url` (`globalThis.pjPrefetch`) is read instead of fetching it once,
 * under the same rules; if that prefetch failed, the URL is fetched as usual.
 *
 * `progress`, when set, runs from the event loop as chunks arrive, at most about every 100 ms plus once when the
 * body has ended, always before `done` and never after a cancel. Its total is -1 when the response declares no length,
 * or a Content-Encoding (the page counts decoded bytes, Content-Length the encoded ones).
 *
 * `pinned_sha256`, the lowercase hex SHA-256 the caller requires of the body, lets the browser keep the file across
 * visits (Cache Storage, keyed on the pin and `url`): a kept copy is read instead of fetching, under the same rules,
 * and a download is kept only once its SHA-256 equals the pin. The fetch does not check the pin itself; a kept copy
 * that no longer matches it is dropped, and reported as read (the caller's pin check refuses it). Keeping is best
 * effort: no Cache Storage, no room or any storage error just means the next visit downloads again.
 *
 * `done` runs exactly once, later, from the event loop, unless cancelBrowserFetch() runs first. On failure the
 * file may hold part of the body: removing it is the caller's job, also after a cancel. Browser main (GUI)
 * thread only. Returns the id cancelBrowserFetch() takes.
 */
quint32 browserFetchToFile(
    const QUrl& url, const QString& path, qint64 max_bytes, std::chrono::milliseconds idle_timeout,
    std::function<void(BrowserFetchResult)> done, FetchProgress progress = {}, const QString& pinned_sha256 = {});

/**
 * browserFetchToFile() for a caller that wants the body in memory (a small file): it goes through a scratch MEMFS
 * file, removed once `done` has read it, so it is never held twice. `done(bytes, error, limit_exceeded)` follows
 * browserFetchToFile()'s rules, and is dropped, the transfer cancelled, when `context` is destroyed first.
 */
quint32 browserFetchToBytes(
    const QUrl& url, qint64 max_bytes, std::chrono::milliseconds idle_timeout, const QObject* context,
    std::function<void(QByteArray bytes, QString error, bool limit_exceeded)> done);

/// Aborts the transfer `id`; its `done` never runs. A finished or unknown id is ignored.
void cancelBrowserFetch(quint32 id);

}  // namespace PJ
