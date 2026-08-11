// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Tests for PJ::ExtensionManager
//
// Coverage:
//   [1] Install (Linux direct path): download + extract + register + state persisted
//   [2] Install guard conditions: already installed, concurrent, unsupported platform
//   [3] Uninstall: directory removed and state updated; errors on unknown id
//   [4] Update: removes old files and re-installs cleanly
//   [5] hasUpdate: multi-segment semver comparison using registry fixture data
//   [6] applyPendingInstalls: simulates the Windows post-restart staging path
//   [7] State persistence: installed state derived from disk across manager restarts
//   [8] Platform detection: currentPlatform() format and registry key resolution
//   [9] applyPendingUninstalls: deferred directory cleanup via marker file

#include "pj_marketplace/extension_manager.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileDevice>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <string>
#include <utility>

#ifndef Q_OS_WIN
#include <unistd.h>  // geteuid: root ignores the directory permissions some tests rely on
#endif

#include "pj_marketplace/download_manager.hpp"
#include "pj_marketplace/extension.hpp"
#include "pj_marketplace/platform_utils.hpp"
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

// Spins the Qt event loop until spy receives at least one signal or the deadline expires.
bool waitForSignal(QSignalSpy& spy, int timeout_ms = 5000) {
  QDeadlineTimer deadline(timeout_ms);
  while (spy.isEmpty() && !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  return !spy.isEmpty();
}

bool waitForInstallOutcome(QSignalSpy& finished, QSignalSpy& pending_restart, int timeout_ms = 5000) {
  QDeadlineTimer deadline(timeout_ms);
  while (finished.isEmpty() && pending_restart.isEmpty() && !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  return !finished.isEmpty() || !pending_restart.isEmpty();
}

// Spins the event loop until `predicate` holds, or the deadline expires. Returns
// what the predicate said last, so a caller can assert either outcome: waiting
// for something to APPEAR and waiting to confirm it never does are both real
// assertions here, and neither is a sleep on a guessed duration.
template <typename Predicate>
bool waitUntil(Predicate predicate, int timeout_ms) {
  QDeadlineTimer deadline(timeout_ms);
  while (!deadline.hasExpired()) {
    if (predicate()) {
      return true;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(5);
  }
  return predicate();
}

// Every regular file under `path`, recursively. Used as evidence that an extract
// worker is (or is no longer) writing into the store.
QStringList filesUnder(const QString& path) {
  QStringList found;
  QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    found.append(it.next());
  }
  return found;
}

// Minimal HTTP/1.1 server that answers every request with a fixed in-memory body.
// Binds to a random loopback port; no external network required.
class LocalHttpServer {
 public:
  LocalHttpServer() {
    server_.listen(QHostAddress::LocalHost, 0);
    QObject::connect(&server_, &QTcpServer::newConnection, [this]() {
      QTcpSocket* socket = server_.nextPendingConnection();
      socket->setParent(&server_);
      QObject::connect(socket, &QTcpSocket::readyRead, [this, socket]() {
        socket->readAll();  // discard the HTTP request — content is irrelevant for tests
        const QByteArray header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Content-Length: " +
            QByteArray::number(body_.size()) +
            "\r\n"
            "Connection: close\r\n\r\n";
        socket->write(header + body_);
        socket->flush();
        socket->disconnectFromHost();
      });
    });
  }

  QUrl url() const {
    return QUrl(u"http://127.0.0.1:%1/"_s.arg(server_.serverPort()));
  }

  void setBody(const QByteArray& body) {
    body_ = body;
  }

 private:
  QTcpServer server_;
  QByteArray body_;
};

// Builds an in-memory ZIP archive from a map of { relative_path -> file_content }.
// `capacity` bounds the in-memory archive; raise it for a deliberately large
// multi-entry fixture (see manyEntryPluginZip).
QByteArray buildZip(const QMap<QString, QByteArray>& files, size_t capacity = 4 * 1024 * 1024) {
  std::vector<char> buf(capacity);
  size_t used = 0;

  auto* a = archive_write_new();
  archive_write_set_format_zip(a);
  archive_write_add_filter_none(a);
  archive_write_open_memory(a, buf.data(), buf.size(), &used);

  auto* entry = archive_entry_new();
  for (auto it = files.cbegin(); it != files.cend(); ++it) {
    archive_entry_clear(entry);
    archive_entry_set_pathname(entry, it.key().toUtf8().constData());
    archive_entry_set_size(entry, it.value().size());
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_write_header(a, entry);
    archive_write_data(a, it.value().constData(), static_cast<size_t>(it.value().size()));
  }
  archive_entry_free(entry);
  archive_write_close(a);
  archive_write_free(a);

  return QByteArray(buf.data(), static_cast<int>(used));
}

QByteArray readAll(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  return file.readAll();
}

QString pluginPathForId(const QString& ext_id, const QString& version = "1.0.0") {
  if (ext_id == "mock-data-source" && version == "2.0.0") {
    return QStringLiteral(PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH);
  }
  if (ext_id == "mock-data-source") {
    return QStringLiteral(PJ_MOCK_DATA_SOURCE_PLUGIN_PATH);
  }
  if (ext_id == "mock-file-source") {
    return QStringLiteral(PJ_MOCK_FILE_SOURCE_PLUGIN_PATH);
  }
  if (ext_id == "pinning-data-source" && version == "2.0.0") {
    return QStringLiteral(PJ_MOCK_PINNING_V2_PLUGIN_PATH);
  }
  if (ext_id == "pinning-data-source") {
    return QStringLiteral(PJ_MOCK_PINNING_PLUGIN_PATH);
  }
  if (ext_id == "missing-id-source") {
    return QStringLiteral(PJ_MISSING_ID_PLUGIN_PATH);
  }
  return {};
}

QString pluginFileName() {
  return "plugin" + QString::fromStdString(PlatformUtils::pluginExtension());
}

// Backups are named "<id>-<uuid>" (the old version is no longer dlopened to read
// its version for the name), so look them up by prefix rather than exact name.
QStringList backupDirsFor(const QString& id) {
  return QDir(PlatformUtils::backupDir()).entryList(QStringList{id + "-*"}, QDir::Dirs);
}

void cleanBackups(const QString& id) {
  const QDir dir(PlatformUtils::backupDir());
  for (const QString& name : backupDirsFor(id)) {
    QDir(dir.absoluteFilePath(name)).removeRecursively();
  }
}

bool writePendingIntentForTest(const QString& staged_dir, const QString& id, const QString& version = "1.0.0") {
  QFile intent(QDir(staged_dir).absoluteFilePath(".pj_pending_install"));
  if (!intent.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    return false;
  }
  const QByteArray data = id.toUtf8() + '\n' + version.toUtf8() + '\n';
  return intent.write(data) == data.size();
}

bool copyFixturePlugin(const QString& dst_dir, const QString& fixture_id, const QString& version = "1.0.0") {
  QDir().mkpath(dst_dir);
  QFile::remove(QDir(dst_dir).absoluteFilePath(pluginFileName()));
  return QFile::copy(pluginPathForId(fixture_id, version), QDir(dst_dir).absoluteFilePath(pluginFileName()));
}

bool directoryHasNoChildren(const QString& path) {
  const QFileInfoList entries =
      QDir(path).entryInfoList(QDir::Dirs | QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
  return entries.isEmpty();
}

QByteArray pluginZipWithDso(const QString& ext_id, const QString& fixture_id, const QString& version = "1.0.0") {
  const QByteArray plugin = readAll(pluginPathForId(fixture_id, version));
  if (plugin.isEmpty()) {
    return buildZip({{ext_id + "/not-a-plugin.txt", "missing fixture plugin"}});
  }
  return buildZip({
      {ext_id + "/" + pluginFileName(), plugin},
  });
}

// Returns a ZIP with an <id>/ root directory and a real plugin DSO whose
// embedded manifest is the installed-state source of truth. There is no local
// metadata sidecar.
QByteArray dummyPluginZip(const QString& ext_id, const QString& version = "1.0.0") {
  return pluginZipWithDso(ext_id, ext_id, version);
}

// A valid plugin archive padded with `filler_count` extra small entries, so its
// extraction takes long enough to still be running when a test tears the manager
// down. MANY SMALL entries rather than one big file on purpose: the extract worker
// only observes cancellation at entry and block boundaries, so entry count is what
// makes the in-flight window reliably observable.
QByteArray manyEntryPluginZip(const QString& ext_id, int filler_count) {
  QMap<QString, QByteArray> files;
  files.insert(ext_id + "/" + pluginFileName(), readAll(pluginPathForId(ext_id)));
  const QByteArray filler(4096, 'x');
  for (int index = 0; index < filler_count; ++index) {
    files.insert(u"%1/filler/%2.bin"_s.arg(ext_id).arg(index, 5, 10, QChar('0')), filler);
  }
  return buildZip(files, 64 * 1024 * 1024);
}

QByteArray pluginZipWithTwoDsos(const QString& ext_id, const QString& first_id, const QString& second_id) {
  const QString suffix = QString::fromStdString(PlatformUtils::pluginExtension());
  return buildZip({
      {ext_id + "/first" + suffix, readAll(pluginPathForId(first_id))},
      {ext_id + "/second" + suffix, readAll(pluginPathForId(second_id))},
  });
}

// Scopes QCoreApplication::applicationVersion() for a single test — the value
// is process-wide, so pinning it at the top of the test and restoring it in
// the destructor keeps sibling tests independent. Used by the
// hostCompatibility() coverage below.
class ScopedApplicationVersion {
 public:
  explicit ScopedApplicationVersion(const QString& v) : previous_(QCoreApplication::applicationVersion()) {
    QCoreApplication::setApplicationVersion(v);
  }
  ~ScopedApplicationVersion() {
    QCoreApplication::setApplicationVersion(previous_);
  }
  ScopedApplicationVersion(const ScopedApplicationVersion&) = delete;
  ScopedApplicationVersion& operator=(const ScopedApplicationVersion&) = delete;

 private:
  QString previous_;
};

// Names of the transaction directories sitting directly under `path`.
QStringList transactionDirsIn(const QString& path) {
  return QDir(path).entryList(QStringList{u".pj_install_*"_s}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
}

// The staging area every install extracts into. Deliberately spells out the
// layout ExtensionManager produces rather than asking it: the placement IS what
// the tests below pin down, and it must stay a sibling of the extensions dir -
// same filesystem (so the promoting rename is atomic), outside the recursive
// plugin scan (so scratch is never loadable content).
QString transactionStageDirFor(const QString& extensions_dir) {
  return extensions_dir + u".install_stage"_s;
}

// The journal ExtensionManager records pending removals in. Spelled out for the
// same reason as transactionStageDirFor: the PLACEMENT is the thing under test. It
// has to stay a sibling of the extensions dir so that a record can never live
// inside the payload whose deletion it describes - a recursive delete that removed
// its own retry record was the defect this journal replaces.
QString removalJournalDirFor(const QString& extensions_dir) {
  return extensions_dir + u".state"_s;
}

QString removalIntentFileFor(const QString& extensions_dir, const QString& id) {
  return removalJournalDirFor(extensions_dir) + "/" + id + u".json"_s;
}

// The recorded intent for `id`, or an empty object when nothing is journalled.
QJsonObject removalIntentFor(const QString& extensions_dir, const QString& id) {
  QFile record(removalIntentFileFor(extensions_dir, id));
  if (!record.open(QIODevice::ReadOnly)) {
    return {};
  }
  return QJsonDocument::fromJson(record.readAll()).object();
}

// Plants a journal record by hand, standing in for corruption or for a hostile
// writer trying to borrow the drain's delete privilege. `stem` names the file, so
// a test can file a record under a name that disagrees with the id inside.
bool writeRawRemovalRecord(const QString& extensions_dir, const QString& stem, const QByteArray& contents) {
  if (!QDir().mkpath(removalJournalDirFor(extensions_dir))) {
    return false;
  }
  QFile record(removalJournalDirFor(extensions_dir) + "/" + stem + u".json"_s);
  if (!record.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    return false;
  }
  return record.write(contents) == contents.size();
}

// A well-formed record body, so each test can corrupt exactly one field.
QByteArray removalRecordJson(const QString& operation, const QString& id, const QString& path, int schema = 1) {
  QJsonObject object;
  if (schema != 0) {
    object[u"schema"_s] = schema;
  }
  if (!operation.isNull()) {
    object[u"operation"_s] = operation;
  }
  object[u"id"_s] = id;
  object[u"path"_s] = path;
  return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

// Records the drain moved aside rather than acting on.
QStringList quarantinedRecordsIn(const QString& extensions_dir) {
  return QDir(removalJournalDirFor(extensions_dir))
      .entryList(QStringList{u"*.rejected-*"_s}, QDir::Files | QDir::Hidden);
}

// Clamps a directory's permissions for the duration of a test and restores them
// on scope exit. Used to make a removal fail on purpose; the restore must happen
// even when an ASSERT bails out early, or QTemporaryDir would be unable to clean
// up and the tree would be left behind in the system temp area.
class ScopedDirectoryPermissions {
 public:
  ScopedDirectoryPermissions(QString path, QFileDevice::Permissions locked)
      : path_(std::move(path)), previous_(QFile::permissions(path_)) {
    QFile::setPermissions(path_, locked);
  }
  ~ScopedDirectoryPermissions() {
    QFile::setPermissions(path_, previous_);
  }
  ScopedDirectoryPermissions(const ScopedDirectoryPermissions&) = delete;
  ScopedDirectoryPermissions& operator=(const ScopedDirectoryPermissions&) = delete;

 private:
  QString path_;
  QFileDevice::Permissions previous_;
};

// Builds an Extension whose download artifact for the current platform points to `url`.
// Checksum is empty by default so DownloadManager skips SHA-256 verification.
Extension makeExtension(const QString& id, const QString& version, const QUrl& url, const QString& checksum = {}) {
  Extension ext;
  ext.id = id;
  ext.name = id;
  ext.version = version;

  Platform p;
  p.url = url.toString();
  p.checksum = checksum;
  ext.platforms[PlatformUtils::currentPlatform()] = p;
  return ext;
}

// ---------------------------------------------------------------------------
// Test fixture — isolated temp directories and a fresh manager per test
// ---------------------------------------------------------------------------

class ExtensionManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Several tests place their own QTemporaryDir under PlatformUtils::configDir()
    // (next to backupDir()) so QDir::rename() into the backup is an atomic same-fs
    // move. QTemporaryDir refuses to create itself when the parent does not exist,
    // which is exactly the state of a fresh CI runner. Pre-create configDir() here
    // so those constructions succeed regardless of host history.
    ASSERT_TRUE(QDir().mkpath(PlatformUtils::configDir()));
    ASSERT_TRUE(ext_dir_.isValid());
    ASSERT_TRUE(pending_dir_.isValid());
    downloader_ = new DownloadManager();
    mgr_ = new ExtensionManager(downloader_, ext_dir_.path(), pending_dir_.path());
  }

  void TearDown() override {
    delete mgr_;
    delete downloader_;
    // The staging area sits NEXT TO ext_dir_, so QTemporaryDir does not remove it:
    // a test that abandons an in-flight install (the concurrency guards do, on
    // purpose) orphans a transaction there, exactly as a crashed session would.
    QDir(transactionStageDirFor(ext_dir_.path())).removeRecursively();
  }

  QTemporaryDir ext_dir_;
  QTemporaryDir pending_dir_;
  LocalHttpServer server_;
  DownloadManager* downloader_ = nullptr;
  ExtensionManager* mgr_ = nullptr;
};

// ---------------------------------------------------------------------------
// [1] Direct install (Linux path)
// ---------------------------------------------------------------------------

// A fresh install downloads the ZIP, extracts it, registers the extension, and
// emits the correct signal sequence: installStarted → installFinished(id, true).
TEST_F(ExtensionManagerTest, InstallDirectRegistersExtension) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy_started(mgr_, &ExtensionManager::installStarted);
  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->install(ext);

  // installStarted must be synchronous — no event loop needed.
  ASSERT_EQ(spy_started.count(), 1);
  EXPECT_EQ(spy_started.first().at(0).toString(), "mock-data-source");

  ASSERT_TRUE(waitForSignal(spy_finished)) << "installFinished not received within 5 s";
  ASSERT_EQ(spy_finished.count(), 1);
  EXPECT_EQ(spy_finished.first().at(0).toString(), "mock-data-source");
  EXPECT_TRUE(spy_finished.first().at(1).toBool()) << "install must succeed";
  EXPECT_TRUE(spy_error.isEmpty());

  EXPECT_TRUE(mgr_->isInstalled("mock-data-source"));
  EXPECT_EQ(mgr_->installedExtensions()["mock-data-source"].version, "1.0.0");
}

// The extracted content lands under extensions_dir/<id>/ after a successful install.
TEST_F(ExtensionManagerTest, InstallCreatesExtensionDirectory) {
  server_.setBody(dummyPluginZip("mock-file-source"));
  const Extension ext = makeExtension("mock-file-source", "1.0.0", server_.url());

  QSignalSpy spy(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext);
  ASSERT_TRUE(waitForSignal(spy));
  ASSERT_TRUE(spy.first().at(1).toBool());

  EXPECT_TRUE(QDir(ext_dir_.path() + "/mock-file-source").exists());
}

// installProgress signals are forwarded during the download phase.
// Each signal must carry the correct extension id and a percent in [0, 100].
TEST_F(ExtensionManagerTest, InstallEmitsProgressSignals) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy_progress(mgr_, &ExtensionManager::installProgress);
  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);

  mgr_->install(ext);
  ASSERT_TRUE(waitForSignal(spy_finished));

  EXPECT_GE(spy_progress.count(), 1);
  for (const QList<QVariant>& args : spy_progress) {
    EXPECT_EQ(args.at(0).toString(), "mock-data-source");
    const int pct = args.at(1).toInt();
    EXPECT_GE(pct, 0);
    EXPECT_LE(pct, 100);
  }
}

// ---------------------------------------------------------------------------
// [2] Install guard conditions
// ---------------------------------------------------------------------------

// Calling install() for an extension that is already installed must emit installError
// immediately — it must not start a new download.
TEST_F(ExtensionManagerTest, InstallRejectsAlreadyInstalledExtension) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());

  // First install — must succeed.
  QSignalSpy spy_first(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext);
  ASSERT_TRUE(waitForSignal(spy_first));
  ASSERT_TRUE(spy_first.first().at(1).toBool());

  // Second install — must be rejected with an error.
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);
  mgr_->install(ext);
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_EQ(spy_error.first().at(0).toString(), "mock-data-source");
  EXPECT_FALSE(spy_error.first().at(1).toString().isEmpty());
}

// Calling install() for a second extension while one is already in progress must
// reject the second request immediately via installError.
TEST_F(ExtensionManagerTest, InstallBlocksConcurrentRequests) {
  // A server that accepts TCP connections but never sends any data keeps the first
  // download pending indefinitely without burning CPU or requiring a timeout.
  QTcpServer hanging_server;
  hanging_server.listen(QHostAddress::LocalHost, 0);
  const QUrl hanging_url = QUrl(u"http://127.0.0.1:%1/"_s.arg(hanging_server.serverPort()));

  const Extension ext_a = makeExtension("mock-data-source", "1.0.0", hanging_url);
  const Extension ext_b = makeExtension("mock-file-source", "1.0.0", hanging_url);

  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->install(ext_a);  // begins — will hang until TearDown cleans up
  QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
  mgr_->install(ext_b);  // must be rejected immediately; pending_id_ is already set

  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_EQ(spy_error.first().at(0).toString(), "mock-file-source");
  EXPECT_FALSE(spy_error.first().at(1).toString().isEmpty());
}

// Symmetric to InstallBlocksConcurrentRequests: installFromLocalZip() must
// also reject if a registry install is in flight (both paths share the
// pending_id_ guard). The UI queues sideloads behind card actions so users
// never see this error, but the manager still enforces single-install-at-a-
// time at the boundary — this locks that invariant in either direction.
TEST_F(ExtensionManagerTest, InstallFromLocalZipBlockedByConcurrentInstall) {
  QTcpServer hanging_server;
  hanging_server.listen(QHostAddress::LocalHost, 0);
  const QUrl hanging_url = QUrl(u"http://127.0.0.1:%1/"_s.arg(hanging_server.serverPort()));

  const Extension ext_a = makeExtension("mock-data-source", "1.0.0", hanging_url);

  // A real on-disk zip: installFromLocalZip() reads the file at the boundary
  // before touching the pending_id_ guard, so we cannot fake the path.
  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  const QString zip_path = QDir(src.path()).absoluteFilePath("pkg.zip");
  {
    QFile f(zip_path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(dummyPluginZip("mock-file-source"));
  }

  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->install(ext_a);  // begins — will hang until TearDown cleans up
  QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
  mgr_->installFromLocalZip(zip_path);  // must be rejected immediately

  ASSERT_EQ(spy_error.count(), 1);
  // The failure is reported against the sideload's file label, not the
  // running download's id.
  EXPECT_EQ(spy_error.first().at(0).toString(), "pkg.zip");
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("already in progress"));
}

// If the Extension's platforms map does not contain the current platform, install()
// must emit installError without initiating any download.
TEST_F(ExtensionManagerTest, InstallRejectsUnsupportedPlatform) {
  Extension ext;
  ext.id = "fft-toolbox";
  ext.name = "FFT Toolbox";
  ext.version = "1.0.0";
  // Only register an artifact for a platform we will never run on.
  ext.platforms["nonexistent-platform"] = Platform{"http://example.com/dummy.zip", ""};

  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);
  QSignalSpy spy_started(mgr_, &ExtensionManager::installStarted);

  mgr_->install(ext);

  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_EQ(spy_error.first().at(0).toString(), "fft-toolbox");
  EXPECT_FALSE(spy_error.first().at(1).toString().isEmpty());
  EXPECT_EQ(spy_started.count(), 0) << "installStarted must not fire when the platform is unsupported";
}

// ---------------------------------------------------------------------------
// hostCompatibility() — the primitive: platform match + host >= min version.
// applicationVersion() is process-wide so each test scopes its own value via
// the ScopedApplicationVersion RAII helper declared earlier in this file.
// ---------------------------------------------------------------------------

TEST_F(ExtensionManagerTest, HostCompatibilityAcceptsCurrentPlatformWithNoMinVersion) {
  Extension ext = makeExtension("plugin-x", "1.0.0", server_.url());
  ext.min_plotjuggler_version = "";  // advisory soft floor absent

  const auto compat = mgr_->hostCompatibility(ext);
  EXPECT_TRUE(compat.ok);
  EXPECT_TRUE(compat.reason.isEmpty());
}

TEST_F(ExtensionManagerTest, HostCompatibilityRejectsPlatformNotListed) {
  Extension ext;
  ext.id = "plugin-x";
  ext.name = "plugin-x";
  ext.version = "1.0.0";
  ext.platforms["nonexistent-platform"] = Platform{"http://example.com/dummy.zip", ""};

  const auto compat = mgr_->hostCompatibility(ext);
  EXPECT_FALSE(compat.ok);
  EXPECT_FALSE(compat.reason.isEmpty()) << "reason must be non-empty when compatibility fails";
}

// A locally-sideloaded install (installFromLocalZip → row synthesized by
// MarketplaceWindow::rebuildExtensionList) carries no registry-sourced fields,
// so `platforms` is empty by design. Treating that as incompatible would
// mislabel a working local install; empty-platforms must be read as "no
// registry declaration" and pass the gate.
TEST_F(ExtensionManagerTest, HostCompatibilityAcceptsEmptyPlatformsAsLocallyDeclared) {
  Extension ext;
  ext.id = "local-plugin";
  ext.name = "local-plugin";
  ext.version = "1.0.0";
  // ext.platforms is deliberately left empty — this is what the synthesized
  // local-only marketplace row carries.

  const auto compat = mgr_->hostCompatibility(ext);
  EXPECT_TRUE(compat.ok) << "an ext with an empty platforms map is a local install without registry metadata — "
                            "the platform gate must not fire on it";
  EXPECT_TRUE(compat.reason.isEmpty());
}

TEST_F(ExtensionManagerTest, HostCompatibilityRejectsHostBelowDeclaredMinimum) {
  const ScopedApplicationVersion host{"3.9.0"};
  Extension ext = makeExtension("plugin-x", "1.0.0", server_.url());
  ext.min_plotjuggler_version = "4.0.0";

  const auto compat = mgr_->hostCompatibility(ext);
  EXPECT_FALSE(compat.ok);
  EXPECT_TRUE(compat.reason.contains("4.0.0"))
      << "reason must surface the required minimum: " << compat.reason.toStdString();
}

TEST_F(ExtensionManagerTest, HostCompatibilityAcceptsHostAtOrAboveDeclaredMinimum) {
  const ScopedApplicationVersion host{"4.0.0"};
  Extension ext = makeExtension("plugin-x", "1.0.0", server_.url());
  ext.min_plotjuggler_version = "4.0.0";

  EXPECT_TRUE(mgr_->hostCompatibility(ext).ok) << "host equal to min must be accepted";

  const ScopedApplicationVersion host_newer{"4.5.1"};
  EXPECT_TRUE(mgr_->hostCompatibility(ext).ok) << "host above min must be accepted";
}

// R1: install() must refuse an extension whose declared minimum host is newer
// than the running build. Both installError AND installFinished(id, false)
// must fire — callers that await installFinished would otherwise hang, and
// callers subscribed to installError would miss the failure entirely if only
// one of the two were emitted.
TEST_F(ExtensionManagerTest, InstallRefusesWhenHostBelowMinPlotjugglerVersion) {
  const ScopedApplicationVersion host{"3.9.0"};
  Extension ext = makeExtension("plugin-x", "1.0.0", server_.url());
  ext.min_plotjuggler_version = "4.0.0";

  QSignalSpy started(mgr_, &ExtensionManager::installStarted);
  QSignalSpy error(mgr_, &ExtensionManager::installError);
  QSignalSpy finished(mgr_, &ExtensionManager::installFinished);

  mgr_->install(ext);

  EXPECT_EQ(started.count(), 0) << "installStarted must not fire when the host is below the plugin's minimum";
  ASSERT_EQ(error.count(), 1);
  EXPECT_EQ(error.first().at(0).toString(), "plugin-x");
  EXPECT_TRUE(error.first().at(1).toString().contains("4.0.0"));
  ASSERT_EQ(finished.count(), 1) << "installFinished must fire so callers awaiting it do not hang";
  EXPECT_EQ(finished.first().at(0).toString(), "plugin-x");
  EXPECT_FALSE(finished.first().at(1).toBool());
}

TEST_F(ExtensionManagerTest, InstallRejectsEmbeddedIdMismatch) {
  server_.setBody(pluginZipWithDso("registry-id", "mock-data-source"));
  const Extension ext = makeExtension("registry-id", "1.0.0", server_.url());

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->install(ext);

  ASSERT_TRUE(waitForSignal(spy_finished));
  EXPECT_FALSE(spy_finished.first().at(1).toBool());
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("Embedded plugin id"));
  EXPECT_FALSE(QDir(ext_dir_.path() + "/registry-id").exists());
  EXPECT_FALSE(mgr_->isInstalled("registry-id"));
  EXPECT_FALSE(mgr_->isInstalled("mock-data-source"));
}

TEST_F(ExtensionManagerTest, InstallRejectsEmbeddedVersionMismatch) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "2.0.0", server_.url());

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->install(ext);

  ASSERT_TRUE(waitForSignal(spy_finished));
  EXPECT_FALSE(spy_finished.first().at(1).toBool());
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("Embedded plugin version"));
  EXPECT_FALSE(QDir(ext_dir_.path() + "/mock-data-source").exists());
  EXPECT_FALSE(mgr_->isInstalled("mock-data-source"));
}

TEST_F(ExtensionManagerTest, InstallRejectsManifestMissingId) {
  server_.setBody(dummyPluginZip("missing-id-source"));
  const Extension ext = makeExtension("missing-id-source", "1.0.0", server_.url());

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->install(ext);

  ASSERT_TRUE(waitForSignal(spy_finished));
  EXPECT_FALSE(spy_finished.first().at(1).toBool());
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("not a valid plugin"));
  EXPECT_FALSE(QDir(ext_dir_.path() + "/missing-id-source").exists());
}

TEST_F(ExtensionManagerTest, InstallRejectsExtensionDirectoryWithConflictingEmbeddedIds) {
  server_.setBody(pluginZipWithTwoDsos("mock-data-source", "mock-data-source", "mock-file-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->install(ext);

  ASSERT_TRUE(waitForSignal(spy_finished));
  EXPECT_FALSE(spy_finished.first().at(1).toBool());
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("multiple embedded plugin ids"));
  EXPECT_FALSE(QDir(ext_dir_.path() + "/mock-data-source").exists());
}

TEST_F(ExtensionManagerTest, InstallRejectsWrongTopLevelDirectoryWithoutLeavingStrays) {
  server_.setBody(pluginZipWithDso("wrong-root", "mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->install(ext);

  ASSERT_TRUE(waitForSignal(spy_finished));
  EXPECT_FALSE(spy_finished.first().at(1).toBool());
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("top-level directory"));
  EXPECT_FALSE(QDir(ext_dir_.path() + "/wrong-root").exists());
  EXPECT_FALSE(QDir(ext_dir_.path() + "/mock-data-source").exists());
  EXPECT_TRUE(directoryHasNoChildren(ext_dir_.path()));
  EXPECT_FALSE(mgr_->isInstalled("mock-data-source"));
}

TEST_F(ExtensionManagerTest, InstallRejectsExtraTopLevelDirectoryWithoutLeavingStrays) {
  server_.setBody(buildZip({
      {"mock-data-source/" + pluginFileName(), readAll(pluginPathForId("mock-data-source"))},
      {"unrelated-extension/" + pluginFileName(), readAll(pluginPathForId("mock-file-source"))},
  }));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->install(ext);

  ASSERT_TRUE(waitForSignal(spy_finished));
  EXPECT_FALSE(spy_finished.first().at(1).toBool());
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("top-level directory"));
  EXPECT_FALSE(QDir(ext_dir_.path() + "/mock-data-source").exists());
  EXPECT_FALSE(QDir(ext_dir_.path() + "/unrelated-extension").exists());
  EXPECT_TRUE(directoryHasNoChildren(ext_dir_.path()));
  EXPECT_FALSE(mgr_->isInstalled("mock-data-source"));
  EXPECT_FALSE(mgr_->isInstalled("unrelated-extension"));
}

// ---------------------------------------------------------------------------
// [3] Uninstall
// ---------------------------------------------------------------------------

// Uninstall stages the removal on every platform: the directory stays on disk
// carrying the marker until the next launch drains it, while the extension
// leaves the installed set right away. Removing in place would free the
// directory NAME for reuse with the session's DSO still mapped, and dlopen
// resolves by path name — a reinstall into that name would then be answered
// from the resident image instead of the payload on disk.
TEST_F(ExtensionManagerTest, UninstallStagesRemovalUntilRestart) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy_install(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext);
  ASSERT_TRUE(waitForSignal(spy_install));
  ASSERT_TRUE(spy_install.first().at(1).toBool());

  const QString ext_path = ext_dir_.path() + "/mock-data-source";
  ASSERT_TRUE(QDir(ext_path).exists());

  QSignalSpy spy_pending(mgr_, &ExtensionManager::uninstallPendingRestart);
  QSignalSpy spy_finished(mgr_, &ExtensionManager::uninstallFinished);
  mgr_->uninstall("mock-data-source");

  ASSERT_EQ(spy_pending.count(), 1);
  EXPECT_EQ(spy_pending.first().at(0).toString(), "mock-data-source");
  EXPECT_TRUE(spy_finished.isEmpty()) << "a staged uninstall must not report as completed";

  EXPECT_FALSE(mgr_->isInstalled("mock-data-source"));
  EXPECT_TRUE(mgr_->hasPendingUninstall("mock-data-source"));
  EXPECT_TRUE(QDir(ext_path).exists()) << "the directory name must stay taken until the restart";

  // The restart-time drain is what actually removes it.
  mgr_->applyPendingUninstalls();
  EXPECT_FALSE(QDir(ext_path).exists());
}

// Installing over a staged uninstall would be erased by the next startup drain,
// which deletes whatever occupies that directory name. isInstalled() no longer
// speaks for the id at this point, so the pending marker is the guard.
TEST_F(ExtensionManagerTest, InstallRejectsAnIdWithAStagedUninstall) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy_install(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext);
  ASSERT_TRUE(waitForSignal(spy_install));
  mgr_->uninstall("mock-data-source");
  ASSERT_TRUE(mgr_->hasPendingUninstall("mock-data-source"));
  ASSERT_FALSE(mgr_->isInstalled("mock-data-source"));

  spy_install.clear();
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);
  mgr_->install(ext);

  ASSERT_TRUE(waitForSignal(spy_install));
  EXPECT_FALSE(spy_install.first().at(1).toBool());
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("staged"))
      << spy_error.first().at(1).toString().toStdString();
}

// The sideload path does not route through doInstall(), so it needs the guard of
// its own: its fresh-install branch clears the destination directory — marker
// included — which would make the pending removal disappear unannounced.
TEST_F(ExtensionManagerTest, InstallFromLocalZipRejectsAnIdWithAStagedUninstall) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  QSignalSpy spy_install(mgr_, &ExtensionManager::installFinished);
  mgr_->install(makeExtension("mock-data-source", "1.0.0", server_.url()));
  ASSERT_TRUE(waitForSignal(spy_install));
  mgr_->uninstall("mock-data-source");
  ASSERT_TRUE(mgr_->hasPendingUninstall("mock-data-source"));

  const QString zip_path = QDir(ext_dir_.path()).absoluteFilePath("sideload.zip");
  QFile zip(zip_path);
  ASSERT_TRUE(zip.open(QIODevice::WriteOnly));
  zip.write(dummyPluginZip("mock-data-source"));
  zip.close();

  spy_install.clear();
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);
  mgr_->installFromLocalZip(zip_path);

  ASSERT_TRUE(waitForSignal(spy_install));
  EXPECT_FALSE(spy_install.first().at(1).toBool());
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("staged"))
      << spy_error.first().at(1).toString().toStdString();
  EXPECT_TRUE(mgr_->hasPendingUninstall("mock-data-source")) << "the pending removal must survive the refused install";
}

// The scan takes the id from the embedded manifest, so an extension directory
// placed by hand can be named anything. The journal records the path uninstall()
// actually found, keyed by id; reporting the pending state by looking only under
// "<id>" would miss it, leaving the card without its "Needs Restart" state and the
// install guards inert for a removal that is genuinely staged.
TEST_F(ExtensionManagerTest, PendingUninstallIsVisibleForADirectoryNotNamedForItsId) {
  const QString odd_dir = ext_dir_.path() + "/oddly-named-dir";
  ASSERT_TRUE(copyFixturePlugin(odd_dir, "mock-data-source"));
  mgr_->refreshInstalledFromDisk();
  ASSERT_TRUE(mgr_->isInstalled("mock-data-source"));
  ASSERT_FALSE(QDir(ext_dir_.path() + "/mock-data-source").exists()) << "the directory name must differ from the id";

  QSignalSpy spy_pending(mgr_, &ExtensionManager::uninstallPendingRestart);
  mgr_->uninstall("mock-data-source");
  ASSERT_EQ(spy_pending.count(), 1);

  const QJsonObject intent = removalIntentFor(ext_dir_.path(), "mock-data-source");
  EXPECT_EQ(intent.value("operation").toString(), "uninstall");
  EXPECT_EQ(QDir::cleanPath(intent.value("path").toString()), QDir::cleanPath(odd_dir))
      << "the record must name the directory the scan actually found, not the id's canonical path";
  EXPECT_FALSE(QFile::exists(odd_dir + "/.pj_pending_uninstall"))
      << "nothing is written into the payload any more; the record lives outside it";
  EXPECT_TRUE(mgr_->hasPendingUninstall("mock-data-source"))
      << "the staged removal must be visible to the badge and the install guards";

  // The drain finds it by scanning, so the removal itself was never at risk.
  mgr_->applyPendingUninstalls();
  EXPECT_FALSE(QDir(odd_dir).exists());
  mgr_->refreshInstalledFromDisk();
  EXPECT_FALSE(mgr_->hasPendingUninstall("mock-data-source"));
}

// The record outlives installedExtensions() so a UI can still show the pending
// state, and retires itself once the marker is gone.
TEST_F(ExtensionManagerTest, StagedUninstallKeepsTheRecordUntilTheMarkerIsGone) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  QSignalSpy spy_install(mgr_, &ExtensionManager::installFinished);
  mgr_->install(makeExtension("mock-data-source", "1.0.0", server_.url()));
  ASSERT_TRUE(waitForSignal(spy_install));
  ASSERT_TRUE(mgr_->stagedUninstalls().isEmpty());

  mgr_->uninstall("mock-data-source");
  ASSERT_FALSE(mgr_->isInstalled("mock-data-source"));
  const auto staged = mgr_->stagedUninstalls();
  ASSERT_TRUE(staged.contains("mock-data-source"));
  EXPECT_EQ(staged["mock-data-source"].version, "1.0.0");
  EXPECT_FALSE(staged["mock-data-source"].name.isEmpty()) << "the row needs a label to render";

  // A rescan while the removal is still pending keeps it.
  mgr_->refreshInstalledFromDisk();
  EXPECT_TRUE(mgr_->stagedUninstalls().contains("mock-data-source"));

  // Once the restart-time drain has removed the directory, the record retires.
  mgr_->applyPendingUninstalls();
  mgr_->refreshInstalledFromDisk();
  EXPECT_FALSE(mgr_->stagedUninstalls().contains("mock-data-source"));
}

// Attempting to uninstall an extension that was never installed must emit
// uninstallError and uninstallFinished(id, false) without touching the filesystem.
TEST_F(ExtensionManagerTest, UninstallUnknownExtensionEmitsError) {
  QSignalSpy spy_error(mgr_, &ExtensionManager::uninstallError);
  QSignalSpy spy_finished(mgr_, &ExtensionManager::uninstallFinished);

  mgr_->uninstall("nonexistent-extension");

  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_EQ(spy_error.first().at(0).toString(), "nonexistent-extension");
  EXPECT_FALSE(spy_error.first().at(1).toString().isEmpty());

  ASSERT_EQ(spy_finished.count(), 1);
  EXPECT_FALSE(spy_finished.first().at(1).toBool());
}

// ---------------------------------------------------------------------------
// [4] Update
// ---------------------------------------------------------------------------

// update() stages the new version instead of hot-swapping the live install:
// it emits installPendingRestart, leaves the running version in place, and only
// a restart (applyPendingInstalls) promotes the staged copy. ext/pending/backup
// share one filesystem so the promotion renames (staged -> extensions, old ->
// backup) are atomic moves, not cross-device copies.
TEST_F(ExtensionManagerTest, UpdateStagesNewVersionUntilRestart) {
  // Ensure clean backup state before test (in case previous run failed mid-test).
  cleanBackups("mock-data-source");

  QTemporaryDir local_ext_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_ext_XXXXXX"));
  ASSERT_TRUE(local_ext_dir.isValid());
  QTemporaryDir local_pending_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_pending_XXXXXX"));
  ASSERT_TRUE(local_pending_dir.isValid());

  DownloadManager local_dl;
  ExtensionManager local_mgr(&local_dl, local_ext_dir.path(), local_pending_dir.path());

  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext_v1 = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy_finished(&local_mgr, &ExtensionManager::installFinished);
  local_mgr.install(ext_v1);
  ASSERT_TRUE(waitForSignal(spy_finished));
  ASSERT_TRUE(spy_finished.first().at(1).toBool());

  // Update to v2: must STAGE (installPendingRestart), not swap the live install.
  server_.setBody(dummyPluginZip("mock-data-source", "2.0.0"));
  const Extension ext_v2 = makeExtension("mock-data-source", "2.0.0", server_.url());

  QSignalSpy spy_pending(&local_mgr, &ExtensionManager::installPendingRestart);
  spy_finished.clear();
  local_mgr.update(ext_v2);

  ASSERT_TRUE(waitForSignal(spy_pending)) << "update must stage and emit installPendingRestart";
  EXPECT_EQ(spy_pending.first().at(0).toString(), "mock-data-source");
  EXPECT_TRUE(spy_finished.isEmpty()) << "update must NOT promote immediately";
  // The live install is untouched and the new version waits in the pending dir.
  EXPECT_EQ(local_mgr.installedExtensions()["mock-data-source"].version, "1.0.0");
  EXPECT_TRUE(local_mgr.hasPendingInstall("mock-data-source"));
  EXPECT_TRUE(QDir(local_pending_dir.path() + "/mock-data-source").exists());

  // Restart: applyPendingInstalls() promotes the staged v2 (emits synchronously).
  QSignalSpy spy_promoted(&local_mgr, &ExtensionManager::installFinished);
  local_mgr.applyPendingInstalls();
  ASSERT_EQ(spy_promoted.count(), 1);
  EXPECT_TRUE(spy_promoted.first().at(1).toBool());
  EXPECT_EQ(local_mgr.installedExtensions()["mock-data-source"].version, "2.0.0");
  EXPECT_FALSE(QDir(local_pending_dir.path() + "/mock-data-source").exists());

  cleanBackups("mock-data-source");
}

// Promoting a staged update backs up the previous version into backupDir().
// The backup now happens at promotion time (applyPendingInstalls), not in
// update(). ext/pending/backup share one filesystem so the renames are atomic.
TEST_F(ExtensionManagerTest, UpdatePromotionBacksUpOldVersion) {
  // Ensure clean backup state before test (in case previous run failed mid-test).
  cleanBackups("mock-data-source");

  QTemporaryDir local_ext_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_ext_XXXXXX"));
  ASSERT_TRUE(local_ext_dir.isValid());
  QTemporaryDir local_pending_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_pending_XXXXXX"));
  ASSERT_TRUE(local_pending_dir.isValid());

  DownloadManager local_dl;
  ExtensionManager local_mgr(&local_dl, local_ext_dir.path(), local_pending_dir.path());

  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext_v1 = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy_install(&local_mgr, &ExtensionManager::installFinished);
  local_mgr.install(ext_v1);
  ASSERT_TRUE(waitForSignal(spy_install));
  ASSERT_TRUE(spy_install.first().at(1).toBool());

  ASSERT_TRUE(QFile::exists(local_ext_dir.path() + "/mock-data-source/" + pluginFileName()));

  // Stage the update, then promote it as a restart would.
  server_.setBody(dummyPluginZip("mock-data-source", "2.0.0"));
  const Extension ext_v2 = makeExtension("mock-data-source", "2.0.0", server_.url());
  QSignalSpy spy_pending(&local_mgr, &ExtensionManager::installPendingRestart);
  local_mgr.update(ext_v2);
  ASSERT_TRUE(waitForSignal(spy_pending));

  QSignalSpy spy_promoted(&local_mgr, &ExtensionManager::installFinished);
  local_mgr.applyPendingInstalls();
  ASSERT_EQ(spy_promoted.count(), 1);
  EXPECT_TRUE(spy_promoted.first().at(1).toBool()) << "promotion must succeed";
  EXPECT_EQ(local_mgr.installedExtensions()["mock-data-source"].version, "2.0.0");

  const QStringList backups = backupDirsFor("mock-data-source");
  ASSERT_EQ(backups.size(), 1) << "exactly one backup must exist after promotion";
  const QString backup_dir = PlatformUtils::backupDir() + "/" + backups.first();
  EXPECT_TRUE(QFile::exists(backup_dir + "/" + pluginFileName())) << "original plugin file must be preserved in backup";

  cleanBackups("mock-data-source");
}

// Counterpart to InstallResetsDisabledState: a staged UPDATE promotion must
// PRESERVE the user's disabled choice. A plugin the user disabled must not come
// back enabled just because its update was promoted at the next restart.
TEST_F(ExtensionManagerTest, UpdatePromotionPreservesDisabledState) {
  cleanBackups("mock-data-source");

  QTemporaryDir local_ext_dir(QDir(PlatformUtils::backupDir()).absoluteFilePath("../test_ext_XXXXXX"));
  ASSERT_TRUE(local_ext_dir.isValid());
  QTemporaryDir local_pending_dir(QDir(PlatformUtils::backupDir()).absoluteFilePath("../test_pending_XXXXXX"));
  ASSERT_TRUE(local_pending_dir.isValid());

  DownloadManager local_dl;
  ExtensionManager local_mgr(&local_dl, local_ext_dir.path(), local_pending_dir.path());

  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext_v1 = makeExtension("mock-data-source", "1.0.0", server_.url());
  QSignalSpy spy_install(&local_mgr, &ExtensionManager::installFinished);
  local_mgr.install(ext_v1);
  ASSERT_TRUE(waitForSignal(spy_install));

  // The user disables the installed plugin.
  local_mgr.setEnabled("mock-data-source", false);
  ASSERT_FALSE(local_mgr.isEnabled("mock-data-source"));

  // Stage an update and promote it as a restart would.
  server_.setBody(dummyPluginZip("mock-data-source", "2.0.0"));
  const Extension ext_v2 = makeExtension("mock-data-source", "2.0.0", server_.url());
  QSignalSpy spy_pending(&local_mgr, &ExtensionManager::installPendingRestart);
  local_mgr.update(ext_v2);
  ASSERT_TRUE(waitForSignal(spy_pending));
  local_mgr.applyPendingInstalls();

  EXPECT_EQ(local_mgr.installedExtensions()["mock-data-source"].version, "2.0.0") << "the update must have promoted";
  EXPECT_FALSE(local_mgr.installedExtensions()["mock-data-source"].enabled)
      << "a staged update promotion must preserve the disabled state, not re-enable";
  EXPECT_FALSE(local_mgr.isEnabled("mock-data-source"));
  EXPECT_TRUE(ExtensionManager::disabledExtensionIds().contains("mock-data-source"))
      << "the persisted disabled entry must survive an update promotion";

  local_mgr.setEnabled("mock-data-source", true);  // cleanup: don't leak into other tests
  cleanBackups("mock-data-source");
}

// A failed update fails during staging, before the live install is touched: the
// installed version stays fully intact, nothing is staged, and no backup is made
// (there is nothing to recover — the running copy was never moved).
TEST_F(ExtensionManagerTest, FailedUpdateStagingLeavesLiveInstallUntouched) {
  cleanBackups("mock-data-source");

  QTemporaryDir local_ext_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_ext_XXXXXX"));
  ASSERT_TRUE(local_ext_dir.isValid());
  QTemporaryDir local_pending_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_pending_XXXXXX"));
  ASSERT_TRUE(local_pending_dir.isValid());

  DownloadManager local_dl;
  ExtensionManager local_mgr(&local_dl, local_ext_dir.path(), local_pending_dir.path());

  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext_v1 = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy_install(&local_mgr, &ExtensionManager::installFinished);
  local_mgr.install(ext_v1);
  ASSERT_TRUE(waitForSignal(spy_install));
  ASSERT_TRUE(spy_install.first().at(1).toBool());

  spy_install.clear();
  // Serve garbage data — libarchive will fail to extract it and DownloadManager
  // will emit failed(), which propagates to installFinished(id, false).
  server_.setBody(QByteArray("not_a_valid_zip"));
  const Extension ext_v2 = makeExtension("mock-data-source", "2.0.0", server_.url());

  QSignalSpy spy_error(&local_mgr, &ExtensionManager::installError);
  local_mgr.update(ext_v2);

  ASSERT_TRUE(waitForSignal(spy_install)) << "installFinished must fire even on failure";
  EXPECT_FALSE(spy_install.first().at(1).toBool()) << "update must have failed";
  EXPECT_FALSE(spy_error.isEmpty()) << "installError must be emitted on failure";

  // v1 is still the live, installed version — staging never touched it.
  EXPECT_TRUE(local_mgr.isInstalled("mock-data-source"));
  EXPECT_EQ(local_mgr.installedExtensions()["mock-data-source"].version, "1.0.0");
  EXPECT_TRUE(QFile::exists(local_ext_dir.path() + "/mock-data-source/" + pluginFileName()));
  // Nothing left staged, and no backup was created.
  EXPECT_FALSE(local_mgr.hasPendingInstall("mock-data-source"));
  EXPECT_FALSE(QDir(local_pending_dir.path() + "/mock-data-source").exists());
  EXPECT_TRUE(backupDirsFor("mock-data-source").isEmpty()) << "a failed staging must not create a backup";
}

// A second update() against an id that already has a staged install must be
// refused up front rather than redownloading and re-staging the same payload on
// top of the pending dir — the installed version stays live until restart, so
// hasUpdate() keeps returning true and a caller relying on it can innocently
// ask again. The manager guard is the safety net for callers that are not the
// marketplace UI (the UI additionally gates the "Update All" button off).
TEST_F(ExtensionManagerTest, UpdateRejectsAlreadyPendingInstall) {
  cleanBackups("mock-data-source");

  QTemporaryDir local_ext_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_ext_XXXXXX"));
  ASSERT_TRUE(local_ext_dir.isValid());
  QTemporaryDir local_pending_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_pending_XXXXXX"));
  ASSERT_TRUE(local_pending_dir.isValid());

  DownloadManager local_dl;
  ExtensionManager local_mgr(&local_dl, local_ext_dir.path(), local_pending_dir.path());

  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext_v1 = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy_finished(&local_mgr, &ExtensionManager::installFinished);
  local_mgr.install(ext_v1);
  ASSERT_TRUE(waitForSignal(spy_finished));
  ASSERT_TRUE(spy_finished.first().at(1).toBool());

  // First update() stages v2 (installPendingRestart fires).
  server_.setBody(dummyPluginZip("mock-data-source", "2.0.0"));
  const Extension ext_v2 = makeExtension("mock-data-source", "2.0.0", server_.url());

  QSignalSpy spy_pending(&local_mgr, &ExtensionManager::installPendingRestart);
  local_mgr.update(ext_v2);
  ASSERT_TRUE(waitForSignal(spy_pending));
  ASSERT_TRUE(local_mgr.hasPendingInstall("mock-data-source"));

  // Second update() must be refused synchronously — no download, no re-stage.
  QSignalSpy spy_error(&local_mgr, &ExtensionManager::installError);
  QSignalSpy spy_finished_2(&local_mgr, &ExtensionManager::installFinished);
  QSignalSpy spy_pending_2(&local_mgr, &ExtensionManager::installPendingRestart);
  local_mgr.update(ext_v2);

  ASSERT_EQ(spy_error.count(), 1) << "second update on an already-pending id must emit installError";
  EXPECT_EQ(spy_error.first().at(0).toString(), "mock-data-source");
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("already staged"))
      << "error message should surface why the update was refused";
  EXPECT_EQ(spy_pending_2.count(), 0) << "no second installPendingRestart";
  EXPECT_TRUE(spy_finished_2.isEmpty() || !spy_finished_2.first().at(1).toBool())
      << "installFinished must not fire success";

  cleanBackups("mock-data-source");
}

// A pending-uninstall marker on an id must also block a subsequent update():
// on Windows the uninstall path stages the removal, and update() during that
// window would resurrect an id the user just asked to remove. The manager guard
// covers this along with hasPendingInstall; the marker file is portable enough
// to exercise on any platform.
TEST_F(ExtensionManagerTest, UpdateRejectsAlreadyPendingUninstall) {
  cleanBackups("mock-data-source");

  QTemporaryDir local_ext_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_ext_XXXXXX"));
  ASSERT_TRUE(local_ext_dir.isValid());
  QTemporaryDir local_pending_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_pending_XXXXXX"));
  ASSERT_TRUE(local_pending_dir.isValid());

  DownloadManager local_dl;
  ExtensionManager local_mgr(&local_dl, local_ext_dir.path(), local_pending_dir.path());

  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext_v1 = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy_finished(&local_mgr, &ExtensionManager::installFinished);
  local_mgr.install(ext_v1);
  ASSERT_TRUE(waitForSignal(spy_finished));
  ASSERT_TRUE(spy_finished.first().at(1).toBool());

  // Stand in for an older build's staged uninstall by dropping its in-payload
  // marker. Current code journals removals outside the payload instead, but the
  // marker's EXISTENCE still has to register as a pending removal so an upgrade
  // does not let update() run over one — content is never read (see
  // sweepLegacyUninstallMarkers).
  const QString marker = local_ext_dir.path() + "/mock-data-source/.pj_pending_uninstall";
  QFile marker_file(marker);
  ASSERT_TRUE(marker_file.open(QIODevice::WriteOnly));
  marker_file.close();
  ASSERT_TRUE(local_mgr.hasPendingUninstall("mock-data-source"));

  server_.setBody(dummyPluginZip("mock-data-source", "2.0.0"));
  const Extension ext_v2 = makeExtension("mock-data-source", "2.0.0", server_.url());

  QSignalSpy spy_error(&local_mgr, &ExtensionManager::installError);
  QSignalSpy spy_pending(&local_mgr, &ExtensionManager::installPendingRestart);
  local_mgr.update(ext_v2);

  ASSERT_EQ(spy_error.count(), 1) << "update() must refuse when a pending uninstall marker is present";
  EXPECT_EQ(spy_error.first().at(0).toString(), "mock-data-source");
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("already staged"))
      << "error message should surface why the update was refused";
  EXPECT_EQ(spy_pending.count(), 0) << "no installPendingRestart must fire";

  cleanBackups("mock-data-source");
}

// ---------------------------------------------------------------------------
// [5] hasUpdate — version comparison
//
// Extension data below mirrors the registry.json fixture:
//   mock-data-source   v1.0.0
//   mock-file-source v1.0.0
// ---------------------------------------------------------------------------

// Returns false when the extension is not installed.
TEST_F(ExtensionManagerTest, HasUpdateReturnsFalseWhenNotInstalled) {
  Extension ext;
  ext.id = "mock-data-source";
  ext.version = "1.0.0";
  EXPECT_FALSE(mgr_->hasUpdate(ext));
}

// Returns false when the installed and registry versions are identical.
TEST_F(ExtensionManagerTest, HasUpdateReturnsFalseForSameVersion) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext);
  ASSERT_TRUE(waitForSignal(spy));

  EXPECT_FALSE(mgr_->hasUpdate(ext));
}

// Returns true when the registry version is strictly higher than the installed one.
TEST_F(ExtensionManagerTest, HasUpdateReturnsTrueForNewerVersion) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext_v1 = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext_v1);
  ASSERT_TRUE(waitForSignal(spy));

  Extension ext_v2 = ext_v1;
  ext_v2.version = "2.0.0";
  EXPECT_TRUE(mgr_->hasUpdate(ext_v2));
}

// installedVersion() answers from what the startup seed reported writing, not
// from the scan snapshot, because the snapshot predates that write and the
// process cannot re-read it (glibc serves a second dlopen of the same path from
// the first-loaded image).
//
// The discriminating case is the seed's restore of an incompatible copy: it
// leaves the managed dir BELOW the scanned version, so a rule that only ever
// moved the answer upwards would keep reporting the version it just replaced.
TEST_F(ExtensionManagerTest, SeededVersionOverridesTheScanSnapshotDownwards) {
  server_.setBody(dummyPluginZip("mock-data-source", "2.0.0"));
  const Extension ext_v2 = makeExtension("mock-data-source", "2.0.0", server_.url());

  QSignalSpy spy(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext_v2);
  ASSERT_TRUE(waitForSignal(spy));
  ASSERT_EQ(mgr_->installedVersion("mock-data-source"), "2.0.0") << "the scan sees what was installed";

  // The host restored the bundled 1.0.0 over that incompatible 2.0.0.
  mgr_->setBundledVersions({{"mock-data-source", "1.0.0"}});
  mgr_->setSeededVersions({{"mock-data-source", "1.0.0"}});

  EXPECT_EQ(mgr_->installedVersion("mock-data-source"), "1.0.0");
  Extension registry_v2 = ext_v2;
  EXPECT_TRUE(mgr_->hasUpdate(registry_v2)) << "1.0.0 on disk against a 2.0.0 registry is an update again";
  EXPECT_FALSE(mgr_->hasNewerInstalledVersion(registry_v2)) << "the restored copy is not newer than the registry";
}

// QVersionNumber must compare versions numerically, not lexically:
// "10.0.0" > "2.0.0" — a raw string compare would invert this result.
TEST_F(ExtensionManagerTest, HasUpdateHandlesMultiSegmentVersionsCorrectly) {
  server_.setBody(dummyPluginZip("mock-data-source", "2.0.0"));
  const Extension ext_installed = makeExtension("mock-data-source", "2.0.0", server_.url());

  QSignalSpy spy(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext_installed);
  ASSERT_TRUE(waitForSignal(spy));

  // "10.0.0" is numerically greater but lexically smaller than "2.0.0".
  Extension ext_registry = ext_installed;
  ext_registry.version = "10.0.0";
  EXPECT_TRUE(mgr_->hasUpdate(ext_registry));
}

TEST_F(ExtensionManagerTest, HasNewerInstalledVersionReturnsTrueWhenLocalVersionIsAhead) {
  server_.setBody(dummyPluginZip("mock-data-source", "2.0.0"));
  const Extension ext_v2 = makeExtension("mock-data-source", "2.0.0", server_.url());

  QSignalSpy spy(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext_v2);
  ASSERT_TRUE(waitForSignal(spy));

  Extension ext_v1 = ext_v2;
  ext_v1.version = "1.0.0";
  EXPECT_TRUE(mgr_->hasNewerInstalledVersion(ext_v1));
  EXPECT_FALSE(mgr_->hasUpdate(ext_v1));
}

// installedVersion() is the single source of truth for what's on disk. Three
// branches — id absent, no bundled set (falls back to scanned), and bundled
// higher than scanned (the seed-refreshed core-plugin case that motivates
// the helper — see the header comment for the STB_GNU_UNIQUE / NODELETE trap).

TEST_F(ExtensionManagerTest, InstalledVersionIsEmptyForUnknownId) {
  EXPECT_TRUE(mgr_->installedVersion("never-installed").isEmpty());
}

TEST_F(ExtensionManagerTest, InstalledVersionFallsBackToScannedWithNoBundledSet) {
  server_.setBody(dummyPluginZip("mock-data-source", "1.0.0"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext);
  ASSERT_TRUE(waitForSignal(spy));

  EXPECT_EQ(mgr_->installedVersion("mock-data-source"), "1.0.0");
}

// The seed-refresh case: the scanner still sees the pre-seed version because its
// dlopen was poisoned by NODELETE, and the seed reports the version it promoted.
// installedVersion() has to report the promoted one — it is what is really on
// disk.
TEST_F(ExtensionManagerTest, InstalledVersionReportsSeededWhenNewerThanScanned) {
  server_.setBody(dummyPluginZip("mock-data-source", "1.0.0"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext);
  ASSERT_TRUE(waitForSignal(spy));

  QMap<QString, QString> promoted;
  promoted.insert("mock-data-source", "2.0.0");
  mgr_->setBundledVersions(promoted);
  mgr_->setSeededVersions(promoted);

  EXPECT_EQ(mgr_->installedVersion("mock-data-source"), "2.0.0");
}

// When the marketplace-installed copy is newer than the bundled one (the user
// upgraded), scanned wins — the seed is not going to overwrite a user's newer
// install, so scanned is what is on disk.
TEST_F(ExtensionManagerTest, InstalledVersionKeepsScannedWhenBundledIsOlder) {
  server_.setBody(dummyPluginZip("mock-data-source", "2.0.0"));
  const Extension ext = makeExtension("mock-data-source", "2.0.0", server_.url());

  QSignalSpy spy(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext);
  ASSERT_TRUE(waitForSignal(spy));

  QMap<QString, QString> bundled;
  bundled.insert("mock-data-source", "1.0.0");
  mgr_->setBundledVersions(bundled);

  EXPECT_EQ(mgr_->installedVersion("mock-data-source"), "2.0.0");
}

// The consequence that motivates the whole change: hasUpdate stops offering an
// update when bundled >= registry, even if the scanner still sees the pre-seed
// version.
TEST_F(ExtensionManagerTest, HasUpdateIsFalseWhenTheSeededVersionMatchesTheRegistry) {
  server_.setBody(dummyPluginZip("mock-data-source", "1.0.0"));
  const Extension registry_v2 = makeExtension("mock-data-source", "2.0.0", server_.url());

  QSignalSpy spy(mgr_, &ExtensionManager::installFinished);
  mgr_->install(makeExtension("mock-data-source", "1.0.0", server_.url()));
  ASSERT_TRUE(waitForSignal(spy));

  QMap<QString, QString> promoted;
  promoted.insert("mock-data-source", "2.0.0");
  mgr_->setBundledVersions(promoted);
  mgr_->setSeededVersions(promoted);

  EXPECT_FALSE(mgr_->hasUpdate(registry_v2));
}

// Returns false when the registry version is older than the installed one (downgrade scenario).
TEST_F(ExtensionManagerTest, HasUpdateReturnsFalseForOlderVersion) {
  server_.setBody(dummyPluginZip("mock-data-source", "2.0.0"));
  const Extension ext_v2 = makeExtension("mock-data-source", "2.0.0", server_.url());

  QSignalSpy spy(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext_v2);
  ASSERT_TRUE(waitForSignal(spy));

  Extension ext_v1 = ext_v2;
  ext_v1.version = "1.0.0";
  EXPECT_FALSE(mgr_->hasUpdate(ext_v1));
}

TEST_F(ExtensionManagerTest, UpdateRejectsDowngradeWhenInstalledVersionIsNewer) {
  server_.setBody(dummyPluginZip("mock-data-source", "2.0.0"));
  const Extension ext_v2 = makeExtension("mock-data-source", "2.0.0", server_.url());

  QSignalSpy spy_install(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext_v2);
  ASSERT_TRUE(waitForSignal(spy_install));
  ASSERT_TRUE(spy_install.first().at(1).toBool());

  server_.setBody(dummyPluginZip("mock-data-source", "1.0.0"));
  const Extension ext_v1 = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy_update(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);
  mgr_->update(ext_v1);

  ASSERT_TRUE(waitForSignal(spy_update));
  EXPECT_FALSE(spy_update.first().at(1).toBool());
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("newer than registry"));
  EXPECT_EQ(mgr_->installedExtensions()["mock-data-source"].version, "2.0.0");
}

// ---------------------------------------------------------------------------
// [6] applyPendingInstalls — post-restart staging promotion
//
// update() stages the new version into the configured pending directory. On the
// next startup, applyPendingInstalls() moves the directory into extensions/ and
// registers it from the DSO's embedded manifest. These tests create that
// directory structure manually and verify the promotion logic (the function is
// always safe to call).
// ---------------------------------------------------------------------------

// applyPendingInstalls() promotes a staged extension to extensions/ and registers it.
TEST_F(ExtensionManagerTest, ApplyPendingInstallsPromotesStagedExtension) {
  const QString staged_dir = pending_dir_.path() + "/mock-data-source";
  ASSERT_TRUE(copyFixturePlugin(staged_dir, "mock-data-source"));
  ASSERT_TRUE(writePendingIntentForTest(staged_dir, "mock-data-source"));

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  mgr_->applyPendingInstalls();

  ASSERT_EQ(spy_finished.count(), 1);
  EXPECT_EQ(spy_finished.first().at(0).toString(), "mock-data-source");
  EXPECT_TRUE(spy_finished.first().at(1).toBool());

  // Extension must be queryable as installed.
  EXPECT_TRUE(mgr_->isInstalled("mock-data-source"));
  EXPECT_EQ(mgr_->installedExtensions()["mock-data-source"].version, "1.0.0");

  // The active directory lives under extensions_dir, not pending_dir.
  EXPECT_TRUE(QDir(ext_dir_.path() + "/mock-data-source").exists());
  EXPECT_FALSE(QDir(staged_dir).exists());
}

// Constructing the manager (the "one restart") must reflect a staged update over
// an existing install as installed in a SINGLE pass: initComponents() promotes
// staged work first and only then snapshots disk, so the first in-process open
// of the extension path reads the promoted payload instead of pinning a stale
// pre-update image that would make the update look un-applied until a second
// restart.
TEST_F(ExtensionManagerTest, ConstructionReflectsStagedUpdateInOnePass) {
  cleanBackups("mock-data-source");
  // Same filesystem as backupDir(): promoting over the existing v1 backs it up
  // via QDir::rename(), which cannot cross filesystems (e.g. a tmpfs /tmp).
  QTemporaryDir ext_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_ext_XXXXXX"));
  ASSERT_TRUE(ext_dir.isValid());
  QTemporaryDir pend_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_pending_XXXXXX"));
  ASSERT_TRUE(pend_dir.isValid());

  // Pre-existing v1 install.
  ASSERT_TRUE(copyFixturePlugin(ext_dir.path() + "/mock-data-source", "mock-data-source", "1.0.0"));
  // Staged v2 update + intent, as update() would leave it.
  const QString staged = pend_dir.path() + "/mock-data-source";
  ASSERT_TRUE(copyFixturePlugin(staged, "mock-data-source", "2.0.0"));
  ASSERT_TRUE(writePendingIntentForTest(staged, "mock-data-source", "2.0.0"));

  DownloadManager dl;
  ExtensionManager mgr(&dl, ext_dir.path(), pend_dir.path());

  EXPECT_EQ(mgr.installedExtensions()["mock-data-source"].version, "2.0.0");
  EXPECT_FALSE(QDir(staged).exists()) << "staged dir must be consumed by promotion";

  // The marketplace window re-scans disk on showEvent. That second pass, in the
  // same process that just promoted the update, must NOT read the stale
  // pre-promotion image: dlopen caches a plugin DSO by path name and never
  // unloads it (STB_GNU_UNIQUE), so opening the old version before promotion
  // would poison every later read of that path. Promotion avoids opening the old
  // directory, so the re-scan still reports the promoted version.
  mgr.refreshInstalledFromDisk();
  EXPECT_EQ(mgr.installedExtensions()["mock-data-source"].version, "2.0.0")
      << "a re-scan after promotion must not revert to the stale old version";

  cleanBackups("mock-data-source");
}

// A staged batch must report every promoted extension at its NEW version even
// when a SIBLING's pre-update DSO stays resident after dlclose (NODELETE via a
// bound STB_GNU_UNIQUE symbol — the pinning fixture models real plugin builds).
// A conflict scan that dlopens a sibling's not-yet-promoted old build pins its
// image, and the pinned image keeps answering for that path after promotion:
// the rescan reports the pre-update version and the marketplace re-offers an
// update that is already on disk. Contract: no sibling dir is opened before
// its own promotion. The single-id test above cannot catch this — the
// conflict scan skips its own target.
TEST_F(ExtensionManagerTest, ConstructionPromotesStagedBatchDespitePinnedSibling) {
  cleanBackups("mock-data-source");
  cleanBackups("pinning-data-source");

  // Same filesystem as backupDir() so promotion's backup renames are moves.
  QTemporaryDir ext_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_ext_XXXXXX"));
  ASSERT_TRUE(ext_dir.isValid());
  QTemporaryDir pend_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_pending_XXXXXX"));
  ASSERT_TRUE(pend_dir.isValid());

  // Installed v1 pair + staged v2 pair, laid out exactly as update() leaves
  // them. Nothing may dlopen the installed paths before the manager
  // constructs: the drain must be this process's first touch of them.
  // "mock-data-source" sorts before "pinning-data-source", so the old pinning
  // build is the sibling a pre-promotion conflict scan would open.
  ASSERT_TRUE(copyFixturePlugin(ext_dir.path() + "/mock-data-source", "mock-data-source", "1.0.0"));
  ASSERT_TRUE(copyFixturePlugin(ext_dir.path() + "/pinning-data-source", "pinning-data-source", "1.0.0"));
  const QString staged_mock = pend_dir.path() + "/mock-data-source";
  ASSERT_TRUE(copyFixturePlugin(staged_mock, "mock-data-source", "2.0.0"));
  ASSERT_TRUE(writePendingIntentForTest(staged_mock, "mock-data-source", "2.0.0"));
  const QString staged_pinning = pend_dir.path() + "/pinning-data-source";
  ASSERT_TRUE(copyFixturePlugin(staged_pinning, "pinning-data-source", "2.0.0"));
  ASSERT_TRUE(writePendingIntentForTest(staged_pinning, "pinning-data-source", "2.0.0"));

  DownloadManager dl;
  ExtensionManager mgr(&dl, ext_dir.path(), pend_dir.path());

  EXPECT_EQ(mgr.installedVersion("mock-data-source"), "2.0.0");
  EXPECT_EQ(mgr.installedVersion("pinning-data-source"), "2.0.0")
      << "the drain read a pinned pre-update sibling image instead of the promoted file";

#ifdef Q_OS_LINUX
  // Canary that the scenario was real: the promoted pinning DSO must be
  // resident (the post-promotion scans pin the NEW image), and no displaced
  // pre-update image may linger under the backup dir (a mapping there means a
  // pre-promotion open pinned it). If the fixture ever stops pinning after a
  // toolchain change, the first expectation fails loudly — rework the
  // fixture's anchor rather than deleting the check.
  QFile maps("/proc/self/maps");
  ASSERT_TRUE(maps.open(QIODevice::ReadOnly));
  const QString mapped = QString::fromUtf8(maps.readAll());
  EXPECT_TRUE(mapped.contains(QDir(ext_dir.path()).canonicalPath() + "/pinning-data-source/"));
  // Canonicalized like the kernel reports mapping paths; the dir exists here —
  // both promotions displaced an existing install into it.
  const QString backup_prefix = QDir(PlatformUtils::backupDir()).canonicalPath();
  ASSERT_FALSE(backup_prefix.isEmpty());
  EXPECT_FALSE(mapped.contains(backup_prefix + "/pinning-data-source-"));
#endif

  cleanBackups("mock-data-source");
  cleanBackups("pinning-data-source");
}

TEST_F(ExtensionManagerTest, StageInstallRejectsEmbeddedIdMismatchBeforeRestart) {
  server_.setBody(pluginZipWithDso("registry-id", "mock-data-source"));
  const Extension ext = makeExtension("registry-id", "1.0.0", server_.url());

  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);
  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->testDoInstall(ext, /*staging=*/true);

  ASSERT_TRUE(waitForInstallOutcome(spy_finished, spy_pending));
  EXPECT_EQ(spy_pending.count(), 0);
  ASSERT_EQ(spy_finished.count(), 1);
  EXPECT_EQ(spy_finished.first().at(0).toString(), "registry-id");
  EXPECT_FALSE(spy_finished.first().at(1).toBool());
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("Embedded plugin id"));
  EXPECT_FALSE(QDir(pending_dir_.path() + "/registry-id").exists());
}

TEST_F(ExtensionManagerTest, StageInstallRejectsEmbeddedVersionMismatchBeforeRestart) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "2.0.0", server_.url());

  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);
  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->testDoInstall(ext, /*staging=*/true);

  ASSERT_TRUE(waitForInstallOutcome(spy_finished, spy_pending));
  EXPECT_EQ(spy_pending.count(), 0);
  ASSERT_EQ(spy_finished.count(), 1);
  EXPECT_EQ(spy_finished.first().at(0).toString(), "mock-data-source");
  EXPECT_FALSE(spy_finished.first().at(1).toBool());
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("Embedded plugin version"));
  EXPECT_FALSE(QDir(pending_dir_.path() + "/mock-data-source").exists());
}

TEST_F(ExtensionManagerTest, ApplyPendingInstallsRejectsStagedVersionMismatchAgainstRegistryIntent) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);
  QSignalSpy spy_stage_finished(mgr_, &ExtensionManager::installFinished);

  mgr_->testDoInstall(ext, /*staging=*/true);

  ASSERT_TRUE(waitForInstallOutcome(spy_stage_finished, spy_pending));
  ASSERT_EQ(spy_pending.count(), 1);
  ASSERT_EQ(spy_stage_finished.count(), 0);

  const QString staged_plugin = pending_dir_.path() + "/mock-data-source/" + pluginFileName();
  ASSERT_TRUE(QFile::remove(staged_plugin));
  ASSERT_TRUE(QFile::copy(pluginPathForId("mock-data-source", "2.0.0"), staged_plugin));

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);
  mgr_->applyPendingInstalls();

  ASSERT_EQ(spy_finished.count(), 1);
  EXPECT_EQ(spy_finished.first().at(0).toString(), "mock-data-source");
  EXPECT_FALSE(spy_finished.first().at(1).toBool());
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("Embedded plugin version"));
  EXPECT_FALSE(QDir(pending_dir_.path() + "/mock-data-source").exists());
  EXPECT_FALSE(QDir(ext_dir_.path() + "/mock-data-source").exists());
  EXPECT_FALSE(mgr_->isInstalled("mock-data-source"));
}

// A staged directory that lacks the registry-intent marker is rejected and removed;
// otherwise every startup would silently skip the same broken stage forever.
TEST_F(ExtensionManagerTest, ApplyPendingInstallsRejectsEmptyStagingDirectory) {
  const QString staged_dir = pending_dir_.path() + "/bad-extension";
  ASSERT_TRUE(QDir().mkpath(staged_dir));

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);
  mgr_->applyPendingInstalls();

  ASSERT_EQ(spy_finished.count(), 1);
  EXPECT_EQ(spy_finished.first().at(0).toString(), "bad-extension");
  EXPECT_FALSE(spy_finished.first().at(1).toBool());
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("registry intent"));
  EXPECT_FALSE(mgr_->isInstalled("bad-extension"));
  EXPECT_FALSE(QDir(staged_dir).exists());
  ASSERT_FALSE(mgr_->diagnostics().isEmpty());
  EXPECT_TRUE(mgr_->diagnostics().back().message.contains("registry intent"));
}

// Windows update path: when applyPendingInstalls() promotes a staged update over an
// existing install, it must move the previous version into PlatformUtils::backupDir()
// before the rename, mirroring the synchronous backup that update() performs on
// Linux/macOS. Without this, a Windows update would silently overwrite the previous
// version with no recovery path.
TEST_F(ExtensionManagerTest, ApplyPendingInstallsBacksUpExistingExtensionBeforePromotion) {
  // Place ext_dir + pending_dir on the same filesystem as backupDir() so all the
  // QDir::rename moves are atomic (no cross-device copy fallback).
  QTemporaryDir local_ext_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_ext_XXXXXX"));
  QTemporaryDir local_pending_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_pending_XXXXXX"));
  ASSERT_TRUE(local_ext_dir.isValid());
  ASSERT_TRUE(local_pending_dir.isValid());
  // Clean any stale backup from a previous failed run.
  cleanBackups("mock-data-source");

  DownloadManager local_dl;
  ExtensionManager local_mgr(&local_dl, local_ext_dir.path(), local_pending_dir.path());

  // 1. Install v1 directly to populate extensions/<id>/.
  server_.setBody(dummyPluginZip("mock-data-source"));
  QSignalSpy spy_install(&local_mgr, &ExtensionManager::installFinished);
  local_mgr.install(makeExtension("mock-data-source", "1.0.0", server_.url()));
  ASSERT_TRUE(waitForSignal(spy_install));
  ASSERT_TRUE(spy_install.first().at(1).toBool());
  spy_install.clear();

  // 2. Manually stage a v2 update with intent file (mirrors what doInstall(staging=true)
  //    leaves on disk on Windows before the user restarts).
  const QString staged_dir = local_pending_dir.path() + "/mock-data-source";
  ASSERT_TRUE(copyFixturePlugin(staged_dir, "mock-data-source", "2.0.0"));
  ASSERT_TRUE(writePendingIntentForTest(staged_dir, "mock-data-source", "2.0.0"));

  // 3. Restart-time apply.
  local_mgr.applyPendingInstalls();

  ASSERT_EQ(spy_install.count(), 1);
  EXPECT_TRUE(spy_install.first().at(1).toBool()) << "staged update must promote";
  EXPECT_EQ(local_mgr.installedExtensions()["mock-data-source"].version, "2.0.0");

  // 4. The previous version must be preserved in backup, recoverable manually.
  const QStringList backups = backupDirsFor("mock-data-source");
  ASSERT_EQ(backups.size(), 1) << "applyPendingInstalls must back up the previous version before overwriting it";
  const QString backup_dir = PlatformUtils::backupDir() + "/" + backups.first();
  EXPECT_TRUE(QFile::exists(backup_dir + "/" + pluginFileName()))
      << "previous plugin file must survive in backup for manual rollback";

  QDir(backup_dir).removeRecursively();
}

// applyPendingInstalls() is a no-op when the pending directory contains no sub-directories.
TEST_F(ExtensionManagerTest, ApplyPendingInstallsIsNoOpForEmptyDirectory) {
  QSignalSpy spy(mgr_, &ExtensionManager::installFinished);
  mgr_->applyPendingInstalls();
  EXPECT_EQ(spy.count(), 0);
}

// Multiple staged extensions in the pending directory are all promoted in a single call.
TEST_F(ExtensionManagerTest, ApplyPendingInstallsPromotesMultipleExtensions) {
  for (const QString& id : QStringList{"mock-data-source", "mock-file-source"}) {
    const QString staged = pending_dir_.path() + "/" + id;
    ASSERT_TRUE(copyFixturePlugin(staged, id));
    ASSERT_TRUE(writePendingIntentForTest(staged, id));
  }

  QSignalSpy spy(mgr_, &ExtensionManager::installFinished);
  mgr_->applyPendingInstalls();

  EXPECT_EQ(spy.count(), 2);
  EXPECT_TRUE(mgr_->isInstalled("mock-data-source"));
  EXPECT_TRUE(mgr_->isInstalled("mock-file-source"));
}

TEST_F(ExtensionManagerTest, ApplyPendingInstallsRejectsEmbeddedIdMismatch) {
  const QString staged_dir = pending_dir_.path() + "/registry-id";
  ASSERT_TRUE(copyFixturePlugin(staged_dir, "mock-data-source"));
  ASSERT_TRUE(writePendingIntentForTest(staged_dir, "registry-id"));

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);
  mgr_->applyPendingInstalls();

  ASSERT_EQ(spy_finished.count(), 1);
  EXPECT_EQ(spy_finished.first().at(0).toString(), "registry-id");
  EXPECT_FALSE(spy_finished.first().at(1).toBool());
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_FALSE(QDir(ext_dir_.path() + "/registry-id").exists());
  EXPECT_FALSE(mgr_->isInstalled("registry-id"));
  EXPECT_FALSE(mgr_->isInstalled("mock-data-source"));
}

TEST_F(ExtensionManagerTest, ApplyPendingInstallsKeepsExistingInstallWhenStagedUpdateFailsValidation) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy_install(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext);
  ASSERT_TRUE(waitForSignal(spy_install));
  ASSERT_TRUE(spy_install.first().at(1).toBool());
  ASSERT_TRUE(QDir(ext_dir_.path() + "/mock-data-source").exists());

  const QString staged_dir = pending_dir_.path() + "/mock-data-source";
  ASSERT_TRUE(copyFixturePlugin(staged_dir, "mock-file-source"));
  ASSERT_TRUE(writePendingIntentForTest(staged_dir, "mock-data-source"));

  spy_install.clear();
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);
  mgr_->applyPendingInstalls();

  ASSERT_EQ(spy_install.count(), 1);
  EXPECT_FALSE(spy_install.first().at(1).toBool());
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(mgr_->isInstalled("mock-data-source"));
  EXPECT_TRUE(QDir(ext_dir_.path() + "/mock-data-source").exists());
  EXPECT_FALSE(QDir(staged_dir).exists());
}

TEST_F(ExtensionManagerTest, ApplyPendingInstallsRejectsBrokenDso) {
  const QString staged_dir = pending_dir_.path() + "/bad-extension";
  ASSERT_TRUE(QDir().mkpath(staged_dir));

  QFile broken(QDir(staged_dir).absoluteFilePath(pluginFileName()));
  ASSERT_TRUE(broken.open(QIODevice::WriteOnly));
  broken.write("not a shared library");
  broken.close();
  ASSERT_TRUE(writePendingIntentForTest(staged_dir, "bad-extension"));

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);
  mgr_->applyPendingInstalls();

  ASSERT_EQ(spy_finished.count(), 1);
  EXPECT_EQ(spy_finished.first().at(0).toString(), "bad-extension");
  EXPECT_FALSE(spy_finished.first().at(1).toBool());
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_FALSE(QDir(ext_dir_.path() + "/bad-extension").exists());
}

TEST_F(ExtensionManagerTest, HasPendingInstallRequiresDsoAndRegistryIntent) {
  const QString staged_dir = pending_dir_.path() + "/mock-data-source";
  ASSERT_TRUE(QDir().mkpath(staged_dir));

  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);
  EXPECT_FALSE(mgr_->hasPendingInstall("mock-data-source"));
  EXPECT_EQ(spy_pending.count(), 0);

  ASSERT_TRUE(writePendingIntentForTest(staged_dir, "mock-data-source"));
  EXPECT_FALSE(mgr_->hasPendingInstall("mock-data-source"));
  ASSERT_TRUE(QFile::remove(QDir(staged_dir).absoluteFilePath(".pj_pending_install")));

  ASSERT_TRUE(copyFixturePlugin(staged_dir, "mock-data-source"));
  EXPECT_FALSE(mgr_->hasPendingInstall("mock-data-source"));

  ASSERT_TRUE(writePendingIntentForTest(staged_dir, "mock-data-source"));
  EXPECT_TRUE(mgr_->hasPendingInstall("mock-data-source"));
  EXPECT_EQ(spy_pending.count(), 0);
}

// ---------------------------------------------------------------------------
// [7] State persistence
// ---------------------------------------------------------------------------

// A new ExtensionManager pointing to the same directory discovers the same extensions
// by scanning disk — this simulates an application restart.
TEST_F(ExtensionManagerTest, StatePersistsAcrossManagerRestarts) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext);
  ASSERT_TRUE(waitForSignal(spy));
  ASSERT_TRUE(spy.first().at(1).toBool());

  // Simulate restart: a brand-new manager reads the same extensions_dir.
  DownloadManager downloader2;
  ExtensionManager mgr2(&downloader2, ext_dir_.path(), pending_dir_.path());

  EXPECT_TRUE(mgr2.isInstalled("mock-data-source"));
  EXPECT_EQ(mgr2.installedExtensions()["mock-data-source"].version, "1.0.0");
}

// Uninstalling removes the directory from disk; a fresh manager scanning the same
// directory must not report the extension as installed.
TEST_F(ExtensionManagerTest, UninstallRemovesEntryFromPersistentState) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy_install(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext);
  ASSERT_TRUE(waitForSignal(spy_install));

  mgr_->uninstall("mock-data-source");

  // The next process drains the staged removal in initComponents() before it
  // snapshots installed state, so the id is gone AND so is its directory. That
  // drain belongs to the instance that owns the store, so this one must release
  // it first — as the restart being simulated would.
  delete mgr_;
  mgr_ = nullptr;

  DownloadManager downloader2;
  ExtensionManager mgr2(&downloader2, ext_dir_.path(), pending_dir_.path());
  EXPECT_FALSE(mgr2.isInstalled("mock-data-source"));
  EXPECT_FALSE(QDir(ext_dir_.path() + "/mock-data-source").exists());
}

TEST_F(ExtensionManagerTest, RefreshEvictsExtensionWhenDsoIsRemovedExternally) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext);
  ASSERT_TRUE(waitForSignal(spy));
  ASSERT_TRUE(spy.first().at(1).toBool());
  ASSERT_TRUE(QFile::remove(ext_dir_.path() + "/mock-data-source/" + pluginFileName()));

  mgr_->refreshInstalledFromDisk();

  EXPECT_FALSE(mgr_->isInstalled("mock-data-source"));
  EXPECT_TRUE(mgr_->installedExtensions().isEmpty());
}

TEST_F(ExtensionManagerTest, LoadStateIgnoresDirectoriesWithoutValidPluginDso) {
  const QString invalid_dir = ext_dir_.path() + "/old-layout";
  ASSERT_TRUE(QDir().mkpath(invalid_dir));
  QFile readme(invalid_dir + "/README.txt");
  ASSERT_TRUE(readme.open(QIODevice::WriteOnly));
  readme.write("not a plugin");
  readme.close();

  mgr_->refreshInstalledFromDisk();

  EXPECT_FALSE(mgr_->isInstalled("old-layout"));
  EXPECT_TRUE(mgr_->installedExtensions().isEmpty());
}

// A new manager pointing to an empty extensions directory starts with no installed
// extensions — no crash or undefined behaviour on first run.
TEST_F(ExtensionManagerTest, FreshManagerHasNoInstalledExtensions) {
  EXPECT_TRUE(mgr_->installedExtensions().isEmpty());
}

// ---------------------------------------------------------------------------
// [9] applyPendingUninstalls — deferred directory cleanup simulation
//
// On Windows, uninstall() writes a .pj_pending_uninstall marker inside the
// extension directory when removeRecursively() fails. On the next startup,
// applyPendingUninstalls() removes every extension directory that carries that
// marker. These tests create that state manually so the cleanup logic can be
// verified on any platform.
// ---------------------------------------------------------------------------

// A directory containing the marker is removed by applyPendingUninstalls().
TEST_F(ExtensionManagerTest, ApplyPendingUninstallsRemovesMarkedDirectory) {
  const QString ext_path = ext_dir_.path() + "/mock-data-source";
  ASSERT_TRUE(QDir().mkpath(ext_path));
  QFile marker(ext_path + "/.pj_pending_uninstall");
  ASSERT_TRUE(marker.open(QIODevice::WriteOnly));
  marker.close();

  mgr_->applyPendingUninstalls();

  EXPECT_FALSE(QDir(ext_path).exists());
}

// A directory without the marker is left untouched.
TEST_F(ExtensionManagerTest, ApplyPendingUninstallsIgnoresUnmarkedDirectory) {
  const QString ext_path = ext_dir_.path() + "/mock-data-source";
  ASSERT_TRUE(QDir().mkpath(ext_path));

  mgr_->applyPendingUninstalls();

  EXPECT_TRUE(QDir(ext_path).exists());
}

// applyPendingUninstalls() is a no-op when extensions_dir contains no sub-directories.
TEST_F(ExtensionManagerTest, ApplyPendingUninstallsIsNoOpForEmptyDirectory) {
  mgr_->applyPendingUninstalls();  // must not crash
}

// ---------------------------------------------------------------------------
// [8] Platform detection
// ---------------------------------------------------------------------------

// currentPlatform() must return a non-empty string in "<os>-<arch>" format.
TEST(PlatformDetectionTest, CurrentPlatformHasExpectedFormat) {
  const QString platform = PlatformUtils::currentPlatform();
  EXPECT_FALSE(platform.isEmpty());
  EXPECT_TRUE(platform.contains('-')) << "Expected '<os>-<arch>' format, got: " << platform.toStdString();
}

// On the primary Linux x86_64 build/CI host, the reported platform must match the
// key used in the registry fixture so that install() can resolve the download artifact.
TEST(PlatformDetectionTest, LinuxX86PlatformMatchesRegistryKey) {
  // Skip on every non-Linux host, not just Windows: macOS reports macos-<arch>.
  if (!PlatformUtils::currentPlatform().startsWith("linux-")) {
    GTEST_SKIP() << "test pins the Linux x86_64 platform key";
  }
  EXPECT_EQ(PlatformUtils::currentPlatform(), "linux-x86_64");
}

// Verify that PlatformUtils::currentPlatform() returns a key that would exist
// in a typical registry entry, so install() can resolve the download artifact.
TEST(PlatformDetectionTest, CurrentPlatformResolvesRegistryArtifact) {
  // Test fixture with fake URLs - we only check that the platform key exists
  Extension ext;
  ext.id = "test-extension";
  ext.version = "1.0.0";
  ext.platforms["linux-x86_64"] = {
      "https://example.com/test/extension-linux-x86_64.zip",
      "sha256:0000000000000000000000000000000000000000000000000000000000000000"};
  ext.platforms["windows-x86_64"] = {
      "https://example.com/test/extension-windows-x64.zip",
      "sha256:0000000000000000000000000000000000000000000000000000000000000000"};
  // Apple silicon and Intel Macs report distinct keys, and a registry entry that
  // ships for macOS carries both.
  ext.platforms["macos-arm64"] = {
      "https://example.com/test/extension-macos-arm64.zip",
      "sha256:0000000000000000000000000000000000000000000000000000000000000000"};
  ext.platforms["macos-x86_64"] = {
      "https://example.com/test/extension-macos-x86_64.zip",
      "sha256:0000000000000000000000000000000000000000000000000000000000000000"};

  EXPECT_TRUE(ext.platforms.contains(PlatformUtils::currentPlatform()))
      << "Platform '" << PlatformUtils::currentPlatform().toStdString()
      << "' is not listed in the mock-data-source registry entry";
}

// On Linux, install() must write directly to extensions_dir (no staging).
// isWindows() must return false to confirm the code path is exercised.
TEST(PlatformDetectionTest, IsWindowsReturnsFalseOnLinux) {
  if (PlatformUtils::isWindows()) {
    GTEST_SKIP() << "test asserts the Linux code path is exercised";
  }
  EXPECT_FALSE(PlatformUtils::isWindows());
}

// ---------------------------------------------------------------------------
// Refresh during an in-progress install
// ---------------------------------------------------------------------------

// Regression test for the case where refreshInstalledFromDisk() is invoked
// while the async checksum/extract worker is still writing into the
// transaction directory. Before the guard on pending_extract_dir_, the sweep
// would wipe the in-flight `.pj_install_*` directory and the install would
// fail its downstream validation. With the guard the install completes
// normally.
TEST_F(ExtensionManagerTest, RefreshDuringInstallDoesNotWipeInProgressTransaction) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  // Trigger a refresh the moment the worker announces its extract phase. The
  // receiver is mgr_ so the lambda runs on the manager's thread, matching how
  // a real UI Refresh button click would arrive.
  QObject::connect(
      mgr_, &ExtensionManager::installPhase, mgr_, [this](const QString&, DownloadManager::WorkPhase phase) {
        if (phase == DownloadManager::WorkPhase::Extracting) {
          mgr_->refreshInstalledFromDisk();
        }
      });

  mgr_->install(ext);

  ASSERT_TRUE(waitForSignal(spy_finished)) << "installFinished not received within 5 s";
  ASSERT_EQ(spy_finished.count(), 1);
  EXPECT_TRUE(spy_finished.first().at(1).toBool()) << "install must succeed despite refresh mid-flight";
  EXPECT_TRUE(spy_error.isEmpty());
  EXPECT_TRUE(mgr_->isInstalled("mock-data-source"));
}

// Once the install is finished, refreshInstalledFromDisk() must resume its
// normal duty of sweeping stale transaction directories (this asserts that
// the guard is not overly broad — it only protects the CURRENT install).
TEST_F(ExtensionManagerTest, RefreshWipesStaleTransactionDirsAfterInstall) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext);
  ASSERT_TRUE(waitForSignal(spy_finished));

  // Drop a fake stale transaction directory in extensions_dir_. This
  // simulates a leftover from a crashed install on a previous run.
  const QString stale_dir = QDir(ext_dir_.path()).absoluteFilePath(u".pj_install_stale_abc"_s);
  ASSERT_TRUE(QDir().mkpath(stale_dir));
  ASSERT_TRUE(QFile::exists(stale_dir));

  mgr_->refreshInstalledFromDisk();

  EXPECT_FALSE(QFile::exists(stale_dir)) << "stale .pj_install_* should have been swept";
  EXPECT_TRUE(mgr_->isInstalled("mock-data-source")) << "live install must remain registered";
}

// ---------------------------------------------------------------------------
// [10] installFromLocalZip: sideload of a plugin the registry does not list
// ---------------------------------------------------------------------------

// Writes `zip` to a file inside `dir` and returns its path.
QString writeZipFile(const QTemporaryDir& dir, const QByteArray& zip, const QString& name = "pkg.zip") {
  const QString path = QDir(dir.path()).absoluteFilePath(name);
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly)) {
    return {};
  }
  f.write(zip);
  f.close();
  return path;
}

// The happy path, and with it the load-bearing assumption of the whole feature:
// fetch() serves a file:// URL, so a local archive reuses the same
// download/verify/extract pipeline as a registry artifact.
TEST_F(ExtensionManagerTest, InstallFromLocalZipRegistersDiscoveredExtension) {
  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  const QString zip = writeZipFile(src, dummyPluginZip("mock-data-source"));
  ASSERT_FALSE(zip.isEmpty());

  QSignalSpy spy_started(mgr_, &ExtensionManager::installStarted);
  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->installFromLocalZip(zip);

  ASSERT_TRUE(waitForInstallOutcome(spy_finished, spy_pending));
  for (const auto& e : spy_error) {
    qWarning("installFromLocalZip error: %s", qPrintable(e.at(1).toString()));
  }
  ASSERT_EQ(spy_error.count(), 0);
  ASSERT_EQ(spy_finished.count(), 1);
  EXPECT_EQ(spy_finished.first().at(0).toString(), "mock-data-source");
  EXPECT_TRUE(spy_finished.first().at(1).toBool());

  // installStarted is emitted only once the id has been read from the manifest.
  ASSERT_EQ(spy_started.count(), 1);
  EXPECT_EQ(spy_started.first().at(0).toString(), "mock-data-source");

  // Id and version come from the embedded manifest, not from any caller-supplied
  // declaration, and the directory is keyed by the discovered id.
  EXPECT_TRUE(mgr_->isInstalled("mock-data-source"));
  EXPECT_EQ(mgr_->installedExtensions()["mock-data-source"].version, "1.0.0");
  EXPECT_TRUE(QDir(QDir(ext_dir_.path()).absoluteFilePath("mock-data-source")).exists());
}

// The archive's top-level directory name is irrelevant: the managed layout keys
// directories by the id the manifest declares.
TEST_F(ExtensionManagerTest, InstallFromLocalZipKeysDirectoryByEmbeddedId) {
  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  // Directory named "some-folder", manifest inside declares "mock-data-source".
  const QString zip = writeZipFile(src, pluginZipWithDso("some-folder", "mock-data-source"));
  ASSERT_FALSE(zip.isEmpty());

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);

  mgr_->installFromLocalZip(zip);

  ASSERT_TRUE(waitForInstallOutcome(spy_finished, spy_pending));
  ASSERT_EQ(spy_finished.count(), 1);
  EXPECT_TRUE(spy_finished.first().at(1).toBool());
  EXPECT_TRUE(QDir(QDir(ext_dir_.path()).absoluteFilePath("mock-data-source")).exists());
  EXPECT_FALSE(QDir(QDir(ext_dir_.path()).absoluteFilePath("some-folder")).exists());
}

// Provisional policy: replacing an installed extension needs the staged
// apply-at-restart path, so a re-sideload is refused rather than swapping a DSO
// the running session may hold open.
TEST_F(ExtensionManagerTest, InstallFromLocalZipRejectsAlreadyInstalled) {
  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  const QString zip = writeZipFile(src, dummyPluginZip("mock-data-source"));
  ASSERT_FALSE(zip.isEmpty());

  QSignalSpy first_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy first_pending(mgr_, &ExtensionManager::installPendingRestart);
  mgr_->installFromLocalZip(zip);
  ASSERT_TRUE(waitForInstallOutcome(first_finished, first_pending));
  ASSERT_TRUE(mgr_->isInstalled("mock-data-source"));

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->installFromLocalZip(zip);

  ASSERT_TRUE(waitForInstallOutcome(spy_finished, spy_pending));
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("already installed"));
  ASSERT_EQ(spy_finished.count(), 1);
  EXPECT_FALSE(spy_finished.first().at(1).toBool());
  // The installed copy is left exactly as it was.
  EXPECT_EQ(mgr_->installedExtensions()["mock-data-source"].version, "1.0.0");
}

// A ZIP with no loadable plugin is refused, and nothing is left behind.
TEST_F(ExtensionManagerTest, InstallFromLocalZipRejectsArchiveWithoutPlugin) {
  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  const QString zip = writeZipFile(src, buildZip({{"some-folder/readme.txt", QByteArray("not a plugin")}}));
  ASSERT_FALSE(zip.isEmpty());

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->installFromLocalZip(zip);

  ASSERT_TRUE(waitForInstallOutcome(spy_finished, spy_pending));
  ASSERT_EQ(spy_error.count(), 1);
  ASSERT_EQ(spy_finished.count(), 1);
  EXPECT_FALSE(spy_finished.first().at(1).toBool());
  EXPECT_TRUE(mgr_->installedExtensions().isEmpty());
  EXPECT_FALSE(QDir(QDir(ext_dir_.path()).absoluteFilePath("some-folder")).exists());
}

// More than one top-level entry is ambiguous: which directory is the plugin?
TEST_F(ExtensionManagerTest, InstallFromLocalZipRejectsMultipleTopLevelEntries) {
  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  const QString suffix = QString::fromStdString(PlatformUtils::pluginExtension());
  const QString zip = writeZipFile(
      src, buildZip({
               {"first/plugin" + suffix, readAll(pluginPathForId("mock-data-source"))},
               {"second/plugin" + suffix, readAll(pluginPathForId("mock-file-source"))},
           }));
  ASSERT_FALSE(zip.isEmpty());

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->installFromLocalZip(zip);

  ASSERT_TRUE(waitForInstallOutcome(spy_finished, spy_pending));
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("exactly one top-level directory"));
  EXPECT_TRUE(mgr_->installedExtensions().isEmpty());
}

// With a confirmation that says yes, a second install stages instead of failing:
// the live directory is untouched until the next launch promotes the stage.
TEST_F(ExtensionManagerTest, InstallFromLocalZipStagesConfirmedReplacement) {
  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  // Seeded straight onto disk: installFromLocalZip refreshes installed state
  // itself, so a directory is a sufficient precondition and this avoids paying for
  // an extract/dlopen round-trip just to reach the case under test.
  ASSERT_TRUE(copyFixturePlugin(ext_dir_.path() + "/mock-data-source", "mock-data-source"));

  // A different version, so the promoted result is distinguishable.
  const QString second = writeZipFile(src, dummyPluginZip("mock-data-source", "2.0.0"), "v2.zip");
  ASSERT_FALSE(second.isEmpty());

  QString asked_id;
  QString asked_installed;
  QString asked_archive;
  QObject confirmation_owner;
  mgr_->setReplaceConfirmation(
      &confirmation_owner, [&](const QString& id, const QString& installed_version, const QString& archive_version) {
        asked_id = id;
        asked_installed = installed_version;
        asked_archive = archive_version;
        return true;
      });

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->installFromLocalZip(second);

  ASSERT_TRUE(waitForInstallOutcome(spy_finished, spy_pending));
  EXPECT_EQ(spy_error.count(), 0);
  // Staged, not finished: promotion happens on the next launch.
  ASSERT_EQ(spy_pending.count(), 1);
  EXPECT_EQ(spy_pending.first().at(0).toString(), "mock-data-source");
  EXPECT_EQ(spy_finished.count(), 0);

  // The confirmation was asked with both versions, so the UI can name them.
  EXPECT_EQ(asked_id, "mock-data-source");
  EXPECT_EQ(asked_installed, "1.0.0");
  EXPECT_EQ(asked_archive, "2.0.0");

  // The live install still reads as the old version until promotion.
  EXPECT_EQ(mgr_->installedExtensions()["mock-data-source"].version, "1.0.0");
  EXPECT_TRUE(mgr_->hasPendingInstall("mock-data-source"));
}

// A declined confirmation leaves everything exactly as it was.
TEST_F(ExtensionManagerTest, InstallFromLocalZipHonoursDeclinedReplacement) {
  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  const QString zip = writeZipFile(src, dummyPluginZip("mock-data-source"));
  ASSERT_FALSE(zip.isEmpty());
  ASSERT_TRUE(copyFixturePlugin(ext_dir_.path() + "/mock-data-source", "mock-data-source"));

  bool asked = false;
  QObject confirmation_owner;
  mgr_->setReplaceConfirmation(&confirmation_owner, [&](const QString&, const QString&, const QString&) {
    asked = true;
    return false;
  });

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->installFromLocalZip(zip);

  ASSERT_TRUE(waitForInstallOutcome(spy_finished, spy_pending));
  EXPECT_TRUE(asked);
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("cancelled"));
  EXPECT_EQ(spy_pending.count(), 0);
  EXPECT_FALSE(mgr_->hasPendingInstall("mock-data-source"));
  EXPECT_EQ(mgr_->installedExtensions()["mock-data-source"].version, "1.0.0");
}

// The question is put long after installFromLocalZip() returns, so the object the
// callback captured can be gone by then (a marketplace window closed while the
// archive was still extracting). That must resolve as a decline, and must leave the
// registration cleared rather than armed for the next archive.
TEST_F(ExtensionManagerTest, ReplaceConfirmationSurvivesOwnerDestruction) {
  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  const QString zip = writeZipFile(src, dummyPluginZip("mock-data-source"));
  ASSERT_FALSE(zip.isEmpty());
  ASSERT_TRUE(copyFixturePlugin(ext_dir_.path() + "/mock-data-source", "mock-data-source"));

  // Heap-allocated so it can die before the answer is needed. The sentinel lives on
  // the test stack rather than inside the owner, so an unguarded call still reports
  // itself here: the actual dangling capture is only visible under a sanitizer.
  auto* confirmation_owner = new QObject;
  bool confirmation_invoked = false;
  mgr_->setReplaceConfirmation(
      confirmation_owner, [&confirmation_invoked](const QString&, const QString&, const QString&) {
        confirmation_invoked = true;
        return true;
      });
  delete confirmation_owner;

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->installFromLocalZip(zip);

  ASSERT_TRUE(waitForInstallOutcome(spy_finished, spy_pending));
  EXPECT_FALSE(confirmation_invoked) << "a confirmation whose owner was destroyed must never be invoked";
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("cancelled"));
  EXPECT_EQ(spy_pending.count(), 0) << "a decision nobody could take must not stage a replacement";
  EXPECT_FALSE(mgr_->hasPendingInstall("mock-data-source"));
  EXPECT_EQ(mgr_->installedExtensions()["mock-data-source"].version, "1.0.0");

  // The dead registration is dropped, not retried: a second archive now takes the
  // no-confirmation branch, which is how the cleared state is observable from here.
  QSignalSpy spy_finished_again(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_pending_again(mgr_, &ExtensionManager::installPendingRestart);
  QSignalSpy spy_error_again(mgr_, &ExtensionManager::installError);

  mgr_->installFromLocalZip(zip);

  ASSERT_TRUE(waitForInstallOutcome(spy_finished_again, spy_pending_again));
  ASSERT_EQ(spy_error_again.count(), 1);
  EXPECT_TRUE(spy_error_again.first().at(1).toString().contains("already installed"))
      << "the stale registration must be cleared, so the conflict is refused outright";
  EXPECT_FALSE(confirmation_invoked);
}

// Two windows can share one manager and the last registration wins, so clearing has
// to be scoped to the registrant: a displaced window closing must not disarm the
// live registration.
TEST_F(ExtensionManagerTest, ClearReplaceConfirmationOnlyRemovesOwnCallback) {
  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  const QString zip = writeZipFile(src, dummyPluginZip("mock-data-source"));
  ASSERT_FALSE(zip.isEmpty());
  ASSERT_TRUE(copyFixturePlugin(ext_dir_.path() + "/mock-data-source", "mock-data-source"));

  QObject first_owner;
  QObject second_owner;
  bool first_invoked = false;
  bool second_invoked = false;
  mgr_->setReplaceConfirmation(&first_owner, [&first_invoked](const QString&, const QString&, const QString&) {
    first_invoked = true;
    return true;
  });
  mgr_->setReplaceConfirmation(&second_owner, [&second_invoked](const QString&, const QString&, const QString&) {
    second_invoked = true;
    return false;
  });

  // The displaced owner going away: a no-op, since it no longer holds the slot.
  mgr_->clearReplaceConfirmation(&first_owner);

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->installFromLocalZip(zip);

  ASSERT_TRUE(waitForInstallOutcome(spy_finished, spy_pending));
  EXPECT_TRUE(second_invoked) << "clearing with a displaced owner must leave the live registration armed";
  EXPECT_FALSE(first_invoked);
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("cancelled"));
  EXPECT_FALSE(spy_error.first().at(1).toString().contains("already installed"));
}

// The answer comes out of a modal dialog, which spins the event loop, so another
// window can take the registration over WHILE the question is open. A live
// registrant is then not enough to accept the answer: this one was given by a
// registration that no longer holds the slot, and honouring it would stage a
// replacement the window now in charge never approved.
TEST_F(ExtensionManagerTest, ReentrantReregistrationDiscardsDisplacedOwnersAnswer) {
  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  const QString zip = writeZipFile(src, dummyPluginZip("mock-data-source"));
  ASSERT_FALSE(zip.isEmpty());
  ASSERT_TRUE(copyFixturePlugin(ext_dir_.path() + "/mock-data-source", "mock-data-source"));

  QObject first_owner;
  QObject second_owner;
  bool second_invoked = false;
  // Re-registering from inside the callback is what a modal event loop makes
  // reachable, and it is also the reentrancy that must not touch the std::function
  // being executed. The displaced owner still answers "yes".
  mgr_->setReplaceConfirmation(&first_owner, [&](const QString&, const QString&, const QString&) {
    mgr_->setReplaceConfirmation(&second_owner, [&second_invoked](const QString&, const QString&, const QString&) {
      second_invoked = true;
      return true;
    });
    return true;
  });

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->installFromLocalZip(zip);

  ASSERT_TRUE(waitForInstallOutcome(spy_finished, spy_pending));
  EXPECT_EQ(spy_pending.count(), 0) << "an answer from a displaced registration must not stage a replacement";
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("replaced while the question was open"))
      << "actual: " << spy_error.first().at(1).toString().toStdString();
  EXPECT_FALSE(second_invoked) << "the successor must not be asked again for the same conflict";
  EXPECT_FALSE(mgr_->hasPendingInstall("mock-data-source"));
  EXPECT_EQ(mgr_->installedExtensions()["mock-data-source"].version, "1.0.0")
      << "the live install must be left exactly as it was";

  // Discarding the displaced answer must not disarm the successor: the window now
  // in charge still owns the decision for the next archive.
  QSignalSpy spy_finished_again(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_pending_again(mgr_, &ExtensionManager::installPendingRestart);

  mgr_->installFromLocalZip(zip);

  ASSERT_TRUE(waitForInstallOutcome(spy_finished_again, spy_pending_again));
  EXPECT_TRUE(second_invoked) << "the successor's registration must survive the discarded answer";
  EXPECT_EQ(spy_pending_again.count(), 1) << "the successor accepted, so the replacement stages normally";
}

// The staged replacement is promoted on the next launch, into the same extensions
// dir every other install uses, and the displaced copy is kept in the backup dir.
TEST_F(ExtensionManagerTest, StagedLocalReplacementIsPromotedOnNextLaunch) {
  cleanBackups("mock-data-source");

  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  // Same filesystem as backupDir(): promoting over the existing v1 backs it up
  // via QDir::rename(), which cannot cross filesystems (e.g. a tmpfs /tmp).
  QTemporaryDir local_ext_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_ext_XXXXXX"));
  ASSERT_TRUE(local_ext_dir.isValid());
  QTemporaryDir local_pending_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_pending_XXXXXX"));
  ASSERT_TRUE(local_pending_dir.isValid());

  DownloadManager local_dl;
  ExtensionManager local_mgr(&local_dl, local_ext_dir.path(), local_pending_dir.path());

  const QString v2 = writeZipFile(src, dummyPluginZip("mock-data-source", "2.0.0"), "v2.zip");
  ASSERT_FALSE(v2.isEmpty());
  ASSERT_TRUE(copyFixturePlugin(local_ext_dir.path() + "/mock-data-source", "mock-data-source", "1.0.0"));
  local_mgr.refreshInstalledFromDisk();

  QObject confirmation_owner;
  local_mgr.setReplaceConfirmation(
      &confirmation_owner, [](const QString&, const QString&, const QString&) { return true; });
  QSignalSpy spy_pending(&local_mgr, &ExtensionManager::installPendingRestart);
  QSignalSpy spy_finished(&local_mgr, &ExtensionManager::installFinished);
  local_mgr.installFromLocalZip(v2);
  ASSERT_TRUE(waitForInstallOutcome(spy_finished, spy_pending));
  ASSERT_EQ(spy_pending.count(), 1);
  EXPECT_TRUE(local_mgr.hasPendingInstall("mock-data-source"));
  // Untouched until promotion: the running session may still have this DSO loaded.
  EXPECT_EQ(local_mgr.installedExtensions()["mock-data-source"].version, "1.0.0");

  // Next launch.
  local_mgr.applyPendingInstalls();

  EXPECT_EQ(local_mgr.installedExtensions()["mock-data-source"].version, "2.0.0")
      << "the staged replacement must be promoted into the extensions dir";
  EXPECT_FALSE(local_mgr.hasPendingInstall("mock-data-source")) << "the stage must be consumed by promotion";

  cleanBackups("mock-data-source");
}

// A path that is not a readable file fails before any transaction directory is
// created, and reports against the file name since no id exists yet.
TEST_F(ExtensionManagerTest, InstallFromLocalZipRejectsUnreadablePath) {
  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->installFromLocalZip(QDir(ext_dir_.path()).absoluteFilePath("does-not-exist.zip"));

  ASSERT_TRUE(waitForInstallOutcome(spy_finished, spy_pending));
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("Cannot read"));
  EXPECT_TRUE(mgr_->installedExtensions().isEmpty());
}

// ---------------------------------------------------------------------------
// [10] Enable / disable (installed but not loaded)
// ---------------------------------------------------------------------------
// A disabled extension stays on disk but the host must skip loading it. The
// three surfaces that need to agree — the in-memory record on installed_[id],
// the static disabledExtensionIds() the runtime catalog reads, and isEnabled()
// — are exercised here so any one of them drifting alone would surface.

// A fixture that starts every test from a clean QSettings key. The manager reads
// the "Marketplace/disabledExtensions" list via QSettings() default scope, so a
// residual list from a previous test (or the developer's machine) would leak in
// and produce non-deterministic assertions.
class ExtensionManagerEnableDisableTest : public ExtensionManagerTest {
 protected:
  void SetUp() override {
    ExtensionManagerTest::SetUp();
    QSettings().remove(kDisabledExtensionsKey);
  }
  void TearDown() override {
    QSettings().remove(kDisabledExtensionsKey);
    ExtensionManagerTest::TearDown();
  }

  // Installs the standard fixture extension and returns its directory, or an empty
  // string if the install never completed. Empty is the caller's cue to ASSERT.
  QString installVictim() {
    server_.setBody(dummyPluginZip("mock-data-source"));
    QSignalSpy install_spy(mgr_, &ExtensionManager::installFinished);
    mgr_->install(makeExtension("mock-data-source", "1.0.0", server_.url()));
    if (!waitForSignal(install_spy)) {
      return {};
    }
    return ext_dir_.path() + "/mock-data-source";
  }

  // The manager owns this string as a private constant. Duplicated here so a
  // rename on that side breaks the test at compile time (via the sync check
  // further down) instead of silently starting to leak state.
  static constexpr const char* kDisabledExtensionsKey = "Marketplace/disabledExtensions";
};

// A newly-arrived extension defaults to enabled: setEnabled() has never been
// called and the QSettings list is empty.
TEST_F(ExtensionManagerEnableDisableTest, IsEnabledDefaultsToTrueForUnknownIds) {
  EXPECT_TRUE(mgr_->isEnabled("never-heard-of-it"));
  EXPECT_TRUE(ExtensionManager::disabledExtensionIds().isEmpty());
}

// Disabling an id appends it to the persisted list AND updates the in-memory
// installed_[id].enabled record so the UI reads the new state without a
// refreshInstalledFromDisk() round trip. The runtime catalog, which reads via
// the static disabledExtensionIds(), sees the same list.
TEST_F(ExtensionManagerEnableDisableTest, SetEnabledFalsePersistsAndUpdatesInMemoryRecord) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());
  QSignalSpy spy(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext);
  ASSERT_TRUE(waitForSignal(spy));
  ASSERT_TRUE(mgr_->installedExtensions()["mock-data-source"].enabled);

  mgr_->setEnabled("mock-data-source", false);

  EXPECT_FALSE(mgr_->isEnabled("mock-data-source"));
  EXPECT_FALSE(mgr_->installedExtensions()["mock-data-source"].enabled)
      << "in-memory record must reflect the change without a rescan";
  EXPECT_EQ(ExtensionManager::disabledExtensionIds(), QStringList{"mock-data-source"})
      << "the static reader (used by PluginRuntimeCatalog::setDisabledIds) must see the same list";
}

// Enabling an already-enabled id (and disabling an already-disabled one) is a
// no-op: the persisted list is unchanged. Guards against a bug where setEnabled
// silently duplicated the id in the list, which would then double-count on
// unset (an already-disabled id would need TWO enables to actually enable).
TEST_F(ExtensionManagerEnableDisableTest, SetEnabledIsIdempotent) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());
  QSignalSpy spy(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext);
  ASSERT_TRUE(waitForSignal(spy));

  mgr_->setEnabled("mock-data-source", false);
  mgr_->setEnabled("mock-data-source", false);  // idempotent
  EXPECT_EQ(ExtensionManager::disabledExtensionIds(), QStringList{"mock-data-source"});

  mgr_->setEnabled("mock-data-source", true);
  mgr_->setEnabled("mock-data-source", true);  // idempotent
  EXPECT_TRUE(ExtensionManager::disabledExtensionIds().isEmpty());
  EXPECT_TRUE(mgr_->isEnabled("mock-data-source"));
}

// Re-enabling removes the id and reflects it in the in-memory record too, so a
// disable-then-enable sequence lands back where it started on both surfaces.
TEST_F(ExtensionManagerEnableDisableTest, SetEnabledTrueRemovesIdFromDisabledList) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());
  QSignalSpy spy(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext);
  ASSERT_TRUE(waitForSignal(spy));

  mgr_->setEnabled("mock-data-source", false);
  ASSERT_FALSE(mgr_->isEnabled("mock-data-source"));

  mgr_->setEnabled("mock-data-source", true);
  EXPECT_TRUE(mgr_->isEnabled("mock-data-source"));
  EXPECT_TRUE(mgr_->installedExtensions()["mock-data-source"].enabled);
  EXPECT_TRUE(ExtensionManager::disabledExtensionIds().isEmpty());
}

// refreshInstalledFromDisk() must reflect the persisted disable state onto
// every installed_[id].enabled record it rebuilds. Without this, a rescan
// (bulk install, uninstall of an unrelated plugin, promotion of a staged
// install) would silently reset the enabled flag to true for every plugin,
// hiding the runtime effect of the toggle even though QSettings still holds
// the disabled list.
TEST_F(ExtensionManagerEnableDisableTest, RefreshFromDiskReflectsPersistedDisableState) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());
  QSignalSpy spy(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext);
  ASSERT_TRUE(waitForSignal(spy));

  mgr_->setEnabled("mock-data-source", false);
  mgr_->refreshInstalledFromDisk();  // rebuild the installed_ map from scratch

  EXPECT_FALSE(mgr_->installedExtensions()["mock-data-source"].enabled)
      << "refresh must not clobber the persisted disabled state";
  EXPECT_FALSE(mgr_->isEnabled("mock-data-source"));
}

// A fresh install of an id resets any stale disabled entry — a previous
// uninstall should normally have cleaned it up, but if a residual entry
// lingers, the install wins and the plugin starts enabled. This also
// guarantees the sideload path (disable-then-install) doesn't leave the
// user with a silently unloaded plugin they just chose to install.
TEST_F(ExtensionManagerEnableDisableTest, InstallResetsDisabledState) {
  mgr_->setEnabled("mock-data-source", false);
  ASSERT_FALSE(mgr_->isEnabled("mock-data-source"));

  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());
  QSignalSpy spy(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext);
  ASSERT_TRUE(waitForSignal(spy));

  EXPECT_TRUE(mgr_->installedExtensions()["mock-data-source"].enabled)
      << "install must clear any pre-existing disabled state";
  EXPECT_TRUE(mgr_->isEnabled("mock-data-source"));
  EXPECT_FALSE(ExtensionManager::disabledExtensionIds().contains("mock-data-source"))
      << "install must remove the id from the persisted disabled set";
}

// The disabled entry outlives the removal as a TOMBSTONE. Staging sets it because
// the files are still there; the drain deliberately does NOT clear it, which is
// what keeps journal processing free of any authority to enable an id. The entry
// costs nothing once the payload is gone, and a reinstall clears it.
TEST_F(ExtensionManagerEnableDisableTest, UninstallLeavesADisabledTombstoneOnceTheRemovalIsApplied) {
  ASSERT_FALSE(installVictim().isEmpty());

  mgr_->setEnabled("mock-data-source", false);
  ASSERT_TRUE(ExtensionManager::disabledExtensionIds().contains("mock-data-source"));

  QSignalSpy uninstall_spy(mgr_, &ExtensionManager::uninstallPendingRestart);
  mgr_->uninstall("mock-data-source");
  ASSERT_TRUE(waitForSignal(uninstall_spy));

  EXPECT_TRUE(ExtensionManager::disabledExtensionIds().contains("mock-data-source"))
      << "the staged removal must keep the id unloadable while its directory is still there";

  mgr_->applyPendingUninstalls();
  EXPECT_TRUE(ExtensionManager::disabledExtensionIds().contains("mock-data-source"))
      << "the tombstone must survive the removal: re-enabling is a power the drain must not have";
}

// Staging a removal must make the id unloadable straight away. The plugin the
// user asked to remove is still on disk until the next launch drains it, and the
// loader decides what to load from the disabled list alone — it knows nothing
// about pending removals. Leaving the id enabled is what lets a failed deletion
// resurrect it (see PartiallyFailedUninstallKeepsItsRetryRecordAndDisabledState).
TEST_F(ExtensionManagerEnableDisableTest, StagedUninstallDisablesUntilRemovalVerified) {
  ASSERT_FALSE(installVictim().isEmpty());
  ASSERT_TRUE(mgr_->isEnabled("mock-data-source"));

  QSignalSpy uninstall_spy(mgr_, &ExtensionManager::uninstallPendingRestart);
  mgr_->uninstall("mock-data-source");
  ASSERT_TRUE(waitForSignal(uninstall_spy));

  EXPECT_TRUE(ExtensionManager::disabledExtensionIds().contains("mock-data-source"))
      << "the id must be disabled while its directory is still on disk, or the loader would load it again";
  EXPECT_FALSE(mgr_->isEnabled("mock-data-source"));

  // The verified removal deletes the payload and leaves the tombstone standing.
  mgr_->applyPendingUninstalls();
  ASSERT_FALSE(QDir(ext_dir_.path() + "/mock-data-source").exists());
  EXPECT_TRUE(ExtensionManager::disabledExtensionIds().contains("mock-data-source"))
      << "the drain must never enable an id, so the entry stays as a tombstone";
}

// The regression test for the defect that sank the in-payload marker, and the
// portable form of the Windows CI failure that exposed it.
//
// removeRecursively() deletes children before it can fail on the one it cannot
// unlink, so a removal that loses part-way through has ALREADY destroyed some of
// the directory's content. When the record of the removal lived in that same
// directory, it was simply among the casualties: the payload survived (its locked
// DSO still there) while the intent that would have retried the deletion was gone,
// and with the old code the disabled entry had been cleared at staging time too, so
// the next launch loaded the plugin the user removed.
//
// Poisoning a SUBDIRECTORY reproduces that shape, and deliberately on EVERY
// platform rather than skipping where the Windows original was seen: an open file
// handle blocks the unlink on Windows (which IS the production failure — a mapped
// DSO), and dropping write permission on the holding directory blocks it on POSIX.
// Both are applied, so the drain fails part-way here and on CI alike.
TEST_F(ExtensionManagerEnableDisableTest, PartiallyFailedUninstallKeepsItsRetryRecordAndDisabledState) {
#ifndef Q_OS_WIN
  if (geteuid() == 0) {
    GTEST_SKIP() << "running as root: directory permissions cannot make a POSIX unlink fail";
  }
#endif
  const QString ext_path = installVictim();
  ASSERT_FALSE(ext_path.isEmpty());
  const QString locked_child = ext_path + "/locked-subdir";
  ASSERT_TRUE(QDir().mkpath(locked_child));
  // Held OPEN for the whole drain: that alone is what defeats the delete on Windows.
  QFile undeletable(locked_child + "/undeletable.bin");
  ASSERT_TRUE(undeletable.open(QIODevice::WriteOnly));
  undeletable.write("content the drain must fail to remove");
  ASSERT_TRUE(undeletable.flush());
  // A top-level payload file the drain WILL delete. It sits exactly where the old
  // in-payload marker sat, so its disappearance below is the standing proof that a
  // record kept in there cannot survive a partial deletion.
  QFile doomed(ext_path + "/doomed.txt");
  ASSERT_TRUE(doomed.open(QIODevice::WriteOnly));
  doomed.write("removed before the failure");
  doomed.close();

  QSignalSpy uninstall_spy(mgr_, &ExtensionManager::uninstallPendingRestart);
  mgr_->uninstall("mock-data-source");
  ASSERT_TRUE(waitForSignal(uninstall_spy));
  ASSERT_TRUE(ExtensionManager::disabledExtensionIds().contains("mock-data-source"));
  ASSERT_FALSE(removalIntentFor(ext_dir_.path(), "mock-data-source").isEmpty()) << "the removal must be journalled";

  {
    // The POSIX half: its child cannot be unlinked, so removeRecursively() fails
    // after having deleted everything it could reach.
    const ScopedDirectoryPermissions locked(locked_child, QFileDevice::ReadOwner | QFileDevice::ExeOwner);

    mgr_->applyPendingUninstalls();

    ASSERT_TRUE(QDir(ext_path).exists()) << "the drain was supposed to fail on the poisoned child";
    EXPECT_FALSE(QFile::exists(ext_path + "/doomed.txt"))
        << "the removal must really have got part-way (this file sits where the old marker did, so its loss is "
           "why an in-payload record could not survive), or this is not testing the partial-deletion case";
    EXPECT_FALSE(removalIntentFor(ext_dir_.path(), "mock-data-source").isEmpty())
        << "the retry record lives outside the payload, so a partial deletion cannot destroy it";
    EXPECT_TRUE(mgr_->hasPendingUninstall("mock-data-source")) << "the removal is still pending and must report so";
    EXPECT_TRUE(ExtensionManager::disabledExtensionIds().contains("mock-data-source"))
        << "the DSO is still on disk, so the desired-removed state must outlive the failed deletion";
  }

  // The retry at the following launch succeeds, and only then does anything retire.
  undeletable.close();
  mgr_->applyPendingUninstalls();
  EXPECT_FALSE(QDir(ext_path).exists());
  EXPECT_TRUE(removalIntentFor(ext_dir_.path(), "mock-data-source").isEmpty()) << "a drained record must be dropped";
  EXPECT_TRUE(ExtensionManager::disabledExtensionIds().contains("mock-data-source"))
      << "the tombstone outlives the payload; only a reinstall clears it";
}

// The staging window has two persisted writes, and a crash can land between them.
// Order decides which way the gap fails. The disabled entry is written and synced
// FIRST, so the reachable intermediate state is "unloadable but still installed" —
// recoverable, and never a plugin the user removed coming back.
//
// The crash is simulated by reproducing that first step alone (no journal record
// follows) and then draining, which is exactly what the next launch would do.
TEST_F(ExtensionManagerEnableDisableTest, CrashBetweenDisableAndJournalLeavesThePluginUnloadable) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  QSignalSpy install_spy(mgr_, &ExtensionManager::installFinished);
  mgr_->install(makeExtension("mock-data-source", "1.0.0", server_.url()));
  ASSERT_TRUE(waitForSignal(install_spy));
  const QString ext_path = ext_dir_.path() + "/mock-data-source";

  // Step 1 of uninstall() and nothing more: the process dies before the journal.
  mgr_->setEnabled("mock-data-source", false);
  QSettings().sync();
  ASSERT_TRUE(removalIntentFor(ext_dir_.path(), "mock-data-source").isEmpty()) << "no record must have been written";

  // The next launch. Nothing schedules a deletion, so the files stay.
  mgr_->applyPendingUninstalls();
  EXPECT_TRUE(QDir(ext_path).exists()) << "with no record there is nothing to drain; the payload stays put";
  EXPECT_TRUE(ExtensionManager::disabledExtensionIds().contains("mock-data-source"))
      << "the surviving half of the intent must be the one that keeps the plugin from loading";
  EXPECT_FALSE(mgr_->isEnabled("mock-data-source"));
}

// A pending-uninstall marker is package-controlled filesystem content: an archive
// can ship one, and it names whatever the packager put in it. Nothing may be
// inferred from that content — least of all an id whose enable/disable state to
// change, which would let one package silently re-enable an unrelated plugin the
// user had disabled. Existence alone schedules a deletion of the directory holding
// it, so a package can only ever harm itself.
TEST_F(ExtensionManagerEnableDisableTest, InjectedUninstallMarkerCannotSteerAnotherExtensionsState) {
  // A victim the user has deliberately disabled.
  server_.setBody(dummyPluginZip("mock-file-source"));
  QSignalSpy victim_spy(mgr_, &ExtensionManager::installFinished);
  mgr_->install(makeExtension("mock-file-source", "1.0.0", server_.url()));
  ASSERT_TRUE(waitForSignal(victim_spy));
  mgr_->setEnabled("mock-file-source", false);
  ASSERT_TRUE(ExtensionManager::disabledExtensionIds().contains("mock-file-source"));

  // A package that ships a marker naming the victim rather than itself.
  const QByteArray plugin = readAll(pluginPathForId("mock-data-source"));
  ASSERT_FALSE(plugin.isEmpty());
  const QString zip_path = QDir(ext_dir_.path()).absoluteFilePath("injected.zip");
  QFile zip(zip_path);
  ASSERT_TRUE(zip.open(QIODevice::WriteOnly));
  zip.write(buildZip({
      {"mock-data-source/" + pluginFileName(), plugin},
      {"mock-data-source/.pj_pending_uninstall", "mock-file-source\n"},
  }));
  zip.close();

  QSignalSpy install_spy(mgr_, &ExtensionManager::installFinished);
  QSignalSpy pending_spy(mgr_, &ExtensionManager::installPendingRestart);
  mgr_->installFromLocalZip(zip_path);
  ASSERT_TRUE(waitForInstallOutcome(install_spy, pending_spy));

  // Whatever the install did on its own merits, the drain must not act on the
  // injected bytes.
  mgr_->applyPendingUninstalls();

  EXPECT_TRUE(ExtensionManager::disabledExtensionIds().contains("mock-file-source"))
      << "payload content must never re-enable an id; the victim's disabled state is the user's, not the package's";
  EXPECT_FALSE(mgr_->isEnabled("mock-file-source"));
  EXPECT_TRUE(QDir(ext_dir_.path() + "/mock-file-source").exists())
      << "the victim's files must be untouched by another package's marker";
  EXPECT_TRUE(removalIntentFor(ext_dir_.path(), "mock-file-source").isEmpty())
      << "no journal record may be conjured from package content";
}

// ---------------------------------------------------------------------------
// [11] Removal-journal record validation (the drain's delete privilege)
// ---------------------------------------------------------------------------
// A journal record authorises `QDir::removeRecursively()`. That makes the journal
// a capability, and the store lease does not protect it: these records live inside
// the rightful writer's own state directory, so corruption or anything that can
// drop a file there would otherwise inherit the drain's privilege. Every record is
// therefore validated before ANY deletion, and every refusal is fail-closed:
// nothing deleted, record quarantined, diagnostic raised.

// Installs one extension and returns its directory, so each case below has a real
// payload that must still be standing afterwards.
class RemovalJournalValidationTest : public ExtensionManagerEnableDisableTest {
 protected:
  // Drives the drain over a hand-planted record and asserts the universal
  // fail-closed outcome: the named target survives, the record is quarantined
  // rather than left to be re-refused forever, and the user is told.
  void expectRefused(const QString& stem, const QByteArray& record, const QString& must_survive) {
    ASSERT_TRUE(writeRawRemovalRecord(ext_dir_.path(), stem, record));
    QSignalSpy diagnostics(mgr_, &ExtensionManager::diagnosticReported);

    mgr_->applyPendingUninstalls();

    EXPECT_TRUE(QFileInfo::exists(must_survive))
        << "a refused record must delete NOTHING: " << must_survive.toStdString();
    EXPECT_TRUE(removalIntentFor(ext_dir_.path(), stem).isEmpty()) << "the refused record must not stay live";
    EXPECT_FALSE(quarantinedRecordsIn(ext_dir_.path()).isEmpty())
        << "a refused record must be quarantined, not deleted";
    EXPECT_FALSE(diagnostics.isEmpty()) << "a refused record must be reported, never silently skipped";
  }
};

// The store root itself: the most damaging target, and the one a path-confinement
// bug hands over first.
TEST_F(RemovalJournalValidationTest, RecordTargetingTheStoreRootIsRefused) {
  const QString victim = installVictim();
  ASSERT_FALSE(victim.isEmpty());
  expectRefused(u"evil"_s, removalRecordJson(u"uninstall"_s, u"evil"_s, ext_dir_.path()), ext_dir_.path());
  EXPECT_TRUE(QDir(victim).exists()) << "the store's contents must be untouched";
}

// The journal's own directory, and the staging sibling: both are OUTSIDE the store
// (siblings, not children), which the direct-child rule refuses on its own.
TEST_F(RemovalJournalValidationTest, RecordTargetingAStateOrStagingRootIsRefused) {
  ASSERT_FALSE(installVictim().isEmpty());
  const QString staging = transactionStageDirFor(ext_dir_.path());
  ASSERT_TRUE(QDir().mkpath(staging));
  expectRefused(u"evil"_s, removalRecordJson(u"uninstall"_s, u"evil"_s, staging), staging);

  const QString journal = removalJournalDirFor(ext_dir_.path());
  expectRefused(u"evil2"_s, removalRecordJson(u"uninstall"_s, u"evil2"_s, journal), journal);
}

// "../" traversal and a wholly unrelated absolute path.
TEST_F(RemovalJournalValidationTest, RecordEscapingTheStoreByTraversalOrAbsolutePathIsRefused) {
  ASSERT_FALSE(installVictim().isEmpty());
  QTemporaryDir elsewhere;
  ASSERT_TRUE(elsewhere.isValid());
  const QString outsider = QDir(elsewhere.path()).absoluteFilePath(u"precious"_s);
  ASSERT_TRUE(QDir().mkpath(outsider));

  expectRefused(u"evil"_s, removalRecordJson(u"uninstall"_s, u"evil"_s, outsider), outsider);
  // Traversal that lands back outside the store after normalisation.
  const QString traversal = ext_dir_.path() + "/../";
  expectRefused(
      u"evil2"_s, removalRecordJson(u"uninstall"_s, u"evil2"_s, traversal), QFileInfo(ext_dir_.path()).absolutePath());
}

// A symlink planted inside the store must not carry the delete out of it: the
// target is confined on its CANONICAL form, not the name used to reach it.
TEST_F(RemovalJournalValidationTest, RecordWhoseTargetSymlinksOutOfTheStoreIsRefused) {
#ifdef Q_OS_WIN
  GTEST_SKIP() << "symlink creation needs elevation on Windows; the canonical-path rule is platform-neutral";
#else
  ASSERT_FALSE(installVictim().isEmpty());
  QTemporaryDir elsewhere;
  ASSERT_TRUE(elsewhere.isValid());
  const QString outsider = QDir(elsewhere.path()).absoluteFilePath(u"precious"_s);
  ASSERT_TRUE(QDir().mkpath(outsider));
  QFile guard(outsider + "/keep.txt");
  ASSERT_TRUE(guard.open(QIODevice::WriteOnly));
  guard.close();

  // A DIRECT child of the store by name, resolving outside it.
  const QString bait = ext_dir_.path() + "/bait";
  ASSERT_TRUE(QFile::link(outsider, bait));

  expectRefused(u"bait"_s, removalRecordJson(u"uninstall"_s, u"bait"_s, bait), outsider);
  EXPECT_TRUE(QFile::exists(outsider + "/keep.txt")) << "the symlink target's contents must be untouched";
#endif
}

// A record filed as A.json claiming to be extension B: acting on it would delete
// B's payload and, when it succeeded, retire a record that was never B's.
TEST_F(RemovalJournalValidationTest, RecordWhoseFilenameDisagreesWithItsIdIsRefused) {
  const QString victim = installVictim();
  ASSERT_FALSE(victim.isEmpty());
  expectRefused(u"evil"_s, removalRecordJson(u"uninstall"_s, u"mock-data-source"_s, victim), victim);
}

// An unknown or absent operation is invalid. It must never fall back to
// "uninstall", which is the destructive reading.
TEST_F(RemovalJournalValidationTest, RecordWithUnknownOrAbsentOperationIsRefused) {
  const QString victim = installVictim();
  ASSERT_FALSE(victim.isEmpty());
  expectRefused(u"mock-data-source"_s, removalRecordJson(u"purge"_s, u"mock-data-source"_s, victim), victim);
  expectRefused(u"mock-data-source"_s, removalRecordJson(QString(), u"mock-data-source"_s, victim), victim);
}

// No schema, and a schema from a shape this build does not know.
TEST_F(RemovalJournalValidationTest, RecordWithMissingOrUnknownSchemaIsRefused) {
  const QString victim = installVictim();
  ASSERT_FALSE(victim.isEmpty());
  expectRefused(
      u"mock-data-source"_s, removalRecordJson(u"uninstall"_s, u"mock-data-source"_s, victim, /*schema=*/0), victim);
  expectRefused(
      u"mock-data-source"_s, removalRecordJson(u"uninstall"_s, u"mock-data-source"_s, victim, /*schema=*/99), victim);
}

TEST_F(RemovalJournalValidationTest, MalformedRecordIsRefused) {
  const QString victim = installVictim();
  ASSERT_FALSE(victim.isEmpty());
  expectRefused(u"mock-data-source"_s, QByteArray("{ this is not json"), victim);
}

// The whole point of the validation: a legitimate record still drains.
// An extension id may begin with a dot — invalidExtensionIdReason only rejects
// exactly "." / ".." and path separators — and the record is named for the id, so
// ".local-plugin" is journalled as ".local-plugin.json": a HIDDEN file on POSIX.
// A journal listing that does not ask for hidden entries never sees it, so the
// removal stays pending forever while the payload sits on disk, still loadable.
TEST_F(RemovalJournalValidationTest, RecordForADottedIdIsDrained) {
  const QString payload = ext_dir_.path() + "/.local-plugin";
  ASSERT_TRUE(QDir().mkpath(payload));
  QFile content(payload + "/plugin.bin");
  ASSERT_TRUE(content.open(QIODevice::WriteOnly));
  content.write("payload the drain must remove");
  content.close();

  ASSERT_TRUE(writeRawRemovalRecord(
      ext_dir_.path(), u".local-plugin"_s, removalRecordJson(u"uninstall"_s, u".local-plugin"_s, payload)));
  ASSERT_FALSE(removalIntentFor(ext_dir_.path(), ".local-plugin").isEmpty())
      << "the record must be on disk to begin with";

  mgr_->applyPendingUninstalls();

  EXPECT_FALSE(QDir(payload).exists()) << "a hidden record must be listed and drained, not skipped forever";
  EXPECT_TRUE(removalIntentFor(ext_dir_.path(), ".local-plugin").isEmpty()) << "the drained record must be retired";
  EXPECT_TRUE(quarantinedRecordsIn(ext_dir_.path()).isEmpty()) << "a valid record must not be quarantined";
}

TEST_F(RemovalJournalValidationTest, AValidRecordStillDrains) {
  const QString victim = installVictim();
  ASSERT_FALSE(victim.isEmpty());
  QSignalSpy uninstall_spy(mgr_, &ExtensionManager::uninstallPendingRestart);
  mgr_->uninstall(u"mock-data-source"_s);
  ASSERT_TRUE(waitForSignal(uninstall_spy));

  mgr_->applyPendingUninstalls();

  EXPECT_FALSE(QDir(victim).exists()) << "a valid record must still delete its payload";
  EXPECT_TRUE(quarantinedRecordsIn(ext_dir_.path()).isEmpty()) << "a valid record must not be quarantined";
}

// A reinstall is what clears the tombstone, which is the whole reason the drain can
// safely be denied any power to enable an id.
TEST_F(RemovalJournalValidationTest, ReinstallAfterUninstallClearsTheTombstone) {
  ASSERT_FALSE(installVictim().isEmpty());
  QSignalSpy uninstall_spy(mgr_, &ExtensionManager::uninstallPendingRestart);
  mgr_->uninstall(u"mock-data-source"_s);
  ASSERT_TRUE(waitForSignal(uninstall_spy));
  mgr_->applyPendingUninstalls();
  ASSERT_TRUE(ExtensionManager::disabledExtensionIds().contains("mock-data-source"));

  server_.setBody(dummyPluginZip("mock-data-source"));
  QSignalSpy reinstall_spy(mgr_, &ExtensionManager::installFinished);
  mgr_->install(makeExtension("mock-data-source", "1.0.0", server_.url()));
  ASSERT_TRUE(waitForSignal(reinstall_spy));
  ASSERT_TRUE(reinstall_spy.first().at(1).toBool()) << "the reinstall must succeed once the removal was applied";

  EXPECT_FALSE(ExtensionManager::disabledExtensionIds().contains("mock-data-source"))
      << "a fresh install clears the tombstone, so the plugin loads again";
  EXPECT_TRUE(mgr_->isEnabled("mock-data-source"));
}

// TOCTOU at the delete site: removeRecursively() FOLLOWS a symlink at the target,
// which the parse-time check does not catch when the link resolves to a legitimate
// in-store directory. A record filed for `bait` (a symlink to a real sibling
// payload) passes validation — its canonical form is a direct managed child — yet
// draining it naively would recurse through the link and delete the sibling's
// contents. The exploitable variant of the same window is a link planted to resolve
// OUTSIDE the store between validation and the delete. The delete-time re-confinement
// refuses a symlink target outright: nothing deleted, quarantined, diagnosed.
//
// A managed payload is always a real directory, so this scenario cannot arise from
// the manager's own records; it models corruption or a concurrent writer.
TEST_F(RemovalJournalValidationTest, RecordWhoseTargetIsASymlinkIsRefusedAtDeletion) {
#ifdef Q_OS_WIN
  GTEST_SKIP() << "symlink creation needs elevation on Windows; the canonical-path rule is platform-neutral";
#else
  ASSERT_FALSE(installVictim().isEmpty());
  const QString sibling = ext_dir_.path() + "/mock-data-source";  // a real, legitimate payload
  ASSERT_TRUE(QDir(sibling).exists());

  // `bait` is a symlink to that sibling. It is a direct child of the store BY NAME,
  // and its canonical form resolves to a direct managed child, so parse-time
  // validation admits it. Only the delete-time symlink refusal stops the drain from
  // recursing through it and wiping the sibling.
  const QString bait = ext_dir_.path() + "/bait";
  ASSERT_TRUE(QFile::link(sibling, bait));
  ASSERT_TRUE(writeRawRemovalRecord(ext_dir_.path(), u"bait"_s, removalRecordJson(u"uninstall"_s, u"bait"_s, bait)));

  QSignalSpy diagnostics(mgr_, &ExtensionManager::diagnosticReported);
  mgr_->applyPendingUninstalls();

  EXPECT_TRUE(QDir(sibling).exists()) << "the drain must not follow a symlink target and delete the real payload";
  EXPECT_FALSE(QDir(sibling).entryList(QDir::Files).isEmpty())
      << "the sibling payload's contents must be intact, not recursively removed through the link";
  EXPECT_TRUE(removalIntentFor(ext_dir_.path(), "bait").isEmpty()) << "the refused record must not stay live";
  EXPECT_FALSE(quarantinedRecordsIn(ext_dir_.path()).isEmpty()) << "the record must be quarantined at deletion time";
  EXPECT_FALSE(diagnostics.isEmpty()) << "the delete-time refusal must be reported";
#endif
}

// The lexical-vs-canonical asymmetry: unmanagedPayloadRejection compares a record's
// lexical parent against the CANONICAL store root, so if the configured store path
// itself contains a symlink component, even a legitimate uninstall record is
// rejected and the removal is stranded. Canonicalizing the store dir once at
// construction is what puts the two comparisons on one footing. RED before that
// fix: the record is quarantined and the payload never drains.
TEST_F(ExtensionManagerTest, UninstallDrainsUnderASymlinkedExtensionsDir) {
#ifdef Q_OS_WIN
  GTEST_SKIP() << "symlink creation needs elevation on Windows; the canonical-path rule is platform-neutral";
#else
  QSettings().remove("Marketplace/disabledExtensions");
  QTemporaryDir root;
  ASSERT_TRUE(root.isValid());
  const QString real_store = QDir(root.path()).absoluteFilePath(u"real_store"_s);
  const QString link_store = QDir(root.path()).absoluteFilePath(u"link_store"_s);
  ASSERT_TRUE(QDir().mkpath(real_store));
  ASSERT_TRUE(QFile::link(real_store, link_store));

  DownloadManager downloader;
  // The store is reached through a symlink, exactly as a symlinked $HOME/config
  // would produce.
  ExtensionManager mgr(&downloader, link_store, pending_dir_.path());
  ASSERT_TRUE(mgr.hasStoreWriteAccess());

  server_.setBody(dummyPluginZip("mock-data-source"));
  QSignalSpy install_spy(&mgr, &ExtensionManager::installFinished);
  mgr.install(makeExtension("mock-data-source", "1.0.0", server_.url()));
  ASSERT_TRUE(waitForSignal(install_spy));
  ASSERT_TRUE(mgr.isInstalled("mock-data-source"));

  QSignalSpy uninstall_spy(&mgr, &ExtensionManager::uninstallPendingRestart);
  mgr.uninstall(u"mock-data-source"_s);
  ASSERT_TRUE(waitForSignal(uninstall_spy)) << "a valid uninstall under a symlinked store must not be rejected";

  mgr.applyPendingUninstalls();
  EXPECT_FALSE(QDir(real_store + "/mock-data-source").exists())
      << "the uninstall must actually drain; a symlinked store path must not strand it";
  EXPECT_TRUE(quarantinedRecordsIn(real_store).isEmpty())
      << "a legitimate record must not be quarantined as if it escaped the store";

  mgr.setEnabled(u"mock-data-source"_s, true);  // clear the tombstone: QSettings key is process-global
  QSettings().remove("Marketplace/disabledExtensions");
#endif
}

// A legacy in-payload marker whose directory will not delete survives every launch.
// externalizeLegacyUninstallMarkers must NOT file a fresh cleanup record on each
// pass, or the journal grows without bound. RED before the dedup: the second pass
// adds a second cleanup-<uuid>.json.
//
// The payload directory ITSELF is made read-only, not a subdirectory: that stops
// removeRecursively from unlinking any child — the marker included — so the marker
// (and thus the reason to re-externalize) genuinely persists across passes. A
// locked subdirectory would not do: the recursive delete would remove the top-level
// marker before failing deeper, and the second pass would find nothing to re-file.
TEST_F(ExtensionManagerEnableDisableTest, StuckLegacyMarkerIsNotReExternalizedEveryLaunch) {
#ifndef Q_OS_WIN
  if (geteuid() == 0) {
    GTEST_SKIP() << "running as root: directory permissions cannot make a POSIX unlink fail";
  }
#endif
  const QString ext_path = ext_dir_.path() + "/legacy-plugin";
  ASSERT_TRUE(QDir().mkpath(ext_path));

  // The marker must SURVIVE both drain attempts, or the second pass has no marker
  // to re-externalize and the test would pass without exercising the dedup at all.
  // Blocking the delete takes a different mechanism on each platform, so use both:
  // an open handle is what stops a delete on Windows (a read-only DIRECTORY does
  // not — that is why this test failed there), and dropping write permission on the
  // holding directory is what stops the unlink on POSIX (where an open handle does
  // not). Held open across BOTH passes.
  QFile marker(ext_path + "/.pj_pending_uninstall");
  ASSERT_TRUE(marker.open(QIODevice::WriteOnly));
  marker.write("legacy in-payload marker");
  ASSERT_TRUE(marker.flush());

  const auto cleanupRecordCount = [&]() {
    return QDir(removalJournalDirFor(ext_dir_.path()))
        .entryList(QStringList{u"cleanup-*.json"_s}, QDir::Files | QDir::Hidden)
        .size();
  };

  {
    const ScopedDirectoryPermissions locked(ext_path, QFileDevice::ReadOwner | QFileDevice::ExeOwner);

    mgr_->applyPendingUninstalls();
    ASSERT_TRUE(QFile::exists(ext_path + "/.pj_pending_uninstall")) << "the marker must survive the failed delete";
    ASSERT_EQ(cleanupRecordCount(), 1) << "the first pass externalizes the marker into one cleanup record";

    mgr_->applyPendingUninstalls();
    EXPECT_EQ(cleanupRecordCount(), 1) << "a second pass over the same stuck marker must not add another record";
  }
  marker.close();
}

// A downgrade-to-bundled is a version change, not a removal: the plugin stays
// installed (only its version reverts to the shipped baseline). It therefore
// PRESERVES the user's enable/disable choice, exactly like an update does (see
// UpdatePromotionPreservesDisabledState). So a disabled plugin stays disabled
// across a downgrade, drain included.
//
// The contrast with uninstall is about which entry gets WRITTEN, not about anything
// being cleared: an uninstall adds a disabled tombstone that PERSISTS after the
// files are gone (only a reinstall clears it), while a downgrade adds none and
// leaves whatever the user chose exactly as it was. Neither ever re-enables an id —
// draining has no authority to do so at all.
TEST_F(ExtensionManagerEnableDisableTest, DowngradeToBundledPreservesDisabledState) {
  // Install 2.0.0, then declare 1.0.0 as the bundled version so the installed
  // copy sits ABOVE bundled and downgradeToBundled is applicable.
  server_.setBody(dummyPluginZip("mock-data-source", "2.0.0"));
  const Extension ext = makeExtension("mock-data-source", "2.0.0", server_.url());
  QSignalSpy install_spy(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext);
  ASSERT_TRUE(waitForSignal(install_spy));
  mgr_->setBundledVersions({{"mock-data-source", "1.0.0"}});

  // The user disables the plugin, then downgrades it to bundled.
  mgr_->setEnabled("mock-data-source", false);
  ASSERT_TRUE(ExtensionManager::disabledExtensionIds().contains("mock-data-source"));

  QSignalSpy downgrade_spy(mgr_, &ExtensionManager::downgradePendingRestart);
  mgr_->downgradeToBundled("mock-data-source");
  ASSERT_TRUE(waitForSignal(downgrade_spy)) << "downgrade must stage successfully";

  EXPECT_TRUE(ExtensionManager::disabledExtensionIds().contains("mock-data-source"))
      << "downgradeToBundled must preserve the disabled state (a version change, like update), "
         "not clear it like uninstall";

  // The drain that removes the updated copy at the next launch must not re-enable
  // the id either: the host seed restores the bundled version, and the user's
  // choice applies to it just the same.
  mgr_->applyPendingUninstalls();
  EXPECT_TRUE(ExtensionManager::disabledExtensionIds().contains("mock-data-source"))
      << "applying a staged downgrade must leave the disabled state alone; only an uninstall's own "
         "marker retires it";
}

// A staged downgrade keeps the plugin in installed_ until the next launch: it is
// still on disk and loaded this session, exactly like a staged update. The row
// must keep reporting it as installed (with a "Needs Restart" badge driven by
// hasPendingUninstall), not flip to the "—" not-installed placeholder.
TEST_F(ExtensionManagerTest, DowngradeToBundledKeepsInstalledUntilRestart) {
  server_.setBody(dummyPluginZip("mock-data-source", "2.0.0"));
  const Extension ext = makeExtension("mock-data-source", "2.0.0", server_.url());
  QSignalSpy install_spy(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext);
  ASSERT_TRUE(waitForSignal(install_spy));
  mgr_->setBundledVersions({{"mock-data-source", "1.0.0"}});

  QSignalSpy downgrade_spy(mgr_, &ExtensionManager::downgradePendingRestart);
  mgr_->downgradeToBundled("mock-data-source");
  ASSERT_TRUE(waitForSignal(downgrade_spy)) << "downgrade must stage successfully";

  EXPECT_TRUE(mgr_->isInstalled("mock-data-source"))
      << "a staged downgrade must keep the plugin installed until restart, not remove the record now";
  EXPECT_EQ(mgr_->installedVersion("mock-data-source"), "2.0.0")
      << "the still-live updated version must keep being reported until the restart promotes the removal";
  EXPECT_TRUE(mgr_->hasPendingUninstall("mock-data-source"))
      << "the pending-uninstall marker drives the 'Needs Restart' badge";
}

// ---------------------------------------------------------------------------
// [13] Transaction-root placement
// ---------------------------------------------------------------------------

// The plugin scanner walks the extensions dir recursively and has no exclusion
// rule, so a transaction directory holding an extracted DSO is discoverable,
// loadable content for as long as it exists there - and it survives a crash.
// A sideload must therefore extract outside that tree.
TEST_F(ExtensionManagerTest, LocalInstallTransactionRootNeverAppearsInsideTheScannedTree) {
  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  const QString zip = writeZipFile(src, dummyPluginZip("mock-data-source"));
  ASSERT_FALSE(zip.isEmpty());

  const QString stage_dir = transactionStageDirFor(ext_dir_.path());

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);

  mgr_->installFromLocalZip(zip);

  // The transaction root is created synchronously before the fetch is handed to
  // the worker, so this observes the install mid-flight without racing it.
  EXPECT_TRUE(transactionDirsIn(ext_dir_.path()).isEmpty())
      << "in-flight transaction dir inside the scanned tree: "
      << transactionDirsIn(ext_dir_.path()).join(u", "_s).toStdString();
  EXPECT_FALSE(transactionDirsIn(stage_dir).isEmpty())
      << "no transaction dir under the staging sibling " << stage_dir.toStdString();

  ASSERT_TRUE(waitForInstallOutcome(spy_finished, spy_pending));
  ASSERT_EQ(spy_finished.count(), 1);
  EXPECT_TRUE(spy_finished.first().at(1).toBool());
  EXPECT_TRUE(mgr_->isInstalled("mock-data-source"));

  EXPECT_TRUE(transactionDirsIn(ext_dir_.path()).isEmpty())
      << "transaction residue left in the scanned tree after the install completed";
  EXPECT_TRUE(transactionDirsIn(stage_dir).isEmpty()) << "transaction residue left in the staging sibling";
}

// Same rule for the non-staged registry path: a fresh install promotes
// immediately, but its extraction window is just as exposed to the scan.
TEST_F(ExtensionManagerTest, FreshRegistryInstallExtractsOutsideTheScannedTree) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());

  const QString stage_dir = transactionStageDirFor(ext_dir_.path());

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext);

  EXPECT_TRUE(transactionDirsIn(ext_dir_.path()).isEmpty())
      << "in-flight transaction dir inside the scanned tree: "
      << transactionDirsIn(ext_dir_.path()).join(u", "_s).toStdString();
  EXPECT_FALSE(transactionDirsIn(stage_dir).isEmpty())
      << "no transaction dir under the staging sibling " << stage_dir.toStdString();

  ASSERT_TRUE(waitForSignal(spy_finished));
  ASSERT_EQ(spy_finished.count(), 1);
  EXPECT_TRUE(spy_finished.first().at(1).toBool());
  EXPECT_TRUE(mgr_->isInstalled("mock-data-source"));

  EXPECT_TRUE(transactionDirsIn(ext_dir_.path()).isEmpty())
      << "transaction residue left in the scanned tree after the install completed";
  EXPECT_TRUE(transactionDirsIn(stage_dir).isEmpty()) << "transaction residue left in the staging sibling";
}

// Residue written by an older build sits inside the extensions dir, where the
// scan can still reach it. The name-prefix sweep stays the migration path for
// those, so a first launch after the upgrade clears them.
TEST_F(ExtensionManagerTest, LegacyInTreeTransactionResidueIsStillSweptAtStartup) {
  const QString legacy = QDir(ext_dir_.path()).absoluteFilePath(u".pj_install_legacy_abc"_s);
  ASSERT_TRUE(copyFixturePlugin(legacy, u"mock-data-source"_s));
  ASSERT_TRUE(QFile::exists(legacy));

  // Sweeping is the store writer's job, so the "first launch after the upgrade"
  // being modelled here has to be the instance that owns the store: the fixture's
  // manager hands the lease over first, exactly as the previous process would.
  delete mgr_;
  mgr_ = nullptr;

  DownloadManager downloader;
  ExtensionManager mgr(&downloader, ext_dir_.path(), pending_dir_.path());

  EXPECT_FALSE(QFile::exists(legacy)) << "legacy in-tree transaction residue must still be swept at startup";
  EXPECT_FALSE(mgr.isInstalled("mock-data-source")) << "residue must never register as an installed extension";
}

// ---------------------------------------------------------------------------
// [14] Interprocess single-writer lock over the managed store
//
// Two ExtensionManagers over the same directories look exactly like two running
// PlotJuggler processes to the filesystem — the lock is arbitrated by file, not
// by process — so one test process covers most of the contract; the
// SecondProcessCannotMutateStore case below crosses a real process boundary.
// ---------------------------------------------------------------------------

// Re-exec flag that turns this test binary into a bare lock holder.
QString holdStoreLockFlag() {
  return u"--hold-store-lock"_s;
}
constexpr int kHolderBadArguments = 64;
constexpr int kHolderLeaseRefused = 65;
constexpr int kHolderMarkerFailed = 66;

// Child-process entry point: take the store's writer lease, announce it by
// creating `ready_file`, then hold it until the parent creates "<ready_file>.stop".
// The parent therefore controls the window explicitly, with no sleep on either side
// guessing how long the other needs.
int runStoreLockHolder(const QString& extensions_dir, const QString& ready_file) {
  DownloadManager downloader;
  ExtensionManager manager(&downloader, extensions_dir, extensions_dir + u"_pending"_s);
  if (!manager.hasStoreWriteAccess()) {
    return kHolderLeaseRefused;
  }

  QFile marker(ready_file);
  if (!marker.open(QIODevice::WriteOnly)) {
    return kHolderMarkerFailed;
  }
  marker.close();

  // The deadline is a backstop only: a parent that dies without writing the stop
  // marker must not leave this process holding the lease forever.
  const QString stop_file = ready_file + u".stop"_s;
  QDeadlineTimer deadline(60000);
  while (!QFile::exists(stop_file) && !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(10);
  }
  return 0;
}

// An instance that does not own the store runs read-only: it still scans and
// reports installed state, its startup cleanup leaves the owner's live
// transaction directory alone, and a mutating call refuses with a reason the
// user can act on.
TEST_F(ExtensionManagerTest, SecondManagerCannotMutateLockedStore) {
  server_.setBody(dummyPluginZip("mock-file-source"));
  const Extension installed_ext = makeExtension("mock-file-source", "1.0.0", server_.url());
  QSignalSpy spy_installed(mgr_, &ExtensionManager::installFinished);
  mgr_->install(installed_ext);
  ASSERT_TRUE(waitForSignal(spy_installed));
  ASSERT_TRUE(spy_installed.first().at(1).toBool());

  // Stands in for an extraction the owning instance has in flight.
  const QString live_transaction = ext_dir_.path() + "/.pj_install_xyz";
  ASSERT_TRUE(QDir().mkpath(live_transaction));

  DownloadManager downloader2;
  ExtensionManager mgr2(&downloader2, ext_dir_.path(), pending_dir_.path());

  EXPECT_TRUE(QDir(live_transaction).exists())
      << "a second instance must not delete the owner's in-flight transaction directory at startup";
  EXPECT_TRUE(mgr2.isInstalled("mock-file-source")) << "a read-only session must still scan installed state";

  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());
  QSignalSpy spy_error(&mgr2, &ExtensionManager::installError);
  QSignalSpy spy_finished(&mgr2, &ExtensionManager::installFinished);

  mgr2.install(ext);

  ASSERT_EQ(spy_error.count(), 1) << "install must be refused synchronously, before any download starts";
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("Another PlotJuggler instance is managing extensions"))
      << "actual diagnostic: " << spy_error.first().at(1).toString().toStdString();
  ASSERT_EQ(spy_finished.count(), 1);
  EXPECT_FALSE(spy_finished.first().at(1).toBool());
  EXPECT_FALSE(QDir(ext_dir_.path() + "/mock-data-source").exists()) << "a refused install must write nothing";
}

// The transaction directory of a live writer survives a second process starting
// up, in BOTH places cleanup looks: the staging sibling every install actually
// extracts into, and the extensions dir itself, where an older build could have
// left one. Neither name says who owns it, so neither sweep may run without the
// lease.
TEST_F(ExtensionManagerTest, SecondProcessNeverDeletesFirstProcessTransaction) {
  const QString live_in_staging =
      QDir(transactionStageDirFor(ext_dir_.path())).absoluteFilePath(u".pj_install_alive"_s);
  const QString live_legacy_in_tree = QDir(ext_dir_.path()).absoluteFilePath(u".pj_install_alive"_s);
  ASSERT_TRUE(QDir().mkpath(live_in_staging));
  ASSERT_TRUE(QDir().mkpath(live_legacy_in_tree));

  DownloadManager downloader2;
  ExtensionManager mgr2(&downloader2, ext_dir_.path(), pending_dir_.path());
  // The marketplace window rescans on show; that pass must not sweep either.
  mgr2.refreshInstalledFromDisk();

  EXPECT_TRUE(QDir(live_in_staging).exists()) << "extraction directory of the live writer was deleted";
  EXPECT_TRUE(QDir(live_legacy_in_tree).exists()) << "legacy in-tree transaction of the live writer was deleted";

  QDir(transactionStageDirFor(ext_dir_.path())).removeRecursively();
}

// Releasing the lock hands the store to the next instance: it may sweep the
// leftovers of the process that exited, and its mutating operations work again.
TEST_F(ExtensionManagerTest, LockReleasedOnDestructionRestoresWriteMode) {
  const QString stale_transaction = ext_dir_.path() + "/.pj_install_stale";
  ASSERT_TRUE(QDir().mkpath(stale_transaction));

  // The owner exits, exactly as it would before a restart.
  delete mgr_;
  mgr_ = nullptr;

  DownloadManager downloader2;
  ExtensionManager mgr2(&downloader2, ext_dir_.path(), pending_dir_.path());

  EXPECT_FALSE(QDir(stale_transaction).exists())
      << "the new owner must sweep the transaction directory left by the instance that exited";

  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());
  QSignalSpy spy_finished(&mgr2, &ExtensionManager::installFinished);
  QSignalSpy spy_error(&mgr2, &ExtensionManager::installError);

  mgr2.install(ext);

  ASSERT_TRUE(waitForSignal(spy_finished)) << "install must proceed once the lock is free";
  EXPECT_TRUE(spy_finished.first().at(1).toBool())
      << (spy_error.isEmpty() ? std::string("install failed with no error signal")
                              : spy_error.first().at(1).toString().toStdString());
  EXPECT_TRUE(mgr2.isInstalled("mock-data-source"));
}

// The lease is a real interprocess lock, so the case it exists for has to be shown
// across a process boundary: two ExtensionManagers in one process share nothing but
// the file, yet an in-process test cannot rule out that some accident of shared
// state is doing the work. The holder runs as a child of this very binary (see
// holdStoreLockFlag() in main), which keeps the fixture self-contained.
TEST_F(ExtensionManagerTest, SecondProcessCannotMutateStore) {
  // A store of its own, so the child's lease covers nothing else in the suite.
  QTemporaryDir store;
  ASSERT_TRUE(store.isValid());
  const QString ready_file = QDir(store.path()).absoluteFilePath(u"holder.ready"_s);
  const QString stop_file = ready_file + u".stop"_s;
  const QString live_transaction = QDir(transactionStageDirFor(store.path())).absoluteFilePath(u".pj_install_alive"_s);

  QProcess holder;
  holder.setProgram(QCoreApplication::applicationFilePath());
  holder.setArguments({holdStoreLockFlag(), store.path(), ready_file});
  holder.start();
  ASSERT_TRUE(holder.waitForStarted(10000)) << "could not start the lock-holder process";
  // Readiness marker, not a sleep: the child writes it only after its lease is in
  // hand, so the assertions below cannot race the child's startup.
  ASSERT_TRUE(waitUntil([&] { return QFile::exists(ready_file); }, 15000))
      << "holder process never reported taking the store lease";

  // Created only now, with the lease already held elsewhere: it stands for an
  // extraction the OTHER process has in flight. (Created any earlier it would be
  // pre-lease residue, which that process is entitled to sweep at its own startup.)
  ASSERT_TRUE(QDir().mkpath(live_transaction));

  DownloadManager downloader;
  ExtensionManager mgr(&downloader, store.path(), pending_dir_.path());

  EXPECT_FALSE(mgr.hasStoreWriteAccess()) << "the lease is held by another PROCESS, so this one is read-only";
  EXPECT_TRUE(QDir(live_transaction).exists())
      << "startup cleanup deleted a transaction directory owned by the process holding the lease";

  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());
  QSignalSpy spy_error(&mgr, &ExtensionManager::installError);
  QSignalSpy spy_finished(&mgr, &ExtensionManager::installFinished);

  mgr.install(ext);

  ASSERT_EQ(spy_error.count(), 1) << "install must be refused synchronously while another process holds the lease";
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("Another PlotJuggler instance is managing extensions"))
      << "actual diagnostic: " << spy_error.first().at(1).toString().toStdString();
  ASSERT_EQ(spy_finished.count(), 1);
  EXPECT_FALSE(spy_finished.first().at(1).toBool());
  EXPECT_FALSE(QDir(store.path() + "/mock-data-source").exists()) << "a refused install must write nothing";

  // Hand the store back: the holder exits, and the lease becomes available to the
  // next process (here, the next manager) exactly as it would after a restart.
  QFile stop(stop_file);
  ASSERT_TRUE(stop.open(QIODevice::WriteOnly));
  stop.close();
  ASSERT_TRUE(holder.waitForFinished(20000)) << "holder process did not exit after the stop marker";
  EXPECT_EQ(holder.exitStatus(), QProcess::NormalExit);
  EXPECT_EQ(holder.exitCode(), 0) << "holder process reported a failure taking or holding the lease";

  ExtensionManager successor(&downloader, store.path(), pending_dir_.path());
  EXPECT_TRUE(successor.hasStoreWriteAccess()) << "the lease must be free once the holding process is gone";

  QDir(transactionStageDirFor(store.path())).removeRecursively();
}

// The extract worker writes into the transaction directory inside the store, so the
// lease has to outlive it: releasing the lock while a worker is still unpacking
// hands the store to the next process and then keeps writing into it. The
// destructor must cancel, drain, and only then let go.
TEST_F(ExtensionManagerTest, DestroyMidExtractionReleasesLockOnlyAfterDrain) {
  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  const QString zip = writeZipFile(src, manyEntryPluginZip("mock-data-source", 1200));
  ASSERT_FALSE(zip.isEmpty());

  const QString stage_dir = transactionStageDirFor(ext_dir_.path());
  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);

  mgr_->installFromLocalZip(zip);

  // Wait for the worker to have actually written something: a state marker, not a
  // guessed duration. Past this point extraction is provably in flight, with the
  // overwhelming majority of the filler entries still to go.
  ASSERT_TRUE(waitUntil([&] { return !filesUnder(stage_dir).isEmpty(); }, 15000))
      << "extraction never started, so the drain cannot be observed";
  ASSERT_TRUE(spy_finished.isEmpty() && spy_pending.isEmpty())
      << "extraction already finished; the fixture is too small to cover the in-flight case";

  delete mgr_;
  mgr_ = nullptr;

  // Everything the worker could have been writing is gone with the transaction root
  // the destructor retired. A worker still running would put it straight back:
  // extractFromMemory mkpaths each entry's parent before opening the file, so any
  // reappearance below is a write that happened AFTER the lock was released.
  QDir(stage_dir).removeRecursively();
  ASSERT_FALSE(QDir(stage_dir).exists())
      << "the staging tree came back while it was being removed, so a worker is still extracting into the store "
         "after the manager was destroyed";

  DownloadManager successor_downloader;
  ExtensionManager successor(&successor_downloader, ext_dir_.path(), pending_dir_.path());
  EXPECT_TRUE(successor.hasStoreWriteAccess()) << "the destructor must release the lease";

  EXPECT_FALSE(waitUntil([&] { return QDir(stage_dir).exists() || !filesUnder(ext_dir_.path()).isEmpty(); }, 2000))
      << "an extraction worker wrote into the store after the manager released the lease";
}

// Both the staging area and the lease file are SIBLINGS of the extensions dir, and
// a configured path can be a symlink (a packaged install pointing at a data volume,
// a developer linking the store elsewhere). Derived textually, the sibling is
// created next to the LINK: the promoting rename then crosses filesystems, and two
// aliases of one store take two different locks and both believe they are the
// writer. Both must be derived from the resolved target instead.
TEST_F(ExtensionManagerTest, StoreSiblingsFollowTheCanonicalExtensionsDir) {
#ifdef Q_OS_WIN
  GTEST_SKIP() << "symlink creation needs elevation on Windows; the canonical-path rule is platform-neutral";
#else
  QTemporaryDir root;
  ASSERT_TRUE(root.isValid());
  const QString real_store = QDir(root.path()).absoluteFilePath(u"real_store"_s);
  const QString link_store = QDir(root.path()).absoluteFilePath(u"link_store"_s);
  ASSERT_TRUE(QDir().mkpath(real_store));
  ASSERT_TRUE(QFile::link(real_store, link_store));

  DownloadManager downloader;
  ExtensionManager mgr(&downloader, link_store, pending_dir_.path());
  ASSERT_TRUE(mgr.hasStoreWriteAccess());

  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());
  QSignalSpy spy_finished(&mgr, &ExtensionManager::installFinished);
  QSignalSpy spy_error(&mgr, &ExtensionManager::installError);

  mgr.install(ext);

  // The transaction root is created synchronously by install(), so this observes
  // the placement without racing the worker.
  EXPECT_FALSE(transactionDirsIn(transactionStageDirFor(real_store)).isEmpty())
      << "the staging area must be created beside the RESOLVED store";
  EXPECT_FALSE(QDir(transactionStageDirFor(link_store)).exists())
      << "a staging area beside the symlink can resolve onto another filesystem, which breaks promote-by-rename";

  ASSERT_TRUE(waitForSignal(spy_finished));
  EXPECT_TRUE(spy_finished.first().at(1).toBool())
      << (spy_error.isEmpty() ? std::string("install failed with no error signal")
                              : spy_error.first().at(1).toString().toStdString());
  EXPECT_TRUE(QDir(real_store + "/mock-data-source").exists()) << "the install lands in the resolved store";

  // Same rule for the removal journal: two aliases of one store must share it, or
  // each would hold its own record of the same pending removal and drain blind to
  // the other's.
  QSignalSpy spy_pending(&mgr, &ExtensionManager::uninstallPendingRestart);
  mgr.uninstall(u"mock-data-source"_s);
  ASSERT_TRUE(waitForSignal(spy_pending));
  EXPECT_FALSE(removalIntentFor(real_store, u"mock-data-source"_s).isEmpty())
      << "the journal must be created beside the RESOLVED store";
  EXPECT_FALSE(QDir(removalJournalDirFor(link_store)).exists())
      << "a journal beside the symlink would let an alias of the same store keep a second, divergent record";
  // uninstall() disables the id in process-wide QSettings; this fixture does not
  // scope that key, so put it back before it reaches a sibling test.
  mgr.setEnabled(u"mock-data-source"_s, true);

  // Same rule for the lease, with a consequence of its own: an alias of a locked
  // store must not hand out a second writer lease.
  EXPECT_TRUE(QFile::exists(QDir(root.path()).absoluteFilePath(u".real_store.lock"_s)))
      << "the lease file must be keyed to the resolved store, not to the name used to reach it";
  DownloadManager alias_downloader;
  ExtensionManager alias(&alias_downloader, real_store, pending_dir_.path());
  EXPECT_FALSE(alias.hasStoreWriteAccess()) << "a symlink alias of a locked store must not get a second writer lease";

  QDir(transactionStageDirFor(real_store)).removeRecursively();
#endif
}

// A lease that could not be taken because the lock file cannot be CREATED is a
// broken config dir, not a second instance. Telling that user to close another
// PlotJuggler sends them hunting for a process that does not exist, so the two
// causes must not share a message.
TEST_F(ExtensionManagerTest, UnwritableLockLocationIsNotReportedAsContention) {
#ifdef Q_OS_WIN
  GTEST_SKIP() << "POSIX directory permissions do not model the Windows failure the same way";
#else
  if (geteuid() == 0) {
    GTEST_SKIP() << "running as root: directory permissions cannot make the lock location unwritable";
  }
  QTemporaryDir root;
  ASSERT_TRUE(root.isValid());
  // The lease is a sibling of the store, so it is the store's PARENT that has to
  // refuse the write.
  const QString parent = QDir(root.path()).absoluteFilePath(u"readonly_parent"_s);
  const QString store = QDir(parent).absoluteFilePath(u"extensions"_s);
  ASSERT_TRUE(QDir().mkpath(store));
  ASSERT_TRUE(QFile::setPermissions(parent, QFileDevice::ReadOwner | QFileDevice::ExeOwner));

  DownloadManager downloader;
  {
    ExtensionManager mgr(&downloader, store, pending_dir_.path());
    EXPECT_FALSE(mgr.hasStoreWriteAccess()) << "an uncreatable lock file must not grant a writer lease";

    server_.setBody(dummyPluginZip("mock-data-source"));
    const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());
    QSignalSpy spy_error(&mgr, &ExtensionManager::installError);
    mgr.install(ext);

    ASSERT_EQ(spy_error.count(), 1);
    const QString message = spy_error.first().at(1).toString();
    EXPECT_FALSE(message.contains("Another PlotJuggler instance"))
        << "a filesystem failure must not be reported as contention: " << message.toStdString();
    EXPECT_TRUE(message.contains("lock file"))
        << "the message must name what could not be created: " << message.toStdString();
  }

  // Restore write permission so QTemporaryDir can clean up after itself.
  ASSERT_TRUE(QFile::setPermissions(parent, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
#endif
}

// Contention has no retry path: a read-only session never reacquires the lease, so
// the advice has to be to restart once the other instance is closed.
TEST_F(ExtensionManagerTest, ContentionDiagnosticTellsTheUserToRestart) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());

  DownloadManager downloader2;
  ExtensionManager mgr2(&downloader2, ext_dir_.path(), pending_dir_.path());
  ASSERT_FALSE(mgr2.hasStoreWriteAccess());

  QSignalSpy spy_error(&mgr2, &ExtensionManager::installError);
  mgr2.install(ext);

  ASSERT_EQ(spy_error.count(), 1);
  const QString message = spy_error.first().at(1).toString();
  EXPECT_TRUE(message.contains("Another PlotJuggler instance is managing extensions")) << message.toStdString();
  EXPECT_TRUE(message.contains("restart"))
      << "the user must be told a restart is what makes this session the writer: " << message.toStdString();
  EXPECT_FALSE(message.contains("try again"))
      << "nothing retries the lease, so promising that is wrong: " << message.toStdString();
}

}  // namespace
}  // namespace PJ

// ---------------------------------------------------------------------------
// main — QCoreApplication is required for the Qt network event loop
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  // ExtensionManager::disabledExtensionIds() reads via QSettings() with the
  // process-wide default scope. Pin unique org/app names so a residual
  // Marketplace/disabledExtensions entry from another PJ4 binary on the same
  // developer machine cannot leak into these tests.
  QCoreApplication::setOrganizationName("pj4-test-extension-manager");
  QCoreApplication::setApplicationName("extension_manager_test");

  // Lock-holder mode (see SecondProcessCannotMutateStore): this binary re-executes
  // itself to own a store from a SECOND process, which is the only way to test an
  // interprocess lock honestly. Handled before gtest so the child never runs tests.
  const QStringList args = QCoreApplication::arguments();
  if (const int flag = static_cast<int>(args.indexOf(PJ::holdStoreLockFlag())); flag >= 0) {
    if (args.size() < flag + 3) {
      return PJ::kHolderBadArguments;
    }
    return PJ::runStoreLockHolder(args.at(flag + 1), args.at(flag + 2));
  }

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
