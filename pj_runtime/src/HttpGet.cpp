// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/HttpGet.h"

#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QTimer>
#include <memory>
#include <utility>

#ifdef PJ_TARGET_WASM
#include "pj_runtime/BrowserFetch.h"
#endif

using namespace Qt::StringLiterals;

namespace PJ {

namespace {

constexpr int kMaxRedirects = 4;

// Shared tail of every one-shot request: deliver the finished reply to the
// callback on `context`'s thread, then release it.
QNetworkReply* attachFinished(QNetworkReply* reply, QObject* context, std::function<void(QNetworkReply&)> on_finished) {
  QObject::connect(reply, &QNetworkReply::finished, context, [reply, callback = std::move(on_finished)]() {
    callback(*reply);
    reply->deleteLater();
  });
  return reply;
}

}  // namespace

QNetworkReply* httpGetWithTimeout(
    QNetworkAccessManager& network, QNetworkRequest request, std::chrono::milliseconds timeout, QObject* context,
    std::function<void(QNetworkReply&)> on_finished) {
  request.setTransferTimeout(static_cast<int>(timeout.count()));
  return attachFinished(network.get(request), context, std::move(on_finished));
}

QNetworkReply* httpPostWithTimeout(
    QNetworkAccessManager& network, QNetworkRequest request, const QByteArray& body, std::chrono::milliseconds timeout,
    QObject* context, std::function<void(QNetworkReply&)> on_finished) {
  request.setTransferTimeout(static_cast<int>(timeout.count()));
  return attachFinished(network.post(request, body), context, std::move(on_finished));
}

QNetworkReply* httpGetWithLimit(
    QNetworkAccessManager& network, QNetworkRequest request, qint64 max_bytes, std::chrono::milliseconds timeout,
    QObject* context, std::function<void(QNetworkReply&, bool limit_exceeded)> finished) {
  request.setTransferTimeout(static_cast<int>(timeout.count()));
  QNetworkReply* reply = network.get(request);

  // `exceeded` tells our own abort from a network error; `context_gone` mutes the callback while
  // the context's destructor aborts the reply (its connections are still live at that point).
  struct State {
    bool exceeded = false;
    bool context_gone = false;
  };
  auto state = std::make_shared<State>();

  // metaDataChanged for the declared length, readyRead for the bytes actually buffered; never
  // downloadProgress, which Qt throttles so a trickling body could slip past. Nothing reads the
  // reply before it finishes, so bytesAvailable() is the whole body so far.
  const auto enforce_cap = [reply, state, max_bytes]() {
    if (state->exceeded) {
      return;
    }
    const QVariant declared = reply->header(QNetworkRequest::ContentLengthHeader);
    if ((declared.isValid() && declared.toLongLong() > max_bytes) || reply->bytesAvailable() > max_bytes) {
      state->exceeded = true;
      // Never abort inside a reply signal: Qt's browser backend emits them holding the lock abort() takes.
      QMetaObject::invokeMethod(
          reply,
          [reply]() {
            if (!reply->isFinished()) {
              reply->abort();
            }
          },
          Qt::QueuedConnection);
    }
  };
  QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, enforce_cap);
  QObject::connect(reply, &QNetworkReply::readyRead, reply, enforce_cap);

  QObject::connect(
      reply, &QNetworkReply::finished, context, [reply, state, max_bytes, callback = std::move(finished)]() {
        if (state->context_gone) {
          return;
        }
        state->exceeded = state->exceeded || reply->bytesAvailable() > max_bytes;
        callback(*reply, state->exceeded);
      });
  // The reply's own cleanup does not depend on the context surviving.
  QObject::connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
  QObject::connect(context, &QObject::destroyed, reply, [reply, state]() {
    state->context_gone = true;
    if (!reply->isFinished()) {  // the context may die inside this reply's own finished
      reply->abort();
    }
  });
  return reply;
}

CancelFetch fetchBytesWithLimit(
    const QUrl& url, qint64 max_bytes, std::chrono::milliseconds timeout, QObject* context,
    std::function<void(FetchedBytes)> done, QNetworkAccessManager* network, FetchProgress progress) {
  auto cancelled = std::make_shared<bool>(false);
  auto deliver = [cancelled, done = std::move(done)](FetchedBytes fetched) {
    if (!*cancelled) {
      done(std::move(fetched));
    }
  };
  const CancelFetch cancel = [cancelled]() { *cancelled = true; };
  if (!isHttpUrl(url)) {
    QTimer::singleShot(0, context, [url, deliver = std::move(deliver)]() {
      deliver({.final_url = url, .error = u"only http(s) URLs can be fetched: %1"_s.arg(url.toString())});
    });
    return cancel;
  }
#ifdef PJ_TARGET_WASM
  Q_UNUSED(network);
  Q_UNUSED(progress);
  const quint32 id = browserFetchToBytes(
      url, max_bytes, timeout, context,
      [url, deliver = std::move(deliver)](QByteArray bytes, QString error, bool limit_exceeded) {
        deliver({std::move(bytes), url, std::move(error), limit_exceeded});
      });
  return [id]() { cancelBrowserFetch(id); };
#else
  if (network == nullptr) {
    network = context->findChild<QNetworkAccessManager*>(QString(), Qt::FindDirectChildrenOnly);
  }
  if (network == nullptr) {
    network = new QNetworkAccessManager(context);
  }
  QNetworkRequest request(url);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  request.setMaximumRedirectsAllowed(kMaxRedirects);
  const QPointer<QNetworkReply> reply = httpGetWithLimit(
      *network, request, max_bytes, timeout, context,
      [max_bytes, deliver = std::move(deliver)](QNetworkReply& finished, bool limit_exceeded) {
        FetchedBytes fetched{.final_url = finished.url(), .limit_exceeded = limit_exceeded};
        if (limit_exceeded) {
          fetched.error = downloadLimitMessage(max_bytes);
        } else if (finished.error() != QNetworkReply::NoError) {
          fetched.error = finished.errorString();
        } else {
          fetched.bytes = finished.readAll();
        }
        deliver(std::move(fetched));
      });
  if (progress) {
    // Qt already throttles downloadProgress (about every 100 ms, plus the last one).
    QObject::connect(
        reply, &QNetworkReply::downloadProgress, context,
        [cancelled, progress = std::move(progress)](qint64 received, qint64 total) {
          if (!*cancelled) {
            progress(received, total);
          }
        });
  }
  return [cancel, reply]() {
    cancel();
    if (reply && !reply->isFinished()) {
      reply->abort();  // finishes synchronously, muted by `cancel`
    }
  };
#endif
}

QString downloadLimitMessage(qint64 max_bytes) {
  return u"download exceeds the %1 MiB limit"_s.arg(max_bytes / (1024 * 1024));
}

}  // namespace PJ
