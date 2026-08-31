// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Tests for PJ::RegistryManager
//
// Coverage:
//   [1] Downloads JSON from a local test URL
//   [2] Parses JSON correctly and constructs Extension objects
//   [3] Emits fetchStarted, fetchFinished, and fetchError with the right values
//   [4] Handles network errors gracefully (connection refused, invalid JSON, missing fields)

#include "pj_marketplace/registry_manager.hpp"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

// ---------------------------------------------------------------------------
// Minimal HTTP/1.1 server — serves one fixed JSON body per connection
// ---------------------------------------------------------------------------

class TestHttpServer : public QTcpServer {
 public:
  explicit TestHttpServer(QObject* parent = nullptr) : QTcpServer(parent) {
    connect(this, &QTcpServer::newConnection, this, &TestHttpServer::onNewConnection);
  }

  void setResponseBody(const QByteArray& body) {
    body_ = body;
  }

  // Returns the base URL after a successful listen()
  QUrl url() const {
    return QUrl(QString("http://127.0.0.1:%1").arg(serverPort()));
  }

 private:
  void onNewConnection() {
    QTcpSocket* socket = nextPendingConnection();
    connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
      socket->readAll();  // discard the HTTP request — content doesn't matter for tests

      QByteArray response;
      response += "HTTP/1.1 200 OK\r\n";
      response += "Content-Type: application/json\r\n";
      response += "Content-Length: " + QByteArray::number(body_.size()) + "\r\n";
      response += "Connection: close\r\n";
      response += "\r\n";
      response += body_;

      socket->write(response);
      socket->flush();
      socket->disconnectFromHost();
      socket->deleteLater();
    });
  }

  QByteArray body_;
};

// ---------------------------------------------------------------------------
// JSON fixtures
// ---------------------------------------------------------------------------

// Full extension with every supported field populated
static const QByteArray kFullRegistryJson = R"({
  "extensions": [
    {
      "id": "csv-loader",
      "name": "CSV Loader",
      "version": "1.2.0",
      "description": "Load CSV/TSV files",
      "author": "Test Author",
      "publisher": "test-org",
      "license": "MIT",
      "website": "https://example.com",
      "repository": "https://github.com/example/csv-loader",
      "icon_url": "https://example.com/icon.png",
      "category": "data_loader",
      "min_sdk_required": "0.21.0",
      "min_plotjuggler_version": "3.8.0",
      "tags": ["csv", "tsv", "file"],
      "platforms": {
        "linux-x86_64": {
          "url": "https://example.com/csv-loader-linux.so",
          "checksum": "sha256:deadbeef"
        },
        "windows-x86_64": {
          "url": "https://example.com/csv-loader-win.dll",
          "checksum": "sha256:cafebabe"
        }
      },
      "changelog": {
        "1.2.0": "Added TSV support",
        "1.0.0": "Initial release"
      }
    }
  ]
})";

// Two minimal extensions (required fields only)
static const QByteArray kMultiExtensionJson = R"({
  "extensions": [
    { "id": "ext-a", "name": "Extension A", "version": "1.0.0" },
    { "id": "ext-b", "name": "Extension B", "version": "2.3.1" }
  ]
})";

static const QByteArray kEmptyExtensionsJson = R"({ "extensions": [] })";

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

namespace PJ {
namespace {

class RegistryManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    server_ = new TestHttpServer();
    ASSERT_TRUE(server_->listen(QHostAddress::LocalHost))
        << "TestHttpServer failed to bind — check that a loopback interface is available";
  }

  void TearDown() override {
    server_->close();
    delete server_;
  }

  TestHttpServer* server_ = nullptr;
};

// ---------------------------------------------------------------------------
// [3] Signals — fetchStarted
// ---------------------------------------------------------------------------

// fetchStarted is emitted synchronously, before any I/O takes place
TEST_F(RegistryManagerTest, EmitsFetchStartedImmediatelyOnCall) {
  RegistryManager mgr;
  QSignalSpy spy(&mgr, &RegistryManager::fetchStarted);

  server_->setResponseBody(kFullRegistryJson);
  mgr.fetchRegistry(server_->url());

  // No event loop processing needed — must already be emitted
  EXPECT_EQ(spy.count(), 1);
}

// Two consecutive calls each emit fetchStarted
TEST_F(RegistryManagerTest, EmitsFetchStartedOnEachCall) {
  RegistryManager mgr;
  QSignalSpy spy_started(&mgr, &RegistryManager::fetchStarted);
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody(kFullRegistryJson);
  mgr.fetchRegistry(server_->url());
  mgr.fetchRegistry(server_->url());  // cancels the previous one and starts fresh

  EXPECT_EQ(spy_started.count(), 2);

  // Wait for the second request to complete
  ASSERT_TRUE(spy_finished.wait(3000));
}

// ---------------------------------------------------------------------------
// [1] + [3] Download and fetchFinished(true) on success
// ---------------------------------------------------------------------------

TEST_F(RegistryManagerTest, EmitsFetchFinishedTrueOnSuccessfulDownload) {
  RegistryManager mgr;
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody(kFullRegistryJson);
  mgr.fetchRegistry(server_->url());

  ASSERT_TRUE(spy_finished.wait(3000)) << "fetchFinished was not emitted within 3 seconds";
  ASSERT_EQ(spy_finished.count(), 1);
  EXPECT_TRUE(spy_finished.first().at(0).toBool());
}

// ---------------------------------------------------------------------------
// [2] Parsing — required fields
// ---------------------------------------------------------------------------

TEST_F(RegistryManagerTest, ParsesRequiredFields) {
  RegistryManager mgr;
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody(kFullRegistryJson);
  mgr.fetchRegistry(server_->url());
  ASSERT_TRUE(spy_finished.wait(3000));

  const QList<Extension> exts = mgr.extensions();
  ASSERT_EQ(exts.size(), 1);

  const Extension& ext = exts.at(0);
  EXPECT_EQ(ext.id, "csv-loader");
  EXPECT_EQ(ext.name, "CSV Loader");
  EXPECT_EQ(ext.version, "1.2.0");
}

// [2] Optional string fields
TEST_F(RegistryManagerTest, ParsesOptionalStringFields) {
  RegistryManager mgr;
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody(kFullRegistryJson);
  mgr.fetchRegistry(server_->url());
  ASSERT_TRUE(spy_finished.wait(3000));

  const QList<Extension> exts = mgr.extensions();
  ASSERT_EQ(exts.size(), 1);

  const Extension& ext = exts.at(0);
  EXPECT_EQ(ext.description, "Load CSV/TSV files");
  EXPECT_EQ(ext.author, "Test Author");
  EXPECT_EQ(ext.publisher, "test-org");
  EXPECT_EQ(ext.license, "MIT");
  EXPECT_EQ(ext.website, "https://example.com");
  EXPECT_EQ(ext.repository, "https://github.com/example/csv-loader");
  EXPECT_EQ(ext.icon_url, "https://example.com/icon.png");
  EXPECT_EQ(ext.category, "data_loader");
  EXPECT_EQ(ext.min_sdk_required, "0.21.0");
  EXPECT_EQ(ext.min_plotjuggler_version, "3.8.0");
}

// [2] Tags array
TEST_F(RegistryManagerTest, ParsesTags) {
  RegistryManager mgr;
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody(kFullRegistryJson);
  mgr.fetchRegistry(server_->url());
  ASSERT_TRUE(spy_finished.wait(3000));

  const QStringList tags = mgr.extensions().at(0).tags;
  ASSERT_EQ(tags.size(), 3);
  EXPECT_TRUE(tags.contains("csv"));
  EXPECT_TRUE(tags.contains("tsv"));
  EXPECT_TRUE(tags.contains("file"));
}

// [2] Platforms map (url + checksum per platform key)
TEST_F(RegistryManagerTest, ParsesPlatformArtifacts) {
  RegistryManager mgr;
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody(kFullRegistryJson);
  mgr.fetchRegistry(server_->url());
  ASSERT_TRUE(spy_finished.wait(3000));

  const auto& platforms = mgr.extensions().at(0).platforms;
  ASSERT_EQ(platforms.size(), 2);

  ASSERT_TRUE(platforms.contains("linux-x86_64"));
  EXPECT_EQ(platforms["linux-x86_64"].url, "https://example.com/csv-loader-linux.so");
  EXPECT_EQ(platforms["linux-x86_64"].checksum, "sha256:deadbeef");

  ASSERT_TRUE(platforms.contains("windows-x86_64"));
  EXPECT_EQ(platforms["windows-x86_64"].url, "https://example.com/csv-loader-win.dll");
}

// compatibleExtensions(platform) returns only entries whose `platforms` map
// contains the requested key, while extensions() stays untouched.
TEST_F(RegistryManagerTest, CompatibleExtensionsFiltersByRequestedPlatform) {
  static const QByteArray k_mixed_platforms_json = R"({
    "extensions": [
      { "id": "linux-only", "name": "Linux Only", "version": "1.0.0",
        "platforms": { "linux-x86_64": { "url": "u1", "checksum": "sha256:1" } } },
      { "id": "windows-only", "name": "Windows Only", "version": "1.0.0",
        "platforms": { "windows-x86_64": { "url": "u2", "checksum": "sha256:2" } } },
      { "id": "cross", "name": "Cross", "version": "1.0.0",
        "platforms": {
          "linux-x86_64":   { "url": "u3", "checksum": "sha256:3" },
          "windows-x86_64": { "url": "u4", "checksum": "sha256:4" }
        }
      }
    ]
  })";

  RegistryManager mgr;
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody(k_mixed_platforms_json);
  mgr.fetchRegistry(server_->url());
  ASSERT_TRUE(spy_finished.wait(3000));

  // extensions() returns the parsed list verbatim, regardless of platform.
  EXPECT_EQ(mgr.extensions().size(), 3);

  const QList<Extension> linux_compat = mgr.compatibleExtensions("linux-x86_64");
  ASSERT_EQ(linux_compat.size(), 2);
  EXPECT_EQ(linux_compat.at(0).id, "linux-only");
  EXPECT_EQ(linux_compat.at(1).id, "cross");

  const QList<Extension> win_compat = mgr.compatibleExtensions("windows-x86_64");
  ASSERT_EQ(win_compat.size(), 2);
  EXPECT_EQ(win_compat.at(0).id, "windows-only");
  EXPECT_EQ(win_compat.at(1).id, "cross");

  // Unknown platform key drops everything.
  EXPECT_TRUE(mgr.compatibleExtensions("imaginary-os-arch").isEmpty());
}

// [2] Changelog map (version -> description)
TEST_F(RegistryManagerTest, ParsesChangelog) {
  RegistryManager mgr;
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody(kFullRegistryJson);
  mgr.fetchRegistry(server_->url());
  ASSERT_TRUE(spy_finished.wait(3000));

  const auto& changelog = mgr.extensions().at(0).changelog;
  ASSERT_EQ(changelog.size(), 2);
  EXPECT_EQ(changelog["1.2.0"], "Added TSV support");
  EXPECT_EQ(changelog["1.0.0"], "Initial release");
}

// [2] Registry with multiple extensions
TEST_F(RegistryManagerTest, ParsesMultipleExtensions) {
  RegistryManager mgr;
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody(kMultiExtensionJson);
  mgr.fetchRegistry(server_->url());
  ASSERT_TRUE(spy_finished.wait(3000));

  const QList<Extension> exts = mgr.extensions();
  ASSERT_EQ(exts.size(), 2);
  EXPECT_EQ(exts.at(0).id, "ext-a");
  EXPECT_EQ(exts.at(1).id, "ext-b");
}

// [2] Empty extensions array is valid — results in an empty list
TEST_F(RegistryManagerTest, AcceptsEmptyExtensionsArray) {
  RegistryManager mgr;
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody(kEmptyExtensionsJson);
  mgr.fetchRegistry(server_->url());
  ASSERT_TRUE(spy_finished.wait(3000));

  EXPECT_TRUE(spy_finished.first().at(0).toBool());
  EXPECT_TRUE(mgr.extensions().isEmpty());
}

// [2] findById returns the matching extension
TEST_F(RegistryManagerTest, FindByIdReturnsCorrectExtension) {
  RegistryManager mgr;
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody(kFullRegistryJson);
  mgr.fetchRegistry(server_->url());
  ASSERT_TRUE(spy_finished.wait(3000));

  const Extension ext = mgr.findById("csv-loader");
  EXPECT_EQ(ext.id, "csv-loader");
  EXPECT_EQ(ext.name, "CSV Loader");
}

// [2] findById returns a default-constructed (empty id) extension when not found
TEST_F(RegistryManagerTest, FindByIdReturnsEmptyExtensionOnMiss) {
  RegistryManager mgr;
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody(kFullRegistryJson);
  mgr.fetchRegistry(server_->url());
  ASSERT_TRUE(spy_finished.wait(3000));

  EXPECT_TRUE(mgr.findById("nonexistent-plugin").id.isEmpty());
}

// ---------------------------------------------------------------------------
// [4] Network error handling — connection refused
// ---------------------------------------------------------------------------

// Closing the server before the request ensures nothing is listening on that port
TEST_F(RegistryManagerTest, EmitsFetchErrorOnConnectionRefused) {
  const quint16 dead_port = server_->serverPort();
  server_->close();

  RegistryManager mgr;
  QSignalSpy spy_error(&mgr, &RegistryManager::fetchError);
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  mgr.fetchRegistry(QUrl(QString("http://127.0.0.1:%1/registry.json").arg(dead_port)));

  ASSERT_TRUE(spy_finished.wait(5000)) << "fetchFinished was not emitted after a connection error";
  EXPECT_FALSE(spy_finished.first().at(0).toBool());
  ASSERT_GE(spy_error.count(), 1);
  EXPECT_FALSE(spy_error.first().at(0).toString().isEmpty());
}

// ---------------------------------------------------------------------------
// [4] Parse error handling
// ---------------------------------------------------------------------------

TEST_F(RegistryManagerTest, EmitsFetchErrorOnMalformedJson) {
  RegistryManager mgr;
  QSignalSpy spy_error(&mgr, &RegistryManager::fetchError);
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody("{ this is: definitely [not valid json !!!");
  mgr.fetchRegistry(server_->url());

  ASSERT_TRUE(spy_finished.wait(3000));
  EXPECT_FALSE(spy_finished.first().at(0).toBool());
  ASSERT_GE(spy_error.count(), 1);
  EXPECT_TRUE(mgr.extensions().isEmpty());
}

// [4] JSON root is an array instead of an object
TEST_F(RegistryManagerTest, EmitsFetchErrorWhenRootIsNotObject) {
  RegistryManager mgr;
  QSignalSpy spy_error(&mgr, &RegistryManager::fetchError);
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody(R"([{"id":"x","name":"X","version":"1.0"}])");
  mgr.fetchRegistry(server_->url());

  ASSERT_TRUE(spy_finished.wait(3000));
  EXPECT_FALSE(spy_finished.first().at(0).toBool());
  EXPECT_GE(spy_error.count(), 1);
}

// [4] Root object is missing the "extensions" key
TEST_F(RegistryManagerTest, EmitsFetchErrorWhenExtensionsKeyMissing) {
  RegistryManager mgr;
  QSignalSpy spy_error(&mgr, &RegistryManager::fetchError);
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody(R"({"plugins":[]})");
  mgr.fetchRegistry(server_->url());

  ASSERT_TRUE(spy_finished.wait(3000));
  EXPECT_FALSE(spy_finished.first().at(0).toBool());
  EXPECT_GE(spy_error.count(), 1);
}

// [4] Required field "id" is absent from an extension entry
TEST_F(RegistryManagerTest, EmitsFetchErrorOnMissingRequiredFieldId) {
  RegistryManager mgr;
  QSignalSpy spy_error(&mgr, &RegistryManager::fetchError);
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody(R"({"extensions":[{"name":"No ID","version":"1.0.0"}]})");
  mgr.fetchRegistry(server_->url());

  ASSERT_TRUE(spy_finished.wait(3000));
  EXPECT_FALSE(spy_finished.first().at(0).toBool());
  EXPECT_GE(spy_error.count(), 1);
  EXPECT_TRUE(mgr.extensions().isEmpty());
}

// [4] Required field "version" is absent from an extension entry
TEST_F(RegistryManagerTest, EmitsFetchErrorOnMissingRequiredFieldVersion) {
  RegistryManager mgr;
  QSignalSpy spy_error(&mgr, &RegistryManager::fetchError);
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody(R"({"extensions":[{"id":"ext-x","name":"Ext X"}]})");
  mgr.fetchRegistry(server_->url());

  ASSERT_TRUE(spy_finished.wait(3000));
  EXPECT_FALSE(spy_finished.first().at(0).toBool());
  EXPECT_GE(spy_error.count(), 1);
}

TEST_F(RegistryManagerTest, RejectsEmptyRequiredString) {
  for (const QString field : {"id", "name"}) {
    RegistryManager mgr;
    QSignalSpy spy_error(&mgr, &RegistryManager::fetchError);
    QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

    QJsonObject extension{{"id", "plugin"}, {"name", "Plugin"}, {"version", "1.0.0"}};
    extension[field] = "";
    server_->setResponseBody(QJsonDocument(QJsonObject{{"extensions", QJsonArray{extension}}}).toJson());
    mgr.fetchRegistry(server_->url());

    ASSERT_TRUE(spy_finished.wait(3000));
    EXPECT_FALSE(spy_finished.first().at(0).toBool()) << field.toStdString();
    ASSERT_EQ(spy_error.count(), 1) << field.toStdString();
    EXPECT_TRUE(spy_error.first().at(0).toString().contains(field));
    EXPECT_TRUE(mgr.extensions().isEmpty());
  }
}

TEST_F(RegistryManagerTest, RejectsMalformedRegistryVersion) {
  RegistryManager mgr;
  QSignalSpy spy_error(&mgr, &RegistryManager::fetchError);
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody(R"({"extensions":[{"id":"bad","name":"Bad","version":"4.1"}]})");
  mgr.fetchRegistry(server_->url());

  ASSERT_TRUE(spy_finished.wait(3000));
  EXPECT_FALSE(spy_finished.first().at(0).toBool());
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(0).toString().contains("invalid", Qt::CaseInsensitive));
  EXPECT_TRUE(spy_error.first().at(0).toString().contains("4.1"));
  EXPECT_TRUE(mgr.extensions().isEmpty());
}

TEST_F(RegistryManagerTest, RejectsMalformedRegistrySdkFloor) {
  RegistryManager mgr;
  QSignalSpy spy_error(&mgr, &RegistryManager::fetchError);
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody(R"({"extensions":[{"id":"bad","name":"Bad","version":"1.0.0","min_sdk_required":"0.21"}]})");
  mgr.fetchRegistry(server_->url());

  ASSERT_TRUE(spy_finished.wait(3000));
  EXPECT_FALSE(spy_finished.first().at(0).toBool());
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(0).toString().contains("SDK", Qt::CaseInsensitive));
  EXPECT_TRUE(spy_error.first().at(0).toString().contains("0.21"));
  EXPECT_TRUE(mgr.extensions().isEmpty());
}

TEST_F(RegistryManagerTest, RejectsWrongTypeCompatibilityFloor) {
  for (const QString field : {"min_sdk_required", "min_plotjuggler_version"}) {
    RegistryManager mgr;
    QSignalSpy spy_error(&mgr, &RegistryManager::fetchError);
    QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

    server_->setResponseBody(
        QString(R"({"extensions":[{"id":"bad","name":"Bad","version":"1.0.0","%1":21}]})").arg(field).toUtf8());
    mgr.fetchRegistry(server_->url());

    ASSERT_TRUE(spy_finished.wait(3000));
    EXPECT_FALSE(spy_finished.first().at(0).toBool()) << field.toStdString();
    ASSERT_EQ(spy_error.count(), 1) << field.toStdString();
    EXPECT_TRUE(spy_error.first().at(0).toString().contains(field)) << spy_error.first().at(0).toString().toStdString();
    EXPECT_TRUE(mgr.extensions().isEmpty());
  }
}

// [4] extensions() is empty after a parse error, even if a previous fetch succeeded
TEST_F(RegistryManagerTest, ExtensionsEmptyAfterParseError) {
  RegistryManager mgr;
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  // First request succeeds
  server_->setResponseBody(kFullRegistryJson);
  mgr.fetchRegistry(server_->url());
  ASSERT_TRUE(spy_finished.wait(3000));
  ASSERT_EQ(mgr.extensions().size(), 1);

  spy_finished.clear();

  // Second request returns invalid JSON — list must be cleared
  server_->setResponseBody("not json at all");
  mgr.fetchRegistry(server_->url());
  ASSERT_TRUE(spy_finished.wait(3000));
  EXPECT_TRUE(mgr.extensions().isEmpty());
}

// [2] A duplicate id collapses to a single entry, keeping the highest version —
// otherwise the marketplace shows a phantom second row that can never be
// selected (the footer always resolves an id to its first match).
TEST_F(RegistryManagerTest, DeduplicatesByIdKeepingHighestVersion) {
  RegistryManager mgr;
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  // The newer version is listed FIRST to prove the winner is chosen by semver,
  // not by array order, and that "1.10.0" beats "1.9.0" (not a string compare).
  server_->setResponseBody(R"({"extensions":[
    {"id":"dup","name":"Dup","version":"1.10.0","description":"new"},
    {"id":"dup","name":"Dup","version":"1.9.0","description":"old"},
    {"id":"other","name":"Other","version":"1.0.0"}
  ]})");
  mgr.fetchRegistry(server_->url());
  ASSERT_TRUE(spy_finished.wait(3000));

  const QList<Extension> exts = mgr.extensions();
  ASSERT_EQ(exts.size(), 2);

  const Extension& dup = exts.at(0);
  EXPECT_EQ(dup.id, "dup");
  EXPECT_EQ(dup.version, "1.10.0");
  EXPECT_EQ(dup.description, "new");
  EXPECT_EQ(exts.at(1).id, "other");
}

// The duplicate-id collapse is destructive, so eligibility has to be decided
// BEFORE it: platform filtering (compatibleExtensions) runs on the already
// collapsed list, so a higher-versioned entry built for another platform would
// otherwise discard the only artifact this host can install, for good.
TEST_F(RegistryManagerTest, PlatformEligibilityPrecedesVersionOrdering) {
  RegistryManager mgr;
  mgr.setEligibility("linux-x86_64", "4.0.0");
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody(R"({"extensions":[
    {"id":"dup","name":"Dup","version":"1.0.0",
     "platforms":{"linux-x86_64":{"url":"u-linux","checksum":"sha256:1"}}},
    {"id":"dup","name":"Dup","version":"2.0.0",
     "platforms":{"windows-x86_64":{"url":"u-win","checksum":"sha256:2"}}}
  ]})");
  mgr.fetchRegistry(server_->url());
  ASSERT_TRUE(spy_finished.wait(3000));

  const QList<Extension> exts = mgr.extensions();
  ASSERT_EQ(exts.size(), 1);
  EXPECT_EQ(exts.at(0).version, "1.0.0") << "a windows-only 2.0.0 must not displace the installable linux 1.0.0";
  EXPECT_EQ(mgr.compatibleExtensions("linux-x86_64").size(), 1)
      << "the surviving entry must still be the one this platform can install";
}

// Among platform-eligible duplicates the winner is the highest version the
// running host can actually accept — a newer build that demands a newer
// PlotJuggler must not hide an installable older one.
TEST_F(RegistryManagerTest, HighestHostCompatibleVersionWinsWhenOneExists) {
  RegistryManager mgr;
  mgr.setEligibility("linux-x86_64", "4.0.0");
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody(R"({"extensions":[
    {"id":"dup","name":"Dup","version":"3.0.0","min_plotjuggler_version":"5.0.0",
     "platforms":{"linux-x86_64":{"url":"u3","checksum":"sha256:3"}}},
    {"id":"dup","name":"Dup","version":"2.0.0","min_plotjuggler_version":"4.0.0",
     "platforms":{"linux-x86_64":{"url":"u2","checksum":"sha256:2"}}},
    {"id":"dup","name":"Dup","version":"1.0.0",
     "platforms":{"linux-x86_64":{"url":"u1","checksum":"sha256:1"}}}
  ]})");
  mgr.fetchRegistry(server_->url());
  ASSERT_TRUE(spy_finished.wait(3000));

  const QList<Extension> exts = mgr.extensions();
  ASSERT_EQ(exts.size(), 1);
  EXPECT_EQ(exts.at(0).version, "2.0.0") << "3.0.0 needs PlotJuggler 5.0.0, so the highest installable one must win";
}

// When no candidate is host-compatible the highest one is kept rather than
// dropped, so the marketplace can still show the row and flag it as
// incompatible (an entry that vanishes from the list is unexplainable to the
// user; a flagged one is not).
TEST_F(RegistryManagerTest, KeepsHighestVersionWhenNoCandidateIsHostCompatible) {
  RegistryManager mgr;
  mgr.setEligibility("linux-x86_64", "4.0.0");
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody(R"({"extensions":[
    {"id":"dup","name":"Dup","version":"2.0.0","min_plotjuggler_version":"5.0.0",
     "platforms":{"linux-x86_64":{"url":"u2","checksum":"sha256:2"}}},
    {"id":"dup","name":"Dup","version":"3.0.0","min_plotjuggler_version":"6.0.0",
     "platforms":{"linux-x86_64":{"url":"u3","checksum":"sha256:3"}}}
  ]})");
  mgr.fetchRegistry(server_->url());
  ASSERT_TRUE(spy_finished.wait(3000));

  const QList<Extension> exts = mgr.extensions();
  ASSERT_EQ(exts.size(), 1);
  EXPECT_EQ(exts.at(0).version, "3.0.0");
}

// A pre-release must lose the duplicate-id race against its own release, which
// is only true once the shared comparator implements SemVer precedence.
TEST_F(RegistryManagerTest, DeduplicationPrefersReleaseOverPreRelease) {
  RegistryManager mgr;
  mgr.setEligibility("linux-x86_64", "4.0.0");
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody(R"({"extensions":[
    {"id":"dup","name":"Dup","version":"2.0.0-rc.1",
     "platforms":{"linux-x86_64":{"url":"u-rc","checksum":"sha256:1"}}},
    {"id":"dup","name":"Dup","version":"2.0.0",
     "platforms":{"linux-x86_64":{"url":"u-final","checksum":"sha256:2"}}}
  ]})");
  mgr.fetchRegistry(server_->url());
  ASSERT_TRUE(spy_finished.wait(3000));

  const QList<Extension> exts = mgr.extensions();
  ASSERT_EQ(exts.size(), 1);
  EXPECT_EQ(exts.at(0).version, "2.0.0");
  EXPECT_EQ(exts.at(0).platforms["linux-x86_64"].url, "u-final");
}

TEST_F(RegistryManagerTest, RejectsReusedSemanticVersionSlot) {
  RegistryManager mgr;
  mgr.setEligibility("linux-x86_64", "4.0.0");
  QSignalSpy spy_error(&mgr, &RegistryManager::fetchError);
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody(R"({"extensions":[
    {"id":"dup","name":"Dup","version":"1.0.0+linux",
     "platforms":{"linux-x86_64":{"url":"u1","checksum":"sha256:1"}}},
    {"id":"dup","name":"Dup","version":"1.0.0+replacement",
     "platforms":{"linux-x86_64":{"url":"u2","checksum":"sha256:2"}}}
  ]})");
  mgr.fetchRegistry(server_->url());

  ASSERT_TRUE(spy_finished.wait(3000));
  EXPECT_FALSE(spy_finished.first().at(0).toBool());
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(0).toString().contains("reuses", Qt::CaseInsensitive));
  EXPECT_TRUE(spy_error.first().at(0).toString().contains("1.0.0"));
  EXPECT_TRUE(mgr.extensions().isEmpty());
}

TEST_F(RegistryManagerTest, SdkCompatibilityPrecedesVersionOrdering) {
  RegistryManager mgr;
  mgr.setEligibility("linux-x86_64", "4.0.0");
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody(R"({"extensions":[
    {"id":"dup","name":"Dup","version":"3.0.0","min_sdk_required":"99.0.0",
     "platforms":{"linux-x86_64":{"url":"u3","checksum":"sha256:3"}}},
    {"id":"dup","name":"Dup","version":"2.0.0","min_sdk_required":"0.21.0",
     "platforms":{"linux-x86_64":{"url":"u2","checksum":"sha256:2"}}}
  ]})");
  mgr.fetchRegistry(server_->url());

  ASSERT_TRUE(spy_finished.wait(3000));
  const QList<Extension> exts = mgr.extensions();
  ASSERT_EQ(exts.size(), 1);
  EXPECT_EQ(exts.at(0).version, "2.0.0");
}

TEST_F(RegistryManagerTest, NoPlatformCandidateFallsBackToHighestVersionOverall) {
  RegistryManager mgr;
  mgr.setEligibility("linux-x86_64", "4.0.0");
  QSignalSpy spy_finished(&mgr, &RegistryManager::fetchFinished);

  server_->setResponseBody(R"({"extensions":[
    {"id":"dup","name":"Dup","version":"2.0.0",
     "platforms":{"windows-x86_64":{"url":"u2","checksum":"sha256:2"}}},
    {"id":"dup","name":"Dup","version":"3.0.0","min_plotjuggler_version":"99.0.0",
     "platforms":{"macos-arm64":{"url":"u3","checksum":"sha256:3"}}}
  ]})");
  mgr.fetchRegistry(server_->url());

  ASSERT_TRUE(spy_finished.wait(3000));
  const QList<Extension> exts = mgr.extensions();
  ASSERT_EQ(exts.size(), 1);
  EXPECT_EQ(exts.at(0).version, "3.0.0");
}

}  // namespace
}  // namespace PJ

// ---------------------------------------------------------------------------
// main — QCoreApplication is required for the QNetworkAccessManager event loop
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
