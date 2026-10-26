// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Shared scaffolding for the scene3D layer/reconstructor tests that drive a real
// SessionManager + ObjectStore through a mock object parser. Collapses the
// per-file copies of: the MessageParserHandle/vtable/bindSchema boilerplate
// (makeBoundHandle), the per-parse-counting mock parser (CountingObjectParser),
// topic registration (registerObjectTopic), and the async-decode event pump
// (pumpUntil). Header-only; consumers already link pj_scene3d_widgets + GTest.

#include <gtest/gtest.h>

#include <QCoreApplication>
#ifdef PJ_SCENE3D_FIXTURES_DIR
#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#endif
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "pj_base/buffer_anchor.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"

namespace pj::scene3d::test {

// A MessageParserPluginBase mock registering ONE schema handler that delegates to
// a caller-supplied `emit` (the genuinely test-specific object construction —
// SAME signature as SchemaHandler::parse_object) and, when `counter` is non-null,
// bumps it once per parse so a rebind/coalescing test can tell which parser
// instance a decode reached. `emit` and `counter` are typically file-scope (a
// free function + a global atomic) because vtableWithCreate's create_fn must be
// captureless.
template <typename EmitFn>
class CountingObjectParser : public PJ::MessageParserPluginBase {
 public:
  // NOTE: the callable param is named emit_fn, NOT emit — `emit` is a Qt
  // keyword-macro (#define emit) that would expand to nothing here.
  CountingObjectParser(
      std::string_view schema, PJ::sdk::BuiltinObjectType object_type, std::atomic<int>* counter, EmitFn emit_fn) {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = object_type;
    handler.parse_object = [counter, emit_fn](
                               PJ::Timestamp ts, PJ::sdk::PayloadView payload) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      if (counter != nullptr) {
        counter->fetch_add(1, std::memory_order_relaxed);
      }
      return emit_fn(ts, payload);
    };
    registerSchemaHandler(schema, std::move(handler));
  }
};

// Build a bound MessageParserHandle for `schema`. `create_fn` MUST be a distinct
// captureless lambda type per call site: vtableWithCreate caches one `static`
// vtable per CreateFn instantiation, so a shared function-pointer type would
// latch the first create function for every handle. Asserts valid + bound.
template <typename CreateFn>
std::unique_ptr<PJ::MessageParserHandle> makeBoundHandle(
    std::string_view schema, CreateFn create_fn,
    const char* manifest = R"({"id":"mock","name":"Mock Parser","version":"1.0.0","encoding":["mock"]})") {
  auto handle =
      std::make_unique<PJ::MessageParserHandle>(PJ::MessageParserPluginBase::vtableWithCreate(create_fn, manifest));
  EXPECT_TRUE(handle->valid());
  EXPECT_TRUE(handle->bindSchema(schema, {}).has_value());
  return handle;
}

// Push one LAZY entry whose fetcher re-produces `payload` on every resolve and
// bumps *counter — the scaffolding of the resolve-before-memo regressions: a
// test renders twice and asserts the count did not grow. Pair it with an
// out-of-order push (which resets the store's warm latestAt cache), or a
// redundant resolve hides behind that cache and the test proves nothing.
inline void pushLazyCounting(
    PJ::ObjectStore& store, PJ::ObjectTopicId topic, PJ::Timestamp timestamp, std::vector<uint8_t> payload,
    std::shared_ptr<int> counter) {
  ASSERT_TRUE(store
                  .pushLazy(
                      topic, timestamp,
                      [payload = std::move(payload), counter = std::move(counter)]() -> PJ::sdk::PayloadView {
                        ++*counter;
                        return PJ::sdk::makePayloadView(payload);
                      })
                  .has_value());
}

// Register one ObjectStore topic (dataset_id default 1) and return its id. EXPECT
// (not ASSERT — this returns a value) flags a registration failure.
inline PJ::ObjectTopicId registerObjectTopic(
    PJ::SessionManager& session, const std::string& topic_name, uint32_t dataset_id = 1) {
  const auto topic = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{.dataset_id = dataset_id, .topic_name = topic_name, .metadata_json = "{}"});
  EXPECT_TRUE(topic.has_value());
  return topic.has_value() ? topic.value() : PJ::ObjectTopicId{};
}

// Pump the Qt event loop until `predicate()` holds or `timeout` elapses — drains
// a QtConcurrent + QFutureWatcher async decode onto the GUI thread. Returns the
// final predicate value so the caller can assert with its own message.
template <typename Predicate>
bool pumpUntil(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      break;
    }
    QCoreApplication::processEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return predicate();
}

#ifdef PJ_SCENE3D_FIXTURES_DIR
// The fixture runner's (pj_scene3d_widget_fixture_tests, which links Qt6::Network).

// `count` distinct copies of the cube fixture in `dir`: MeshLoader caches by
// path, and a cached future would finish without occupying a worker.
inline std::vector<QString> cubeCopies(const QTemporaryDir& dir, int count) {
  std::vector<QString> paths;
  for (int index = 0; index < count; ++index) {
    const QString path = dir.filePath(QStringLiteral("cube_%1.stl").arg(index));
    EXPECT_TRUE(QFile::copy(QStringLiteral(PJ_SCENE3D_FIXTURES_DIR "/meshes/cube.stl"), path));
    paths.push_back(path);
  }
  return paths;
}

// The bytes of tests/fixtures/meshes/`name`; a missing fixture fails the test.
inline QByteArray readFixtureBytes(const char* name) {
  QFile file(QStringLiteral(PJ_SCENE3D_FIXTURES_DIR "/meshes/") + QLatin1String(name));
  EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << "missing fixture: " << name;
  return file.readAll();
}

// Serves `files` over HTTP on localhost by request path (404 for any other) and
// records every requested path.
class FileHttpServer {
 public:
  explicit FileHttpServer(QHash<QString, QByteArray> files) : files_(std::move(files)) {
    EXPECT_TRUE(server_.listen(QHostAddress::LocalHost, 0));
    QObject::connect(&server_, &QTcpServer::newConnection, &server_, [this]() {
      QTcpSocket* socket = server_.nextPendingConnection();
      QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
        const QString path = QString::fromLatin1(socket->readAll().split(' ').value(1));
        paths_.push_back(path);
        const auto file = files_.constFind(path);
        const bool found = file != files_.cend();
        const QByteArray body = found ? *file : QByteArray();
        socket->write(
            QByteArray(found ? "HTTP/1.1 200 OK" : "HTTP/1.1 404 Not Found") +
            "\r\nConnection: close\r\nCache-Control: no-store\r\nContent-Length: " + QByteArray::number(body.size()) +
            "\r\n\r\n" + body);
        socket->disconnectFromHost();
      });
    });
  }
  [[nodiscard]] QString url(const QString& path) const {
    return QStringLiteral("http://127.0.0.1:%1%2").arg(server_.serverPort()).arg(path);
  }
  [[nodiscard]] const QStringList& paths() const {
    return paths_;
  }

 private:
  QHash<QString, QByteArray> files_;
  QTcpServer server_;
  QStringList paths_;
};

// An http URL for `name` on `server`, which listens on localhost from the first
// call on: it accepts connections and never responds, so a fetch stays in flight.
inline QString silentServerUrl(QTcpServer& server, const QString& name) {
  EXPECT_TRUE(server.isListening() || server.listen(QHostAddress::LocalHost, 0));
  return QStringLiteral("http://127.0.0.1:%1/%2").arg(server.serverPort()).arg(name);
}
#endif

}  // namespace pj::scene3d::test
