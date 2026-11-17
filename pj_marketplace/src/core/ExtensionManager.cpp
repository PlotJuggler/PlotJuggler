// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QStorageInfo>
#include <QStringList>
#include <QUuid>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>

#include "pj_marketplace/artifact_store.hpp"
#include "pj_marketplace/content_manifest.hpp"
#include "pj_marketplace/download_manager.hpp"
#include "pj_marketplace/extension_manager.hpp"
#include "pj_marketplace/generation_digest.hpp"
#include "pj_marketplace/loaded_profile.hpp"
#include "pj_marketplace/platform_utils.hpp"
#include "pj_marketplace/plugin_check_runner.hpp"
#include "pj_marketplace/plugin_compatibility.hpp"
#include "pj_marketplace/store_session.hpp"
#include "pj_marketplace/utf8_path.hpp"
#include "pj_marketplace/version_compare.hpp"
#include "pj_plugins/host/plugin_catalog.hpp"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

static constexpr const char* kPendingUninstallMarker = ".pj_pending_uninstall";
static constexpr const char* kPendingInstallIntent = ".pj_pending_install";
static constexpr const char* kQuarantinePrefix = ".pj_quarantine_";
static constexpr int kMaxDiagnostics = 50;
// Persisted name: the runtime plugin catalog reads the same list through disabledExtensionIds().
static constexpr const char* kDisabledExtensionsKey = "Marketplace/disabledExtensions";

QString extRoot(const QString& extensions_dir, const QString& id) {
  return QDir(extensions_dir).absoluteFilePath(id);
}

QString pendingRoot(const QString& pending_dir, const QString& id) {
  return QDir(pending_dir).absoluteFilePath(id);
}

// Content identity of an extension tree: the hash of every file, paths included. Empty when the tree cannot be
// characterised (missing, or a symlink): "not comparable", never a match.
std::string contentDigestOf(const QString& directory) {
  const auto files = inspectContentManifest(std::filesystem::path(directory.toStdU16String()));
  if (!files) {
    return {};
  }
  return aggregateContentManifestDigest(*files);
}

// True when a rename between the two paths is atomic (same mounted filesystem, no EXDEV).
bool sameFilesystem(const QString& first, const QString& second) {
  const QStorageInfo first_volume(first);
  const QStorageInfo second_volume(second);
  return first_volume.isValid() && second_volume.isValid() && first_volume.device() == second_volume.device();
}

struct DirectoryDiscovery {
  bool found_plugin = false;
  QString error;
  InstalledExtension record;
};

struct PendingInstallIntent {
  bool valid = false;
  QString id;
  QString version;
  QString error;
};

QString pendingInstallIntentPath(const QString& root) {
  return QDir(root).absoluteFilePath(kPendingInstallIntent);
}

QString pendingUninstallMarkerPath(const QString& root) {
  return QDir(root).absoluteFilePath(kPendingUninstallMarker);
}

QString invalidExtensionIdReason(const QString& id) {
  if (id.isEmpty()) {
    return "Extension id is empty";
  }
  if (id == "." || id == ".." || id.contains('/') || id.contains('\\')) {
    return QString("Extension id \"%1\" is not safe for filesystem paths").arg(id);
  }
  return {};
}

// What a journal record asks of this host. A record that names none of these is invalid, never assumed.
enum class RemovalOperation {
  kUninstall,  // the user removed the extension; the id keeps its disabled tombstone
  kDowngrade,  // a version revert; the user's enable/disable choice is untouched
  kCleanup,    // path-only: delete this directory, no id and no authority over one
};

// A removal this host owes the user. `record_path` is the file it was parsed from: the drain retires that file, never
// one the record names.
struct RemovalIntent {
  RemovalOperation operation = RemovalOperation::kCleanup;
  QString id;  // empty for kCleanup, which carries no id authority at all
  QString path;
  QString record_path;
};

// Persisted record shape; a record with another schema is rejected, not guessed at.
constexpr int kRemovalJournalSchema = 1;

// Removal journal root: a SIBLING of the extensions dir, outside the recursive plugin scan and outside the payload, so
// a delete that fails part-way cannot consume its own retry record. Derived from the CANONICAL store root like the
// lease file, so two aliases of a symlinked store share one journal. `store_root` must already be canonical (resolved
// once at construction): hasPendingUninstall() calls this per catalog row on every marketplace table rebuild.
QString removalJournalRoot(const QString& store_root) {
  return store_root + u".state"_s;
}

// One record per extension id, named for it: the drain requires the file stem to equal the id inside.
QString removalIntentPath(const QString& store_root, const QString& id) {
  return QDir(removalJournalRoot(store_root)).absoluteFilePath(id + u".json"_s);
}

QString cleanupIntentPath(const QString& store_root) {
  return QDir(removalJournalRoot(store_root))
      .absoluteFilePath(u"cleanup-%1.json"_s.arg(QUuid::createUuid().toString(QUuid::Id128)));
}

QString removalOperationToken(RemovalOperation operation) {
  switch (operation) {
    case RemovalOperation::kUninstall:
      return u"uninstall"_s;
    case RemovalOperation::kDowngrade:
      return u"downgrade"_s;
    case RemovalOperation::kCleanup:
      return u"cleanup"_s;
  }
  return {};
}

// Empty when `path` may be recursively deleted, else the reason it may not. A record is a deletion capability, so the
// target must be a DIRECT child of the canonical store root (which excludes the root, its ancestors, ".." and the
// sibling state/staging/seed roots and lease file) and, once it exists, must also resolve inside the store so a
// planted symlink cannot carry the delete out. A path that does not exist yet passes the canonical check: there is
// nothing to delete.
QString unmanagedPayloadRejection(const QString& store_root, const QString& path) {
  if (path.isEmpty()) {
    return u"record names no path"_s;
  }
  if (QDir::isRelativePath(path)) {
    return u"record path is not absolute"_s;
  }
  // `store_root` is already canonical; only the record's target needs resolving.
  const QString root = QDir::cleanPath(store_root);
  const QString target = QDir::cleanPath(path);
  if (target == root) {
    return u"record targets the extensions store itself"_s;
  }
  if (QFileInfo(target).absolutePath() != root) {
    return u"record targets a path outside the extensions store"_s;
  }
  // Empty for a path that does not exist (already drained): not an error.
  const QString canonical = QFileInfo(target).canonicalFilePath();
  if (!canonical.isEmpty()) {
    if (QFileInfo(canonical).absolutePath() != root) {
      return u"record path resolves outside the extensions store"_s;
    }
    if (!QFileInfo(canonical).isDir()) {
      return u"record path is not a directory"_s;
    }
  }
  return {};
}

// Maps a record's operation token onto the enum. nullopt for absent or unrecognised: defaulting would pick
// "uninstall", the destructive reading.
std::optional<RemovalOperation> parseRemovalOperationToken(const QJsonObject& object) {
  const QString token = object.value(u"operation"_s).toString();
  if (token == u"uninstall"_s) {
    return RemovalOperation::kUninstall;
  }
  if (token == u"downgrade"_s) {
    return RemovalOperation::kDowngrade;
  }
  if (token == u"cleanup"_s) {
    return RemovalOperation::kCleanup;
  }
  return std::nullopt;
}

// Empty when the record's identity agrees with the file it is filed under: otherwise a record filed as A.json could
// speak for B and delete B's payload. A cleanup record is the mirror case: it must own no id.
QString removalIdentityRejection(RemovalOperation operation, const QString& id, const QString& stem) {
  if (operation == RemovalOperation::kCleanup) {
    if (!id.isEmpty()) {
      return u"a cleanup record must carry no extension id"_s;
    }
    if (!stem.startsWith(u"cleanup-"_s)) {
      return u"a cleanup record must be filed under a cleanup name"_s;
    }
    return {};
  }
  if (const QString id_error = invalidExtensionIdReason(id); !id_error.isEmpty()) {
    return id_error;
  }
  if (stem != id) {
    return u"record for \"%1\" is filed under the name \"%2\""_s.arg(id, stem);
  }
  return {};
}

// Parses and fully validates one record; returns the refusal reason, empty on success. Fails closed: a record only
// authorises a recursive delete, so nothing is defaulted, inferred or repaired.
QString parseRemovalIntent(const QString& store_root, const QString& file_path, RemovalIntent* out) {
  QFile record(file_path);
  if (!record.open(QIODevice::ReadOnly)) {
    return u"record could not be opened"_s;
  }
  QJsonParseError parse_error;
  const QJsonDocument document = QJsonDocument::fromJson(record.readAll(), &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    return u"record is not a JSON object"_s;
  }
  const QJsonObject object = document.object();

  const QJsonValue schema = object.value(u"schema"_s);
  if (!schema.isDouble() || schema.toInt() != kRemovalJournalSchema) {
    return u"record does not declare schema %1"_s.arg(kRemovalJournalSchema);
  }

  const std::optional<RemovalOperation> operation = parseRemovalOperationToken(object);
  if (!operation) {
    return u"record declares no known operation"_s;
  }

  RemovalIntent intent;
  intent.operation = *operation;
  intent.id = object.value(u"id"_s).toString();
  const QString stem = QFileInfo(file_path).completeBaseName();
  if (const QString identity_error = removalIdentityRejection(intent.operation, intent.id, stem);
      !identity_error.isEmpty()) {
    return identity_error;
  }

  intent.path = object.value(u"path"_s).toString();
  if (const QString path_error = unmanagedPayloadRejection(store_root, intent.path); !path_error.isEmpty()) {
    return path_error;
  }

  intent.record_path = file_path;
  *out = intent;
  return {};
}

RemovalIntent makeIdRemovalIntent(
    RemovalOperation operation, const QString& store_root, const QString& id, const QString& path) {
  RemovalIntent intent;
  intent.operation = operation;
  intent.id = id;
  intent.path = path;
  intent.record_path = removalIntentPath(store_root, id);
  return intent;
}

RemovalIntent makeCleanupIntent(const QString& store_root, const QString& path) {
  RemovalIntent intent;
  intent.operation = RemovalOperation::kCleanup;
  intent.path = path;
  intent.record_path = cleanupIntentPath(store_root);
  return intent;
}

// Commits atomically with QSaveFile (temporary file + rename): a crash mid-write leaves the previous state.
bool writeRemovalIntent(const RemovalIntent& intent) {
  if (!QDir().mkpath(QFileInfo(intent.record_path).absolutePath())) {
    return false;
  }
  QSaveFile record(intent.record_path);
  if (!record.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    return false;
  }
  QJsonObject object;
  object[u"schema"_s] = kRemovalJournalSchema;
  object[u"operation"_s] = removalOperationToken(intent.operation);
  object[u"id"_s] = intent.id;
  object[u"path"_s] = intent.path;
  const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
  if (record.write(payload) != payload.size()) {
    record.cancelWriting();
    return false;
  }
  return record.commit();
}

// Renames a refused record to `*.rejected-<uuid>`: the evidence is kept and it no longer matches the `*.json` glob, so
// it is not re-refused every launch.
bool quarantineRemovalIntent(const QString& file_path) {
  const QString rejected = u"%1.rejected-%2"_s.arg(file_path, QUuid::createUuid().toString(QUuid::Id128));
  return QFile::rename(file_path, rejected);
}

// Every VALID record in the journal. A non-null `rejections` quarantines each refused record and appends a message;
// nullptr is a pure read for callers that must not mutate the store (the disk scan runs without needing the lease).
QList<RemovalIntent> readRemovalIntents(const QString& extensions_dir, QStringList* rejections) {
  QList<RemovalIntent> intents;
  const QDir journal(removalJournalRoot(extensions_dir));
  // Hidden|System is load-bearing: an id may start with a dot (".local-plugin.json"), and without them that record
  // would never be seen, so its removal would stay pending while the payload stays loadable.
  for (const QFileInfo& entry :
       journal.entryInfoList(QStringList{u"*.json"_s}, QDir::Files | QDir::Hidden | QDir::System)) {
    const QString record_path = entry.absoluteFilePath();
    RemovalIntent intent;
    const QString rejection = parseRemovalIntent(extensions_dir, record_path, &intent);
    if (rejection.isEmpty()) {
      intents.append(intent);
      continue;
    }
    if (rejections == nullptr) {
      continue;
    }
    const bool moved = quarantineRemovalIntent(record_path);
    rejections->append(
        QString("Ignored a removal record: %1 (\"%2\")%3")
            .arg(rejection, record_path, moved ? QString() : QString(" and it could not be quarantined")));
  }
  return intents;
}

QString makeTransactionRoot(const QString& parent, const QString& id) {
  return QDir(parent).absoluteFilePath(
      QString(".pj_install_%1_%2").arg(id, QUuid::createUuid().toString(QUuid::Id128)));
}

// Where every install extracts: a SIBLING of the extensions dir, on its filesystem (the promoting rename stays atomic)
// and outside the recursive plugin scan, which has no exclusion rule: an unpacked DSO orphaned by a crash would
// otherwise be loadable content.
QString transactionStageRoot(const QString& extensions_dir) {
  return PlatformUtils::canonicalStoreRoot(extensions_dir) + u".install_stage"_s;
}

QString candidateRoot(const QString& transaction_root, const QString& id) {
  return QDir(transaction_root).absoluteFilePath(id);
}

void removeDirectoryIfSet(const QString& path) {
  if (!path.isEmpty()) {
    QDir(path).removeRecursively();
  }
}

// Retires a finished transaction and, once empty, the staging area (rmdir refuses a non-empty directory, so a
// concurrent transaction is left alone).
void removeTransactionRoot(const QString& path) {
  if (path.isEmpty()) {
    return;
  }
  const QString stage = QFileInfo(path).absolutePath();
  QDir(path).removeRecursively();
  QDir().rmdir(stage);
}

bool isTransactionDirectoryName(const QString& name) {
  return name.startsWith(".pj_install_");
}

// The single top-level directory a freshly extracted archive must contain; empty when it holds anything else.
QString soleTopLevelDirectory(const QString& transaction_root) {
  const QFileInfoList entries =
      QDir(transaction_root)
          .entryInfoList(QDir::Dirs | QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
  if (entries.size() != 1 || !entries.first().isDir()) {
    return {};
  }
  return entries.first().absoluteFilePath();
}

// Registry installs also require that directory to be named for the registry id; a sideload cannot, its id comes from
// the manifest inside.
QString validateTransactionContents(const QString& transaction_root, const QString& expected_id) {
  const QString root = soleTopLevelDirectory(transaction_root);
  if (root.isEmpty() || QFileInfo(root).fileName() != expected_id) {
    return QString("Downloaded artifact must contain exactly one top-level directory named \"%1\"").arg(expected_id);
  }
  return {};
}

DirectoryDiscovery discoverExtensionDirectory(const QString& ext_root) {
  DirectoryDiscovery result;
  const auto scan = scanPluginDsos(std::filesystem::path(ext_root.toStdU16String()));
  if (!scan) {
    result.error = QString::fromStdString(scan.error());
    return result;
  }

  for (const auto& diag : scan->diagnostics) {
    qWarning(
        "ExtensionManager: plugin discovery diagnostic for '%s': %s", pathToUtf8(diag.path).c_str(),
        diag.message.c_str());
  }

  if (scan->plugins.empty()) {
    if (!scan->diagnostics.empty()) {
      result.error = QString::fromStdString(scan->diagnostics.front().message);
      return result;
    }
    result.error = u"no valid plugin DSO found"_s;
    return result;
  }

  const PluginDescriptor& first = scan->plugins.front();
  std::string effective_min_plotjuggler_version;
  std::string effective_min_sdk_required;
  std::string effective_suggested_sdk_version;
  const auto retainHighestFloor = [](std::string_view declared, std::string& effective) {
    if (declared.empty()) {
      return;
    }
    if (effective.empty()) {
      effective = declared;
      return;
    }

    const auto candidate = SemVer::parse(declared);
    const auto incumbent = SemVer::parse(effective);
    // A malformed floor survives discovery so the extension stays visible and the shared compatibility gate rejects
    // it; no valid sibling claim can then make the extension look compatible.
    if (!candidate) {
      effective = declared;
    } else if (incumbent && candidate->compare(*incumbent) == std::strong_ordering::greater) {
      effective = declared;
    }
  };

  for (const PluginDescriptor& descriptor : scan->plugins) {
    if (descriptor.id != first.id) {
      result.error = u"multiple embedded plugin ids in one extension directory: \"%1\" and \"%2\""_s.arg(
          QString::fromStdString(first.id), QString::fromStdString(descriptor.id));
      return result;
    }
    if (descriptor.version != first.version) {
      result.error = u"multiple embedded plugin versions in one extension directory for \"%1\""_s.arg(
          QString::fromStdString(first.id));
      return result;
    }
    retainHighestFloor(descriptor.min_plotjuggler_version, effective_min_plotjuggler_version);
    retainHighestFloor(descriptor.min_sdk_required, effective_min_sdk_required);
    retainHighestFloor(descriptor.suggested_sdk_version, effective_suggested_sdk_version);
  }

  result.found_plugin = true;
  result.record.id = QString::fromStdString(first.id);
  result.record.version = QString::fromStdString(first.version);
  result.record.install_date = QFileInfo(ext_root).lastModified();
  result.record.path = ext_root;
  result.record.enabled = true;
  result.record.name = first.name.empty() ? result.record.id : QString::fromStdString(first.name);
  result.record.description = QString::fromStdString(first.description);
  result.record.category = QString::fromStdString(first.category);
  result.record.abi_major = first.abi_major;
  result.record.min_sdk_required = QString::fromStdString(effective_min_sdk_required);
  result.record.suggested_sdk_version = QString::fromStdString(effective_suggested_sdk_version);
  result.record.min_plotjuggler_version = QString::fromStdString(effective_min_plotjuggler_version);
  return result;
}

// discoverExtensionDirectory() in a throwaway child process, used for admission (bytes that just arrived: a registry
// download or a local archive). A plugin that aborts in a static initializer cannot take the application down, and
// opening the payload at its staging path would pin that image (docs/TECHNICAL_NOTES.md "dlopen pins the first
// image") and poison the check at its final path. Directories already admitted are inspected in-process.
DirectoryDiscovery discoverExtensionDirectoryOutOfProcess(const QString& ext_root) {
  DirectoryDiscovery result;
  const PluginCheckResult check = PluginCheckRunner{}.run(ext_root);

  for (const PluginCheckDiagnostic& diag : check.diagnostics) {
    qWarning(
        "ExtensionManager: plugin discovery diagnostic for '%s': %s", qUtf8Printable(diag.path),
        qUtf8Printable(diag.message));
  }

  if (!check.ok) {
    // A helper that never produced a verdict (spawn failure, crash, timeout) is
    // reported as-is: the payload is unproven, not proven bad.
    result.error = check.parent_error.isEmpty() ? u"plugin admission helper failed"_s : check.parent_error;
    return result;
  }

  if (check.plugins.isEmpty()) {
    result.error =
        check.diagnostics.isEmpty() ? u"no valid plugin DSO found"_s : check.diagnostics.constFirst().message;
    return result;
  }

  const PluginCheckDescriptor& first = check.plugins.constFirst();
  QString effective_min_plotjuggler_version;
  QString effective_min_sdk_required;
  QString effective_suggested_sdk_version;
  const auto retainHighestFloor = [](const QString& declared, QString& effective) {
    if (declared.isEmpty()) {
      return;
    }
    if (effective.isEmpty()) {
      effective = declared;
      return;
    }
    const auto candidate = SemVer::parse(declared.toStdString());
    const auto incumbent = SemVer::parse(effective.toStdString());
    // Same rule as the in-process path: a malformed floor wins so the shared gate rejects it.
    if (!candidate) {
      effective = declared;
    } else if (incumbent && candidate->compare(*incumbent) == std::strong_ordering::greater) {
      effective = declared;
    }
  };

  for (const PluginCheckDescriptor& descriptor : check.plugins) {
    if (descriptor.id != first.id) {
      result.error =
          u"multiple embedded plugin ids in one extension directory: \"%1\" and \"%2\""_s.arg(first.id, descriptor.id);
      return result;
    }
    if (descriptor.version != first.version) {
      result.error = u"multiple embedded plugin versions in one extension directory for \"%1\""_s.arg(first.id);
      return result;
    }
    retainHighestFloor(descriptor.min_plotjuggler_version, effective_min_plotjuggler_version);
    retainHighestFloor(descriptor.min_sdk_required, effective_min_sdk_required);
    retainHighestFloor(descriptor.suggested_sdk_version, effective_suggested_sdk_version);
  }

  result.found_plugin = true;
  result.record.id = first.id;
  result.record.version = first.version;
  result.record.install_date = QFileInfo(ext_root).lastModified();
  result.record.path = ext_root;
  result.record.enabled = true;
  result.record.name = first.name.isEmpty() ? first.id : first.name;
  result.record.description = first.description;
  result.record.category = first.category;
  result.record.abi_major = first.abi_major;
  result.record.min_sdk_required = effective_min_sdk_required;
  result.record.min_plotjuggler_version = effective_min_plotjuggler_version;
  result.record.suggested_sdk_version = effective_suggested_sdk_version;
  return result;
}

QString validateRegistryIntent(
    const DirectoryDiscovery& discovered, const QString& registry_id, const QString& registry_version) {
  if (!discovered.found_plugin) {
    return QString("Installed artifact is not a valid plugin: %1").arg(discovered.error);
  }
  if (discovered.record.id != registry_id) {
    return QString("Embedded plugin id \"%1\" does not match registry id \"%2\"")
        .arg(discovered.record.id, registry_id);
  }
  if (discovered.record.version != registry_version) {
    return QString("Embedded plugin version \"%1\" does not match registry version \"%2\"")
        .arg(discovered.record.version, registry_version);
  }
  return {};
}

bool writePendingInstallIntent(const QString& root, const QString& id, const QString& version, QString* error) {
  QFile file(pendingInstallIntentPath(root));
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    if (error != nullptr) {
      *error = QString("Could not write staged install intent: %1").arg(file.errorString());
    }
    return false;
  }

  const QByteArray data = id.toUtf8() + '\n' + version.toUtf8() + '\n';
  if (file.write(data) != data.size()) {
    if (error != nullptr) {
      *error = QString("Could not write staged install intent: %1").arg(file.errorString());
    }
    return false;
  }
  return true;
}

PendingInstallIntent readPendingInstallIntent(const QString& root) {
  PendingInstallIntent intent;
  QFile file(pendingInstallIntentPath(root));
  if (!file.exists()) {
    intent.error = "Staged install is missing registry intent";
    return intent;
  }
  if (!file.open(QIODevice::ReadOnly)) {
    intent.error = QString("Could not read staged install intent: %1").arg(file.errorString());
    return intent;
  }

  const QList<QByteArray> lines = file.readAll().split('\n');
  if (lines.size() < 2 || lines[0].trimmed().isEmpty() || lines[1].trimmed().isEmpty()) {
    intent.error = "Staged install registry intent is invalid";
    return intent;
  }

  const QString id = QString::fromUtf8(lines[0].trimmed());
  const QString version = QString::fromUtf8(lines[1].trimmed());
  // Defend against tampered or corrupted intent files: a path-traversal id, or a
  // version that contains anything outside the semver alphabet, must not be
  // trusted later as a directory name or version comparison input.
  if (const QString id_error = invalidExtensionIdReason(id); !id_error.isEmpty()) {
    intent.error = QString("Staged install registry intent has unsafe id: %1").arg(id_error);
    return intent;
  }
  static const QRegularExpression k_version_re(u"^[0-9A-Za-z._+-]+$"_s);
  if (!k_version_re.match(version).hasMatch()) {
    intent.error = QString("Staged install registry intent has unsafe version \"%1\"").arg(version);
    return intent;
  }

  intent.valid = true;
  intent.id = id;
  intent.version = version;
  return intent;
}

}  // namespace

ExtensionManager::ExtensionManager()
    : QObject(nullptr), extensions_dir_(PlatformUtils::extensionsDir()), pending_dir_(PlatformUtils::pendingDir()) {
  initComponents();
}

ExtensionManager::ExtensionManager(
    DownloadManager* downloader, const QString& extensions_dir, const QString& pending_dir, DiagnosticSink sink,
    QObject* parent)
    : QObject(parent),
      downloader_(downloader),
      extensions_dir_(extensions_dir),
      pending_dir_(pending_dir),
      sink_(std::move(sink)) {
  initComponents();
}

ExtensionManager::~ExtensionManager() {
  // Order is the point: cancel every operation, then release the store session (and the writer lease with it) last, so
  // a running extract worker never writes into a store the next process already owns. Member destruction alone gives
  // the wrong order: the session would die before ~QObject deletes the child downloader that drains the workers.
  // Handlers capture `this`; disconnect them before anything is torn down.
  disconnectDlConns();

  // cancelAndWait returns once the worker has stopped touching the transaction directory.
  if (pending_op_id_ != -1 && downloader_ != nullptr) {
    downloader_->cancelAndWait(pending_op_id_);
    pending_op_id_ = -1;
    pending_id_.clear();
  }

  // Retire the cancelled operation's scratch here: its completion handler is disconnected.
  if (hasStoreWriteAccess()) {
    removeTransactionRoot(pending_extract_dir_);
  }
  pending_extract_dir_.clear();

  // Deleted here, not by ~QObject: its destructor drains the remaining workers, which must finish while the lease is
  // held. A caller-supplied downloader stays the caller's.
  if (owns_downloader_) {
    delete downloader_;
    downloader_ = nullptr;
  }

  // artifact_store_ references the session, so it goes first.
  artifact_store_.reset();
  store_session_.reset();
}

bool ExtensionManager::hasStoreWriteAccess() const {
  return store_session_ != nullptr && store_session_->hasStoreWriteAccess();
}

QString ExtensionManager::storeWriteRefusal() const {
  return store_lock_refusal_;
}

void ExtensionManager::acquireStoreLock() {
  // One session owns the writer lease and everything derived from it, the content-addressed store included. Contention
  // and a lock-file failure both leave it read-only but call for opposite actions (close the other instance vs. fix
  // the config dir), so the text shown depends on the cause.
  const auto refusalText = [](const StoreRejection* refusal) -> QString {
    if (refusal != nullptr && refusal->code == StoreRejectionCode::kWriterLeaseUnavailable) {
      return u"Another PlotJuggler instance is managing extensions. Close it and restart PlotJuggler to manage "
             u"extensions here."_s;
    }
    return u"Cannot create the extensions lock file. Extensions cannot be installed, updated or removed this "
           u"session."_s;
  };
  auto session = StoreSession::open(extensions_dir_);
  if (!session) {
    store_lock_refusal_ = refusalText(&session.error());
    reportDiagnostic(
        {},
        u"%1 This session can browse installed extensions but cannot install, update or remove them."_s.arg(
            store_lock_refusal_),
        false);
    return;
  }
  store_session_ = std::make_unique<StoreSession>(std::move(*session));
  if (!store_session_->hasStoreWriteAccess()) {
    store_lock_refusal_ = refusalText(store_session_->writeRefusal());
    reportDiagnostic(
        {},
        u"%1 This session can browse installed extensions but cannot install, update or remove them."_s.arg(
            store_lock_refusal_),
        false);
    return;
  }
  store_lock_refusal_.clear();
}

void ExtensionManager::initComponents() {
  if (!downloader_) {
    // Not a child of `this`: the destructor deletes it before releasing the lease; a child would die after.
    downloader_ = new DownloadManager();
    owns_downloader_ = true;
  }
  if (!QDir().mkpath(extensions_dir_)) {
    reportDiagnostic({}, QString("Could not create extensions directory \"%1\"").arg(extensions_dir_), true);
  }
  // Canonicalize once the directory exists: the lease, the staging and journal siblings and record confinement all
  // compare against the canonical root, so a symlinked config or home would otherwise quarantine every valid uninstall.
  extensions_dir_ = PlatformUtils::canonicalStoreRoot(extensions_dir_);
  // Before any cleanup: lease ownership decides whether the drains below may delete anything.
  acquireStoreLock();
  // The content-addressed store shares the session, so the store has one lock. A read-only session still constructs
  // it; its writes no-op.
  if (store_session_ != nullptr) {
    artifact_store_ = std::make_unique<ArtifactStore>(*store_session_);
    // Which object backs each id, from the previous session (applyPendingInstalls re-adopts staged updates on top).
    loaded_digests_ = readLoadedProfile(store_session_->layout().profilesRoot());
  }
  // Drain restart-deferred work, THEN snapshot installed state: once glibc has dlopen'ed an extension path it keeps
  // returning that first image (plugin DSOs carry STB_GNU_UNIQUE symbols, so dlclose never unloads them), so no path
  // may be opened before a pending update replaces it (docs/TECHNICAL_NOTES.md "dlopen pins the first image").
  applyPendingUninstalls();
  applyPendingInstalls();
  refreshInstalledFromDisk();
  // Drop store objects no surviving install references, before the runtime maps any object.
  reconcileStore();
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void ExtensionManager::install(const Extension& ext) {
  if (const QString refusal = storeWriteRefusal(); !refusal.isEmpty()) {
    emitInstallFailure(ext.id, refusal);
    return;
  }
  refreshInstalledFromDisk();
  doInstall(ext, /*staging=*/false);
}

ExtensionManager::HostCompatibility ExtensionManager::hostCompatibility(const Extension& ext) const {
  // The platform check applies only when the record declares `platforms`: a sideloaded row carries no registry fields,
  // and its DSO was already validated on this build.
  const QString platform = PlatformUtils::currentPlatform();
  if (!ext.platforms.isEmpty() && !ext.platforms.contains(platform)) {
    return {false, tr("Not available for this platform (%1)").arg(platform), {}};
  }
  const std::string version = ext.version.toStdString();
  const std::string minimum_sdk = ext.min_sdk_required.toStdString();
  const PluginCompatibilityResult result = evaluatePluginCompatibility(
      {
          .version = version,
          .abi_major = 0,
          .min_sdk_required = minimum_sdk,
      },
      currentPluginHostCompatibility());
  return {
      result.ok,
      QString::fromStdString(result.reason),
      QString::fromStdString(evaluateFeatureCompleteness(ext.suggested_sdk_version.toStdString())),
  };
}

ExtensionManager::HostCompatibility ExtensionManager::hostCompatibility(const InstalledExtension& installed) const {
  // A sideload has no registry platform map; the ABI and SDK claims come from the embedded descriptor.
  const std::string version = installed.version.toStdString();
  const std::string minimum_sdk = installed.min_sdk_required.toStdString();
  const PluginCompatibilityResult result = evaluatePluginCompatibility(
      {
          .version = version,
          .abi_major = installed.abi_major,
          .min_sdk_required = minimum_sdk,
      },
      currentPluginHostCompatibility());
  return {
      result.ok,
      QString::fromStdString(result.reason),
      QString::fromStdString(evaluateFeatureCompleteness(installed.suggested_sdk_version.toStdString())),
  };
}

void ExtensionManager::dropReplaceConfirmation() {
  replace_confirmation_ = {};
  replace_confirmation_context_ = nullptr;
  ++replace_confirmation_generation_;
}

void ExtensionManager::setReplaceConfirmation(QObject* context, ReplaceConfirmation confirm) {
  // An incomplete registration clears instead of arming a callback without its lifetime guard.
  if (context == nullptr || !confirm) {
    dropReplaceConfirmation();
    return;
  }
  replace_confirmation_context_ = context;
  replace_confirmation_ = std::move(confirm);
  ++replace_confirmation_generation_;
}

void ExtensionManager::clearReplaceConfirmation(QObject* context) {
  if (replace_confirmation_context_ != context) {
    return;
  }
  dropReplaceConfirmation();
}

ExtensionManager::ReplaceDecision ExtensionManager::askReplaceConfirmation(
    const QString& id, const QString& archive_version) {
  if (!replace_confirmation_) {
    return ReplaceDecision::kNoConfirmation;
  }
  // The callback's captures died with its context: do not run it, and refuse the next conflict instead of asking again.
  if (replace_confirmation_context_.isNull()) {
    dropReplaceConfirmation();
    return ReplaceDecision::kOwnerGone;
  }

  // Snapshot the registration: a modal dialog spins the event loop, so another window can re-register while the
  // callback is on the stack, and assigning to the executing std::function is undefined. (context, generation) says
  // which registration asked.
  const ReplaceConfirmation confirm = replace_confirmation_;
  const QPointer<QObject> asked_context = replace_confirmation_context_;
  const quint64 asked_generation = replace_confirmation_generation_;

  const bool accepted = confirm(id, installedVersion(id), archive_version);

  // The registrant died while its question was up: decline. Drop the slot only if it still holds THIS registration.
  if (asked_context.isNull()) {
    if (replace_confirmation_generation_ == asked_generation) {
      dropReplaceConfirmation();
    }
    return ReplaceDecision::kOwnerGone;
  }
  // The slot moved on: accepting would stage a replacement the live window never approved.
  if (replace_confirmation_generation_ != asked_generation) {
    return ReplaceDecision::kSuperseded;
  }
  return accepted ? ReplaceDecision::kAccepted : ReplaceDecision::kDeclined;
}

void ExtensionManager::installFromLocalZip(const QString& zip_path) {
  // Until the manifest is read there is no id to name, so failures raised before
  // that point are reported against the file name.
  const QString file_label = QFileInfo(zip_path).fileName();

  if (const QString refusal = storeWriteRefusal(); !refusal.isEmpty()) {
    emitInstallFailure(file_label, refusal);
    return;
  }
  if (!pending_id_.isEmpty()) {
    emitInstallFailure(file_label, QString("Install of \"%1\" is already in progress").arg(pending_id_));
    return;
  }
  if (const QFileInfo info(zip_path); !info.isFile() || !info.isReadable()) {
    emitInstallFailure(file_label, QString("Cannot read \"%1\"").arg(zip_path));
    return;
  }

  // After the cheap guards: this walks the managed dir and opens each plugin. It also sweeps stale transaction
  // directories and keeps installed_ coherent for the registration below.
  refreshInstalledFromDisk();

  QDir().mkpath(extensions_dir_);
  // Extraction lands in the staging sibling (outside the scanned tree, on the destination's filesystem). Named after
  // the file: the id is not known yet.
  const QString transaction_root = makeTransactionRoot(transactionStageRoot(extensions_dir_), u"local"_s);
  QDir().mkpath(transaction_root);
  if (!sameFilesystem(transaction_root, extensions_dir_)) {
    // Refuse rather than copy across devices: promotion is a rename so that a half-installed extension is unreachable.
    removeTransactionRoot(transaction_root);
    emitInstallFailure(
        file_label,
        u"Extraction area \"%1\" is on a different filesystem than \"%2\", so the install could not be "
        u"completed atomically"_s.arg(transactionStageRoot(extensions_dir_), extensions_dir_));
    return;
  }

  pending_id_ = file_label;
  pending_extract_dir_ = QDir::cleanPath(transaction_root);

  dl_finished_conn_ = connect(downloader_, &DownloadManager::finished, this, [this, transaction_root](int id) {
    if (id != pending_op_id_) {
      return;
    }
    disconnectDlConns();
    pending_id_.clear();
    pending_op_id_ = -1;

    auto fail = [&](const QString& failed_id, const QString& message) {
      removeTransactionRoot(transaction_root);
      pending_extract_dir_.clear();
      emitInstallFailure(failed_id, message);
    };

    const QString root = soleTopLevelDirectory(transaction_root);
    if (root.isEmpty()) {
      fail({}, u"ZIP must contain exactly one top-level directory holding the plugin"_s);
      return;
    }

    // The manifest read (out of process) is the whole validation: it reports the id and version the DSO declares.
    const DirectoryDiscovery discovered = discoverExtensionDirectoryOutOfProcess(root);
    if (!discovered.found_plugin) {
      fail({}, QString("Not a valid plugin package: %1").arg(discovered.error));
      return;
    }

    const QString ext_id = discovered.record.id;
    if (const QString id_error = invalidExtensionIdReason(ext_id); !id_error.isEmpty()) {
      fail(ext_id, id_error);
      return;
    }
    // A staged uninstall already dropped the id from installed_: without this check the fresh-install branch below
    // would clear the destination and the pending removal would vanish. doInstall() refuses the same case.
    if (hasPendingUninstall(ext_id)) {
      fail(ext_id, QString("Uninstall of \"%1\" is staged; restart to apply it before installing again").arg(ext_id));
      return;
    }

    // The full ABI/SDK gate precedes both branches below, so an incompatible archive neither reaches the extensions dir
    // nor prompts for a replacement it would refuse to run.
    if (const HostCompatibility compat = hostCompatibility(discovered.record); !compat.ok) {
      fail(ext_id, compat.reason);
      return;
    }

    // A byte-identical rebuild has nothing to install: no question, no staged copy, no restart. After the compatibility
    // gate, so a refusable archive is still refused. Both trees are hashed now, not taken from a digest recorded at
    // install time, so an installed tree edited underneath us takes the ordinary path. An unreadable tree yields an
    // empty digest, never a match.
    //
    // Skipped while an install is staged for this id: the next launch promotes the stage, so matching the live bytes
    // would drop the archive and leave a different payload queued.
    //
    // Compared against the copy the scan resolved to, not whatever occupies extensions/<id>: two directories can embed
    // one id and the scan keeps the highest version, so matching the loser would discard an archive that changes what
    // runs.
    const QString install_dst = extRoot(extensions_dir_, ext_id);
    const auto installed_record = installed_.constFind(ext_id);
    const QString resolved_install =
        installed_record != installed_.constEnd() ? QFileInfo(installed_record->path).canonicalFilePath() : QString();
    // An empty canonical path (missing or unreadable) must not compare equal to anything, another empty one included.
    const bool dst_holds_the_live_copy =
        !resolved_install.isEmpty() && resolved_install == QFileInfo(install_dst).canonicalFilePath();
    if (dst_holds_the_live_copy && !hasPendingInstall(ext_id)) {
      const std::string installed_digest = contentDigestOf(install_dst);
      if (!installed_digest.empty() && installed_digest == contentDigestOf(root)) {
        removeTransactionRoot(transaction_root);
        pending_extract_dir_.clear();
        // Paired so a listening UI sees a well-formed operation.
        emit installStarted(ext_id);
        emit installUnchanged(ext_id);
        return;
      }
    }

    if (isInstalled(ext_id)) {
      switch (askReplaceConfirmation(ext_id, discovered.record.version)) {
        case ReplaceDecision::kNoConfirmation:
          fail(ext_id, QString("Extension \"%1\" is already installed").arg(ext_id));
          return;
        case ReplaceDecision::kOwnerGone:
          fail(ext_id, QString("Replacing \"%1\" was cancelled: no window is left to confirm it").arg(ext_id));
          return;
        case ReplaceDecision::kSuperseded:
          fail(
              ext_id, QString(
                          "Replacing \"%1\" was cancelled: the window that asked was replaced while the question "
                          "was open")
                          .arg(ext_id));
          return;
        case ReplaceDecision::kDeclined:
          fail(ext_id, QString("Replacing \"%1\" was cancelled").arg(ext_id));
          return;
        case ReplaceDecision::kAccepted:
          break;
      }

      emit installStarted(ext_id);

      // Staged, not written over the live directory: the session may hold this DSO and dlopen keys its cache by path,
      // so a same-path replacement would never be re-read. No version comparison: rebuilding a local archive without a
      // version bump is normal.
      QString intent_error;
      if (!writePendingInstallIntent(root, ext_id, discovered.record.version, &intent_error)) {
        fail(ext_id, intent_error);
        return;
      }
      QDir().mkpath(pending_dir_);
      const QString staged_root = pendingRoot(pending_dir_, ext_id);
      if (QDir(staged_root).exists() && !QDir(staged_root).removeRecursively()) {
        fail(ext_id, QString("Could not replace existing staged install directory \"%1\"").arg(staged_root));
        return;
      }
      if (!QDir().rename(root, staged_root)) {
        fail(ext_id, QString("Could not stage install to \"%1\"").arg(staged_root));
        return;
      }

      removeTransactionRoot(transaction_root);
      pending_extract_dir_.clear();
      emit installPendingRestart(ext_id);
      return;
    }

    emit installStarted(ext_id);

    // The managed layout keys directories by extension id, not by the name the archive's author chose.
    const QString dst = extRoot(extensions_dir_, ext_id);
    replaceConflictingInstallDirs(ext_id, dst);
    if (QDir(dst).exists() && !QDir(dst).removeRecursively()) {
      fail(ext_id, QString("Could not replace existing extension directory \"%1\"").arg(dst));
      return;
    }
    if (!QDir().rename(root, dst)) {
      fail(ext_id, QString("Could not install to \"%1\"").arg(dst));
      return;
    }

    // Re-read at the final location: rpath or relative-path assumptions can hold in the transaction directory and break
    // here. A different path, so the dlopen cache cannot serve a stale image.
    const DirectoryDiscovery final_check = discoverExtensionDirectory(dst);
    if (!final_check.found_plugin || final_check.record.id != ext_id) {
      QDir(dst).removeRecursively();
      fail(
          ext_id, QString("Post-install validation failed: %1")
                      .arg(
                          final_check.found_plugin ? QString("embedded id changed to \"%1\"").arg(final_check.record.id)
                                                   : final_check.error));
      return;
    }
    if (const HostCompatibility compat = hostCompatibility(final_check.record); !compat.ok) {
      QDir(dst).removeRecursively();
      fail(ext_id, QString("Post-install validation failed: %1").arg(compat.reason));
      return;
    }

    removeTransactionRoot(transaction_root);
    pending_extract_dir_.clear();
    registerInstalledExtension(ext_id, dst, final_check.record);
    emit installFinished(ext_id, true);
  });

  dl_failed_conn_ =
      connect(downloader_, &DownloadManager::failed, this, [this, transaction_root](int id, const QString& error) {
        if (id != pending_op_id_) {
          return;
        }
        disconnectDlConns();
        const QString failed_label = pending_id_;
        pending_id_.clear();
        pending_op_id_ = -1;
        pending_extract_dir_.clear();
        removeTransactionRoot(transaction_root);
        emitInstallFailure(failed_label, error);
      });

  // A local file goes through the fetch path; an empty expected checksum skips verification (a local archive has no
  // provenance, the manifest scan is the gate).
  pending_op_id_ = downloader_->fetch(QUrl::fromLocalFile(zip_path), /*expected_checksum=*/QString(), transaction_root);
}

void ExtensionManager::doInstall(const Extension& ext, bool staging, bool allow_existing) {
  // Backstop for every entry point, including the testing hook.
  if (const QString refusal = storeWriteRefusal(); !refusal.isEmpty()) {
    emitInstallFailure(ext.id, refusal);
    return;
  }

  if (const QString id_error = invalidExtensionIdReason(ext.id); !id_error.isEmpty()) {
    emitInstallFailure(ext.id, id_error);
    return;
  }

  if (!pending_id_.isEmpty()) {
    emitInstallFailure(ext.id, QString("Install of \"%1\" is already in progress").arg(pending_id_));
    return;
  }

  if (!allow_existing && isInstalled(ext.id)) {
    emitInstallFailure(ext.id, QString("Extension \"%1\" is already installed").arg(ext.id));
    return;
  }

  // A staged uninstall dropped the id from installed_, yet its directory is still on disk and the next launch deletes
  // whatever occupies that name: refuse until the removal has been applied.
  if (hasPendingUninstall(ext.id)) {
    emitInstallFailure(
        ext.id, QString("Uninstall of \"%1\" is staged; restart to apply it before installing again").arg(ext.id));
    return;
  }

  // Refuse an extension the host cannot run (wrong platform, or an SDK older than its floor); the reason is shown.
  if (const HostCompatibility compat = hostCompatibility(ext); !compat.ok) {
    emitInstallFailure(ext.id, compat.reason);
    return;
  }
  const QString platform = PlatformUtils::currentPlatform();
  const Platform& artifact = ext.platforms[platform];

  // Extract into a transaction directory outside every scanned root, on the destination's filesystem so the final
  // rename is atomic. The destination is pending_dir_ for a deferred update, extensions_dir_ for a fresh install. The
  // DSO is verified there before the rename and again at its final location after it.
  const QString dest_dir = staging ? pending_dir_ : extensions_dir_;
  QDir().mkpath(dest_dir);
  const QString transaction_root = makeTransactionRoot(transactionStageRoot(extensions_dir_), ext.id);
  // Created eagerly so the sweep guard (pending_extract_dir_) has a real directory to spare from the moment install()
  // returns.
  QDir().mkpath(transaction_root);
  if (!sameFilesystem(transaction_root, dest_dir)) {
    // Refuse rather than copy across devices: promotion is a rename so that a half-installed extension is unreachable.
    removeTransactionRoot(transaction_root);
    emitInstallFailure(
        ext.id,
        u"Extraction area \"%1\" is on a different filesystem than \"%2\", so the install could not be "
        u"completed atomically"_s.arg(transactionStageRoot(extensions_dir_), dest_dir));
    return;
  }

  pending_id_ = ext.id;
  pending_extract_dir_ = QDir::cleanPath(transaction_root);
  emit installStarted(ext.id);

  dl_progress_conn_ =
      connect(downloader_, &DownloadManager::progress, this, [this](int id, qint64 received, qint64 total) {
        if (id != pending_op_id_) {
          return;
        }

        if (total > 0 && !disk_space_checked_) {
          disk_space_checked_ = true;
          constexpr qint64 kExtractionOverheadFactor = 3;
          if (QStorageInfo(extensions_dir_).bytesAvailable() < total * kExtractionOverheadFactor) {
            cancel_reason_ = "Not enough disk space to install the extension";
            downloader_->cancel(pending_op_id_);
            return;
          }
        }

        const int percent = (total > 0) ? static_cast<int>(received * 100 / total) : 0;
        emit installProgress(pending_id_, percent);
      });

  dl_phase_conn_ =
      connect(downloader_, &DownloadManager::phaseChanged, this, [this](int id, DownloadManager::WorkPhase phase) {
        if (id != pending_op_id_) {
          return;
        }
        emit installPhase(pending_id_, phase);
      });

  dl_finished_conn_ =
      connect(downloader_, &DownloadManager::finished, this, [this, ext, staging, transaction_root](int id) {
        if (id != pending_op_id_) {
          return;
        }
        disconnectDlConns();
        disk_space_checked_ = false;

        const QString finished_id = pending_id_;
        pending_id_.clear();
        pending_op_id_ = -1;

        auto fail_after_extraction = [&](const QString& message) {
          removeTransactionRoot(transaction_root);
          pending_extract_dir_.clear();
          emitInstallFailure(finished_id, message);
        };

        if (const QString tx_error = validateTransactionContents(transaction_root, ext.id); !tx_error.isEmpty()) {
          fail_after_extraction(tx_error);
          return;
        }

        const QString root = candidateRoot(transaction_root, ext.id);
        const DirectoryDiscovery discovered = discoverExtensionDirectoryOutOfProcess(root);
        const QString validation_error = validateRegistryIntent(discovered, ext.id, ext.version);
        if (!validation_error.isEmpty()) {
          fail_after_extraction(validation_error);
          return;
        }
        if (const HostCompatibility compat = hostCompatibility(discovered.record); !compat.ok) {
          fail_after_extraction(compat.reason);
          return;
        }

        if (staging) {
          QString intent_error;
          if (!writePendingInstallIntent(root, ext.id, ext.version, &intent_error)) {
            fail_after_extraction(intent_error);
            return;
          }

          const QString staged_root = pendingRoot(pending_dir_, ext.id);
          if (QDir(staged_root).exists() && !QDir(staged_root).removeRecursively()) {
            fail_after_extraction(
                QString("Could not replace existing staged install directory \"%1\"").arg(staged_root));
            return;
          }
          if (!QDir().rename(root, staged_root)) {
            fail_after_extraction(QString("Could not stage install to \"%1\"").arg(staged_root));
            return;
          }

          removeTransactionRoot(transaction_root);
          pending_extract_dir_.clear();
          pending_backup_path_.clear();
          emit installPendingRestart(finished_id);
          return;
        }

        const QString dst = extRoot(extensions_dir_, ext.id);
        // Replace a prior copy of this id stored under another directory name, so "<id>" is the sole install.
        replaceConflictingInstallDirs(ext.id, dst);
        if (QDir(dst).exists() && !QDir(dst).removeRecursively()) {
          fail_after_extraction(QString("Could not replace existing extension directory \"%1\"").arg(dst));
          return;
        }
        if (!QDir().rename(root, dst)) {
          fail_after_extraction(QString("Could not promote install to \"%1\"").arg(dst));
          return;
        }

        // Re-verify at the final location: rpath or relative-path assumptions can hold in the transaction directory and
        // break here.
        const DirectoryDiscovery final_check = discoverExtensionDirectory(dst);
        const QString final_error = validateRegistryIntent(final_check, ext.id, ext.version);
        if (!final_error.isEmpty()) {
          QDir(dst).removeRecursively();
          fail_after_extraction(QString("Post-promotion validation failed: %1").arg(final_error));
          return;
        }
        if (const HostCompatibility compat = hostCompatibility(final_check.record); !compat.ok) {
          QDir(dst).removeRecursively();
          fail_after_extraction(QString("Post-promotion validation failed: %1").arg(compat.reason));
          return;
        }

        removeTransactionRoot(transaction_root);
        pending_extract_dir_.clear();
        pending_backup_path_.clear();
        registerInstalledExtension(ext.id, dst, final_check.record);
        emit installFinished(finished_id, true);
      });

  dl_failed_conn_ =
      connect(downloader_, &DownloadManager::failed, this, [this, transaction_root](int id, const QString& error) {
        if (id != pending_op_id_) {
          return;
        }
        disconnectDlConns();
        disk_space_checked_ = false;

        const QString failed_id = pending_id_;
        pending_id_.clear();
        pending_op_id_ = -1;
        pending_extract_dir_.clear();

        removeTransactionRoot(transaction_root);
        emitInstallFailure(failed_id, error);
      });

  dl_cancelled_conn_ = connect(downloader_, &DownloadManager::cancelled, this, [this, transaction_root](int id) {
    if (id != pending_op_id_) {
      return;
    }
    disconnectDlConns();

    const QString cancelled_id = pending_id_;
    pending_id_.clear();
    pending_op_id_ = -1;
    disk_space_checked_ = false;
    pending_extract_dir_.clear();

    removeTransactionRoot(transaction_root);

    const QString reason = cancel_reason_.isEmpty() ? "Installation was cancelled" : cancel_reason_;
    cancel_reason_.clear();
    emitInstallFailure(cancelled_id, reason);
  });

  pending_op_id_ = downloader_->fetch(QUrl(artifact.url), artifact.checksum, transaction_root);
}

void ExtensionManager::uninstall(const QString& extension_id) {
  if (const QString refusal = storeWriteRefusal(); !refusal.isEmpty()) {
    emitUninstallFailure(extension_id, refusal);
    return;
  }
  refreshInstalledFromDisk();

  if (!installed_.contains(extension_id)) {
    emitUninstallFailure(extension_id, QString("Extension \"%1\" is not installed").arg(extension_id));
    return;
  }

  // A core extension at its bundled version cannot be removed. One updated above it can be reverted, so the uninstall
  // is allowed and the seed restores the bundled version at the next launch. Mirrors the UI guard.
  if (isBundled(extension_id)) {
    if (comparePluginVersions(
            installedVersion(extension_id).toStdString(), bundledVersion(extension_id).toStdString()) <= 0) {
      emitUninstallFailure(
          extension_id,
          QString("Extension \"%1\" ships with the application and cannot be uninstalled").arg(extension_id));
      return;
    }
  }

  // Staged on every platform, never removed in place: deleting the directory would free its name for reuse while the
  // loaded DSO stays mapped, and dlopen resolves by path name, so a later install under that name would be answered
  // from the resident image. downgradeToBundled() stages the same way; applyPendingUninstalls() drains both.
  const QString dir_path = installed_[extension_id].path;

  // Fail-closed order. The loader skips exactly the ids on the disabled list and knows nothing of pending removals, so
  // an id left enabled would load again if the deletion at the next launch loses to a locked file or antivirus. The
  // disabled entry is therefore persisted FIRST and its sync CHECKED; a crash afterwards leaves a plugin that does not
  // load and is still installed, never a removed one that comes back. The entry stays as a tombstone after the files
  // are gone (see applyPendingUninstalls); a reinstall clears it (registerInstalledExtension).
  const bool was_enabled = isEnabled(extension_id);
  if (!writeDisabledState(extension_id, false)) {
    restoreEnabledState(extension_id, was_enabled);
    emitUninstallFailure(
        extension_id,
        QString("Could not persist the disabled state for \"%1\"; uninstall not scheduled").arg(extension_id));
    return;
  }

  if (!writeRemovalIntent(makeIdRemovalIntent(RemovalOperation::kUninstall, extensions_dir_, extension_id, dir_path))) {
    // Nothing was scheduled: restore the user's own enable/disable choice.
    restoreEnabledState(extension_id, was_enabled);
    emitUninstallFailure(
        extension_id, QString("Could not record the removal of \"%1\"; uninstall not scheduled").arg(dir_path));
    return;
  }

  // Keep the record: the extension stays on disk and running until the restart, and a UI composing rows from the
  // installed set would have nothing to show for it (see stagedUninstalls).
  staged_uninstalls_.insert(extension_id, installed_[extension_id]);
  installed_.remove(extension_id);
  emit uninstallPendingRestart(extension_id);
}

void ExtensionManager::downgradeToBundled(const QString& extension_id) {
  if (const QString refusal = storeWriteRefusal(); !refusal.isEmpty()) {
    emitUninstallFailure(extension_id, refusal);
    return;
  }
  refreshInstalledFromDisk();

  if (!installed_.contains(extension_id)) {
    emitUninstallFailure(extension_id, QString("Extension \"%1\" is not installed").arg(extension_id));
    return;
  }
  if (!isBundled(extension_id)) {
    emitUninstallFailure(
        extension_id, QString("Extension \"%1\" does not ship with the application").arg(extension_id));
    return;
  }
  if (comparePluginVersions(installedVersion(extension_id).toStdString(), bundledVersion(extension_id).toStdString()) <=
      0) {
    emitUninstallFailure(extension_id, QString("Extension \"%1\" is already at its bundled version").arg(extension_id));
    return;
  }

  // Staged, not deleted: removing now would hot-swap the loaded DSO. At the next launch applyPendingUninstalls removes
  // the updated copy and the host seed restores the bundled version.
  const QString dir_path = installed_[extension_id].path;
  // Journalled as a downgrade: it leaves the user's enable/disable choice untouched and no tombstone.
  if (!writeRemovalIntent(makeIdRemovalIntent(RemovalOperation::kDowngrade, extensions_dir_, extension_id, dir_path))) {
    emitUninstallFailure(extension_id, QString("Could not stage the downgrade of \"%1\"").arg(extension_id));
    return;
  }
  // The record stays in installed_ (the plugin is live until the next launch, so the row keeps "Installed vN" with
  // "Needs Restart") and is also filed as a staged removal: the next scan skips a journalled directory, and the row
  // must survive every rescan this session (see stagedUninstalls). The disabled entry is not touched: a downgrade is
  // a version change, not a removal.
  staged_uninstalls_.insert(extension_id, installed_[extension_id]);
  emit downgradePendingRestart(extension_id);
}

void ExtensionManager::update(const Extension& ext) {
  if (const QString refusal = storeWriteRefusal(); !refusal.isEmpty()) {
    emitInstallFailure(ext.id, refusal);
    return;
  }
  refreshInstalledFromDisk();

  // Updates are staged in pending_dir_ and promoted at the next startup by applyPendingInstalls(), which backs up the
  // old version first, so a loaded DSO is never hot-swapped. A fresh install() promotes immediately: nothing loaded
  // can clash with it.
  if (hasNewerInstalledVersion(ext)) {
    emitInstallFailure(
        ext.id, QString("Installed version \"%1\" is newer than registry version \"%2\"; downgrade is not allowed")
                    .arg(installedVersion(ext.id), ext.version));
    return;
  }

  // A staged install or removal for this id: refuse rather than re-download on top of it, or resurrect an id the user
  // just asked to remove.
  if (hasPendingInstall(ext.id) || hasPendingUninstall(ext.id)) {
    emitInstallFailure(ext.id, QString("Update for \"%1\" is already staged; restart to apply it").arg(ext.id));
    return;
  }

  doInstall(ext, /*staging=*/true, /*allow_existing=*/true);
}

void ExtensionManager::replaceConflictingInstallDirs(const QString& id, const QString& keep_dir) {
  const QString keep = QDir::cleanPath(keep_dir);
  const QString pending_clean = QDir::cleanPath(pending_dir_);
  const QDir dir(extensions_dir_);
  for (const QFileInfo& entry : dir.entryInfoList(QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
    const QString root = entry.absoluteFilePath();
    const QString clean = QDir::cleanPath(root);
    // Never touch the promotion target, the staging area, or transaction/quarantine scratch.
    if (clean == keep || clean == pending_clean) {
      continue;
    }
    const QString name = entry.fileName();
    if (isTransactionDirectoryName(name) || name.startsWith(kQuarantinePrefix)) {
      continue;
    }
    const DirectoryDiscovery item = discoverExtensionDirectory(root);
    if (!item.found_plugin || item.record.id != id) {
      continue;
    }
    // Back up the shadow copy (fall back to deleting it); either way it must leave extensions_dir_.
    QDir().mkpath(PlatformUtils::backupDir());
    const QString backup = QDir(PlatformUtils::backupDir())
                               .absoluteFilePath(id + "-replaced-" + QUuid::createUuid().toString(QUuid::Id128));
    if (QDir().rename(root, backup)) {
      qWarning(
          "ExtensionManager: replaced prior install of '%s' at '%s' (backed up to '%s')", qUtf8Printable(id),
          qUtf8Printable(root), qUtf8Printable(backup));
    } else if (QDir(root).removeRecursively()) {
      qWarning("ExtensionManager: removed prior install of '%s' at '%s'", qUtf8Printable(id), qUtf8Printable(root));
    } else {
      qWarning(
          "ExtensionManager: could not remove prior install of '%s' at '%s'", qUtf8Printable(id), qUtf8Printable(root));
    }
    // Drop the record only if it tracks the displaced copy: after a promotion installed_[id] points at keep_dir.
    const auto tracked = installed_.constFind(id);
    if (tracked != installed_.constEnd() && QDir::cleanPath(tracked->path) == clean) {
      installed_.remove(id);
    }
  }
}

void ExtensionManager::applyPendingInstalls() {
  // Promoting, quarantining and sweeping rewrite the store, and the staged directories may belong to the lease holder.
  if (!hasStoreWriteAccess()) {
    return;
  }
  const QDir pending(pending_dir_);
  if (!pending.exists()) {
    return;
  }

  // Conflict cleanup runs AFTER the whole promotion loop: it opens sibling directories to read their ids, and opening a
  // not-yet-promoted sibling would pin its pre-update image (docs/TECHNICAL_NOTES.md "dlopen pins the first image"), so
  // the rescan would report old versions and the marketplace would re-offer updates already on disk.
  QList<std::pair<QString, QString>> promoted;  // (id, promoted dir)

  for (const QFileInfo& entry :
       pending.entryInfoList(QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
    const QString staged_dir = entry.absoluteFilePath();
    const QString staged_name = entry.fileName();
    if (isTransactionDirectoryName(staged_name)) {
      // Extraction scratch is never a promotable stage.
      removeDirectoryIfSet(staged_dir);
      continue;
    }
    if (staged_name.startsWith(kQuarantinePrefix)) {
      // Left by a failed promotion; kept for manual inspection.
      continue;
    }

    auto fail_staged_install = [&](const QString& signal_id, const QString& message) {
      qWarning(
          "ExtensionManager: staged install '%s' failed validation: %s", qUtf8Printable(staged_dir),
          qUtf8Printable(message));
      QString final_message = message;
      if (!QDir(staged_dir).removeRecursively()) {
        // Removal can fail on Windows while another process locks the DSO. Move the stage aside so every startup does
        // not re-validate and re-report it.
        const QString quarantine =
            QDir(pending_dir_)
                .absoluteFilePath(
                    QString(kQuarantinePrefix) + entry.fileName() + "_" + QUuid::createUuid().toString(QUuid::Id128));
        if (QDir().rename(staged_dir, quarantine)) {
          final_message += QString(" Moved to quarantine: \"%1\".").arg(quarantine);
        } else {
          final_message += QString(" Could not remove or quarantine \"%1\" — manual cleanup required.").arg(staged_dir);
        }
      }
      emitInstallFailure(signal_id, final_message);
    };

    if (staged_name.isEmpty()) {
      fail_staged_install(staged_name, "Staged install directory has no id");
      continue;
    }

    const PendingInstallIntent intent = readPendingInstallIntent(staged_dir);
    if (!intent.valid) {
      fail_staged_install(staged_name, intent.error);
      continue;
    }
    if (intent.id != staged_name) {
      fail_staged_install(
          staged_name,
          QString("Staged install directory \"%1\" does not match registry id \"%2\"").arg(staged_name, intent.id));
      continue;
    }

    const QString dst = extRoot(extensions_dir_, intent.id);

    // Move the current directory aside first: promoting an update must not destroy the previous install. This is the
    // only backup point for updates.
    pending_backup_path_.clear();
    if (QDir(dst).exists()) {
      // Name the backup without opening the old directory: opening it would pin its image (docs/TECHNICAL_NOTES.md
      // "dlopen pins the first image"). The UUID keeps it unique; the embedded manifest records the version.
      QDir().mkpath(PlatformUtils::backupDir());
      const QString candidate = QDir(PlatformUtils::backupDir())
                                    .absoluteFilePath(intent.id + "-" + QUuid::createUuid().toString(QUuid::Id128));

      if (!QDir().rename(dst, candidate)) {
        qWarning("ExtensionManager: failed to back up '%s' before promoting staged install", qUtf8Printable(dst));
        emitInstallFailure(
            intent.id,
            QString("Could not back up \"%1\" before update — staged install left in \"%2\"").arg(dst, staged_dir));
        continue;
      }
      pending_backup_path_ = candidate;
      installed_.remove(intent.id);
    }

    if (!QDir().rename(staged_dir, dst)) {
      qWarning(
          "ExtensionManager: failed to promote staged install '%s' to '%s'", qUtf8Printable(staged_dir),
          qUtf8Printable(dst));
      QString message = QString("Could not promote staged install to \"%1\"").arg(dst);

      // Best-effort rollback so the user is not left without the extension. If it fails pending_backup_path_ stays set
      // and emitInstallFailure names the backup. The next refreshInstalledFromDisk() re-registers the restored copy.
      if (!pending_backup_path_.isEmpty() && QDir().rename(pending_backup_path_, dst)) {
        message += " Previous version restored.";
        pending_backup_path_.clear();
      }
      emitInstallFailure(intent.id, message);
      continue;
    }

    // Validate out of process: an in-process scan resolves a DSO already mapped for this id to the mapped image and
    // reports its identity instead of the bytes on disk, which would admit a payload that lies about its id or reject
    // a legitimate update (docs/TECHNICAL_NOTES.md "dlopen pins the first image").
    const DirectoryDiscovery discovered = discoverExtensionDirectoryOutOfProcess(dst);
    if (const QString validation_error = validateRegistryIntent(discovered, intent.id, intent.version);
        !validation_error.isEmpty()) {
      // Undo the promotion: drop the rejected payload, restore the previous install; the next refresh re-registers it.
      QDir(dst).removeRecursively();
      if (!pending_backup_path_.isEmpty() && QDir().rename(pending_backup_path_, dst)) {
        pending_backup_path_.clear();
      }
      emitInstallFailure(intent.id, validation_error);
      continue;
    }
    if (const HostCompatibility compat = hostCompatibility(discovered.record); !compat.ok) {
      // Same undo as above.
      QDir(dst).removeRecursively();
      if (!pending_backup_path_.isEmpty() && QDir().rename(pending_backup_path_, dst)) {
        pending_backup_path_.clear();
      }
      emitInstallFailure(intent.id, compat.reason);
      continue;
    }

    QFile::remove(pendingInstallIntentPath(dst));
    // A staged promotion is an update of an installed plugin: keep the user's disable choice.
    registerInstalledExtension(intent.id, dst, discovered.record, /*preserve_disabled_state=*/true);
    pending_backup_path_.clear();
    promoted.append({intent.id, dst});
    emit installFinished(intent.id, true);
  }

  // Replace prior copies of a promoted id stored under other directory names (e.g. a bundled plugin).
  for (const auto& [promoted_id, promoted_dir] : promoted) {
    replaceConflictingInstallDirs(promoted_id, promoted_dir);
  }
}

void ExtensionManager::applyPendingUninstalls() {
  // Deleting payloads is a store mutation: only the lease holder drains.
  if (!hasStoreWriteAccess()) {
    return;
  }

  // Markers become records BEFORE anything is deleted, so every deletion here is authorised by a validated record
  // outside the payload.
  externalizeLegacyUninstallMarkers();

  QStringList rejections;
  const QList<RemovalIntent> intents = readRemovalIntents(extensions_dir_, &rejections);
  for (const QString& rejection : rejections) {
    // A refused record is corruption or an attempt to borrow the delete privilege; surface it.
    reportDiagnostic({}, rejection, true);
  }

  for (const RemovalIntent& intent : intents) {
    // Re-confine at deletion time: parseRemovalIntent() skips the canonical check for a target that did not exist yet,
    // leaving a window in which a symlink could be planted under that name, and exists()/removeRecursively() follow it
    // out of the store. A managed payload is a real directory, so a symlink is refused and confinement is re-checked
    // against the disk now.
    const QFileInfo target(intent.path);
    if (target.isSymLink() || target.exists()) {
      const QString rejection = target.isSymLink() ? u"record path became a symlink before deletion"_s
                                                   : unmanagedPayloadRejection(extensions_dir_, intent.path);
      if (!rejection.isEmpty()) {
        quarantineRemovalIntent(intent.record_path);
        reportDiagnostic(
            intent.id, QString("Refused a removal record at deletion time: %1 (\"%2\")").arg(rejection, intent.path),
            true);
        continue;
      }
    }

    // A path already gone satisfies the intent (removeRecursively() reports false for it and would strand the record).
    const bool removed = !target.exists() || QDir(intent.path).removeRecursively();
    if (!removed) {
      // Keep the record so the next launch retries; after a partial deletion it outlives the content because it lives
      // outside it.
      reportDiagnostic(
          intent.id,
          QString("Could not remove extension directory \"%1\"; restart the application or close any process using it")
              .arg(intent.path),
          true);
      continue;
    }

    if (!intent.id.isEmpty()) {
      installed_.remove(intent.id);
    }
    // Nothing here enables anything: an uninstall's disabled entry stays as a tombstone, so a corrupt or hostile record
    // cannot make a plugin loadable and a same-id copy elsewhere cannot be resurrected by this deletion. A reinstall
    // clears it (registerInstalledExtension). Retire the record file that was parsed, never a name recomputed from the
    // id it claims.
    QFile::remove(intent.record_path);
  }
  // Goes with the last record (rmdir refuses a non-empty directory).
  QDir().rmdir(removalJournalRoot(extensions_dir_));
}

void ExtensionManager::externalizeLegacyUninstallMarkers() {
  // A `.pj_pending_uninstall` marker found inside a payload (a package can ship one) is converted into a path-only
  // journal record before the deletion, so a partial failure cannot destroy the retry. The marker's content is never
  // read: it is package-controlled, so the derived record carries no id and no authority over any extension's state.
  //
  // Skip a directory a live cleanup record already covers, or a marker whose deletion keeps failing would file a new
  // record every launch.
  QSet<QString> already_scheduled;
  for (const RemovalIntent& intent : readRemovalIntents(extensions_dir_, /*rejections=*/nullptr)) {
    if (intent.operation == RemovalOperation::kCleanup) {
      already_scheduled.insert(QDir::cleanPath(intent.path));
    }
  }

  const QDir dir(extensions_dir_);
  for (const QFileInfo& entry : dir.entryInfoList(QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
    const QString root = entry.absoluteFilePath();
    if (!QFile::exists(pendingUninstallMarkerPath(root))) {
      continue;
    }
    if (already_scheduled.contains(QDir::cleanPath(root))) {
      continue;
    }
    // Confined like any record: a directory reached through a symlink must not authorise a delete outside the store.
    if (const QString rejection = unmanagedPayloadRejection(extensions_dir_, root); !rejection.isEmpty()) {
      reportDiagnostic(entry.fileName(), QString("Ignored a legacy uninstall marker: %1").arg(rejection), true);
      continue;
    }
    if (!writeRemovalIntent(makeCleanupIntent(extensions_dir_, root))) {
      reportDiagnostic(
          entry.fileName(),
          QString("Could not record the cleanup of \"%1\"; it stays scheduled by its in-payload marker").arg(root),
          true);
    }
  }
}

bool ExtensionManager::isInstalled(const QString& id) const {
  return installed_.contains(id);
}

void ExtensionManager::setBundledVersions(const QMap<QString, QString>& id_to_version) {
  bundled_versions_ = id_to_version;
}

void ExtensionManager::setSeededVersions(const QMap<QString, QString>& id_to_version) {
  seeded_versions_ = id_to_version;
}

bool ExtensionManager::isBundled(const QString& id) const {
  // The host's bundled id -> version map decides; empty (standalone app, --plugin-dir) means nothing is core.
  return bundled_versions_.contains(id);
}

QString ExtensionManager::bundledVersion(const QString& id) const {
  return bundled_versions_.value(id);
}

bool ExtensionManager::hasPendingInstall(const QString& id) const {
  if (!invalidExtensionIdReason(id).isEmpty()) {
    return false;
  }
  const QString root = pendingRoot(pending_dir_, id);
  const PendingInstallIntent intent = readPendingInstallIntent(root);
  if (!intent.valid || intent.id != id) {
    return false;
  }
  const DirectoryDiscovery discovered = discoverExtensionDirectory(root);
  return validateRegistryIntent(discovered, intent.id, intent.version).isEmpty();
}

QMap<QString, InstalledExtension> ExtensionManager::stagedUninstalls() const {
  return staged_uninstalls_;
}

bool ExtensionManager::hasPendingUninstall(const QString& id) const {
  // The journal is keyed by id; the directory name is not necessarily the id.
  if (QFile::exists(removalIntentPath(extensions_dir_, id))) {
    return true;
  }

  // A marker inside the payload (see externalizeLegacyUninstallMarkers) counts too: its existence is a fact the badge
  // and the install guards respect; its content stays untrusted.
  const auto staged = staged_uninstalls_.constFind(id);
  const QString root = staged != staged_uninstalls_.constEnd() ? staged->path : extRoot(extensions_dir_, id);
  return QFile::exists(pendingUninstallMarkerPath(root));
}

QString ExtensionManager::installedVersion(const QString& id) const {
  const auto it = installed_.constFind(id);
  if (it == installed_.constEnd()) {
    return {};
  }
  // What the seed wrote wins, in both directions: it is the later fact about the directory and the scan cannot be
  // redone (see the header). A restore to the bundled build lands below the scanned version.
  const auto seeded = seeded_versions_.constFind(id);
  if (seeded != seeded_versions_.constEnd() && !seeded->isEmpty()) {
    return *seeded;
  }
  return it->version;
}

bool ExtensionManager::hasNewerInstalledVersion(const Extension& ext) const {
  if (!installed_.contains(ext.id)) {
    return false;
  }

  return comparePluginVersions(installedVersion(ext.id).toStdString(), ext.version.toStdString()) > 0;
}

bool ExtensionManager::hasUpdate(const Extension& ext) const {
  if (!installed_.contains(ext.id)) {
    return false;
  }

  return comparePluginVersions(ext.version.toStdString(), installedVersion(ext.id).toStdString()) > 0;
}

QMap<QString, InstalledExtension> ExtensionManager::installedExtensions() const {
  return installed_;
}

QList<ExtensionDiagnostic> ExtensionManager::diagnostics() const {
  return diagnostics_;
}

void ExtensionManager::clearDiagnostics() {
  diagnostics_.clear();
  diagnostics_surfaced_ = diagnostics_recorded_;
}

bool ExtensionManager::hasUnsurfacedDiagnostics() const {
  return !diagnostics_.isEmpty() && diagnostics_recorded_ > diagnostics_surfaced_;
}

void ExtensionManager::markDiagnosticsSurfaced() {
  diagnostics_surfaced_ = diagnostics_recorded_;
}

void ExtensionManager::disconnectDlConns() {
  disconnect(dl_progress_conn_);
  disconnect(dl_phase_conn_);
  disconnect(dl_finished_conn_);
  disconnect(dl_failed_conn_);
  disconnect(dl_cancelled_conn_);
}

void ExtensionManager::reportDiagnostic(const QString& id, const QString& message, bool is_error) {
  diagnostics_.append(ExtensionDiagnostic{id, message, is_error, QDateTime::currentDateTimeUtc()});
  ++diagnostics_recorded_;
  while (diagnostics_.size() > kMaxDiagnostics) {
    diagnostics_.removeFirst();
  }
  emit diagnosticReported(id, message, is_error);
  if (sink_) {
    sink_(
        Diagnostic{
            is_error ? DiagnosticLevel::kError : DiagnosticLevel::kInfo,
            "ExtensionManager",
            id.toStdString(),
            message.toStdString(),
            std::chrono::system_clock::now(),
        });
  }
}

void ExtensionManager::emitInstallFailure(const QString& id, const QString& message) {
  QString diagnostic = message;
  if (!pending_backup_path_.isEmpty()) {
    diagnostic += QString(" Previous version remains in backup: \"%1\".").arg(pending_backup_path_);
  }
  pending_backup_path_.clear();
  reportDiagnostic(id, diagnostic, true);
  emit installError(id, diagnostic);
  emit installFinished(id, false);
}

void ExtensionManager::emitUninstallFailure(const QString& id, const QString& message) {
  reportDiagnostic(id, message, true);
  emit uninstallError(id, message);
  emit uninstallFinished(id, false);
}

void ExtensionManager::registerInstalledExtension(
    const QString& id, const QString& dst, InstalledExtension record, bool preserve_disabled_state) {
  record.path = dst;
  record.install_date = QFileInfo(dst).lastModified();
  // Mirror into the content-addressed store after the visible install is final; best-effort, never gates the result.
  shadowAdoptIntoStore(id, dst);

  if (preserve_disabled_state) {
    // A staged update keeps the user's enable/disable choice; mirror the persisted entry into the record.
    record.enabled = !disabledExtensionIds().contains(id);
    installed_[id] = record;
    return;
  }

  // A fresh install starts enabled: clear any stale disabled entry (e.g. a tombstone from an earlier uninstall).
  record.enabled = true;
  installed_[id] = record;
  setEnabled(id, true);
}

void ExtensionManager::shadowAdoptIntoStore(const QString& id, const QString& dir) {
  if (artifact_store_ == nullptr || !artifact_store_->hasStoreWriteAccess()) {
    return;
  }
  // adoptDirectory is write-once: an identical resident tree is a no-op. A rejected tree (symlink, quota, I/O fault)
  // leaves the visible install, which is what loads, untouched.
  const auto adopted = artifact_store_->adoptDirectory(dir);
  if (!adopted) {
    // The store object no longer matches the installed bytes: drop the mapping so the runtime loads extensions/<id>/.
    reportDiagnostic(
        id,
        QString("Content-addressed shadow skipped for \"%1\": %2")
            .arg(id, QString::fromStdString(adopted.error().message)),
        false);
    if (loaded_digests_.remove(id) > 0) {
      persistLoadedProfile();
    }
    return;
  }
  loaded_digests_[id] = QString::fromStdString(adopted->artifact_digest);
  persistLoadedProfile();
}

void ExtensionManager::persistLoadedProfile() {
  if (store_session_ == nullptr || !store_session_->hasStoreWriteAccess()) {
    return;
  }
  if (!writeLoadedProfile(store_session_->layout().profilesRoot(), loaded_digests_)) {
    reportDiagnostic({}, u"Could not persist the content-addressed load profile"_s, false);
  }
}

void ExtensionManager::dropStoreMapping(const QString& id) {
  if (loaded_digests_.remove(id) > 0) {
    persistLoadedProfile();
  }
}

std::vector<QString> ExtensionManager::managedPluginDirs() {
  std::vector<QString> dirs;
  // Enumerate from disk, not the cached set: the bundled seed writes extensions/<id>/ after that snapshot. The
  // subdirectory name is the id.
  for (const QFileInfo& entry : QDir(extensions_dir_).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    // Prefer the store object when it backs this id and verifies against its recorded manifest; otherwise the classic
    // directory.
    if (artifact_store_ != nullptr) {
      const auto digest_it = loaded_digests_.constFind(entry.fileName());
      if (digest_it != loaded_digests_.constEnd()) {
        if (const auto digest = GenerationDigest::parse(digest_it.value().toStdString())) {
          const QString object_dir = artifact_store_->artifactRootFor(*digest);
          // A rehash, not an existence check: an object corrupted after commit must not load.
          if (artifact_store_->verifyArtifact(*digest)) {
            dirs.push_back(object_dir);
            continue;
          }
          if (QDir(object_dir).exists()) {
            // Present but corrupt (half-written commit, disk fault): never load it, and say why. It stays unusable
            // until reinstall.
            reportDiagnostic(
                entry.fileName(),
                QStringLiteral(
                    "Content-addressed object for \"%1\" failed its integrity check; "
                    "loading the local copy instead. Reinstall to regenerate it.")
                    .arg(entry.fileName()),
                true);
          }
        }
      }
    }
    dirs.push_back(entry.absoluteFilePath());
  }
  return dirs;
}

void ExtensionManager::reconcileStore() {
  if (artifact_store_ == nullptr || !artifact_store_->hasStoreWriteAccess()) {
    return;
  }
  // Drop profile entries whose extensions/<id>/ is gone (an uninstall drained at startup).
  bool profile_changed = false;
  for (const QString& id : loaded_digests_.keys()) {
    if (!QDir(extRoot(extensions_dir_, id)).exists()) {
      loaded_digests_.remove(id);
      profile_changed = true;
    }
  }
  if (profile_changed) {
    persistLoadedProfile();
  }
  // An empty profile alongside live installs means the profile failed to load: sweeping against an empty root set
  // would erase the whole store.
  if (loaded_digests_.isEmpty() && !installed_.isEmpty()) {
    return;
  }
  std::vector<GenerationDigest> live;
  live.reserve(loaded_digests_.size());
  for (const QString& digest_text : loaded_digests_) {
    if (const auto digest = GenerationDigest::parse(digest_text.toStdString())) {
      live.push_back(*digest);
    }
  }
  artifact_store_->collectUnreferenced(live);
}

void ExtensionManager::sweepTransactionRoots(const QString& parent) {
  const QDir dir(parent);
  if (!dir.exists()) {
    return;
  }
  for (const QFileInfo& entry : dir.entryInfoList(QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
    if (!isTransactionDirectoryName(entry.fileName())) {
      continue;
    }
    // Spare the transaction of an install in progress: its worker is writing into it and its completion handler owns
    // the cleanup. Both sides are normalized (trailing slashes, `..`, case-insensitive filesystems).
    if (QDir::cleanPath(entry.absoluteFilePath()) != pending_extract_dir_) {
      removeDirectoryIfSet(entry.absoluteFilePath());
    }
  }
}

void ExtensionManager::refreshInstalledFromDisk() {
  // Directories a journalled removal has claimed are still on disk (and loaded) until the next launch drains them, but
  // must not read as installs. Both projections of the journal come from one pass: paths gate the scan, ids retire
  // staged records below.
  QSet<QString> removal_paths;
  QSet<QString> journalled_ids;
  for (const RemovalIntent& intent : readRemovalIntents(extensions_dir_, /*rejections=*/nullptr)) {
    removal_paths.insert(QDir::cleanPath(intent.path));
    journalled_ids.insert(intent.id);
  }

  QMap<QString, InstalledExtension> discovered;
  const QDir dir(extensions_dir_);
  for (const QFileInfo& entry : dir.entryInfoList(QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
    const QString root = entry.absoluteFilePath();
    if (isTransactionDirectoryName(entry.fileName())) {
      continue;  // extraction scratch, never an install — swept after the loop
    }
    if (removal_paths.contains(QDir::cleanPath(root))) {
      continue;
    }
    if (QFile::exists(pendingUninstallMarkerPath(root))) {
      continue;  // an older build's in-payload marker; existence only, never content
    }

    const DirectoryDiscovery item = discoverExtensionDirectory(root);
    if (!item.found_plugin) {
      qWarning(
          "ExtensionManager: ignoring extension directory '%s': %s", qUtf8Printable(root), qUtf8Printable(item.error));
      continue;
    }
    if (discovered.contains(item.record.id)) {
      // Two directories embed one id: keep the HIGHEST version, not whichever the scan hit first.
      if (comparePluginVersions(item.record.version.toStdString(), discovered[item.record.id].version.toStdString()) <=
          0) {
        qWarning(
            "ExtensionManager: duplicate embedded id '%s' in '%s'; keeping higher version already found",
            qUtf8Printable(item.record.id), qUtf8Printable(root));
        continue;
      }
      qWarning(
          "ExtensionManager: duplicate embedded id '%s'; '%s' supersedes the lower-version copy",
          qUtf8Printable(item.record.id), qUtf8Printable(root));
    }
    discovered[item.record.id] = item.record;
  }
  installed_ = std::move(discovered);

  // Clear scratch a crashed or abandoned install left behind: the staging sibling (removed with its last transaction;
  // rmdir refuses while one is in flight) and any `.pj_install_*` inside the scanned tree, which the recursive plugin
  // scan would find. Writer-only: pending_extract_dir_ recognises only this instance's own transaction, so a read-only
  // session would delete the lock holder's live extraction.
  if (hasStoreWriteAccess()) {
    const QString stage_root = transactionStageRoot(extensions_dir_);
    sweepTransactionRoots(stage_root);
    QDir().rmdir(stage_root);
    sweepTransactionRoots(extensions_dir_);
  }

  // Retire a staged record once nothing schedules its removal. Uses the ids collected above, not hasPendingUninstall(),
  // which would read this map while removeIf erases from it.
  staged_uninstalls_.removeIf([&](const auto& entry) {
    return !journalled_ids.contains(entry.key()) && !QFile::exists(pendingUninstallMarkerPath(entry.value().path));
  });

  // Mirror the persisted enable state into the records so the UI need not read QSettings.
  const QStringList disabled = disabledExtensionIds();
  for (auto it = installed_.begin(); it != installed_.end(); ++it) {
    it.value().enabled = !disabled.contains(it.key());
  }
}

QStringList ExtensionManager::disabledExtensionIds() {
  return QSettings().value(QLatin1String(kDisabledExtensionsKey)).toStringList();
}

bool ExtensionManager::isEnabled(const QString& id) const {
  return !disabledExtensionIds().contains(id);
}

bool ExtensionManager::writeDisabledState(const QString& id, bool enabled) {
  // One QSettings instance does the read, write, flush and verdict: status() belongs to the object that wrote (on
  // Windows a registry failure surfaces on that instance's flush), so checking through another instance would swallow
  // a failure.
  QSettings settings;
  QStringList disabled = settings.value(QLatin1String(kDisabledExtensionsKey)).toStringList();
  const bool currently_disabled = disabled.contains(id);
  if (enabled == !currently_disabled) {
    return true;  // already in the requested state; nothing to persist
  }
  if (enabled) {
    disabled.removeAll(id);
  } else {
    disabled.append(id);
  }
  settings.setValue(QLatin1String(kDisabledExtensionsKey), disabled);
  settings.sync();
  if (settings.status() != QSettings::NoError) {
    return false;
  }
  // Keep the record in sync so the UI updates without a rescan; load/unload happens at the next launch.
  if (installed_.contains(id)) {
    installed_[id].enabled = enabled;
  }
  return true;
}

void ExtensionManager::restoreEnabledState(const QString& id, bool enabled) {
  if (!writeDisabledState(id, enabled)) {
    // Say so: otherwise the user is left guessing why a plugin whose uninstall failed also stopped loading.
    reportDiagnostic(
        id, QString("Could not restore the enabled state of \"%1\"; it may load differently next launch").arg(id),
        true);
  }
}

void ExtensionManager::setEnabled(const QString& id, bool enabled) {
  // The disabled list is one global QSettings key: a read-modify-write from a second instance would discard edits.
  if (const QString refusal = storeWriteRefusal(); !refusal.isEmpty()) {
    reportDiagnostic(id, refusal, true);
    return;
  }
  if (!writeDisabledState(id, enabled)) {
    reportDiagnostic(id, QString("Could not persist the enabled state of \"%1\"").arg(id), true);
  }
}

void ExtensionManager::setInstalledExtensions(QMap<QString, InstalledExtension> installed) {
  installed_ = std::move(installed);
}

}  // namespace PJ
