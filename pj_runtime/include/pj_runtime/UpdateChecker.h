#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;

namespace PJ {

// How the user gets a newer release; decides what the shell offers them.
enum class UpdateRoute {
  Download,          // from the release page (AppImage, source, or a fallback)
  AptUpgrade,        // the apt repository already carries it: `apt upgrade`
  AptAddRepository,  // a .deb installed without the repository, which would carry it
  MaintenanceTool,   // the installed Windows maintenance tool already offers it
};

// A newer release. `name` is the human title (the tag name when GitHub's `name`
// field is empty, the upstream version on the apt and maintenance-tool routes);
// `html_url` is the release page.
struct ReleaseInfo {
  QString name;
  QString html_url;
  UpdateRoute route = UpdateRoute::Download;
};

// `apt-cache policy <package>` as far as the update check needs it.
struct AptPolicy {
  QString installed;             // empty when the package is not installed
  QString candidate;             // empty when apt has no candidate
  bool from_repository = false;  // a remote source provides the package, not just dpkg status
};

// Parses `apt-cache policy` output produced under LC_ALL=C.
AptPolicy parseAptPolicy(const QString& output);

// The version the maintenance tool's `check-updates` offers for `component_id`,
// or an empty string when it offers none.
QString parseMaintenanceToolUpdate(const QString& output, const QString& component_id);

// One-shot "is there a newer release?" check, asked of whatever will deliver it.
//
// setInstallation() picks the source from the compile-time PJ_INSTALLATION stamp:
// "deb" asks apt (the repository the package is upgraded from), "windows" asks
// the installed maintenance tool, anything else asks GitHub's `releases/latest`.
// A channel reports only what its own updater can install, so the notification
// never promises a version the user's update path does not have yet. A .deb
// installed without the repository, and a Windows build without a maintenance
// tool, fall back to GitHub.
//
// checkLatestRelease() emits AT MOST ONE of: updateAvailable, upToDate, or
// checkFailed (a check superseded by a newer checkLatestRelease() emits
// nothing). Every failure mode — network error, HTTP 404 (no release published
// yet), malformed JSON, unparseable tag, a probe that cannot run — routes to
// checkFailed, so callers can stay silent on the automatic startup path.
//
// Widget-free (lives in pj_runtime); the shell connects updateAvailable to
// whatever UI surface it wants (a toast, a menu badge, …).
class UpdateChecker : public QObject {
  Q_OBJECT

 public:
  explicit UpdateChecker(QObject* parent = nullptr);
  ~UpdateChecker() override;

  // Start the check. Any in-flight request is aborted first. Safe to call from
  // the GUI thread; the reply is handled asynchronously on the event loop.
  void checkLatestRelease();

  // Version this check compares the online tag against. Defaults (when empty)
  // to QCoreApplication::applicationVersion() at call time.
  void setCurrentVersion(const QString& version);

  // Override the releases API endpoint. Defaults to the PlotJuggler/PlotJuggler repo.
  void setReleaseApiUrl(const QUrl& url);

  // The releases API endpoint this check will GET (the built-in default, or the
  // last value passed to setReleaseApiUrl).
  QUrl releaseApiUrl() const;

  // The PJ_INSTALLATION stamp ("deb", "windows", "appimage", "source"); empty
  // or unknown asks GitHub.
  void setInstallation(const QString& installation);

  // Overrides for the channel probes (tests point these at fixtures). Defaults:
  // `apt-cache` on PATH, and maintenancetool.exe in the install root, the parent
  // of the executable's directory.
  void setAptCacheProgram(const QString& program);
  void setMaintenanceToolPath(const QString& path);
  QString maintenanceToolPath() const;

 signals:
  // A strictly-newer release exists.
  void updateAvailable(const ReleaseInfo& release);

  // The running version is current (or newer than the published release).
  void upToDate();

  // The check could not be completed (network/HTTP/parse). `reason` is for logs.
  void checkFailed(const QString& reason);

 private:
  QString currentVersion() const;

  // Asks GitHub; a newer release is reported with `route`.
  void checkGitHub(UpdateRoute route);
  // Reply lifetime is owned by httpGetWithTimeout (deleteLater after return).
  void handleReply(QNetworkReply& reply, UpdateRoute route);

  // Runs a probe program; `done` gets its exit code and stdout. A probe that
  // cannot start or times out is reported through checkFailed instead.
  void runProbe(
      const QString& program, const QStringList& args, std::function<void(int exit_code, const QString& output)> done);
  void handleAptPolicy(const QString& output);
  void handleMaintenanceTool(int exit_code, const QString& output);

  QNetworkAccessManager* network_ = nullptr;
  QNetworkReply* pending_reply_ = nullptr;
  QProcess* pending_probe_ = nullptr;
  QString current_version_;
  QUrl release_api_url_;
  QString installation_;
  QString apt_cache_program_;
  QString maintenance_tool_path_;
};

}  // namespace PJ

Q_DECLARE_METATYPE(PJ::ReleaseInfo)
