// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/UpdateChecker.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <chrono>
#include <utility>

#if QT_CONFIG(process)
#include <QProcess>
#include <QProcessEnvironment>
#endif

#include "pj_runtime/HttpGet.h"
#include "pj_runtime/UpdateVersion.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {
// GitHub's REST endpoint returns the latest published, non-draft,
// non-prerelease release — so betas never trigger the nag on their own.
constexpr auto kDefaultReleaseApiUrl = "https://api.github.com/repos/PlotJuggler/PlotJuggler/releases/latest";
constexpr auto kReleaseTagUrl = "https://github.com/PlotJuggler/PlotJuggler/releases/tag/%1";
constexpr auto kPackageName = "plotjuggler4";
constexpr auto kComponentId = "io.plotjuggler.application";
constexpr int kTransferTimeoutMs = 15000;
// The maintenance tool fetches the remote Updates.xml, so it gets the network
// budget plus its own startup.
constexpr int kProbeTimeoutMs = 30000;

// "1:4.1.0-2" -> "4.1.0": the release as users know it, without the Debian
// epoch or package revision.
QString upstreamVersion(QString debian_version) {
  const qsizetype colon = debian_version.indexOf(u':');
  if (colon >= 0) {
    debian_version = debian_version.mid(colon + 1);
  }
  const qsizetype dash = debian_version.lastIndexOf(u'-');
  return dash > 0 ? debian_version.left(dash) : debian_version;
}
}  // namespace

AptPolicy parseAptPolicy(const QString& output) {
  AptPolicy policy;
  static const QRegularExpression kField(
      uR"(^\s*(Installed|Candidate):\s*(\S+)\s*$)"_s, QRegularExpression::MultilineOption);
  for (auto it = kField.globalMatch(output); it.hasNext();) {
    const auto m = it.next();
    const QString value = m.captured(2) == u"(none)"_s ? QString() : m.captured(2);
    (m.captured(1) == u"Installed"_s ? policy.installed : policy.candidate) = value;
  }
  // The version table lists every source: a URL means a repository provides
  // the package; dpkg's own status file alone means it was installed by hand.
  policy.from_repository = output.contains(u"://"_s);
  return policy;
}

QString parseMaintenanceToolUpdate(const QString& output, const QString& component_id) {
  static const QRegularExpression kUpdate(uR"(<update\s([^>]*)>)"_s);
  static const QRegularExpression kVersion(uR"re(\bversion="([^"]*)")re"_s);
  for (auto it = kUpdate.globalMatch(output); it.hasNext();) {
    const QString attributes = it.next().captured(1);
    if (attributes.contains(uR"(id=")"_s + component_id + u'"')) {
      return kVersion.match(attributes).captured(1);
    }
  }
  return {};
}

UpdateChecker::UpdateChecker(QObject* parent)
    : QObject(parent), network_(new QNetworkAccessManager(this)), release_api_url_(kDefaultReleaseApiUrl) {}

UpdateChecker::~UpdateChecker() = default;

void UpdateChecker::setCurrentVersion(const QString& version) {
  current_version_ = version;
}

void UpdateChecker::setReleaseApiUrl(const QUrl& url) {
  release_api_url_ = url;
}

QUrl UpdateChecker::releaseApiUrl() const {
  return release_api_url_;
}

void UpdateChecker::setInstallation(const QString& installation) {
  installation_ = installation;
}

void UpdateChecker::setAptCacheProgram(const QString& program) {
  apt_cache_program_ = program;
}

void UpdateChecker::setMaintenanceToolPath(const QString& path) {
  maintenance_tool_path_ = path;
}

QString UpdateChecker::maintenanceToolPath() const {
  if (!maintenance_tool_path_.isEmpty()) {
    return maintenance_tool_path_;
  }
  // The installer puts the app in <TargetDir>/bin and the tool in <TargetDir>.
  return QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(u"../maintenancetool.exe"_s);
}

QString UpdateChecker::currentVersion() const {
  return current_version_.isEmpty() ? QCoreApplication::applicationVersion() : current_version_;
}

void UpdateChecker::checkLatestRelease() {
  // Abandon any in-flight check. abort() makes that reply emit finished() with
  // OperationCanceledError, which handleReply() drops silently; and because each
  // handler captures its own `reply`, superseding a check can never make an old
  // reply's handler act on the new request. A superseded probe is disowned
  // before it is killed, so its finish can no longer report anything.
  if (pending_reply_ && pending_reply_->isRunning()) {
    pending_reply_->abort();
  }
#if QT_CONFIG(process)
  if (pending_probe_) {
    QProcess* stale = std::exchange(pending_probe_, nullptr);
    stale->disconnect(this);
    stale->kill();
    stale->deleteLater();
  }

  if (installation_ == u"deb"_s) {
    runProbe(
        apt_cache_program_.isEmpty() ? u"apt-cache"_s : apt_cache_program_,
        {u"policy"_s, QString::fromLatin1(kPackageName)},
        [this](int, const QString& output) { handleAptPolicy(output); });
    return;
  }
  if (installation_ == u"windows"_s && QFileInfo::exists(maintenanceToolPath())) {
    runProbe(maintenanceToolPath(), {u"check-updates"_s}, [this](int exit_code, const QString& output) {
      handleMaintenanceTool(exit_code, output);
    });
    return;
  }
#endif
  checkGitHub(UpdateRoute::Download);
}

void UpdateChecker::checkGitHub(UpdateRoute route) {
  QNetworkRequest request(release_api_url_);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  // api.github.com rejects requests without a User-Agent (HTTP 403).
  request.setHeader(QNetworkRequest::UserAgentHeader, u"PlotJuggler"_s);
  request.setRawHeader("Accept", "application/vnd.github+json");

  pending_reply_ = httpGetWithTimeout(
      *network_, std::move(request), std::chrono::milliseconds(kTransferTimeoutMs), this,
      [this, route](QNetworkReply& reply) {
        if (pending_reply_ == &reply) {
          pending_reply_ = nullptr;
        }
        handleReply(reply, route);
      });
}

void UpdateChecker::handleReply(QNetworkReply& reply, UpdateRoute route) {
  // A self-inflicted abort (a newer check superseded this one) is not a
  // user-facing failure — drop it without emitting any outcome.
  if (reply.error() == QNetworkReply::OperationCanceledError) {
    return;
  }

  // A 404 (no release published yet) surfaces here as ContentNotFoundError —
  // treated like any other failure, i.e. silently on the startup path.
  if (reply.error() != QNetworkReply::NoError) {
    emit checkFailed(reply.errorString());
    return;
  }

  const QByteArray data = reply.readAll();

  QJsonParseError parse_error;
  const QJsonDocument doc = QJsonDocument::fromJson(data, &parse_error);
  if (parse_error.error != QJsonParseError::NoError) {
    emit checkFailed(u"release JSON parse error: %1"_s.arg(parse_error.errorString()));
    return;
  }
  if (!doc.isObject()) {
    emit checkFailed(u"release response was not a JSON object"_s);
    return;
  }

  const QJsonObject obj = doc.object();
  const QString tag_name = obj.value(u"tag_name"_s).toString();
  if (tag_name.isEmpty()) {
    emit checkFailed(u"release JSON missing tag_name"_s);
    return;
  }

  if (!isNewerVersion(tag_name, currentVersion())) {
    emit upToDate();
    return;
  }

  const QString name = obj.value(u"name"_s).toString();
  const QString html_url = obj.value(u"html_url"_s).toString();
  emit updateAvailable(ReleaseInfo{name.isEmpty() ? tag_name : name, html_url, route});
}

void UpdateChecker::runProbe(
    const QString& program, const QStringList& args, std::function<void(int exit_code, const QString& output)> done) {
#if QT_CONFIG(process)
  auto* process = new QProcess(this);
  pending_probe_ = process;
  // Parsed output must not depend on the user's locale ("Candidate:" is translated).
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.insert(u"LC_ALL"_s, u"C"_s);
  process->setProcessEnvironment(env);
  process->setProcessChannelMode(QProcess::MergedChannels);

  auto* timeout = new QTimer(process);
  timeout->setSingleShot(true);
  connect(timeout, &QTimer::timeout, process, [process]() { process->kill(); });

  connect(
      process, &QProcess::finished, this,
      [this, process, program, done = std::move(done)](int exit_code, QProcess::ExitStatus status) {
        pending_probe_ = nullptr;
        process->deleteLater();
        if (status != QProcess::NormalExit) {
          emit checkFailed(u"%1 did not finish"_s.arg(program));
          return;
        }
        done(exit_code, QString::fromLocal8Bit(process->readAll()));
      });
  connect(process, &QProcess::errorOccurred, this, [this, process, program](QProcess::ProcessError error) {
    // Crashes and kills arrive through finished(); only a program that never
    // ran has no finished() to follow.
    if (error != QProcess::FailedToStart) {
      return;
    }
    pending_probe_ = nullptr;
    process->deleteLater();
    emit checkFailed(u"could not run %1"_s.arg(program));
  });

  process->start(program, args);
  timeout->start(kProbeTimeoutMs);
#else
  Q_UNUSED(program);
  Q_UNUSED(args);
  Q_UNUSED(done);
  checkGitHub(UpdateRoute::Download);
#endif
}

void UpdateChecker::handleAptPolicy(const QString& output) {
  const AptPolicy policy = parseAptPolicy(output);
  if (!policy.from_repository) {
    // Installed from a downloaded .deb: apt cannot upgrade it, but the
    // repository would. GitHub is the only source that knows about a release.
    checkGitHub(UpdateRoute::AptAddRepository);
    return;
  }
  if (policy.installed.isEmpty() || policy.candidate.isEmpty() || policy.candidate == policy.installed) {
    emit upToDate();
    return;
  }
  const QString version = upstreamVersion(policy.candidate);
  emit updateAvailable(ReleaseInfo{version, QString::fromLatin1(kReleaseTagUrl).arg(version), UpdateRoute::AptUpgrade});
}

void UpdateChecker::handleMaintenanceTool(int exit_code, const QString& output) {
  const QString version = parseMaintenanceToolUpdate(output, QString::fromLatin1(kComponentId));
  if (!version.isEmpty() && isNewerVersion(version, currentVersion())) {
    emit updateAvailable(
        ReleaseInfo{version, QString::fromLatin1(kReleaseTagUrl).arg(version), UpdateRoute::MaintenanceTool});
    return;
  }
  // IFW documents no exit code for check-updates; an offered update is the
  // signal, and a clean exit without one means there is none.
  if (version.isEmpty() && exit_code != 0) {
    emit checkFailed(u"maintenance tool check-updates exited %1"_s.arg(exit_code));
    return;
  }
  emit upToDate();
}

}  // namespace PJ
