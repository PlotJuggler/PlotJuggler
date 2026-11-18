// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Drives TelemetryPing against a local capturing HTTP server, enforcing the privacy contract on the wire: the
// POSTed body holds only the documented environment-fact keys (adding a payload field breaks this test), the user id
// is a stable salted hash (never the raw machine id), unpackaged builds never reach the endpoint, and every failure
// mode stays silent (finished(false), no crash).

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QSysInfo>
#include <QUrl>

#include "http_test_utils.h"
#include "pj_runtime/TelemetryPing.h"
using namespace Qt::StringLiterals;
using PJ::test::LocalHttpServer;
using PJ::test::waitForSignal;

namespace {

// Sends one ping against a fresh capturing server (the server accumulates requests) and returns the POSTed JSON body.
QJsonObject capturePingBody(const QString& installation) {
  LocalHttpServer server;
  server.setResponse("200 OK", "{}");

  PJ::TelemetryPing ping;
  ping.setEndpointUrl(server.url());
  QSignalSpy finished_spy(&ping, &PJ::TelemetryPing::finished);

  ping.send(installation);

  EXPECT_TRUE(waitForSignal(finished_spy));
  EXPECT_TRUE(finished_spy.takeFirst().at(0).toBool());
  EXPECT_TRUE(server.requestHeaders().toLower().contains("content-type: application/json"));
  return QJsonDocument::fromJson(server.requestBody()).object();
}

}  // namespace

TEST(TelemetryPingTest, PostsOnlyDocumentedKeys) {
  const QJsonObject payload = capturePingBody(u"test-channel"_s);
  const QSet<QString> allowed = {u"user_id"_s,      u"os"_s,   u"os_version"_s,    u"version"_s,
                                 u"installation"_s, u"arch"_s, u"display_server"_s};
  const QStringList keys = payload.keys();
  for (const QString& key : keys) {
    EXPECT_TRUE(allowed.contains(key)) << "undocumented telemetry key: " << key.toStdString();
  }
  EXPECT_EQ(payload.value(u"installation"_s).toString(), u"test-channel"_s);
  EXPECT_FALSE(payload.value(u"os"_s).toString().isEmpty());
  EXPECT_FALSE(payload.value(u"arch"_s).toString().isEmpty());
  EXPECT_EQ(payload.value(u"version"_s).toString(), u"9.9.9"_s);  // set by the test environment below
}

TEST(TelemetryPingTest, UserIdIsStableSaltedSha256Hex) {
  const QString first = capturePingBody(u"a"_s).value(u"user_id"_s).toString();
  const QString second = capturePingBody(u"b"_s).value(u"user_id"_s).toString();
  EXPECT_EQ(first, second);
  static const QRegularExpression hex64(u"^[0-9a-f]{64}$"_s);
  EXPECT_TRUE(hex64.match(first).hasMatch()) << first.toStdString();
  const QByteArray raw_machine_id = QSysInfo::machineUniqueId();
  if (!raw_machine_id.isEmpty()) {
    // The formula is part of the contract: changing it re-counts every machine as a new user.
    EXPECT_EQ(
        first.toLatin1(),
        QCryptographicHash::hash(raw_machine_id + "|plotjuggler4-telemetry-v1", QCryptographicHash::Sha256).toHex());
  }
}

TEST(TelemetryPingTest, UnpackagedBuildsNeverReachTheEndpoint) {
  // "source" is the PJ_INSTALLATION default of every self-built tree (developers, CI, automation launching the real
  // app); a ping from one would invent users on an unreleased version. An unstamped build is refused for the same
  // reason.
  for (const QString& installation : {u"source"_s, QString()}) {
    LocalHttpServer server;
    server.setResponse("200 OK", "{}");

    PJ::TelemetryPing ping;
    ping.setEndpointUrl(server.url());
    QSignalSpy finished_spy(&ping, &PJ::TelemetryPing::finished);

    ping.send(installation);

    ASSERT_TRUE(waitForSignal(finished_spy)) << "installation: " << installation.toStdString();
    EXPECT_FALSE(finished_spy.takeFirst().at(0).toBool());
    // Pump past the terminal signal to catch a late request; WaitForMoreEvents idles instead of spinning.
    QDeadlineTimer settle(250);
    while (!settle.hasExpired()) {
      QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, 50);
    }
    EXPECT_TRUE(server.requestHeaders().isEmpty())
        << "unpackaged build reached the endpoint: " << server.requestHeaders().toStdString();
  }
}

TEST(TelemetryPingTest, ServerErrorEmitsFinishedFalse) {
  LocalHttpServer server;
  server.setResponse("500 Internal Server Error", "{}");

  PJ::TelemetryPing ping;
  ping.setEndpointUrl(server.url());
  QSignalSpy finished_spy(&ping, &PJ::TelemetryPing::finished);

  ping.send(u"test-channel"_s);

  ASSERT_TRUE(waitForSignal(finished_spy));
  EXPECT_FALSE(finished_spy.takeFirst().at(0).toBool());
}

TEST(TelemetryPingTest, DefaultEndpointIsProductionUrl) {
  // Wire contract: a typo in the endpoint silently zeroes the usage counter.
  PJ::TelemetryPing ping;
  EXPECT_EQ(ping.endpointUrl(), QUrl(u"https://app.plotjuggler.io/telemetry"_s));
}

// QSettings (the fallback-id path) and applicationVersion() need an app identity; a test-scoped one keeps the
// real PlotJuggler4.conf untouched.
namespace {

class TelemetryPingEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    QCoreApplication::setOrganizationName(u"PlotJugglerTest"_s);
    QCoreApplication::setApplicationName(u"telemetry_ping_test"_s);
    QCoreApplication::setApplicationVersion(u"9.9.9"_s);
  }
};

static auto* const kEnv = ::testing::AddGlobalTestEnvironment(new TelemetryPingEnvironment);

}  // namespace
