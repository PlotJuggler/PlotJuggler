// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Drives TelemetryPing against a local capturing HTTP server. This is where
// the privacy contract is enforced on the wire: the POSTed body contains ONLY
// the documented environment-fact keys (adding a payload field must break this
// test), the user id is a stable salted hash (never the raw machine id),
// unpackaged builds never reach the endpoint at all, and every failure mode
// stays silent (finished(false), no crash).

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
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

// Sends one ping against a fresh capturing server and returns the POSTed JSON
// body (the server accumulates requests, so each send gets its own server).
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
  // Never the raw machine id, in any encoding.
  const QByteArray raw_machine_id = QSysInfo::machineUniqueId();
  if (!raw_machine_id.isEmpty()) {
    EXPECT_NE(first.toLatin1(), raw_machine_id);
    EXPECT_NE(first.toLatin1(), raw_machine_id.toHex());
  }
}

TEST(TelemetryPingTest, UnpackagedBuildsNeverReachTheEndpoint) {
  // "source" is the PJ_INSTALLATION default, so every self-built tree carries
  // it: developers, CI, and any automation that launches the real app. They
  // all report the in-development version, so a leak here invents users on a
  // version that was never released. An unstamped build is refused for the
  // same reason.
  for (const QString& installation : {u"source"_s, QString()}) {
    LocalHttpServer server;
    server.setResponse("200 OK", "{}");

    PJ::TelemetryPing ping;
    ping.setEndpointUrl(server.url());
    QSignalSpy finished_spy(&ping, &PJ::TelemetryPing::finished);

    ping.send(installation);

    ASSERT_TRUE(waitForSignal(finished_spy)) << "installation: " << installation.toStdString();
    EXPECT_FALSE(finished_spy.takeFirst().at(0).toBool());
    // Nothing must arrive even late: keep pumping past the terminal signal.
    // WaitForMoreEvents idles between deliveries instead of spinning.
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
  // The wire contract: PJ4 reports to the same endpoint the PJ3-era dashboard
  // reads from. A typo here silently zeroes the user counter.
  PJ::TelemetryPing ping;
  EXPECT_EQ(ping.endpointUrl(), QUrl(u"https://app.plotjuggler.io/telemetry"_s));
}

// QSettings (the fallback-id path) and applicationVersion() need an app
// identity; use a test-scoped one so the real PlotJuggler4.conf is untouched.
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
