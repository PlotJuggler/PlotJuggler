// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "mcp/McpBrowserLink.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QUrlQuery>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <utility>

#include "mcp/McpProtocol.h"
#include "mcp/McpServer.h"

using json = nlohmann::json;  // NOLINT(readability-identifier-naming)
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

/// The relay holds a poll up to 20 s; past this the poll is given up and sent again.
constexpr int kPollTimeoutMs = 30'000;
constexpr int kRequestTimeoutMs = 10'000;
constexpr int kMaxBackoffMs = 10'000;

int statusOf(const QNetworkReply& reply) {
  return reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

}  // namespace

McpBrowserLink::McpBrowserLink(McpServer& server, QUrl relay, QString title, QObject* parent)
    : QObject(parent),
      server_(server),
      relay_(std::move(relay)),
      key_(QUrlQuery(relay_).queryItemValue(u"key"_s, QUrl::FullyDecoded).toUtf8()),
      title_(std::move(title)) {
  hello();
}

bool McpBrowserLink::acceptableRelay(const QUrl& url) {
  const QString host = url.host();
  return url.isValid() && url.scheme() == u"http"_s && url.port() > 0 && url.userInfo().isEmpty() &&
         (host == u"127.0.0.1"_s || host == u"localhost"_s || host == u"::1"_s);
}

QUrl McpBrowserLink::endpoint(const QString& path) const {
  QUrl url = relay_;
  url.setPath(path);
  url.setQuery(id_.isEmpty() ? QUrlQuery() : QUrlQuery({{u"id"_s, id_}}));
  return url;
}

QNetworkRequest McpBrowserLink::request(const QUrl& url, int timeout_ms) const {
  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
  // The relay's secret, in a header no website can send without passing the relay's CORS preflight.
  request.setRawHeader("X-PJ-Link", key_);
  request.setTransferTimeout(timeout_ms);
  return request;
}

void McpBrowserLink::retryLater(void (McpBrowserLink::*next)()) {
  backoff_ms_ = std::clamp(backoff_ms_ * 2, 1000, kMaxBackoffMs);
  QTimer::singleShot(backoff_ms_, this, next);
}

void McpBrowserLink::hello() {
  id_.clear();
#ifdef PJ_TARGET_WASM
  constexpr bool kBrowser = true;
#else
  constexpr bool kBrowser = false;
#endif
  // The relay keeps a browser page's tool list for its next sessions (it differs from the desktop's).
  const QByteArray body =
      QByteArray::fromStdString(json({{"title", title_.toStdString()}, {"protocol", 1}, {"browser", kBrowser}}).dump());
  QNetworkReply* reply = network_.post(request(endpoint(u"/link/hello"_s), kRequestTimeoutMs), body);
  connect(reply, &QNetworkReply::finished, this, [this, reply] {
    reply->deleteLater();
    const json answer = json::parse(reply->readAll().toStdString(), nullptr, false);
    if (reply->error() != QNetworkReply::NoError || !answer.is_object() || !answer.value("id", json()).is_string()) {
      retryLater(&McpBrowserLink::hello);
      return;
    }
    id_ = QString::fromStdString(answer["id"].get<std::string>());
    backoff_ms_ = 0;
    qCInfo(mcp::lcMcp).noquote() << "linked to the agent relay at"
                                 << relay_.toString(QUrl::RemoveQuery);  // not its secret
    poll();
  });
}

void McpBrowserLink::poll() {
  QNetworkReply* reply = network_.get(request(endpoint(u"/link/next"_s), kPollTimeoutMs));
  connect(reply, &QNetworkReply::finished, this, [this, reply] {
    reply->deleteLater();
    const int status = statusOf(*reply);
    if (status == 404) {
      hello();  // the relay restarted and forgot this page
      return;
    }
    if (reply->error() != QNetworkReply::NoError) {
      retryLater(&McpBrowserLink::poll);
      return;
    }
    backoff_ms_ = 0;
    if (status == 200) {
      const json item = json::parse(reply->readAll().toStdString(), nullptr, false);
      if (item.is_object() && item.value("seq", json()).is_number_integer() && item.contains("message")) {
        const qint64 seq = item["seq"].get<qint64>();
        const QString client = QString::fromStdString(item.value("client", std::string()));
        // The server's reply may come much later: by then this link may be gone (QPointer).
        server_.handleRelayed(
            QByteArray::fromStdString(item["message"].dump()), client, this,
            [self = QPointer<McpBrowserLink>(this), seq](const QByteArray& response) {
              if (self != nullptr) {
                self->reply(seq, response);
              }
            });
      }
    }
    poll();
  });
}

void McpBrowserLink::reply(qint64 seq, const QByteArray& response) {
  json body = {{"seq", seq}, {"response", nullptr}};
  if (!response.isEmpty()) {
    body["response"] = json::parse(response.toStdString(), nullptr, false);
  }
  QNetworkReply* sent = network_.post(
      request(endpoint(u"/link/reply"_s), kRequestTimeoutMs),
      QByteArray::fromStdString(body.dump(-1, ' ', false, json::error_handler_t::replace)));
  // Nothing to retry: a lost answer times out on the agent's side, which tells it to poll get_status.
  connect(sent, &QNetworkReply::finished, sent, &QObject::deleteLater);
}

}  // namespace PJ
