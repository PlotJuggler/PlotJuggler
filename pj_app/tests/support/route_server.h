#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A loopback HTTP server for the preset tests: fixed bodies by path, 404
// otherwise. A held path answers only on release(), which lets a test act
// while that download is in flight. GUI thread only.

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <utility>

namespace pj_app_test {

class RouteServer {
 public:
  RouteServer() {
    server_.listen(QHostAddress::LocalHost, 0);
    QObject::connect(&server_, &QTcpServer::newConnection, [this]() {
      QTcpSocket* socket = server_.nextPendingConnection();
      socket->setParent(&server_);
      QObject::connect(socket, &QTcpSocket::readyRead, [this, socket]() {
        QByteArray& request = pending_[socket];
        request += socket->readAll();
        if (!request.contains("\r\n\r\n")) {
          return;
        }
        const QString path = QString::fromLatin1(request.split(' ').value(1));
        pending_.remove(socket);
        requests.push_back(path);
        if (held_.contains(path)) {
          parked_.push_back({socket, path});
        } else {
          answer(socket, path);
        }
      });
    });
  }

  [[nodiscard]] QUrl url(const QString& path) const {
    return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(server_.serverPort()).arg(path));
  }
  void route(const QString& path, const QByteArray& body) {
    routes_.insert(path, body);
  }
  void hold(const QString& path) {
    held_.insert(path);
  }
  void release() {
    held_.clear();
    for (const auto& [socket, path] : std::as_const(parked_)) {
      if (!socket.isNull()) {
        answer(socket, path);
      }
    }
    parked_.clear();
  }

  // Every request path, in arrival order.
  QStringList requests;

 private:
  void answer(QTcpSocket* socket, const QString& path) const {
    const auto route = routes_.constFind(path);
    const QByteArray body = route != routes_.cend() ? route.value() : QByteArray("missing");
    const QByteArray status = route != routes_.cend() ? "200 OK" : "404 Not Found";
    socket->write(
        "HTTP/1.1 " + status + "\r\nContent-Length: " + QByteArray::number(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body);
    socket->flush();
    socket->disconnectFromHost();
  }

  QTcpServer server_;
  QHash<QTcpSocket*, QByteArray> pending_;
  QHash<QString, QByteArray> routes_;
  QSet<QString> held_;
  QList<std::pair<QPointer<QTcpSocket>, QString>> parked_;
};

}  // namespace pj_app_test
