// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Tests for PJ::ExtensionManager: install, update, uninstall, staged promotion, the removal journal, the single-writer
// lease, sideloading and the content-addressed store. Each test runs against its own temp directories and a local
// HTTP server.

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

#include "pj_marketplace/plugin_compatibility.hpp"

#ifndef Q_OS_WIN
#include <unistd.h>  // geteuid: root ignores the directory permissions some tests rely on
#endif

#include "pj_marketplace/download_manager.hpp"
#include "pj_marketplace/extension.hpp"
#include "pj_marketplace/loaded_profile.hpp"
#include "pj_marketplace/platform_utils.hpp"
#include "pj_test_store_dir.h"
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

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

// Spins the event loop until `predicate` holds or the deadline expires, returning its last answer: both "appears" and
// "never appears" can be asserted without a sleep on a guessed duration.
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

// Every regular file under `path`, recursively: evidence that an extract worker is (or is no longer) writing into the
// store.
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

// Builds an in-memory ZIP from { relative_path -> content }; raise `capacity` for a deliberately large fixture (see
// manyEntryPluginZip).
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
  // The 3.0.0 build of this id declares min_sdk_required "99.0.0": the only fixture with an above-host SDK floor.
  if (ext_id == "mock-data-source" && version == "3.0.0") {
    return QStringLiteral(PJ_MOCK_INCOMPATIBLE_PLUGIN_PATH);
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
  if (ext_id == "malformed-floor") {
    return QStringLiteral(PJ_MOCK_MALFORMED_FLOOR_PLUGIN_PATH);
  }
  if (ext_id == "malformed-version") {
    return QStringLiteral(PJ_MOCK_MALFORMED_VERSION_PLUGIN_PATH);
  }
  if (ext_id == "newer-sdk") {
    return QStringLiteral(PJ_MOCK_NEWER_SDK_PLUGIN_PATH);
  }
  return {};
}

QString pluginFileName() {
  return "plugin" + QString::fromStdString(PlatformUtils::pluginExtension());
}

// Backups are named "<id>-<uuid>" (the old directory is not opened to read its version), so look them up by prefix.
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

// A ZIP with an <id>/ root directory and a real plugin DSO whose embedded manifest is the installed-state source of
// truth (there is no metadata sidecar).
QByteArray dummyPluginZip(const QString& ext_id, const QString& version = "1.0.0") {
  return pluginZipWithDso(ext_id, ext_id, version);
}

// A valid plugin archive padded with `filler_count` small entries, so extraction is still running when a test tears
// the manager down. Many small entries, not one big file: the worker observes cancellation only at entry and block
// boundaries.
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

// Scopes QCoreApplication::applicationVersion(), which is process-wide, to one test.
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

// The staging area every install extracts into. Spelled out rather than asked of the manager, because the placement
// is what the tests pin: a sibling of the extensions dir (same filesystem, so the rename is atomic; outside the
// recursive plugin scan, so scratch is never loadable content).
QString transactionStageDirFor(const QString& extensions_dir) {
  return extensions_dir + u".install_stage"_s;
}

// The removal journal directory. Spelled out because its placement is under test: a sibling of the extensions dir, so
// a record never lives inside the payload whose deletion it describes.
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

// Plants a journal record by hand (corruption, or a hostile writer borrowing the drain's delete privilege); `stem`
// names the file, so a record can be filed under a name that disagrees with its id.
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

// Clamps a directory's permissions for one test to make a removal fail on purpose, and restores them on scope exit,
// even when an ASSERT bails out early.
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

class ExtensionManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Several tests create a QTemporaryDir under PlatformUtils::configDir() (next to backupDir()) so renames into the
    // backup are atomic; QTemporaryDir needs its parent to exist, which a fresh CI runner lacks.
    ASSERT_TRUE(QDir().mkpath(PlatformUtils::configDir()));
    ASSERT_TRUE(ext_dir_.isValid());
    ASSERT_TRUE(pending_dir_.isValid());
    downloader_ = new DownloadManager();
    mgr_ = new ExtensionManager(downloader_, ext_dir_.path(), pending_dir_.path());
  }

  void TearDown() override {
    delete mgr_;
    delete downloader_;
  }

  test::StoreTempDir ext_dir_;
  QTemporaryDir pending_dir_;
  LocalHttpServer server_;
  DownloadManager* downloader_ = nullptr;
  ExtensionManager* mgr_ = nullptr;
};

// A fresh install downloads, extracts and registers: installStarted, then installFinished(id, true).
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

// A successful install records its content digest in the persisted profile and hands the runtime the store object as
// its managed load path; once the install goes away, the startup reconcile prunes the profile and collects the orphan
// object.
TEST_F(ExtensionManagerTest, InstallRecordsStoreObjectForRuntimeLoad) {
  server_.setBody(dummyPluginZip("mock-file-source"));
  const Extension ext = makeExtension("mock-file-source", "1.0.0", server_.url());

  QSignalSpy spy(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext);
  ASSERT_TRUE(waitForSignal(spy));
  ASSERT_TRUE(spy.first().at(1).toBool());

  // The persisted profile maps the id to a non-empty content digest.
  const QMap<QString, QString> profile = readLoadedProfile(mgr_->extensionsDir() + ".profiles");
  ASSERT_TRUE(profile.contains("mock-file-source"));
  EXPECT_FALSE(profile.value("mock-file-source").isEmpty());

  // The runtime is handed exactly one managed dir: the store object, which exists
  // under the artifacts sibling of the extensions dir, not in extensions/<id>/.
  const std::vector<QString> managed = mgr_->managedPluginDirs();
  ASSERT_EQ(managed.size(), 1u);
  const QString object_dir = managed.front();
  EXPECT_TRUE(QDir(object_dir).exists());
  EXPECT_TRUE(object_dir.contains(QLatin1StringView(".artifacts")));

  // Remove extensions/<id>/ (what the startup drain of an uninstall does) and reconcile as a restart would.
  ASSERT_TRUE(QDir(mgr_->extensionsDir() + "/mock-file-source").removeRecursively());
  mgr_->testReconcileFromDisk();
  EXPECT_FALSE(QDir(object_dir).exists());
  EXPECT_FALSE(readLoadedProfile(mgr_->extensionsDir() + ".profiles").contains("mock-file-source"));
  EXPECT_TRUE(mgr_->managedPluginDirs().empty());
}

// installProgress is forwarded during the download, with the extension id and a percent in [0, 100].
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

// install() of an already-installed id emits installError at once and starts no download.
TEST_F(ExtensionManagerTest, InstallRejectsAlreadyInstalledExtension) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy_first(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext);
  ASSERT_TRUE(waitForSignal(spy_first));
  ASSERT_TRUE(spy_first.first().at(1).toBool());

  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);
  mgr_->install(ext);
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_EQ(spy_error.first().at(0).toString(), "mock-data-source");
  EXPECT_FALSE(spy_error.first().at(1).toString().isEmpty());
}

// A second install() while one is in progress is rejected at once via installError.
TEST_F(ExtensionManagerTest, InstallBlocksConcurrentRequests) {
  // A server that accepts connections but never answers keeps the first download pending without burning CPU.
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

// installFromLocalZip() also rejects while a registry install is in flight (both share the pending_id_ guard): the UI
// queues sideloads, but the manager enforces one install at a time at its boundary.
TEST_F(ExtensionManagerTest, InstallFromLocalZipBlockedByConcurrentInstall) {
  QTcpServer hanging_server;
  hanging_server.listen(QHostAddress::LocalHost, 0);
  const QUrl hanging_url = QUrl(u"http://127.0.0.1:%1/"_s.arg(hanging_server.serverPort()));

  const Extension ext_a = makeExtension("mock-data-source", "1.0.0", hanging_url);

  // A real on-disk zip: installFromLocalZip() reads the file before the pending_id_ guard.
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
  // Reported against the sideload's file label, not the running download's id.
  EXPECT_EQ(spy_error.first().at(0).toString(), "pkg.zip");
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("already in progress"));
}

// install() with no artifact for the current platform emits installError and starts no download.
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

TEST_F(ExtensionManagerTest, HostCompatibilityAcceptsCurrentPlatformWithNoMinVersion) {
  Extension ext = makeExtension("plugin-x", "1.0.0", server_.url());
  ext.min_sdk_required = "";  // floor absent

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

// A sideloaded row carries no registry fields, so `platforms` is empty by design: empty platforms read as "no registry
// declaration" and pass the gate.
TEST_F(ExtensionManagerTest, HostCompatibilityAcceptsEmptyPlatformsAsLocallyDeclared) {
  Extension ext;
  ext.id = "local-plugin";
  ext.name = "local-plugin";
  ext.version = "1.0.0";
  // ext.platforms is left empty, as on a synthesized local-only row.

  const auto compat = mgr_->hostCompatibility(ext);
  EXPECT_TRUE(compat.ok) << "an ext with an empty platforms map is a local install without registry metadata — "
                            "the platform gate must not fire on it";
  EXPECT_TRUE(compat.reason.isEmpty());
}

TEST_F(ExtensionManagerTest, HostCompatibilityRejectsHostBelowDeclaredSdkMinimum) {
  Extension ext = makeExtension("plugin-x", "1.0.0", server_.url());
  ext.min_sdk_required = "99.0.0";

  const auto compat = mgr_->hostCompatibility(ext);
  EXPECT_FALSE(compat.ok);
  EXPECT_TRUE(compat.reason.contains("99.0.0"))
      << "reason must surface the required minimum: " << compat.reason.toStdString();
}

TEST_F(ExtensionManagerTest, HostCompatibilityAcceptsHostAtOrAboveDeclaredSdkMinimum) {
  Extension ext = makeExtension("plugin-x", "1.0.0", server_.url());
  ext.min_sdk_required = "0.1.0";
  EXPECT_TRUE(mgr_->hostCompatibility(ext).ok) << "host above min must be accepted";
}

TEST_F(ExtensionManagerTest, DeprecatedApplicationFloorNeverGates) {
  // min_plotjuggler_version is decoded but never gates.
  Extension ext = makeExtension("plugin-x", "1.0.0", server_.url());
  ext.min_plotjuggler_version = "999.0.0";
  EXPECT_TRUE(mgr_->hostCompatibility(ext).ok);
  ext.min_plotjuggler_version = "not-a-version";
  EXPECT_TRUE(mgr_->hostCompatibility(ext).ok);
}

// The reduced-features note is display-only: a compatible plugin with a full-feature floor above this build's SDK
// stays ok=true and carries the note; an absent or unparseable claim yields nothing.
TEST_F(ExtensionManagerTest, FeatureCompletenessNoteIsInformationalOnly) {
  EXPECT_TRUE(evaluateFeatureCompleteness("").empty());
  EXPECT_TRUE(evaluateFeatureCompleteness("not-a-version").empty());
  EXPECT_TRUE(evaluateFeatureCompleteness("0.1.0").empty()) << "at/below the host SDK: full features";
  const std::string note = evaluateFeatureCompleteness("99.0.0");
  EXPECT_TRUE(note.find("99.0.0") != std::string::npos) << note;

  Extension ext = makeExtension("plugin-x", "1.0.0", server_.url());
  ext.suggested_sdk_version = "99.0.0";
  const auto compat = mgr_->hostCompatibility(ext);
  EXPECT_TRUE(compat.ok) << "the note must not gate";
  EXPECT_TRUE(compat.reason.isEmpty());
  EXPECT_TRUE(compat.completeness_note.contains("99.0.0")) << compat.completeness_note.toStdString();

  InstalledExtension installed;
  installed.id = "plugin-x";
  installed.version = "1.0.0";
  installed.suggested_sdk_version = "99.0.0";
  const auto installed_compat = mgr_->hostCompatibility(installed);
  EXPECT_TRUE(installed_compat.ok);
  EXPECT_TRUE(installed_compat.completeness_note.contains("99.0.0"));
}

// install() refuses an extension whose min_sdk_required is above this build's SDK and fires both installError and
// installFinished(id, false): a caller awaiting only one would otherwise hang or miss the failure.
TEST_F(ExtensionManagerTest, InstallRefusesWhenHostBelowMinSdkRequired) {
  Extension ext = makeExtension("plugin-x", "1.0.0", server_.url());
  ext.min_sdk_required = "99.0.0";

  QSignalSpy started(mgr_, &ExtensionManager::installStarted);
  QSignalSpy error(mgr_, &ExtensionManager::installError);
  QSignalSpy finished(mgr_, &ExtensionManager::installFinished);

  mgr_->install(ext);

  EXPECT_EQ(started.count(), 0) << "installStarted must not fire when the host is below the plugin's minimum";
  ASSERT_EQ(error.count(), 1);
  EXPECT_EQ(error.first().at(0).toString(), "plugin-x");
  EXPECT_TRUE(error.first().at(1).toString().contains("99.0.0"));
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

// Uninstall stages the removal on every platform: the extension leaves the installed set at once while the directory
// stays on disk until the next launch drains it. Removing in place would free the directory name for reuse while the
// DSO stays mapped, and dlopen resolves by path name.
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

// Installing over a staged uninstall would be erased by the next startup drain: the pending removal is the guard,
// since isInstalled() no longer speaks for the id.
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

// The sideload path does not route through doInstall(): its fresh-install branch would clear the destination,
// pending removal included, so it needs its own guard.
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

// A directory placed by hand can be named anything (the scan takes the id from the manifest): the pending state is
// keyed by id in the journal, so the row keeps "Needs Restart" and the install guards stay armed.
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

  // The drain deletes the directory the record names.
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

// update() stages the new version: installPendingRestart is emitted, the live version stays, and only a restart
// (applyPendingInstalls) promotes it. ext/pending/backup share one filesystem so the promotion renames are atomic.
TEST_F(ExtensionManagerTest, UpdateStagesNewVersionUntilRestart) {
  cleanBackups("mock-data-source");

  test::StoreTempDir local_ext_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_ext_XXXXXX"));
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

// Promoting a staged update backs up the previous version into backupDir(), at promotion time rather than in
// update(). ext/pending/backup share one filesystem so the renames are atomic.
TEST_F(ExtensionManagerTest, UpdatePromotionBacksUpOldVersion) {
  cleanBackups("mock-data-source");

  test::StoreTempDir local_ext_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_ext_XXXXXX"));
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

// A staged update promotion preserves the user's disabled choice (counterpart of InstallResetsDisabledState).
TEST_F(ExtensionManagerTest, UpdatePromotionPreservesDisabledState) {
  cleanBackups("mock-data-source");

  test::StoreTempDir local_ext_dir(QDir(PlatformUtils::backupDir()).absoluteFilePath("../test_ext_XXXXXX"));
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

// A failed update fails during staging, before the live install is touched: version intact, nothing staged, no
// backup.
TEST_F(ExtensionManagerTest, FailedUpdateStagingLeavesLiveInstallUntouched) {
  cleanBackups("mock-data-source");

  test::StoreTempDir local_ext_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_ext_XXXXXX"));
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
  // Serve garbage: libarchive fails to extract it and DownloadManager's failed() reaches installFinished(id, false).
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

// A second update() for an id with a staged install is refused up front instead of re-downloading on top of the
// pending dir (hasUpdate() stays true until restart, so a caller can innocently ask again). The manager guard backs
// callers other than the marketplace UI.
TEST_F(ExtensionManagerTest, UpdateRejectsAlreadyPendingInstall) {
  cleanBackups("mock-data-source");

  test::StoreTempDir local_ext_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_ext_XXXXXX"));
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

// A staged removal also blocks update(): it would resurrect an id the user just asked to remove.
TEST_F(ExtensionManagerTest, UpdateRejectsAlreadyPendingUninstall) {
  cleanBackups("mock-data-source");

  test::StoreTempDir local_ext_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_ext_XXXXXX"));
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

  // Stand in for a payload-resident `.pj_pending_uninstall` marker: its existence must still register as a pending
  // removal so update() does not run over it; its content is never read (see externalizeLegacyUninstallMarkers).
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

// installedVersion() answers from what the startup seed reported writing, not from the scan snapshot, which predates
// that write and cannot be refreshed (docs/TECHNICAL_NOTES.md "dlopen pins the first image"). The discriminating case
// is the seed's restore of an incompatible copy, which leaves the managed dir below the scanned version.
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

// Versions compare by SemVer precedence, not lexically: "10.0.0" > "2.0.0".
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

// installedVersion() is the single source of truth for what is on disk. Three branches: id absent, no seeded version
// (the scanned one), and the seed-refreshed core plugin (docs/TECHNICAL_NOTES.md "dlopen pins the first image").

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

// The seed-refresh case: the scanner still sees the pre-seed version and the seed reports the version it promoted;
// installedVersion() must report the promoted one.
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

// A marketplace-installed copy newer than the bundled one: scanned wins, since the seed does not overwrite it.
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

// hasUpdate stops offering an update when the seeded version matches the registry, even if the scanner still sees the
// pre-seed version.
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

  EXPECT_TRUE(mgr_->isInstalled("mock-data-source"));
  EXPECT_EQ(mgr_->installedExtensions()["mock-data-source"].version, "1.0.0");

  // The active directory lives under extensions_dir, not pending_dir.
  EXPECT_TRUE(QDir(ext_dir_.path() + "/mock-data-source").exists());
  EXPECT_FALSE(QDir(staged_dir).exists());
}

// A staged payload is untrusted until the restart that promotes it (the host or SDK may have changed, or the stage
// been tampered with): promotion repeats the embedded compatibility gate before moving the current install aside.
TEST_F(ExtensionManagerTest, ApplyPendingInstallsRejectsIncompatibleEmbeddedRequirements) {
  const ScopedApplicationVersion host{"4.0.0"};
  const QString staged_dir = pending_dir_.path() + "/newer-sdk";
  ASSERT_TRUE(copyFixturePlugin(staged_dir, "newer-sdk"));
  ASSERT_TRUE(writePendingIntentForTest(staged_dir, "newer-sdk"));

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);
  mgr_->applyPendingInstalls();

  ASSERT_EQ(spy_finished.count(), 1);
  EXPECT_FALSE(spy_finished.first().at(1).toBool());
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("SDK", Qt::CaseInsensitive));
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("99.0.0"));
  EXPECT_FALSE(QDir(staged_dir).exists());
  EXPECT_FALSE(QDir(ext_dir_.path() + "/newer-sdk").exists());
  EXPECT_FALSE(mgr_->isInstalled("newer-sdk"));
}

// Constructing the manager (the "one restart") reflects a staged update over an existing install in a SINGLE pass:
// initComponents() promotes staged work before snapshotting disk, so the first open of the extension path reads the
// promoted payload instead of pinning the stale pre-update image.
TEST_F(ExtensionManagerTest, ConstructionReflectsStagedUpdateInOnePass) {
  cleanBackups("mock-data-source");
  // Same filesystem as backupDir(): promoting backs up v1 via QDir::rename(), which cannot cross filesystems.
  test::StoreTempDir ext_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_ext_XXXXXX"));
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

  // The marketplace window re-scans disk on showEvent. In the process that just promoted the update this must not read
  // the stale pre-promotion image (dlopen caches a plugin DSO by path name and never unloads it): promotion avoids
  // opening the old directory, so the re-scan reports the promoted version.
  mgr.refreshInstalledFromDisk();
  EXPECT_EQ(mgr.installedExtensions()["mock-data-source"].version, "2.0.0")
      << "a re-scan after promotion must not revert to the stale old version";

  cleanBackups("mock-data-source");
}

// A staged batch reports every promoted extension at its NEW version even when a SIBLING's pre-update DSO stays
// resident after dlclose (NODELETE via a bound STB_GNU_UNIQUE symbol; the pinning fixture models real plugin builds).
// Opening a sibling's not-yet-promoted build in the conflict scan would pin its image and the marketplace would
// re-offer an update already on disk. Contract: no sibling dir is opened before its own promotion. The single-id test
// above cannot catch this: the conflict scan skips its own target.
TEST_F(ExtensionManagerTest, ConstructionPromotesStagedBatchDespitePinnedSibling) {
  cleanBackups("mock-data-source");
  cleanBackups("pinning-data-source");

  // Same filesystem as backupDir() so promotion's backup renames are moves.
  test::StoreTempDir ext_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_ext_XXXXXX"));
  ASSERT_TRUE(ext_dir.isValid());
  QTemporaryDir pend_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_pending_XXXXXX"));
  ASSERT_TRUE(pend_dir.isValid());

  // Installed v1 pair + staged v2 pair, laid out as update() leaves them. Nothing may dlopen the installed paths before
  // the manager constructs: the drain must be this process's first touch. "mock-data-source" sorts before
  // "pinning-data-source", so the old pinning build is the sibling a pre-promotion conflict scan would open.
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
  // Canary that the scenario was real: the promoted pinning DSO must be resident, and no displaced pre-update image may
  // linger under the backup dir (a mapping there means a pre-promotion open pinned it). If the fixture stops pinning
  // after a toolchain change, the first expectation fails: rework the fixture's anchor instead of deleting the check.
  QFile maps("/proc/self/maps");
  ASSERT_TRUE(maps.open(QIODevice::ReadOnly));
  const QString mapped = QString::fromUtf8(maps.readAll());
  EXPECT_TRUE(mapped.contains(QDir(ext_dir.path()).canonicalPath() + "/pinning-data-source/"));
  // Canonicalized like the kernel reports mapping paths; both promotions displaced an install into it, so it exists.
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

// Promoting a staged update over an existing install moves the previous version into PlatformUtils::backupDir()
// first: otherwise the update would overwrite it with no recovery path.
TEST_F(ExtensionManagerTest, ApplyPendingInstallsBacksUpExistingExtensionBeforePromotion) {
  // ext_dir + pending_dir on the same filesystem as backupDir(), so all the QDir::rename moves are atomic.
  test::StoreTempDir local_ext_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_ext_XXXXXX"));
  QTemporaryDir local_pending_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_pending_XXXXXX"));
  ASSERT_TRUE(local_ext_dir.isValid());
  ASSERT_TRUE(local_pending_dir.isValid());
  cleanBackups("mock-data-source");

  DownloadManager local_dl;
  ExtensionManager local_mgr(&local_dl, local_ext_dir.path(), local_pending_dir.path());

  // Install v1 directly to populate extensions/<id>/.
  server_.setBody(dummyPluginZip("mock-data-source"));
  QSignalSpy spy_install(&local_mgr, &ExtensionManager::installFinished);
  local_mgr.install(makeExtension("mock-data-source", "1.0.0", server_.url()));
  ASSERT_TRUE(waitForSignal(spy_install));
  ASSERT_TRUE(spy_install.first().at(1).toBool());
  spy_install.clear();

  // Stage a v2 update with its intent file, as doInstall(staging=true) leaves it before the user restarts.
  const QString staged_dir = local_pending_dir.path() + "/mock-data-source";
  ASSERT_TRUE(copyFixturePlugin(staged_dir, "mock-data-source", "2.0.0"));
  ASSERT_TRUE(writePendingIntentForTest(staged_dir, "mock-data-source", "2.0.0"));

  local_mgr.applyPendingInstalls();

  ASSERT_EQ(spy_install.count(), 1);
  EXPECT_TRUE(spy_install.first().at(1).toBool()) << "staged update must promote";
  EXPECT_EQ(local_mgr.installedExtensions()["mock-data-source"].version, "2.0.0");

  // The previous version is preserved in backup, recoverable manually.
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
  // Promotion backs the live install up into backupDir() by directory rename, so ext/pending must share its filesystem.
  cleanBackups("mock-data-source");
  test::StoreTempDir local_ext_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_ext_XXXXXX"));
  ASSERT_TRUE(local_ext_dir.isValid());
  QTemporaryDir local_pending_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_pending_XXXXXX"));
  ASSERT_TRUE(local_pending_dir.isValid());
  DownloadManager local_dl;
  ExtensionManager local_mgr(&local_dl, local_ext_dir.path(), local_pending_dir.path());

  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy_install(&local_mgr, &ExtensionManager::installFinished);
  local_mgr.install(ext);
  ASSERT_TRUE(waitForSignal(spy_install));
  ASSERT_TRUE(spy_install.first().at(1).toBool());
  ASSERT_TRUE(QDir(local_ext_dir.path() + "/mock-data-source").exists());

  const QString staged_dir = local_pending_dir.path() + "/mock-data-source";
  ASSERT_TRUE(copyFixturePlugin(staged_dir, "mock-file-source"));
  ASSERT_TRUE(writePendingIntentForTest(staged_dir, "mock-data-source"));

  spy_install.clear();
  QSignalSpy spy_error(&local_mgr, &ExtensionManager::installError);
  local_mgr.applyPendingInstalls();

  ASSERT_EQ(spy_install.count(), 1);
  EXPECT_FALSE(spy_install.first().at(1).toBool());
  ASSERT_EQ(spy_error.count(), 1);
  // Production pairs applyPendingInstalls() with a disk refresh (back to back in initComponents()); the refresh
  // re-registers the previous install that the rejected update restored on disk.
  local_mgr.refreshInstalledFromDisk();
  EXPECT_TRUE(local_mgr.isInstalled("mock-data-source"));
  EXPECT_TRUE(QDir(local_ext_dir.path() + "/mock-data-source").exists());
  EXPECT_FALSE(QDir(staged_dir).exists());
  cleanBackups("mock-data-source");
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

  // The next process drains the staged removal in initComponents() before it snapshots installed state, so the id and
  // its directory are gone. The drain belongs to the store owner: this instance releases it first, as a restart would.
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

// A directory carrying a legacy `.pj_pending_uninstall` marker is removed by applyPendingUninstalls().
TEST_F(ExtensionManagerTest, ApplyPendingUninstallsRemovesMarkedDirectory) {
  const QString ext_path = ext_dir_.path() + "/mock-data-source";
  ASSERT_TRUE(QDir().mkpath(ext_path));
  QFile marker(ext_path + "/.pj_pending_uninstall");
  ASSERT_TRUE(marker.open(QIODevice::WriteOnly));
  marker.close();

  mgr_->applyPendingUninstalls();

  EXPECT_FALSE(QDir(ext_path).exists());
}

// A directory without a marker or a journal record is left untouched.
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

// currentPlatform() must return a non-empty string in "<os>-<arch>" format.
TEST(PlatformDetectionTest, CurrentPlatformHasExpectedFormat) {
  const QString platform = PlatformUtils::currentPlatform();
  EXPECT_FALSE(platform.isEmpty());
  EXPECT_TRUE(platform.contains('-')) << "Expected '<os>-<arch>' format, got: " << platform.toStdString();
}

// On the primary Linux x86_64 build/CI host, the reported platform must match the
// key used in the registry fixture so that install() can resolve the download artifact.
TEST(PlatformDetectionTest, LinuxX86PlatformMatchesRegistryKey) {
#if !defined(Q_OS_LINUX) || !defined(Q_PROCESSOR_X86_64)
  GTEST_SKIP() << "test pins the Linux x86_64 platform key";
#endif
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
  ext.platforms["macos-arm64"] = {
      "https://example.com/test/extension-macos-arm64.zip",
      "sha256:0000000000000000000000000000000000000000000000000000000000000000"};

  EXPECT_TRUE(ext.platforms.contains(PlatformUtils::currentPlatform()))
      << "Platform '" << PlatformUtils::currentPlatform().toStdString()
      << "' is not listed in the mock-data-source registry entry";
}

// isWindows() is false on non-Windows builds.
TEST(PlatformDetectionTest, IsWindowsReturnsFalseOnLinux) {
  if (PlatformUtils::isWindows()) {
    GTEST_SKIP() << "test asserts the Linux code path is exercised";
  }
  EXPECT_FALSE(PlatformUtils::isWindows());
}

// refreshInstalledFromDisk() invoked while the extract worker is still writing into the transaction directory must
// not wipe it (the sweep spares pending_extract_dir_); the install then completes normally.
TEST_F(ExtensionManagerTest, RefreshDuringInstallDoesNotWipeInProgressTransaction) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  // Trigger a refresh the moment the worker announces its extract phase; the receiver is mgr_, so the lambda runs on
  // the manager's thread, as a UI Refresh click would.
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

// Once the install is finished, refreshInstalledFromDisk() sweeps stale transaction directories again: the guard
// protects only the CURRENT install.
TEST_F(ExtensionManagerTest, RefreshWipesStaleTransactionDirsAfterInstall) {
  server_.setBody(dummyPluginZip("mock-data-source"));
  const Extension ext = makeExtension("mock-data-source", "1.0.0", server_.url());

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  mgr_->install(ext);
  ASSERT_TRUE(waitForSignal(spy_finished));

  // A fake leftover of a crashed install in extensions_dir_.
  const QString stale_dir = QDir(ext_dir_.path()).absoluteFilePath(u".pj_install_stale_abc"_s);
  ASSERT_TRUE(QDir().mkpath(stale_dir));
  ASSERT_TRUE(QFile::exists(stale_dir));

  mgr_->refreshInstalledFromDisk();

  EXPECT_FALSE(QFile::exists(stale_dir)) << "stale .pj_install_* should have been swept";
  EXPECT_TRUE(mgr_->isInstalled("mock-data-source")) << "live install must remain registered";
}

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

// The happy path. fetch() serves a file:// URL, so a local archive reuses the download/verify/extract pipeline of a
// registry artifact.
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

// Without a registered confirmation, re-sideloading an installed id is refused rather than swapping a DSO the
// running session may hold open.
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

  // A genuinely different payload: re-offering the installed bytes is answered as a no-op before this policy applies.
  const QString newer = writeZipFile(src, dummyPluginZip("mock-data-source", "2.0.0"), "v2.zip");
  ASSERT_FALSE(newer.isEmpty());

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->installFromLocalZip(newer);

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
  // Seeded straight onto disk: installFromLocalZip refreshes installed state itself, so a directory is a sufficient
  // precondition and avoids an extract/dlopen round-trip.
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

// A rebuild that changed nothing is not an install: nothing is staged, no restart is demanded and the replacement
// question is never put.
TEST_F(ExtensionManagerTest, InstallFromLocalZipSkipsAnArchiveIdenticalToTheInstalledTree) {
  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  const QString zip = writeZipFile(src, dummyPluginZip("mock-data-source"));
  ASSERT_FALSE(zip.isEmpty());

  // Installed from that same archive, so the tree on disk holds exactly the bytes the second install carries; seeding
  // the fixture directly would leave that equality resting on the zip builder and the fixture happening to agree.
  QSignalSpy spy_first(mgr_, &ExtensionManager::installFinished);
  mgr_->installFromLocalZip(zip);
  ASSERT_TRUE(waitForSignal(spy_first));
  ASSERT_TRUE(spy_first.first().at(1).toBool());

  bool asked = false;
  QObject confirmation_owner;
  mgr_->setReplaceConfirmation(&confirmation_owner, [&asked](const QString&, const QString&, const QString&) {
    asked = true;
    return true;
  });

  QSignalSpy spy_unchanged(mgr_, &ExtensionManager::installUnchanged);
  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->installFromLocalZip(zip);

  // Its own terminal outcome, not an install: installFinished would have the UI announce an installation that never
  // happened.
  ASSERT_TRUE(waitForSignal(spy_unchanged));
  EXPECT_EQ(spy_unchanged.first().at(0).toString(), "mock-data-source");
  EXPECT_EQ(spy_error.count(), 0);
  EXPECT_EQ(spy_finished.count(), 0);
  // No stage: the extension stays usable and the user is not sent through a restart for a payload that did not move.
  EXPECT_EQ(spy_pending.count(), 0);
  EXPECT_FALSE(mgr_->hasPendingInstall("mock-data-source"));
  EXPECT_FALSE(asked) << "an identical payload has no replacement to confirm";
  EXPECT_TRUE(mgr_->isInstalled("mock-data-source"));
}

// Two directories can embed one id and the scan resolves that to the HIGHEST version, which need not live in the
// directory named for the id. The archive is compared against the copy that won, not against extensions/<id>:
// matching the loser would drop an archive that changes which plugin runs and leave the duplicate in place.
TEST_F(ExtensionManagerTest, InstallFromLocalZipDoesNotSkipWhenAnotherDirectoryHoldsTheWinningCopy) {
  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  // extensions/mock-data-source holds 1.0.0 — the archive's twin, and the loser.
  ASSERT_TRUE(copyFixturePlugin(ext_dir_.path() + "/mock-data-source", "mock-data-source", "1.0.0"));
  // extensions/mock-ds-shadow holds 2.0.0 of the same embedded id — the winner.
  ASSERT_TRUE(copyFixturePlugin(ext_dir_.path() + "/mock-ds-shadow", "mock-data-source", "2.0.0"));

  const QString zip = writeZipFile(src, dummyPluginZip("mock-data-source", "1.0.0"));
  ASSERT_FALSE(zip.isEmpty());

  QObject confirmation_owner;
  mgr_->setReplaceConfirmation(
      &confirmation_owner, [](const QString&, const QString&, const QString&) { return true; });

  QSignalSpy spy_unchanged(mgr_, &ExtensionManager::installUnchanged);
  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);

  mgr_->installFromLocalZip(zip);

  ASSERT_TRUE(waitForInstallOutcome(spy_finished, spy_pending));
  EXPECT_EQ(spy_unchanged.count(), 0) << "the archive does not match the copy that is actually installed";
  EXPECT_EQ(spy_pending.count(), 1);
}

// The skip keys on content, not on the version string: an archive with the same version and one extra file is a
// different payload and takes the staged-replacement path.
TEST_F(ExtensionManagerTest, InstallFromLocalZipStagesAnArchiveThatOnlyDiffersByAnExtraFile) {
  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  const QString first = writeZipFile(src, dummyPluginZip("mock-data-source"), "first.zip");
  ASSERT_FALSE(first.isEmpty());

  QSignalSpy spy_first(mgr_, &ExtensionManager::installFinished);
  mgr_->installFromLocalZip(first);
  ASSERT_TRUE(waitForSignal(spy_first));
  ASSERT_TRUE(spy_first.first().at(1).toBool());

  const QByteArray padded = buildZip({
      {"mock-data-source/" + pluginFileName(), readAll(pluginPathForId("mock-data-source"))},
      {u"mock-data-source/CHANGELOG.md"_s, "one more file than the installed tree has"},
  });
  const QString second = writeZipFile(src, padded, "second.zip");
  ASSERT_FALSE(second.isEmpty());

  QObject confirmation_owner;
  mgr_->setReplaceConfirmation(
      &confirmation_owner, [](const QString&, const QString&, const QString&) { return true; });

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);

  mgr_->installFromLocalZip(second);

  ASSERT_TRUE(waitForInstallOutcome(spy_finished, spy_pending));
  EXPECT_EQ(spy_pending.count(), 1);
  EXPECT_EQ(spy_finished.count(), 0);
}

// A declined confirmation leaves everything exactly as it was.
TEST_F(ExtensionManagerTest, InstallFromLocalZipHonoursDeclinedReplacement) {
  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  // A version the seeded tree does not hold: an archive carrying the installed bytes is a no-op and never reaches the
  // confirmation this case is about.
  const QString zip = writeZipFile(src, dummyPluginZip("mock-data-source", "2.0.0"));
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

// The question is put long after installFromLocalZip() returns, so the object the callback captured can be gone (a
// marketplace window closed while the archive was extracting). That resolves as a decline and leaves the registration
// cleared rather than armed for the next archive.
TEST_F(ExtensionManagerTest, ReplaceConfirmationSurvivesOwnerDestruction) {
  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  // A version the seeded tree does not hold: an archive carrying the installed bytes is a no-op and never reaches the
  // confirmation this case is about.
  const QString zip = writeZipFile(src, dummyPluginZip("mock-data-source", "2.0.0"));
  ASSERT_FALSE(zip.isEmpty());
  ASSERT_TRUE(copyFixturePlugin(ext_dir_.path() + "/mock-data-source", "mock-data-source"));

  // Heap-allocated so it can die before the answer is needed. The sentinel lives on the test stack, so an unguarded
  // call still reports itself; the dangling capture itself is visible only under a sanitizer.
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

  // The dead registration is dropped, not retried: a second archive takes the no-confirmation branch, which is how the
  // cleared state is observable.
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

// Two windows can share one manager and the last registration wins, so clearing is scoped to the registrant: a
// displaced window closing must not disarm the live registration.
TEST_F(ExtensionManagerTest, ClearReplaceConfirmationOnlyRemovesOwnCallback) {
  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  // A version the seeded tree does not hold: an archive carrying the installed bytes is a no-op and never reaches the
  // confirmation this case is about.
  const QString zip = writeZipFile(src, dummyPluginZip("mock-data-source", "2.0.0"));
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

// The answer comes out of a modal dialog, which spins the event loop, so another window can take the registration
// over WHILE the question is open. A live registrant is not enough to accept the answer: it came from a registration
// that no longer holds the slot, and honouring it would stage a replacement the window in charge never approved.
TEST_F(ExtensionManagerTest, ReentrantReregistrationDiscardsDisplacedOwnersAnswer) {
  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  // A version the seeded tree does not hold: an archive carrying the installed bytes is a no-op and never reaches the
  // confirmation this case is about.
  const QString zip = writeZipFile(src, dummyPluginZip("mock-data-source", "2.0.0"));
  ASSERT_FALSE(zip.isEmpty());
  ASSERT_TRUE(copyFixturePlugin(ext_dir_.path() + "/mock-data-source", "mock-data-source"));

  QObject first_owner;
  QObject second_owner;
  bool second_invoked = false;
  // Re-registering from inside the callback is what a modal event loop makes reachable, and the reentrancy must not
  // touch the std::function being executed. The displaced owner still answers "yes".
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

  // Discarding the displaced answer must not disarm the successor, which still owns the next archive's decision.
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
  test::StoreTempDir local_ext_dir(QDir(PlatformUtils::configDir()).absoluteFilePath("test_ext_XXXXXX"));
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

// A sideload gets the same gate as a registry install: a ZIP whose manifest demands a newer SDK is refused instead of
// landing an extension that cannot run.
TEST_F(ExtensionManagerTest, LocalZipInstallRejectsAnIncompatibleMinSdkRequired) {
  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  const QString zip = writeZipFile(src, dummyPluginZip("mock-data-source", "3.0.0"));
  ASSERT_FALSE(zip.isEmpty());

  // The refusal reads exactly like the registry one for the same floor, so the expected text is taken from that gate.
  Extension registry_equivalent = makeExtension("mock-data-source", "3.0.0", server_.url());
  registry_equivalent.min_sdk_required = "99.0.0";
  const QString expected_reason = mgr_->hostCompatibility(registry_equivalent).reason;
  ASSERT_FALSE(expected_reason.isEmpty());

  QSignalSpy spy_started(mgr_, &ExtensionManager::installStarted);
  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->installFromLocalZip(zip);

  ASSERT_TRUE(waitForInstallOutcome(spy_finished, spy_pending));
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_EQ(spy_error.first().at(0).toString(), "mock-data-source");
  EXPECT_EQ(spy_error.first().at(1).toString(), expected_reason)
      << "a local refusal must use the same wording as a registry refusal";
  ASSERT_EQ(spy_finished.count(), 1);
  EXPECT_FALSE(spy_finished.first().at(1).toBool());
  EXPECT_EQ(spy_pending.count(), 0) << "an incompatible sideload must not be staged either";
  EXPECT_EQ(spy_started.count(), 0) << "installStarted must not fire for a refused sideload";

  EXPECT_FALSE(mgr_->isInstalled("mock-data-source"));
  EXPECT_TRUE(mgr_->installedExtensions().isEmpty());
  EXPECT_TRUE(directoryHasNoChildren(ext_dir_.path()))
      << "the transaction directory and its payload must be cleaned up on refusal";
}

// The complement: a fixture whose declared SDK floor this host satisfies installs normally, so the gate rejects on
// the version rule alone, not on the presence of a declared minimum.
TEST_F(ExtensionManagerTest, LocalZipInstallAcceptsACompatibleMinSdkRequired) {
  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  const QByteArray plugin = readAll(QStringLiteral(PJ_MOCK_COMPATIBLE_FLOOR_PLUGIN_PATH));
  ASSERT_FALSE(plugin.isEmpty());
  const QString zip = writeZipFile(src, buildZip({{"mock-data-source/" + pluginFileName(), plugin}}));
  ASSERT_FALSE(zip.isEmpty());

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->installFromLocalZip(zip);

  ASSERT_TRUE(waitForInstallOutcome(spy_finished, spy_pending));
  for (const auto& reported : spy_error) {
    qWarning("installFromLocalZip error: %s", qPrintable(reported.at(1).toString()));
  }
  ASSERT_EQ(spy_error.count(), 0);
  ASSERT_EQ(spy_finished.count(), 1);
  EXPECT_TRUE(spy_finished.first().at(1).toBool());
  EXPECT_TRUE(mgr_->isInstalled("mock-data-source"));
  EXPECT_EQ(mgr_->installedExtensions()["mock-data-source"].version, "3.0.0");
}

// min_sdk_required is a hard contract: a local archive has no registry record to pre-screen, so its embedded
// descriptor carries the gate into the same admission decision as the application floor.
TEST_F(ExtensionManagerTest, LocalZipInstallRejectsANewerSdkRequirement) {
  const ScopedApplicationVersion host{"4.0.0"};

  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  const QString zip = writeZipFile(src, dummyPluginZip("newer-sdk"));
  ASSERT_FALSE(zip.isEmpty());

  QSignalSpy spy_started(mgr_, &ExtensionManager::installStarted);
  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->installFromLocalZip(zip);

  ASSERT_TRUE(waitForInstallOutcome(spy_finished, spy_pending));
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("SDK", Qt::CaseInsensitive));
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("99.0.0"));
  EXPECT_EQ(spy_started.count(), 0);
  EXPECT_EQ(spy_pending.count(), 0);
  EXPECT_FALSE(mgr_->isInstalled("newer-sdk"));
  EXPECT_TRUE(directoryHasNoChildren(ext_dir_.path()));
}

TEST_F(ExtensionManagerTest, LocalZipInstallRejectsMalformedPluginVersion) {
  const ScopedApplicationVersion host{"4.0.0"};

  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  const QString zip = writeZipFile(src, dummyPluginZip("malformed-version", "4.1"));
  ASSERT_FALSE(zip.isEmpty());

  QSignalSpy spy_started(mgr_, &ExtensionManager::installStarted);
  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->installFromLocalZip(zip);

  ASSERT_TRUE(waitForInstallOutcome(spy_finished, spy_pending));
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("Invalid plugin version"));
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("4.1"));
  EXPECT_EQ(spy_started.count(), 0);
  EXPECT_EQ(spy_pending.count(), 0);
  EXPECT_FALSE(mgr_->isInstalled("malformed-version"));
  EXPECT_TRUE(directoryHasNoChildren(ext_dir_.path()));
}

// Registry metadata is an early filter, not permission to trust the payload: an embedded floor stricter than the
// registry's is enforced before the transaction is promoted into the managed store.
TEST_F(ExtensionManagerTest, RegistryInstallRejectsAnIncompatibleEmbeddedFloor) {
  server_.setBody(dummyPluginZip("mock-data-source", "3.0.0"));

  Extension ext = makeExtension("mock-data-source", "3.0.0", server_.url());
  ext.min_sdk_required.clear();

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);
  mgr_->install(ext);

  ASSERT_TRUE(spy_finished.wait(5000));
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("SDK 99.0.0"));
  EXPECT_FALSE(mgr_->isInstalled("mock-data-source"));
  EXPECT_TRUE(directoryHasNoChildren(ext_dir_.path()));
}

// A malformed embedded min_sdk_required is rejected at the SDK scan itself (the decoder validates the format), so the
// payload never becomes an installed extension; the registry cannot sanitize it by omitting or contradicting it.
TEST_F(ExtensionManagerTest, RegistryInstallRejectsAMalformedEmbeddedFloor) {
  server_.setBody(dummyPluginZip("malformed-floor"));

  Extension ext = makeExtension("malformed-floor", "1.0.0", server_.url());
  ext.min_sdk_required.clear();

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);
  mgr_->install(ext);

  ASSERT_TRUE(spy_finished.wait(5000));
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_FALSE(mgr_->isInstalled("malformed-floor"));
  EXPECT_TRUE(directoryHasNoChildren(ext_dir_.path()));
}

// The scan carries the declared floor out of the embedded manifest; an extension that declares none reports an empty
// one, so "no declaration" stays distinguishable from "declares 0".
TEST_F(ExtensionManagerTest, DiscoverExtensionDirectoryCarriesMinSdkRequired) {
  ASSERT_TRUE(copyFixturePlugin(ext_dir_.path() + "/needs-newer-host", "mock-data-source", "3.0.0"));
  ASSERT_TRUE(copyFixturePlugin(ext_dir_.path() + "/no-floor", "mock-file-source"));

  mgr_->refreshInstalledFromDisk();
  const QMap<QString, InstalledExtension> installed = mgr_->installedExtensions();

  ASSERT_TRUE(installed.contains("mock-data-source"));
  EXPECT_EQ(installed["mock-data-source"].min_sdk_required, "99.0.0");
  ASSERT_TRUE(installed.contains("mock-file-source"));
  EXPECT_TRUE(installed["mock-file-source"].min_sdk_required.isEmpty())
      << "a manifest without a floor must leave the field empty, not synthesise one";
}

// min_sdk_required is a compatibility requirement, so an extension with several DSOs must satisfy the strictest valid
// component floor; requiring exact strings would reject a sound package whose modules use different parts of the SDK.
TEST_F(ExtensionManagerTest, DiscoverExtensionDirectoryUsesHighestMinSdkRequiredAcrossDsos) {
  const QString extension_dir = ext_dir_.path() + "/multi-dso";
  ASSERT_TRUE(QDir().mkpath(extension_dir));
  const QString suffix = QString::fromStdString(PlatformUtils::pluginExtension());
  ASSERT_TRUE(
      QFile::copy(
          QStringLiteral(PJ_MOCK_COMPATIBLE_FLOOR_PLUGIN_PATH),
          QDir(extension_dir).absoluteFilePath("floor-4" + suffix)));
  ASSERT_TRUE(
      QFile::copy(
          QStringLiteral(PJ_MOCK_INCOMPATIBLE_PLUGIN_PATH), QDir(extension_dir).absoluteFilePath("floor-5" + suffix)));

  mgr_->refreshInstalledFromDisk();
  const QMap<QString, InstalledExtension> installed = mgr_->installedExtensions();

  ASSERT_TRUE(installed.contains("mock-data-source"))
      << "different valid component floors must not invalidate the complete extension";
  EXPECT_EQ(installed["mock-data-source"].version, "3.0.0");
  EXPECT_EQ(installed["mock-data-source"].min_sdk_required, "99.0.0")
      << "the extension must retain the strictest component requirement";
}

// Compatibility metadata is untrusted input: a non-numeric string must not read as zero and pass on every host, so
// the gate rejects malformed floors itself.
TEST_F(ExtensionManagerTest, HostCompatibilityRejectsMalformedMinSdkRequired) {
  for (const QString malformed : {"future", "1.0", "01.0.0", "1.0.0-01", "1.0.0+"}) {
    InstalledExtension installed;
    installed.id = "malformed-floor";
    installed.min_sdk_required = malformed;

    const ExtensionManager::HostCompatibility result = mgr_->hostCompatibility(installed);

    EXPECT_FALSE(result.ok) << malformed.toStdString();
    EXPECT_TRUE(result.reason.contains("invalid", Qt::CaseInsensitive)) << result.reason.toStdString();
    EXPECT_TRUE(result.reason.contains(malformed)) << result.reason.toStdString();
  }
}

TEST_F(ExtensionManagerTest, MalformedInstalledVersionRemainsVisibleAndManageable) {
  ASSERT_TRUE(copyFixturePlugin(ext_dir_.path() + "/malformed-version", "malformed-version", "4.1"));

  mgr_->refreshInstalledFromDisk();
  const QMap<QString, InstalledExtension> installed = mgr_->installedExtensions();
  ASSERT_TRUE(installed.contains("malformed-version"));
  EXPECT_EQ(installed["malformed-version"].version, "4.1");
  const auto compatibility = mgr_->hostCompatibility(installed["malformed-version"]);
  EXPECT_FALSE(compatibility.ok);
  EXPECT_TRUE(compatibility.reason.contains("Invalid plugin version"));

  QSignalSpy spy_pending(mgr_, &ExtensionManager::uninstallPendingRestart);
  mgr_->uninstall("malformed-version");

  ASSERT_EQ(spy_pending.count(), 1);
  EXPECT_FALSE(mgr_->isInstalled("malformed-version"));
  EXPECT_TRUE(mgr_->hasPendingUninstall("malformed-version"));
}

// A minimum is a real SemVer value: a release outranks its pre-release and numeric pre-release identifiers compare
// numerically. Driven through the pure primitive so the host SDK version is injectable.
TEST_F(ExtensionManagerTest, CompatibilityUsesSemverPrereleasePrecedence) {
  const auto floor_ok = [](std::string_view floor, std::string_view host_sdk) {
    return evaluatePluginCompatibility(
               {.version = "1.0.0", .abi_major = 0, .min_sdk_required = floor},
               {.abi_major = PJ_ABI_VERSION, .sdk_version = host_sdk})
        .ok;
  };
  EXPECT_FALSE(floor_ok("5.0.0", "5.0.0-rc.1"));
  EXPECT_TRUE(floor_ok("5.0.0-rc.10", "5.0.0"));
  EXPECT_FALSE(floor_ok("5.0.0-beta.11", "5.0.0-beta.2"));
  EXPECT_TRUE(floor_ok("5.0.0+publisher-build", "5.0.0+host-build"));
  // Plugin metadata remains strict SemVer: a v-prefixed floor is invalid.
  EXPECT_FALSE(floor_ok("v5.0.0", "5.0.0"));
}

// The compatibility gate precedes replacement policy: a host that cannot run the incoming bytes neither asks the
// user for consent nor leaves staged state that would displace the compatible installation on restart.
TEST_F(ExtensionManagerTest, IncompatibleLocalZipReplacementIsRejectedBeforeConfirmation) {
  ASSERT_TRUE(copyFixturePlugin(ext_dir_.path() + "/mock-data-source", "mock-data-source", "1.0.0"));

  QTemporaryDir src;
  ASSERT_TRUE(src.isValid());
  const QString zip = writeZipFile(src, dummyPluginZip("mock-data-source", "3.0.0"));
  ASSERT_FALSE(zip.isEmpty());

  QObject confirmation_owner;
  bool confirmation_invoked = false;
  mgr_->setReplaceConfirmation(
      &confirmation_owner, [&confirmation_invoked](const QString&, const QString&, const QString&) {
        confirmation_invoked = true;
        return true;
      });

  QSignalSpy spy_finished(mgr_, &ExtensionManager::installFinished);
  QSignalSpy spy_pending(mgr_, &ExtensionManager::installPendingRestart);
  QSignalSpy spy_error(mgr_, &ExtensionManager::installError);

  mgr_->installFromLocalZip(zip);

  ASSERT_TRUE(waitForInstallOutcome(spy_finished, spy_pending));
  EXPECT_FALSE(confirmation_invoked);
  ASSERT_EQ(spy_error.count(), 1);
  EXPECT_TRUE(spy_error.first().at(1).toString().contains("Requires PlotJuggler SDK 99.0.0"));
  EXPECT_EQ(spy_pending.count(), 0);
  EXPECT_FALSE(mgr_->hasPendingInstall("mock-data-source"));
  EXPECT_EQ(mgr_->installedExtensions()["mock-data-source"].version, "1.0.0");
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

// Enable/disable: a disabled extension stays on disk but the host skips loading it. The in-memory record on
// installed_[id], the static disabledExtensionIds() the runtime catalog reads, and isEnabled() must agree.

// Starts every test from a clean QSettings key: the manager reads "Marketplace/disabledExtensions" through the default
// scope, so a residual list from a previous test or the developer's machine would leak in.
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

  // Installs the standard fixture extension and returns its directory, or an empty string if the install never
  // completed (the caller's cue to ASSERT).
  QString installVictim() {
    server_.setBody(dummyPluginZip("mock-data-source"));
    QSignalSpy install_spy(mgr_, &ExtensionManager::installFinished);
    mgr_->install(makeExtension("mock-data-source", "1.0.0", server_.url()));
    if (!waitForSignal(install_spy)) {
      return {};
    }
    return ext_dir_.path() + "/mock-data-source";
  }

  // The persisted key (file-static in ExtensionManager.cpp), duplicated here to reset it between tests.
  static constexpr const char* kDisabledExtensionsKey = "Marketplace/disabledExtensions";
};

// A newly-arrived extension defaults to enabled: setEnabled() has never been
// called and the QSettings list is empty.
TEST_F(ExtensionManagerEnableDisableTest, IsEnabledDefaultsToTrueForUnknownIds) {
  EXPECT_TRUE(mgr_->isEnabled("never-heard-of-it"));
  EXPECT_TRUE(ExtensionManager::disabledExtensionIds().isEmpty());
}

// Disabling an id appends it to the persisted list AND updates installed_[id].enabled, so the UI reads the new state
// without a rescan; the runtime catalog sees the same list through the static disabledExtensionIds().
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

// Enabling an already-enabled id (or disabling a disabled one) leaves the persisted list unchanged: a duplicated id
// would need TWO enables to take effect.
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

// refreshInstalledFromDisk() reflects the persisted disable state onto every installed_[id].enabled it rebuilds:
// otherwise any rescan would reset the flag to true while QSettings still holds the disabled list.
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

// A fresh install of an id resets any stale disabled entry, so a sideload after a disable cannot leave the user with
// a silently unloaded plugin they just chose to install.
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

// Staging a removal makes the id unloadable at once: the plugin is on disk until the next launch drains it, and the
// loader decides from the disabled list alone, so an enabled id would load again if the deletion failed (see
// PartiallyFailedUninstallKeepsItsRetryRecordAndDisabledState).
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

// A removal that fails part-way keeps its retry record and the disabled entry. removeRecursively() deletes children
// before it can fail on the one it cannot unlink, so a record stored inside the payload would be among the
// casualties; the record lives outside it, so the next launch retries and never loads the plugin the user removed.
// A subdirectory is poisoned on every platform: an open handle blocks the unlink on Windows (a mapped DSO in
// production), dropped write permission on the holding directory blocks it on POSIX. Both are applied, so the drain
// fails part-way here and on CI alike.
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
  // A top-level payload file the drain WILL delete: its disappearance below shows that a record kept inside the payload
  // cannot survive a partial deletion.
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

// The staging window has two persisted writes and a crash can land between them. The disabled entry is written and
// synced FIRST, so the reachable intermediate state is "unloadable but still installed": recoverable, and never a
// removed plugin coming back. The crash is simulated by reproducing that first step alone and then draining, as the
// next launch would.
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

// A `.pj_pending_uninstall` marker is package-controlled content: an archive can ship one naming any id. Nothing is
// inferred from it, least of all an id whose enable state to change (one package re-enabling an unrelated plugin the
// user disabled); its existence alone schedules a deletion of the directory holding it, so a package can only harm
// itself.
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

// A journal record authorises `QDir::removeRecursively()`, so the journal is a capability the store lease does not
// protect: corruption, or anything able to drop a file in the state directory, would inherit the drain's privilege.
// Every record is validated before ANY deletion and every refusal is fail-closed: nothing deleted, record
// quarantined, diagnostic raised.

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

// A legitimate record still drains. An id may begin with a dot (invalidExtensionIdReason rejects only "." / ".." and
// separators), so ".local-plugin" is journalled as the HIDDEN file ".local-plugin.json": a listing that ignores
// hidden entries never sees it and the removal stays pending forever.
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

// Delete-time TOCTOU: removeRecursively() FOLLOWS a symlink at the target, which the parse-time check does not catch
// when the link resolves to a legitimate in-store directory. A record for `bait` (a symlink to a real sibling payload)
// passes validation, yet draining it naively would delete the sibling's contents. The delete-time re-confinement
// refuses a symlink target outright: nothing deleted, record quarantined, diagnostic raised. A managed payload is
// always a real directory, so this models corruption or a concurrent writer.
TEST_F(RemovalJournalValidationTest, RecordWhoseTargetIsASymlinkIsRefusedAtDeletion) {
#ifdef Q_OS_WIN
  GTEST_SKIP() << "symlink creation needs elevation on Windows; the canonical-path rule is platform-neutral";
#else
  ASSERT_FALSE(installVictim().isEmpty());
  const QString sibling = ext_dir_.path() + "/mock-data-source";  // a real, legitimate payload
  ASSERT_TRUE(QDir(sibling).exists());

  // `bait` is a symlink to that sibling: a direct child of the store by name, resolving to a direct managed child, so
  // parse-time validation admits it. Only the delete-time symlink refusal stops the drain from wiping the sibling.
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

// unmanagedPayloadRejection compares a record's lexical parent against the CANONICAL store root, so a configured store
// path with a symlink component would reject even a legitimate uninstall record and strand the removal.
// Canonicalizing the store dir once at construction puts both comparisons on one footing.
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

// An in-payload marker whose directory will not delete survives every launch: externalizeLegacyUninstallMarkers
// must not file a fresh cleanup record on each pass, or the journal grows without bound. The payload directory ITSELF
// is made read-only, not a subdirectory: that stops removeRecursively from unlinking the marker, so it persists
// across passes (a locked subdirectory would let the recursive delete remove the top-level marker first).
TEST_F(ExtensionManagerEnableDisableTest, StuckLegacyMarkerIsNotReExternalizedEveryLaunch) {
#ifndef Q_OS_WIN
  if (geteuid() == 0) {
    GTEST_SKIP() << "running as root: directory permissions cannot make a POSIX unlink fail";
  }
#endif
  const QString ext_path = ext_dir_.path() + "/legacy-plugin";
  ASSERT_TRUE(QDir().mkpath(ext_path));

  // The marker must SURVIVE both drain attempts, or the second pass has nothing to re-externalize and the test passes
  // without exercising the dedup. Blocking the delete takes a different mechanism per platform, so both are used: an
  // open handle stops a delete on Windows (a read-only DIRECTORY does not), dropped write permission on the holding
  // directory stops the unlink on POSIX. Held open across BOTH passes.
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

// A downgrade-to-bundled is a version change, not a removal: the plugin stays installed and keeps the user's
// enable/disable choice, like an update (see UpdatePromotionPreservesDisabledState). An uninstall adds a disabled
// tombstone that persists after the files are gone, while a downgrade adds none; neither ever re-enables an id, since
// draining has no authority to.
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

// The plugin scanner walks the extensions dir recursively and has no exclusion rule, so a transaction directory
// holding an extracted DSO would be loadable content for as long as it exists there (and it survives a crash): a
// sideload extracts outside that tree.
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

// A `.pj_install_*` directory inside the extensions dir, where the scan can reach it, is still swept at startup.
TEST_F(ExtensionManagerTest, LegacyInTreeTransactionResidueIsStillSweptAtStartup) {
  const QString legacy = QDir(ext_dir_.path()).absoluteFilePath(u".pj_install_legacy_abc"_s);
  ASSERT_TRUE(copyFixturePlugin(legacy, u"mock-data-source"_s));
  ASSERT_TRUE(QFile::exists(legacy));

  // Sweeping is the store writer's job, so the modelled first launch must be the instance that owns the store: the
  // fixture's manager hands the lease over first, as the previous process would.
  delete mgr_;
  mgr_ = nullptr;

  DownloadManager downloader;
  ExtensionManager mgr(&downloader, ext_dir_.path(), pending_dir_.path());

  EXPECT_FALSE(QFile::exists(legacy)) << "legacy in-tree transaction residue must still be swept at startup";
  EXPECT_FALSE(mgr.isInstalled("mock-data-source")) << "residue must never register as an installed extension";
}

// Interprocess single-writer lock: two ExtensionManagers over the same directories look like two PlotJuggler
// processes to the filesystem (the lock is arbitrated by file), so most cases run in one process;
// SecondProcessCannotMutateStore crosses a real process boundary.

// Re-exec flag that turns this test binary into a bare lock holder.
QString holdStoreLockFlag() {
  return u"--hold-store-lock"_s;
}
constexpr int kHolderBadArguments = 64;
constexpr int kHolderLeaseRefused = 65;
constexpr int kHolderMarkerFailed = 66;

// Child-process entry point: take the store's writer lease, announce it by creating `ready_file`, then hold it until
// the parent creates "<ready_file>.stop", so the parent controls the window with no sleep on either side.
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

// The transaction directory of a live writer survives a second process starting up, in BOTH places cleanup looks:
// the staging sibling every install extracts into and the extensions dir itself. Neither name says who owns it, so
// neither sweep may run without the lease.
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

// The lease is a real interprocess lock, so it is shown across a process boundary: two ExtensionManagers in one
// process share nothing but the file, yet an in-process test cannot rule out an accident of shared state. The holder
// runs as a child of this binary (see holdStoreLockFlag() in main).
TEST_F(ExtensionManagerTest, SecondProcessCannotMutateStore) {
  // A store of its own, so the child's lease covers nothing else in the suite.
  test::StoreTempDir store;
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

  // Created only now, with the lease held elsewhere: it stands for an extraction the OTHER process has in flight. Any
  // earlier it would be pre-lease residue, which that process may sweep at its own startup.
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

  // Wait for the worker to have written something (a state marker, not a guessed duration): past this point extraction
  // is provably in flight, with most of the filler entries still to go.
  ASSERT_TRUE(waitUntil([&] { return !filesUnder(stage_dir).isEmpty(); }, 15000))
      << "extraction never started, so the drain cannot be observed";
  ASSERT_TRUE(spy_finished.isEmpty() && spy_pending.isEmpty())
      << "extraction already finished; the fixture is too small to cover the in-flight case";

  delete mgr_;
  mgr_ = nullptr;

  // Everything the worker could have written is gone with the transaction root the destructor retired. A worker still
  // running would put it back (extractFromMemory mkpaths each entry's parent), so any reappearance is a write after the
  // lock was released.
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

// The staging area and the lease file are SIBLINGS of the extensions dir, and a configured path can be a symlink.
// Derived textually, the sibling lands next to the LINK: the promoting rename crosses filesystems, and two aliases of
// one store take two locks and both believe they are the writer. Both must derive from the resolved target.
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

  // Same rule for the removal journal: two aliases of one store must share it, or each holds its own record of the same
  // pending removal and drains blind to the other's.
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

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  // ExtensionManager::disabledExtensionIds() reads QSettings() in the process-wide default scope: unique org/app
  // names keep a residual Marketplace/disabledExtensions entry from another PJ4 binary on the machine out of the tests.
  QCoreApplication::setOrganizationName("pj4-test-extension-manager");
  QCoreApplication::setApplicationName("extension_manager_test");

  // Lock-holder mode (see SecondProcessCannotMutateStore): this binary re-executes itself to own a store from a SECOND
  // process, the only honest way to test an interprocess lock. Handled before gtest so the child never runs tests.
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
