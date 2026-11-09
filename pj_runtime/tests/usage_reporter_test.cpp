// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Drives UsageReporter against a local capturing HTTP server. The usage report's privacy contract
// is enforced here on the wire: only documented keys, private plugin ids folded into "_private"
// (with their versions dropped), a period id that is neither the raw machine id nor the launch
// ping's user id, unpackaged builds never upload, and days are forgotten only after a 2xx.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QSysInfo>

#include "http_test_utils.h"
#include "pj_runtime/UsageCounters.h"
#include "pj_runtime/UsageReporter.h"

using namespace Qt::StringLiterals;
using PJ::PluginUse;
using PJ::UsageCounters;
using PJ::UsageFeature;
using PJ::UsageReporter;
using PJ::test::LocalHttpServer;
using PJ::test::waitForSignal;

namespace {

struct Fixture {
  QDate date{2026, 11, 1};
  UsageCounters counters{{}, u"4.1.0"_s, [this]() { return date; }};

  // One finished day with a public and two private plugins of the same family.
  Fixture() {
    counters.setEnabled(true);
    counters.count(UsageFeature::kLayoutLoad);
    counters.countPlugin(PluginUse::kToolboxOpen, "pj.public_toolbox");
    counters.countPlugin(PluginUse::kToolboxOpen, "acme.secret_one");
    counters.countPlugin(PluginUse::kToolboxOpen, "acme.secret_two");
    counters.countPlugin(PluginUse::kToolboxOpen, "acme.secret_two");
    counters.setInstalledPlugins(
        {{"pj.public_toolbox", "1.0.0", "toolbox"},
         {"acme.secret_one", "7.7.7", "toolbox"},
         {"acme.secret_two", "8.8.8", "toolbox"}});
    date = date.addDays(1);
  }
};

const std::set<std::string> kPublic = {"pj.public_toolbox"};

QJsonObject postAndCapture(Fixture& fixture, const QByteArray& status, bool* ok) {
  LocalHttpServer server;
  server.setResponse(status, "{}");
  UsageReporter reporter(fixture.counters);
  reporter.setEndpointUrl(server.url());
  QSignalSpy finished_spy(&reporter, &UsageReporter::finished);
  reporter.send(u"test-channel"_s, kPublic);
  EXPECT_TRUE(waitForSignal(finished_spy));
  *ok = finished_spy.takeFirst().at(0).toBool();
  return QJsonDocument::fromJson(server.requestBody()).object();
}

QJsonObject findRow(const QJsonArray& rows, const QString& id) {
  for (const QJsonValue& value : rows) {
    if (value.toObject().value(u"id"_s).toString() == id) {
      return value.toObject();
    }
  }
  return {};
}

}  // namespace

TEST(UsageReporterTest, PostsOnlyDocumentedKeysAndFoldsPrivatePlugins) {
  Fixture fixture;
  bool ok = false;
  const QJsonObject body = postAndCapture(fixture, "200 OK", &ok);
  EXPECT_TRUE(ok);

  EXPECT_EQ(body.keys(), (QStringList{u"reports"_s, u"schema"_s}));
  EXPECT_EQ(body.value(u"schema"_s).toInt(), 2);
  const QJsonArray reports = body.value(u"reports"_s).toArray();
  ASSERT_EQ(reports.size(), 1);
  const QJsonObject report = reports[0].toObject();

  const QSet<QString> allowed = {u"report_id"_s, u"day"_s,          u"period_id"_s, u"version"_s, u"os"_s,
                                 u"arch"_s,      u"installation"_s, u"features"_s,  u"plugins"_s, u"installed"_s};
  for (const QString& key : report.keys()) {
    EXPECT_TRUE(allowed.contains(key)) << "undocumented usage-report key: " << key.toStdString();
  }
  EXPECT_EQ(report.value(u"day"_s).toString(), u"2026-11-01"_s);
  EXPECT_EQ(report.value(u"version"_s).toString(), u"4.1.0"_s);
  EXPECT_EQ(report.value(u"installation"_s).toString(), u"test-channel"_s);
  EXPECT_EQ(report.value(u"features"_s).toObject().value(u"layout.load"_s).toInt(), 1);

  // No private id or version appears anywhere in the body.
  const QByteArray raw = QJsonDocument(body).toJson();
  EXPECT_FALSE(raw.contains("acme")) << raw.toStdString();
  EXPECT_FALSE(raw.contains("7.7.7"));
  EXPECT_FALSE(raw.contains("8.8.8"));

  const QJsonArray plugins = report.value(u"plugins"_s).toArray();
  ASSERT_EQ(plugins.size(), 2);
  EXPECT_EQ(findRow(plugins, u"pj.public_toolbox"_s).value(u"n"_s).toInt(), 1);
  const QJsonObject folded = findRow(plugins, u"_private"_s);
  EXPECT_EQ(folded.value(u"kind"_s).toString(), u"toolbox"_s);
  EXPECT_EQ(folded.value(u"action"_s).toString(), u"open"_s);
  EXPECT_EQ(folded.value(u"n"_s).toInt(), 3);

  const QJsonArray installed = report.value(u"installed"_s).toArray();
  ASSERT_EQ(installed.size(), 2);
  EXPECT_EQ(findRow(installed, u"pj.public_toolbox"_s).value(u"v"_s).toString(), u"1.0.0"_s);
  EXPECT_EQ(findRow(installed, u"_private"_s).value(u"n"_s).toInt(), 2);
  EXPECT_FALSE(findRow(installed, u"_private"_s).contains(u"v"_s));
}

TEST(UsageReporterTest, PeriodIdIsAMonthScopedHashNotTheMachineId) {
  Fixture fixture;
  const auto period = [](const QDate& day) {
    PJ::DayUsage usage;
    usage.day = day;
    return UsageReporter::buildBody({usage}, u"x"_s, {})
        .value(u"reports"_s)
        .toArray()[0]
        .toObject()
        .value(u"period_id"_s)
        .toString();
  };
  const QString november = period({2026, 11, 1});
  static const QRegularExpression hex64(u"^[0-9a-f]{64}$"_s);
  EXPECT_TRUE(hex64.match(november).hasMatch()) << november.toStdString();
  EXPECT_EQ(november, period({2026, 11, 30}));
  EXPECT_NE(november, period({2026, 12, 1}));
  const QByteArray raw_machine_id = QSysInfo::machineUniqueId();
  if (!raw_machine_id.isEmpty()) {
    EXPECT_NE(november.toLatin1(), raw_machine_id.toHex());
  }
}

TEST(UsageReporterTest, AcceptedUploadForgetsTheDaysAndAFailedOneKeepsThem) {
  Fixture fixture;
  bool ok = true;
  postAndCapture(fixture, "500 Internal Server Error", &ok);
  EXPECT_FALSE(ok);
  EXPECT_EQ(fixture.counters.completedDays().size(), 1u);

  postAndCapture(fixture, "200 OK", &ok);
  EXPECT_TRUE(ok);
  EXPECT_TRUE(fixture.counters.completedDays().empty());
}

TEST(UsageReporterTest, NothingToSendMakesNoRequest) {
  QDate date{2026, 11, 1};
  UsageCounters counters({}, u"4.1.0"_s, [&date]() { return date; });
  counters.setEnabled(true);
  counters.count(UsageFeature::kLayoutLoad);  // today only: not yet a completed day

  LocalHttpServer server;
  server.setResponse("200 OK", "{}");
  UsageReporter reporter(counters);
  reporter.setEndpointUrl(server.url());
  QSignalSpy finished_spy(&reporter, &UsageReporter::finished);
  reporter.send(u"test-channel"_s, {});
  ASSERT_TRUE(waitForSignal(finished_spy));
  EXPECT_TRUE(finished_spy.takeFirst().at(0).toBool());
  EXPECT_TRUE(server.requestHeaders().isEmpty());
}

TEST(UsageReporterTest, UnpackagedBuildsNeverReachTheEndpoint) {
  for (const QString& installation : {u"source"_s, u" Source "_s, QString()}) {
    Fixture fixture;
    LocalHttpServer server;
    server.setResponse("200 OK", "{}");
    UsageReporter reporter(fixture.counters);
    reporter.setEndpointUrl(server.url());
    QSignalSpy finished_spy(&reporter, &UsageReporter::finished);

    reporter.send(installation, kPublic);

    ASSERT_TRUE(waitForSignal(finished_spy)) << "installation: " << installation.toStdString();
    EXPECT_FALSE(finished_spy.takeFirst().at(0).toBool());
    QDeadlineTimer settle(250);
    while (!settle.hasExpired()) {
      QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, 50);
    }
    EXPECT_TRUE(server.requestHeaders().isEmpty()) << "unpackaged build reached the endpoint";
    EXPECT_EQ(fixture.counters.completedDays().size(), 1u);
  }
}

TEST(UsageReporterTest, DefaultEndpointIsProductionUrl) {
  UsageCounters counters({}, {});
  UsageReporter reporter(counters);
  EXPECT_EQ(reporter.endpointUrl(), QUrl(u"https://app.plotjuggler.io/telemetry/v2/usage"_s));
}

// QSettings (the fallback machine-id path) needs an app identity; keep the real settings untouched.
namespace {

class UsageReporterEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    QCoreApplication::setOrganizationName(u"PlotJugglerTest"_s);
    QCoreApplication::setApplicationName(u"usage_reporter_test"_s);
  }
};

static auto* const kEnv = ::testing::AddGlobalTestEnvironment(new UsageReporterEnvironment);

}  // namespace
