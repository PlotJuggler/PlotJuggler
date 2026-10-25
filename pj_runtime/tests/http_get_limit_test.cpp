// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// httpGetWithLimit against a loopback server that scripts the response: declared
// and undeclared lengths, a trickle that never ends, and a client that goes away.
// Plus isHttpUrl, the scheme gate in front of it.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <chrono>
#include <functional>
#include <memory>

#include "pj_runtime/HttpGet.h"

using namespace std::chrono_literals;

namespace {

// Answers every request with `head` + `body`, then either closes (`trickle` empty) or keeps
// the connection open appending `trickle` every few milliseconds until the client leaves.
class ScriptedServer {
 public:
  ScriptedServer() {
    server_.listen(QHostAddress::LocalHost, 0);
    QObject::connect(&server_, &QTcpServer::newConnection, [this]() { accept(server_.nextPendingConnection()); });
    QObject::connect(&trickle_timer_, &QTimer::timeout, [this]() {
      if (socket_ != nullptr && socket_->state() == QAbstractSocket::ConnectedState) {
        socket_->write(trickle_);
      }
    });
  }

  QUrl url() const {
    return QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server_.serverPort()));
  }

  void respond(const QByteArray& head, const QByteArray& body, const QByteArray& trickle = {}) {
    head_ = head;
    body_ = body;
    trickle_ = trickle;
  }

  int requests() const {
    return requests_;
  }

  bool clientLeft() const {
    return client_left_;
  }

 private:
  void accept(QTcpSocket* socket) {
    socket->setParent(&server_);
    socket_ = socket;
    QObject::connect(socket, &QTcpSocket::readyRead, [this, socket]() {
      request_ += socket->readAll();
      if (!request_.contains("\r\n\r\n")) {
        return;
      }
      request_.clear();
      ++requests_;
      socket->write(head_ + body_);
      socket->flush();
      if (trickle_.isEmpty()) {
        socket->disconnectFromHost();
      } else {
        trickle_timer_.start(5);
      }
    });
    QObject::connect(socket, &QTcpSocket::disconnected, [this]() {
      client_left_ = true;
      trickle_timer_.stop();
    });
  }

  QTcpServer server_;
  QTimer trickle_timer_;
  QPointer<QTcpSocket> socket_;
  QByteArray request_;
  QByteArray head_;
  QByteArray body_;
  QByteArray trickle_;
  int requests_ = 0;
  bool client_left_ = false;
};

QByteArray headWithLength(qint64 length) {
  return "HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(length) + "\r\nConnection: close\r\n\r\n";
}

const QByteArray kHeadWithoutLength = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n";

struct Outcome {
  int calls = 0;
  bool limit_exceeded = false;
  QNetworkReply::NetworkError error = QNetworkReply::NoError;
  QByteArray body;
};

std::function<void(QNetworkReply&, bool)> record(Outcome& outcome) {
  return [&outcome](QNetworkReply& reply, bool limit_exceeded) {
    ++outcome.calls;
    outcome.limit_exceeded = limit_exceeded;
    outcome.error = reply.error();
    if (reply.error() == QNetworkReply::NoError) {
      outcome.body = reply.readAll();
    }
  };
}

void flushDeferredDeletes() {
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

}  // namespace

TEST(HttpGetLimitTest, DeliversBodyUpToTheCap) {
  ScriptedServer server;
  const QByteArray body(64, 'x');
  server.respond(headWithLength(body.size()), body);
  QNetworkAccessManager network;
  QObject context;
  Outcome outcome;

  PJ::httpGetWithLimit(network, QNetworkRequest(server.url()), body.size(), 5s, &context, record(outcome));

  ASSERT_TRUE(QTest::qWaitFor([&] { return outcome.calls > 0; }));
  EXPECT_FALSE(outcome.limit_exceeded);
  EXPECT_EQ(outcome.error, QNetworkReply::NoError);
  EXPECT_EQ(outcome.body, body);
}

TEST(HttpGetLimitTest, RejectsDeclaredOversize) {
  ScriptedServer server;
  // The declared length alone must end the transfer: the body never arrives and the
  // connection stays open, so only an abort on the headers can finish this request.
  server.respond(headWithLength(1'000'000), QByteArray(16, 'x'), "y");
  QNetworkAccessManager network;
  QObject context;
  Outcome outcome;

  PJ::httpGetWithLimit(network, QNetworkRequest(server.url()), 1024, 30s, &context, record(outcome));

  ASSERT_TRUE(QTest::qWaitFor([&] { return outcome.calls > 0; }));
  EXPECT_EQ(outcome.calls, 1);
  EXPECT_TRUE(outcome.limit_exceeded);
  EXPECT_TRUE(QTest::qWaitFor([&] { return server.clientLeft(); }));
}

TEST(HttpGetLimitTest, BoundsUnknownLength) {
  ScriptedServer server;
  // No Content-Length and a body that never ends: 300 B every 5 ms, far below the cap per
  // read, so only the running total can trip the 1000 B limit.
  server.respond(kHeadWithoutLength, {}, QByteArray(300, 'x'));
  QNetworkAccessManager network;
  QObject context;
  Outcome outcome;

  PJ::httpGetWithLimit(network, QNetworkRequest(server.url()), 1000, 30s, &context, record(outcome));

  ASSERT_TRUE(QTest::qWaitFor([&] { return outcome.calls > 0; }));
  EXPECT_EQ(outcome.calls, 1);
  EXPECT_TRUE(outcome.limit_exceeded);
  EXPECT_TRUE(QTest::qWaitFor([&] { return server.clientLeft(); }));
}

// In the browser, reply signals run under the lock abort() takes, so the cap must abort from the
// event loop, never inside metaDataChanged or readyRead. Still exactly one size-error completion.
TEST(HttpGetLimitTest, CapAbortsOutsideTheReplySignals) {
  for (const bool declared : {true, false}) {
    ScriptedServer server;
    if (declared) {
      server.respond(headWithLength(1'000'000), QByteArray(16, 'x'), "y");
    } else {
      server.respond(kHeadWithoutLength, {}, QByteArray(300, 'x'));
    }
    QNetworkAccessManager network;
    QObject context;
    Outcome outcome;
    QNetworkReply* reply =
        PJ::httpGetWithLimit(network, QNetworkRequest(server.url()), 1000, 30s, &context, record(outcome));
    // Connected after the cap's handlers, so each runs right after them in the same emission.
    bool finished_inside_signal = false;
    const auto observe = [reply, &finished_inside_signal]() {
      finished_inside_signal = finished_inside_signal || reply->isFinished();
    };
    QObject::connect(reply, &QNetworkReply::metaDataChanged, observe);
    QObject::connect(reply, &QNetworkReply::readyRead, observe);

    ASSERT_TRUE(QTest::qWaitFor([&] { return outcome.calls > 0; })) << declared;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    EXPECT_EQ(outcome.calls, 1) << declared;
    EXPECT_TRUE(outcome.limit_exceeded) << declared;
    EXPECT_FALSE(finished_inside_signal) << "aborted inside a reply signal, declared=" << declared;
    EXPECT_TRUE(QTest::qWaitFor([&] { return server.clientLeft(); })) << declared;
  }
}

TEST(HttpGetLimitTest, CountsTheFinalBodyEvenWhenNoLengthIsDeclared) {
  ScriptedServer server;
  const QByteArray body(129, 'x');
  server.respond(kHeadWithoutLength, body);  // one write, then close: no sparse progress to count on
  QNetworkAccessManager network;
  QObject context;
  Outcome outcome;

  PJ::httpGetWithLimit(network, QNetworkRequest(server.url()), body.size() - 1, 5s, &context, record(outcome));

  ASSERT_TRUE(QTest::qWaitFor([&] { return outcome.calls > 0; }));
  EXPECT_TRUE(outcome.limit_exceeded);
}

TEST(HttpGetLimitTest, AbortAndDestroyDoNotDeliverStaleSuccess) {
  ScriptedServer server;
  server.respond(headWithLength(1'000'000), QByteArray(10, 'x'), QByteArray(10, 'x'));
  QNetworkAccessManager network;

  // The caller aborts the reply: finished still runs once, but as a failure.
  {
    QObject context;
    Outcome outcome;
    QNetworkReply* reply =
        PJ::httpGetWithLimit(network, QNetworkRequest(server.url()), 1 << 20, 30s, &context, record(outcome));
    QPointer<QNetworkReply> guard(reply);
    ASSERT_TRUE(QTest::qWaitFor([&] { return server.requests() == 1; }));
    reply->abort();
    ASSERT_TRUE(QTest::qWaitFor([&] { return outcome.calls > 0; }));
    EXPECT_EQ(outcome.calls, 1);
    EXPECT_NE(outcome.error, QNetworkReply::NoError);
    EXPECT_FALSE(outcome.limit_exceeded);
    flushDeferredDeletes();
    EXPECT_TRUE(guard.isNull());
  }

  // The context dies mid-transfer: nothing is delivered, the transfer stops, the reply goes.
  {
    ScriptedServer second_server;
    second_server.respond(headWithLength(1'000'000), QByteArray(10, 'x'), QByteArray(10, 'x'));
    auto context = std::make_unique<QObject>();
    Outcome outcome;
    QNetworkReply* reply = PJ::httpGetWithLimit(
        network, QNetworkRequest(second_server.url()), 1 << 20, 30s, context.get(), record(outcome));
    QPointer<QNetworkReply> guard(reply);
    ASSERT_TRUE(QTest::qWaitFor([&] { return second_server.requests() == 1; }));
    context.reset();
    EXPECT_TRUE(QTest::qWaitFor([&] { return second_server.clientLeft(); }));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    EXPECT_EQ(outcome.calls, 0);
    flushDeferredDeletes();
    EXPECT_TRUE(guard.isNull());
  }
}

TEST(IsHttpUrlTest, AcceptsOnlyAbsoluteHttpUrlsWithAHost) {
  EXPECT_TRUE(PJ::isHttpUrl(QUrl(QStringLiteral("http://example.com/a"))));
  EXPECT_TRUE(PJ::isHttpUrl(QUrl(QStringLiteral("HTTPS://example.com"))));
  EXPECT_FALSE(PJ::isHttpUrl(QUrl(QStringLiteral("file:///etc/passwd"))));
  EXPECT_FALSE(PJ::isHttpUrl(QUrl(QStringLiteral("ftp://example.com/a"))));
  EXPECT_FALSE(PJ::isHttpUrl(QUrl(QStringLiteral("presets/index.json"))));
  EXPECT_FALSE(PJ::isHttpUrl(QUrl(QStringLiteral("http:relative"))));
  EXPECT_FALSE(PJ::isHttpUrl(QUrl()));
}
