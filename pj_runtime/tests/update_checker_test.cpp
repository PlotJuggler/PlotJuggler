// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Exercises UpdateChecker's reply state machine against a local HTTP server, so
// the "emit exactly one of updateAvailable/upToDate/checkFailed" contract, the
// name-fallback, the 404/parse/missing-tag failure routing, and the
// abort-on-supersede behavior are verified without touching real GitHub.
// setCurrentVersion() makes the comparison deterministic without depending on
// the runner's applicationVersion() or a PJ_VERSION_STRING definition.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QFile>
#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <optional>

#include "http_test_utils.h"
#include "pj_runtime/UpdateChecker.h"
using namespace Qt::StringLiterals;
using PJ::test::LocalHttpServer;
using PJ::test::waitForSignal;

TEST(UpdateCheckerTest, NewerReleaseEmitsUpdateAvailable) {
  LocalHttpServer server;
  server.setResponse("200 OK", R"({"tag_name":"4.0.0","name":"PJ 4","html_url":"https://x/rel"})");

  PJ::UpdateChecker checker;
  checker.setReleaseApiUrl(server.url());
  checker.setCurrentVersion(u"3.999.0"_s);

  QSignalSpy update_spy(&checker, &PJ::UpdateChecker::updateAvailable);
  QSignalSpy uptodate_spy(&checker, &PJ::UpdateChecker::upToDate);
  QSignalSpy failed_spy(&checker, &PJ::UpdateChecker::checkFailed);

  checker.checkLatestRelease();

  ASSERT_TRUE(waitForSignal(update_spy));
  EXPECT_EQ(update_spy.count(), 1);
  EXPECT_TRUE(uptodate_spy.isEmpty());
  EXPECT_TRUE(failed_spy.isEmpty());

  const auto release = update_spy.first().at(0).value<PJ::ReleaseInfo>();
  EXPECT_EQ(release.name, u"PJ 4"_s);
  EXPECT_EQ(release.html_url, u"https://x/rel"_s);
}

TEST(UpdateCheckerTest, EmptyNameFallsBackToTag) {
  LocalHttpServer server;
  server.setResponse("200 OK", R"({"tag_name":"4.0.0","html_url":"https://x/rel"})");

  PJ::UpdateChecker checker;
  checker.setReleaseApiUrl(server.url());
  checker.setCurrentVersion(u"3.999.0"_s);

  QSignalSpy update_spy(&checker, &PJ::UpdateChecker::updateAvailable);
  checker.checkLatestRelease();

  ASSERT_TRUE(waitForSignal(update_spy));
  EXPECT_EQ(update_spy.first().at(0).value<PJ::ReleaseInfo>().name, u"4.0.0"_s);
}

TEST(UpdateCheckerTest, EqualVersionIsUpToDate) {
  LocalHttpServer server;
  server.setResponse("200 OK", R"({"tag_name":"3.999.0"})");

  PJ::UpdateChecker checker;
  checker.setReleaseApiUrl(server.url());
  checker.setCurrentVersion(u"3.999.0"_s);

  QSignalSpy update_spy(&checker, &PJ::UpdateChecker::updateAvailable);
  QSignalSpy uptodate_spy(&checker, &PJ::UpdateChecker::upToDate);
  QSignalSpy failed_spy(&checker, &PJ::UpdateChecker::checkFailed);

  checker.checkLatestRelease();

  ASSERT_TRUE(waitForSignal(uptodate_spy));
  EXPECT_EQ(uptodate_spy.count(), 1);
  EXPECT_TRUE(update_spy.isEmpty());
  EXPECT_TRUE(failed_spy.isEmpty());
}

TEST(UpdateCheckerTest, OlderReleaseIsUpToDate) {
  LocalHttpServer server;
  server.setResponse("200 OK", R"({"tag_name":"3.998.0"})");

  PJ::UpdateChecker checker;
  checker.setReleaseApiUrl(server.url());
  checker.setCurrentVersion(u"3.999.0"_s);

  QSignalSpy uptodate_spy(&checker, &PJ::UpdateChecker::upToDate);
  checker.checkLatestRelease();
  EXPECT_TRUE(waitForSignal(uptodate_spy));
}

TEST(UpdateCheckerTest, Http404EmitsCheckFailedSilently) {
  LocalHttpServer server;
  server.setResponse("404 Not Found", R"({"message":"Not Found"})");

  PJ::UpdateChecker checker;
  checker.setReleaseApiUrl(server.url());
  checker.setCurrentVersion(u"3.999.0"_s);

  QSignalSpy update_spy(&checker, &PJ::UpdateChecker::updateAvailable);
  QSignalSpy uptodate_spy(&checker, &PJ::UpdateChecker::upToDate);
  QSignalSpy failed_spy(&checker, &PJ::UpdateChecker::checkFailed);

  checker.checkLatestRelease();

  ASSERT_TRUE(waitForSignal(failed_spy));
  EXPECT_EQ(failed_spy.count(), 1);
  // The whole "dormant until first release" design rests on 404 NOT looking
  // like an update or an up-to-date result.
  EXPECT_TRUE(update_spy.isEmpty());
  EXPECT_TRUE(uptodate_spy.isEmpty());
}

TEST(UpdateCheckerTest, ValidNonObjectJsonEmitsDistinctFailure) {
  LocalHttpServer server;
  server.setResponse("200 OK", "[]");  // valid JSON, wrong shape

  PJ::UpdateChecker checker;
  checker.setReleaseApiUrl(server.url());
  checker.setCurrentVersion(u"3.999.0"_s);

  QSignalSpy failed_spy(&checker, &PJ::UpdateChecker::checkFailed);
  checker.checkLatestRelease();

  ASSERT_TRUE(waitForSignal(failed_spy));
  const QString reason = failed_spy.first().at(0).toString();
  // Must not misreport a valid-but-non-object body as a "parse error: no error".
  EXPECT_TRUE(reason.contains("not a JSON object"_L1));
}

TEST(UpdateCheckerTest, MissingTagNameEmitsCheckFailed) {
  LocalHttpServer server;
  server.setResponse("200 OK", R"({"name":"no tag here"})");

  PJ::UpdateChecker checker;
  checker.setReleaseApiUrl(server.url());
  checker.setCurrentVersion(u"3.999.0"_s);

  QSignalSpy update_spy(&checker, &PJ::UpdateChecker::updateAvailable);
  QSignalSpy failed_spy(&checker, &PJ::UpdateChecker::checkFailed);

  checker.checkLatestRelease();

  ASSERT_TRUE(waitForSignal(failed_spy));
  EXPECT_TRUE(update_spy.isEmpty());
}

TEST(UpdateCheckerTest, SupersededCheckEmitsNothing) {
  // First check points at a server that accepts the connection but never
  // replies; the second check supersedes (and aborts) it.
  QTcpServer hanging;
  hanging.listen(QHostAddress::LocalHost, 0);

  LocalHttpServer good;
  good.setResponse("200 OK", R"({"tag_name":"4.0.0","name":"PJ 4","html_url":"https://x/rel"})");

  PJ::UpdateChecker checker;
  checker.setCurrentVersion(u"3.999.0"_s);
  QSignalSpy update_spy(&checker, &PJ::UpdateChecker::updateAvailable);
  QSignalSpy uptodate_spy(&checker, &PJ::UpdateChecker::upToDate);
  QSignalSpy failed_spy(&checker, &PJ::UpdateChecker::checkFailed);

  checker.setReleaseApiUrl(QUrl(u"http://127.0.0.1:%1/"_s.arg(hanging.serverPort())));
  checker.checkLatestRelease();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

  checker.setReleaseApiUrl(good.url());
  checker.checkLatestRelease();  // aborts the hanging request

  ASSERT_TRUE(waitForSignal(update_spy));
  EXPECT_EQ(update_spy.count(), 1);
  EXPECT_TRUE(uptodate_spy.isEmpty());
  // The aborted (superseded) request must not surface as a failure.
  EXPECT_TRUE(failed_spy.isEmpty());
}

TEST(UpdateCheckerTest, DefaultUrlTargetsPublicRepo) {
  // The check must hit the PUBLIC PlotJuggler/PlotJuggler repo: the private
  // PlotJuggler/PJ4 dev repo is invisible to anonymous clients, so aiming the
  // default there makes every check fail (403 rate-limit / 404 not-found).
  PJ::UpdateChecker checker;
  const std::string url = checker.releaseApiUrl().toString().toStdString();
  EXPECT_NE(url.find("api.github.com"), std::string::npos) << url;
  EXPECT_NE(url.find("/repos/PlotJuggler/PlotJuggler/releases/latest"), std::string::npos) << url;
  EXPECT_EQ(url.find("/PlotJuggler/PJ4/"), std::string::npos) << url;
}

// ---------------------------------------------------------------------------
// Channel routing: deb asks apt, windows asks the maintenance tool.
// ---------------------------------------------------------------------------

namespace {

// Real `apt-cache policy plotjuggler4` output (LC_ALL=C) with the repository
// configured and a newer version published.
constexpr auto kAptUpgradable = R"(plotjuggler4:
  Installed: 4.0.0-1
  Candidate: 4.1.0-1
  Version table:
     4.1.0-1 500
        500 https://apt.plotjuggler.io stable/main amd64 Packages
 *** 4.0.0-1 500
        500 https://apt.plotjuggler.io stable/main amd64 Packages
        100 /var/lib/dpkg/status
)";

constexpr auto kAptCurrent = R"(plotjuggler4:
  Installed: 4.0.0-1
  Candidate: 4.0.0-1
  Version table:
 *** 4.0.0-1 500
        500 https://apt.plotjuggler.io stable/main amd64 Packages
        100 /var/lib/dpkg/status
)";

// A .deb installed from a downloaded file: dpkg status is the only source.
constexpr auto kAptManual = R"(plotjuggler4:
  Installed: 4.0.0-1
  Candidate: 4.0.0-1
  Version table:
 *** 4.0.0-1 100
        100 /var/lib/dpkg/status
)";

// `maintenancetool check-updates` as captured in the Windows rehearsal CI run.
constexpr auto kIfwUpdate = R"([26] Fetching latest update information...
[1176] Loading component scripts...
<?xml version="1.0"?>
<updates>
    <update name="PlotJuggler 4" version="4.0.4" size="297453815" id="io.plotjuggler.application"/>
</updates>
)";

}  // namespace

TEST(UpdateCheckerParseTest, AptPolicyWithRepository) {
  const PJ::AptPolicy policy = PJ::parseAptPolicy(QString::fromLatin1(kAptUpgradable));
  EXPECT_EQ(policy.installed, u"4.0.0-1"_s);
  EXPECT_EQ(policy.candidate, u"4.1.0-1"_s);
  EXPECT_TRUE(policy.from_repository);
}

TEST(UpdateCheckerParseTest, AptPolicyInstalledByHand) {
  EXPECT_FALSE(PJ::parseAptPolicy(QString::fromLatin1(kAptManual)).from_repository);
}

TEST(UpdateCheckerParseTest, AptPolicyNotInstalled) {
  const PJ::AptPolicy policy = PJ::parseAptPolicy(u"plotjuggler4:\n  Installed: (none)\n  Candidate: 4.0.0-1\n"_s);
  EXPECT_TRUE(policy.installed.isEmpty());
  EXPECT_EQ(policy.candidate, u"4.0.0-1"_s);
}

TEST(UpdateCheckerParseTest, MaintenanceToolUpdate) {
  const QString output = QString::fromLatin1(kIfwUpdate);
  EXPECT_EQ(PJ::parseMaintenanceToolUpdate(output, u"io.plotjuggler.application"_s), u"4.0.4"_s);
  EXPECT_TRUE(PJ::parseMaintenanceToolUpdate(output, u"some.other.component"_s).isEmpty());
  EXPECT_TRUE(
      PJ::parseMaintenanceToolUpdate(u"There are currently no updates available."_s, u"io.plotjuggler.application"_s)
          .isEmpty());
}

#if QT_CONFIG(process) && !defined(Q_OS_WIN)

namespace {

// A stand-in for apt-cache or the maintenance tool: prints `output`, exits
// with `exit_code`.
class FakeProgram {
 public:
  FakeProgram(const char* output, int exit_code = 0) : path_(dir_.filePath(u"fake"_s)) {
    QFile file(path_);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("#!/bin/sh\ncat <<'PJ_EOF'\n");
    file.write(output);
    file.write("PJ_EOF\nexit " + QByteArray::number(exit_code) + "\n");
    file.close();
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
  }
  QString path() const {
    return path_;
  }

 private:
  QTemporaryDir dir_;
  QString path_;
};

struct Outcome {
  std::optional<PJ::ReleaseInfo> update;
  bool up_to_date = false;
  bool failed = false;
};

Outcome runCheck(PJ::UpdateChecker& checker) {
  QSignalSpy update_spy(&checker, &PJ::UpdateChecker::updateAvailable);
  QSignalSpy uptodate_spy(&checker, &PJ::UpdateChecker::upToDate);
  QSignalSpy failed_spy(&checker, &PJ::UpdateChecker::checkFailed);
  checker.checkLatestRelease();
  EXPECT_TRUE(
      QTest::qWaitFor([&]() { return update_spy.count() + uptodate_spy.count() + failed_spy.count() > 0; }, 10000));
  EXPECT_EQ(update_spy.count() + uptodate_spy.count() + failed_spy.count(), 1);
  Outcome outcome;
  if (!update_spy.isEmpty()) {
    outcome.update = update_spy.first().at(0).value<PJ::ReleaseInfo>();
  }
  outcome.up_to_date = !uptodate_spy.isEmpty();
  outcome.failed = !failed_spy.isEmpty();
  return outcome;
}

}  // namespace

TEST(UpdateCheckerChannelTest, DebWithRepositoryOffersAptUpgrade) {
  FakeProgram apt(kAptUpgradable);
  PJ::UpdateChecker checker;
  checker.setInstallation(u"deb"_s);
  checker.setAptCacheProgram(apt.path());

  const Outcome outcome = runCheck(checker);
  ASSERT_TRUE(outcome.update.has_value());
  EXPECT_EQ(outcome.update->route, PJ::UpdateRoute::AptUpgrade);
  EXPECT_EQ(outcome.update->name, u"4.1.0"_s);
  EXPECT_TRUE(outcome.update->html_url.endsWith(u"/releases/tag/4.1.0"_s));
}

TEST(UpdateCheckerChannelTest, DebCurrentIsUpToDate) {
  FakeProgram apt(kAptCurrent);
  PJ::UpdateChecker checker;
  checker.setInstallation(u"deb"_s);
  checker.setAptCacheProgram(apt.path());
  EXPECT_TRUE(runCheck(checker).up_to_date);
}

TEST(UpdateCheckerChannelTest, DebWithoutRepositoryAsksGitHubAndSuggestsTheRepository) {
  FakeProgram apt(kAptManual);
  LocalHttpServer server;
  server.setResponse("200 OK", R"({"tag_name":"4.1.0","html_url":"https://x/rel"})");
  PJ::UpdateChecker checker;
  checker.setInstallation(u"deb"_s);
  checker.setAptCacheProgram(apt.path());
  checker.setReleaseApiUrl(server.url());
  checker.setCurrentVersion(u"4.0.0"_s);

  const Outcome outcome = runCheck(checker);
  ASSERT_TRUE(outcome.update.has_value());
  EXPECT_EQ(outcome.update->route, PJ::UpdateRoute::AptAddRepository);
}

TEST(UpdateCheckerChannelTest, DebProbeThatCannotRunFails) {
  PJ::UpdateChecker checker;
  checker.setInstallation(u"deb"_s);
  checker.setAptCacheProgram(u"/nonexistent/apt-cache"_s);
  EXPECT_TRUE(runCheck(checker).failed);
}

TEST(UpdateCheckerChannelTest, WindowsMaintenanceToolOffersUpdate) {
  FakeProgram tool(kIfwUpdate);
  PJ::UpdateChecker checker;
  checker.setInstallation(u"windows"_s);
  checker.setMaintenanceToolPath(tool.path());
  checker.setCurrentVersion(u"4.0.3"_s);

  const Outcome outcome = runCheck(checker);
  ASSERT_TRUE(outcome.update.has_value());
  EXPECT_EQ(outcome.update->route, PJ::UpdateRoute::MaintenanceTool);
  EXPECT_EQ(outcome.update->name, u"4.0.4"_s);
}

TEST(UpdateCheckerChannelTest, WindowsNoUpdateIsUpToDate) {
  FakeProgram tool("There are currently no updates available.\n");
  PJ::UpdateChecker checker;
  checker.setInstallation(u"windows"_s);
  checker.setMaintenanceToolPath(tool.path());
  checker.setCurrentVersion(u"4.0.4"_s);
  EXPECT_TRUE(runCheck(checker).up_to_date);
}

TEST(UpdateCheckerChannelTest, WindowsMaintenanceToolErrorFails) {
  FakeProgram tool("Cannot retrieve remote tree.\n", /*exit_code=*/1);
  PJ::UpdateChecker checker;
  checker.setInstallation(u"windows"_s);
  checker.setMaintenanceToolPath(tool.path());
  checker.setCurrentVersion(u"4.0.3"_s);
  EXPECT_TRUE(runCheck(checker).failed);
}

TEST(UpdateCheckerChannelTest, WindowsWithoutMaintenanceToolAsksGitHub) {
  LocalHttpServer server;
  server.setResponse("200 OK", R"({"tag_name":"4.1.0","html_url":"https://x/rel"})");
  PJ::UpdateChecker checker;
  checker.setInstallation(u"windows"_s);
  checker.setMaintenanceToolPath(u"/nonexistent/maintenancetool.exe"_s);
  checker.setReleaseApiUrl(server.url());
  checker.setCurrentVersion(u"4.0.0"_s);

  const Outcome outcome = runCheck(checker);
  ASSERT_TRUE(outcome.update.has_value());
  EXPECT_EQ(outcome.update->route, PJ::UpdateRoute::Download);
}

#endif  // QT_CONFIG(process) && !Q_OS_WIN: fixtures are POSIX shell scripts

namespace {

class UpdateCheckerEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    qRegisterMetaType<PJ::ReleaseInfo>();  // so QSignalSpy can capture updateAvailable's arg
  }
};

static auto* const kEnv = ::testing::AddGlobalTestEnvironment(new UpdateCheckerEnvironment);

}  // namespace
