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

#include "internal/utf8_path.hpp"
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
#include "pj_marketplace/version_compare.hpp"
#include "pj_plugins/host/plugin_catalog.hpp"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

static constexpr const char* kPendingUninstallMarker = ".pj_pending_uninstall";
static constexpr const char* kPendingInstallIntent = ".pj_pending_install";
static constexpr const char* kQuarantinePrefix = ".pj_quarantine_";
static constexpr int kMaxDiagnostics = 50;
// How long construction waits for the store's single-writer lock. Long enough to
// ride out another instance's in-flight rename, short enough not to stall startup:
// a busy store means read-only, never a hang.
static constexpr int kStoreLockTimeoutMs = 200;
// QSettings key holding the QStringList of disabled extension ids (installed but
// not loaded). Read by both the marketplace and the runtime plugin catalog.
static constexpr const char* kDisabledExtensionsKey = "Marketplace/disabledExtensions";

QString extRoot(const QString& extensions_dir, const QString& id) {
  return QDir(extensions_dir).absoluteFilePath(id);
}

QString pendingRoot(const QString& pending_dir, const QString& id) {
  return QDir(pending_dir).absoluteFilePath(id);
}

// Identity of an extension tree: the hash of every file it holds, paths included.
//
// Empty when the tree cannot be characterised — it is missing, or it holds
// something the walk refuses such as a symlink. Callers must therefore treat an
// empty result as "not comparable" and never as a match, or two unreadable trees
// would be taken for the same tree.
std::string contentDigestOf(const QString& directory) {
  const auto files = inspectContentManifest(marketplace_internal::pathFromUtf8(directory.toUtf8().toStdString()));
  if (!files) {
    return {};
  }
  return aggregateContentManifestDigest(*files);
}

// Path of the single-writer lock guarding `extensions_dir`.
//
// A hidden SIBLING of the managed dir, never a file inside it: everything under
// extensions_dir is treated as extension payload, scanned and swept. The name is
// derived from the store's resolved directory so that two managers over DIFFERENT
// stores (a --plugin-dir run, a test's temp dir) never contend while two names for
// the SAME store always do: the lock identifies the store, not the application and
// not the path spelling.
// True when both paths sit on the same mounted filesystem, so a rename between
// them is an atomic move rather than an EXDEV failure.
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

// What a journal record asks this host to do. Absent or unrecognised is NOT one
// of these: a record that fails to name its operation is invalid, never assumed.
enum class RemovalOperation {
  kUninstall,  // the user removed the extension; the id keeps its disabled tombstone
  kDowngrade,  // a version revert; the user's enable/disable choice is untouched
  kCleanup,    // path-only: delete this directory, no id and no authority over one
};

// A removal this host owes the user. `record_path` is the file this was parsed
// FROM: retiring anything else would let a record name a victim and delete that
// victim's record instead of its own.
struct RemovalIntent {
  RemovalOperation operation = RemovalOperation::kCleanup;
  QString id;  // empty for kCleanup, which carries no id authority at all
  QString path;
  QString record_path;
};

// Bumped when the record shape changes. A record without it, or from a shape this
// build does not know, is rejected rather than guessed at.
constexpr int kRemovalJournalSchema = 1;

// Where removal intent is journalled: a SIBLING of the extensions dir, on the same
// discipline as the ".install_stage" and ".seed_stage" siblings.
//
// The location is the whole point. The intent used to live INSIDE the directory
// being deleted, so a recursive delete that removed the marker and then failed on
// the still-loaded DSO destroyed its own retry record: the directory survived and
// nothing remained to say it should go. A journal outside the payload cannot be
// consumed by the deletion it describes. Being a sibling also keeps it clear of
// the recursive plugin scan, so a record is never discoverable content.
//
// Derived from the CANONICAL store root, like the lease file and the staging area:
// two aliases of a symlinked store must journal into one directory, or each would
// keep its own record of the same pending removal and drain the other's blind.
//
// `store_root` is expected ALREADY canonical — ExtensionManager resolves it once at
// construction. Re-resolving here would put a realpath-class syscall behind every
// call, and hasPendingUninstall() routes through this for each catalog row on every
// marketplace table rebuild.
QString removalJournalRoot(const QString& store_root) {
  return store_root + u".state"_s;
}

// One record per extension id, named for that id. The stem is not decoration: the
// drain requires it to match the id inside, so a record cannot be filed under one
// name and speak for a different extension.
QString removalIntentPath(const QString& store_root, const QString& id) {
  return QDir(removalJournalRoot(store_root)).absoluteFilePath(id + u".json"_s);
}

// A path-only record owns no id, so it cannot be named for one.
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

// Empty when `path` is something this host may recursively delete; otherwise the
// reason it may not.
//
// A journal record is a deletion capability, so the target is bound to the managed
// store rather than trusted: it must be a DIRECT child of the canonical store root.
// That one rule covers the store root itself, any ancestor, an unrelated absolute
// path, anything reached through "..", and the sibling state/staging/seed roots and
// lease file (siblings are not children). Confinement is checked on the CANONICAL
// target as well, so a symlink planted inside the store cannot point the delete out
// of it. A record naming a path that no longer exists is not rejected here — there
// is nothing to delete and therefore no capability to abuse — but it still has to
// name a location inside the store.
QString unmanagedPayloadRejection(const QString& store_root, const QString& path) {
  if (path.isEmpty()) {
    return u"record names no path"_s;
  }
  if (QDir::isRelativePath(path)) {
    return u"record path is not absolute"_s;
  }
  // Already canonical (resolved once at construction); only the per-record TARGET
  // below still needs resolving, since that is what a record controls.
  const QString root = QDir::cleanPath(store_root);
  const QString target = QDir::cleanPath(path);
  if (target == root) {
    return u"record targets the extensions store itself"_s;
  }
  if (QFileInfo(target).absolutePath() != root) {
    return u"record targets a path outside the extensions store"_s;
  }
  // canonicalFilePath() is empty for a path that does not exist, which is the
  // already-drained case rather than an error.
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

// Maps a record's operation token onto the enum. nullopt for absent or
// unrecognised, which the caller must treat as invalid: defaulting would mean
// picking "uninstall", the destructive reading, for a record that never said so.
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

// Empty when the record's identity agrees with the file it is filed under.
// Binding the two is what stops a record filed as A.json from speaking for
// extension B: acting on it would delete B's payload and retire a record that was
// never B's. A cleanup record is the mirror case — it must own no id at all.
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

// Parses and FULLY validates one record. Returns the reason it was refused, empty
// on success. Every check fails closed: nothing is defaulted, inferred, or
// repaired, because the only thing a record does is authorise a recursive delete.
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

// The journal file an id-bearing record is filed under is the id's own, which the
// drain then requires to match the id inside.
RemovalIntent makeIdRemovalIntent(
    RemovalOperation operation, const QString& store_root, const QString& id, const QString& path) {
  RemovalIntent intent;
  intent.operation = operation;
  intent.id = id;
  intent.path = path;
  intent.record_path = removalIntentPath(store_root, id);
  return intent;
}

// A path-only record: no id, hence no authority over any extension's state.
RemovalIntent makeCleanupIntent(const QString& store_root, const QString& path) {
  RemovalIntent intent;
  intent.operation = RemovalOperation::kCleanup;
  intent.path = path;
  intent.record_path = cleanupIntentPath(store_root);
  return intent;
}

// Commits a record atomically: QSaveFile writes to a temporary and renames, so a
// reader never sees a half-written intent and a crash mid-write leaves the
// previous state rather than a corrupt one.
bool writeRemovalIntent(const RemovalIntent& intent) {
  // The record's own location names the journal directory it belongs to.
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

// Takes a refused record out of the journal instead of deleting it: the operator
// keeps the evidence, and the glob no longer matches it so it is not re-refused on
// every launch forever.
bool quarantineRemovalIntent(const QString& file_path) {
  const QString rejected = u"%1.rejected-%2"_s.arg(file_path, QUuid::createUuid().toString(QUuid::Id128));
  return QFile::rename(file_path, rejected);
}

// Every VALID record in the journal. Invalid ones never reach the caller.
//
// `rejections` opts into enforcement: when non-null each refused record is moved
// aside and a message appended, so the drain can report and stop re-refusing it
// every launch. Passing nullptr is a pure read for callers that must not mutate
// the store (the disk scan runs constantly and may hold no lease).
QList<RemovalIntent> readRemovalIntents(const QString& extensions_dir, QStringList* rejections) {
  QList<RemovalIntent> intents;
  const QDir journal(removalJournalRoot(extensions_dir));
  // Hidden|System is load-bearing, not defensive: a record is named for its id, and
  // an id may start with a dot (".local-plugin" -> ".local-plugin.json"), so a
  // listing without them would never see that record and its removal would stay
  // pending forever while the payload stayed on disk and loadable.
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

// Where every install extracts: a SIBLING of the extensions dir, so scratch
// shares that dir's filesystem (the promoting rename stays atomic) while sitting
// outside the recursive plugin scan. A transaction directory holds an unpacked
// DSO and the scanner has no exclusion rule, so one left inside the tree by a
// crash would be discoverable, loadable content. Same discipline as the bundled
// seed's ".seed_stage" sibling.
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

// Retires a finished transaction, and the staging area with it once nothing else
// occupies it — rmdir refuses a non-empty directory, so a concurrently staged
// sibling is left alone.
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

// Absolute path of the single top-level directory a freshly extracted archive must
// contain, or an empty string when it holds anything else.
QString soleTopLevelDirectory(const QString& transaction_root) {
  const QFileInfoList entries =
      QDir(transaction_root)
          .entryInfoList(QDir::Dirs | QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
  if (entries.size() != 1 || !entries.first().isDir()) {
    return {};
  }
  return entries.first().absoluteFilePath();
}

// Registry installs additionally require that directory to be named for the id the
// registry declared; a sideload cannot check that, since the id only becomes known
// once the manifest inside that very directory has been read.
QString validateTransactionContents(const QString& transaction_root, const QString& expected_id) {
  const QString root = soleTopLevelDirectory(transaction_root);
  if (root.isEmpty() || QFileInfo(root).fileName() != expected_id) {
    return QString("Downloaded artifact must contain exactly one top-level directory named \"%1\"").arg(expected_id);
  }
  return {};
}

DirectoryDiscovery discoverExtensionDirectory(const QString& ext_root) {
  DirectoryDiscovery result;
  const auto scan = scanPluginDsos(std::filesystem::path(ext_root.toStdString()));
  if (!scan) {
    result.error = QString::fromStdString(scan.error());
    return result;
  }

  for (const auto& diag : scan->diagnostics) {
    qWarning(
        "ExtensionManager: plugin discovery diagnostic for '%s': %s", diag.path.string().c_str(), diag.message.c_str());
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
    // A malformed floor must survive discovery so the installed extension
    // remains visible/manageable and the shared compatibility gate can reject
    // it. Once one malformed claim exists, no valid sibling claim can make the
    // extension appear compatible.
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

// Same contract as discoverExtensionDirectory, but the DSOs are opened by a
// throwaway child process instead of this one. Used for admission — the first
// look at bytes that just arrived from a registry download or a local archive —
// where a plugin that aborts in a static initializer would otherwise take the
// application down, and where opening the payload at its staging path poisons the
// later provenance check for its final path.
//
// Deliberately not used for directories already admitted: those are inspected
// in-process, as before.
DirectoryDiscovery discoverExtensionDirectoryOutOfProcess(const QString& ext_root) {
  DirectoryDiscovery result;
  const PluginCheckResult check = PluginCheckRunner{}.run(ext_root);

  for (const PluginCheckDiagnostic& diag : check.diagnostics) {
    qWarning(
        "ExtensionManager: plugin discovery diagnostic for '%s': %s", qPrintable(diag.path), qPrintable(diag.message));
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
    // Mirrors the in-process rule: a malformed floor wins so the extension stays
    // visible and the shared compatibility gate is the one that rejects it.
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

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

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
  // The whole point of this body is ORDER: the store session (which holds the
  // writer lease) is released last, once nothing can write into the store any
  // more. An extract worker unpacks into a transaction directory inside the
  // store's staging area, so a lease released while one is running would let the
  // next process take the store and start its own cleanup and promotion against a
  // tree this process is still writing.
  //
  // Member destruction alone cannot give that order: the session dies before
  // ~QObject deletes the child downloader whose destructor drains the workers.

  // Signal wiring first: the completion handlers capture `this` and touch members,
  // and none of them may run against a half-destroyed manager.
  disconnectDlConns();

  // Our own operation, whichever phase it is in. cancelAndWait returns only once
  // the worker has stopped touching the transaction directory.
  if (pending_op_id_ != -1 && downloader_ != nullptr) {
    downloader_->cancelAndWait(pending_op_id_);
    pending_op_id_ = -1;
    pending_id_.clear();
  }

  // Retire the scratch of the operation just cancelled instead of leaving it for
  // the next writer to sweep: this process knows the directory is its own, and the
  // handler that would normally clean it up was disconnected above.
  if (hasStoreWriteAccess()) {
    removeTransactionRoot(pending_extract_dir_);
  }
  pending_extract_dir_.clear();

  // An owned downloader is destroyed HERE rather than by ~QObject after this body:
  // its destructor cancels and drains every worker it still has, and that has to
  // finish while the lock is still held. A caller-supplied downloader is not ours
  // to touch beyond the operation cancelled above.
  if (owns_downloader_) {
    delete downloader_;
    downloader_ = nullptr;
  }

  // The store holds a reference into the session, so it must go first; the session
  // — and with it the lease — is released last.
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
  // One session owns the store's single-writer lease and everything derived from
  // it — the same lease the content-addressed store commits under, the same PID
  // liveness that makes the startup cleanup safe (a drain only ever meets this
  // process's transactions or a dead one's). Opening always yields a session;
  // contention or a lock-file failure yields a read-only one whose refusal the
  // user is told about once, here.
  // Turn a lease refusal into the user's terms. Contention and a filesystem
  // failure both leave the session read-only but call for opposite actions: one
  // means another PlotJuggler is running (restart once it closes), the other means
  // this config dir cannot hold a lock file at all — and telling that user to
  // close another instance sends them after a process that does not exist.
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
    // Not parented to `this`: the destructor deletes it explicitly, before the lock
    // is released, and a QObject child would instead be destroyed after that.
    downloader_ = new DownloadManager();
    owns_downloader_ = true;
  }
  if (!QDir().mkpath(extensions_dir_)) {
    reportDiagnostic({}, QString("Could not create extensions directory \"%1\"").arg(extensions_dir_), true);
  }
  // Resolve the store path to its canonical form, ONCE, now that the directory
  // exists. Everything derived from the store — the lease file, the staging and
  // journal siblings — already goes through canonicalStoreRoot(), and record
  // validation compares a record's parent against that same canonical root. If
  // extensions_dir_ itself kept a symlink component (a symlinked config or home is
  // ordinary), a record's lexical parent would never equal the canonical root and
  // every valid uninstall would be quarantined as if it escaped the store. Pinning
  // the canonical form here puts both sides of that comparison on one footing.
  // canonicalStoreRoot() falls back to a cleaned path when the dir does not exist,
  // covering the first-run case where mkpath just failed.
  extensions_dir_ = PlatformUtils::canonicalStoreRoot(extensions_dir_);
  // Before any cleanup: whether this instance owns the store decides whether the
  // drains below may delete anything at all.
  acquireStoreLock();
  // The content-addressed shadow store rides on the same session as the writer
  // lease acquired above, so there is exactly one lock on the store. Its objects
  // live under extensions.artifacts/, a sibling of extensions_dir_, so nothing
  // here scans them; a read-only session still constructs it, and its writes just
  // no-op through hasStoreWriteAccess().
  if (store_session_ != nullptr) {
    artifact_store_ = std::make_unique<ArtifactStore>(*store_session_);
    // Recover which store object backs each id from the prior session, so a
    // launch that installs nothing still hands the runtime its store load path.
    // applyPendingInstalls (below) re-adopts every staged update on top of this.
    loaded_digests_ = readLoadedProfile(store_session_->layout().profilesRoot());
  }
  // Drain restart-deferred work, THEN snapshot installed state. The order is
  // load-bearing because of a glibc dlopen quirk: once an extension's .so has
  // been opened in this process, dlopen keeps returning that first-loaded image
  // for the same path name (plugin DSOs carry STB_GNU_UNIQUE symbols, so dlclose
  // never unloads them). So an extension path must NOT be opened before a pending
  // update replaces it — otherwise every later scan (including the marketplace
  // window's showEvent refresh) reads the stale old version until the next
  // restart. applyPendingInstalls promotes WITHOUT opening the old directory (it
  // names the backup without inspecting it), and refreshInstalledFromDisk runs
  // AFTER promotion, so the path's first in-process open is the new version.
  applyPendingUninstalls();
  applyPendingInstalls();
  refreshInstalledFromDisk();
  // With the installed set now settled, drop store objects that no surviving
  // install references. Runs here, before the runtime maps any object.
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
  // Platform: only enforced when the extension declares a `platforms` map at
  // all. A locally-sideloaded row (installed via installFromLocalZip, then
  // synthesized by MarketplaceWindow::rebuildExtensionList for the marketplace
  // list) carries no registry-sourced fields — `platforms` is empty by design.
  // Treating that as incompatible would mislabel a working local install as
  // "Not available for this platform"; the caller-side sideloader has already
  // validated the DSO loads on this build, so the check is meaningful only for
  // registry entries with declared platforms.
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
  // There is no registry platform map for a sideload. ABI, SDK contract, and
  // application release claims all come from the embedded descriptor and are
  // evaluated by the same primitive the runtime loader and seed use.
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
  // A callback with no guard is exactly the hazard this signature exists to
  // prevent, so an incomplete registration clears instead of half-arming.
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
  // Whatever the callback captured died with its context, so it must not run.
  // Dropping the registration puts the next conflict on the refuse path instead
  // of asking again through the same dead pointer.
  if (replace_confirmation_context_.isNull()) {
    dropReplaceConfirmation();
    return ReplaceDecision::kOwnerGone;
  }

  // Snapshot the whole registration before asking. The answer usually comes from a
  // modal dialog, which spins the event loop, so another window can register or
  // clear WHILE the callback is on the stack: invoking the member directly would
  // let that assignment destroy the std::function currently executing. The copied
  // callable is independent of the slot, and (owner, generation) records which
  // registration asked.
  const ReplaceConfirmation confirm = replace_confirmation_;
  const QPointer<QObject> asked_context = replace_confirmation_context_;
  const quint64 asked_generation = replace_confirmation_generation_;

  const bool accepted = confirm(id, installedVersion(id), archive_version);

  // The registrant died while its question was up. Its answer belongs to a window
  // the user no longer has, so decline rather than stage a replacement nobody is
  // watching for. Only drop the slot when it still holds THIS registration —
  // clearing a successor's would disarm a live window.
  if (asked_context.isNull()) {
    if (replace_confirmation_generation_ == asked_generation) {
      dropReplaceConfirmation();
    }
    return ReplaceDecision::kOwnerGone;
  }
  // The slot moved on: this answer came from a registration that has since been
  // displaced or cleared. Accepting it because the CURRENT registrant happens to
  // be alive would stage a replacement the live window never approved.
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

  // After the cheap guards: this walks the governed dir and dlopens each plugin, so
  // a rejected click should not pay for it. Kept because it also sweeps stale
  // transaction dirs and keeps installed_ coherent for the registration below; the
  // conflict check no longer depends on it.
  refreshInstalledFromDisk();

  QDir().mkpath(extensions_dir_);
  // Extraction lands in the staging sibling: outside the scanned tree, yet on the
  // filesystem of the final destination so the promoting rename is an atomic
  // move. The transaction directory is named after the file rather than the
  // extension id, which is still unknown at this point.
  const QString transaction_root = makeTransactionRoot(transactionStageRoot(extensions_dir_), u"local"_s);
  QDir().mkpath(transaction_root);
  if (!sameFilesystem(transaction_root, extensions_dir_)) {
    // Refuse rather than fall back to a cross-device copy: promotion is a rename
    // precisely so that a half-installed extension is unreachable, and a copy
    // would trade that guarantee away silently.
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

    // The manifest read here is the whole validation: it dlopens the DSO at its
    // transaction path and reports the id/version it declares about itself.
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
    // Asked of the DESTINATION directory, not of installed_: that snapshot is
    // scoped to the governed dir, which under a --plugin-dir override is not
    // where this install lands. Reading it here is safe even though the comment
    // in applyPendingInstalls warns against opening a directory about to be
    // replaced — a confirmed replacement is staged, so the promotion happens in
    // the next process, where this dlopen cannot serve a stale image.
    //
    // A staged uninstall is checked first and separately: it already dropped the
    // id from installed_, so isInstalled() below would send this down the
    // fresh-install branch, which clears the destination — marker included — and
    // would make the pending removal vanish without a word. doInstall() refuses
    // the same case; this path does not route through it.
    if (hasPendingUninstall(ext_id)) {
      fail(ext_id, QString("Uninstall of \"%1\" is staged; restart to apply it before installing again").arg(ext_id));
      return;
    }

    // Apply the complete embedded ABI/SDK/application compatibility gate. It
    // must precede BOTH branches below —
    // the fresh install and the staged replacement — so an incompatible archive
    // never reaches the extensions dir and never prompts for a replacement it
    // would then refuse to run.
    if (const HostCompatibility compat = hostCompatibility(discovered.record); !compat.ok) {
      fail(ext_id, compat.reason);
      return;
    }

    // A rebuild that produced the very same bytes has nothing to install, so it is
    // answered here rather than travelling the replacement path: no question to the
    // user, no staged copy, and no restart to promote a payload identical to the one
    // already running. Placed after the compatibility gate so an archive that would
    // be refused is still refused, whatever it contains.
    //
    // Both sides are hashed now instead of trusting a digest recorded at install
    // time: an installed tree edited underneath us then compares different and takes
    // the ordinary path, repairing itself. An unreadable tree yields an empty digest,
    // which must never count as a match.
    //
    // Skipped while an install is staged for this id, because the stage — not the
    // live directory — is what the next launch promotes. Matching against the live
    // bytes there would drop the archive while leaving a different payload queued.
    //
    // The comparison targets the copy the scan RESOLVED to, never simply whatever
    // occupies extensions/<id>. Two directories may embed one id, and the scan
    // deliberately keeps the highest version, which can live under any name; the
    // loser is bytes on disk that nothing loads. Comparing against it would discard
    // an archive that does change which plugin runs, and would strand the duplicate,
    // since promoting is what sweeps it.
    const QString install_dst = extRoot(extensions_dir_, ext_id);
    const auto installed_record = installed_.constFind(ext_id);
    const QString resolved_install =
        installed_record != installed_.constEnd() ? QFileInfo(installed_record->path).canonicalFilePath() : QString();
    // An empty canonical path means the directory is gone or unreadable, and must
    // not compare equal to anything — including another empty one.
    const bool dst_holds_the_live_copy =
        !resolved_install.isEmpty() && resolved_install == QFileInfo(install_dst).canonicalFilePath();
    if (dst_holds_the_live_copy && !hasPendingInstall(ext_id)) {
      const std::string installed_digest = contentDigestOf(install_dst);
      if (!installed_digest.empty() && installed_digest == contentDigestOf(root)) {
        removeTransactionRoot(transaction_root);
        pending_extract_dir_.clear();
        // Paired with the outcome below so a listening UI sees a well-formed
        // operation rather than a completion for something it never saw begin.
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

      // Staged rather than written over the live directory: the running session
      // may hold this DSO loaded, and dlopen keys its cache by path name, so a
      // same-path replacement would never be re-read. applyPendingInstalls()
      // promotes the stage at the next launch and backs up the displaced copy.
      // No version comparison: rebuilding without bumping the version is the
      // normal case for a local archive.
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

    // The managed layout keys directories by extension id, whereas the archive
    // used whatever name its author chose.
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

    // Re-read at the final location: rpath or relative-path assumptions can hold
    // inside the transaction directory and break here. This is a different path
    // from the pre-rename read, so the dlopen path-name cache cannot serve a
    // stale image of it.
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

  // A local file is served through the same fetch path as a remote artifact: an
  // empty expected checksum skips verification, which is correct here because a
  // local archive has nothing to be verified against.
  pending_op_id_ = downloader_->fetch(QUrl::fromLocalFile(zip_path), /*expected_checksum=*/QString(), transaction_root);
}

void ExtensionManager::doInstall(const Extension& ext, bool staging, bool allow_existing) {
  // Backstop for every entry point into the install machinery, including the
  // testing hook that bypasses install()/update().
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

  // A staged uninstall drops the id from installed_, so isInstalled() above no
  // longer speaks for it — yet its directory is still on disk carrying the
  // marker, and applyPendingUninstalls() will delete whatever occupies that name
  // at the next launch. Installing into it now would be erased on startup, so
  // refuse until the pending removal has actually been applied. update() already
  // makes the same check for its own reasons.
  if (hasPendingUninstall(ext.id)) {
    emitInstallFailure(
        ext.id, QString("Uninstall of \"%1\" is staged; restart to apply it before installing again").arg(ext.id));
    return;
  }

  // Refuse an extension the host can't run: wrong platform or a host older than
  // the plugin's declared minimum (R1). The reason is surfaced to the user.
  if (const HostCompatibility compat = hostCompatibility(ext); !compat.ok) {
    emitInstallFailure(ext.id, compat.reason);
    return;
  }
  const QString platform = PlatformUtils::currentPlatform();
  const Platform& artifact = ext.platforms[platform];

  // Extraction goes into a hidden transaction directory outside every scanned
  // root, on the filesystem the final destination lives on, so the eventual
  // rename is atomic. That destination is pending_dir_ for a deferred update
  // (promoted at next startup) and extensions_dir_ for an immediate fresh
  // install.
  // The DSO is dlopened and its embedded manifest verified inside the
  // transaction directory BEFORE the rename, then re-verified at the final
  // location AFTER the rename — see the post-promotion check below.
  const QString dest_dir = staging ? pending_dir_ : extensions_dir_;
  QDir().mkpath(dest_dir);
  const QString transaction_root = makeTransactionRoot(transactionStageRoot(extensions_dir_), ext.id);
  // Create the transaction root eagerly so the refresh guard has a real path
  // to protect from the moment install() returns — otherwise a Refresh that
  // fires during the download window would find no directory to skip and the
  // guard would appear untested even though it is exercised in practice.
  QDir().mkpath(transaction_root);
  if (!sameFilesystem(transaction_root, dest_dir)) {
    // Refuse rather than fall back to a cross-device copy: promotion is a rename
    // precisely so that a half-installed extension is unreachable, and a copy
    // would trade that guarantee away silently.
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
        // Replace any prior copy of this id stored under a different directory
        // name so we don't leave a duplicate alongside the promoted "<id>" dir.
        replaceConflictingInstallDirs(ext.id, dst);
        if (QDir(dst).exists() && !QDir(dst).removeRecursively()) {
          fail_after_extraction(QString("Could not replace existing extension directory \"%1\"").arg(dst));
          return;
        }
        if (!QDir().rename(root, dst)) {
          fail_after_extraction(QString("Could not promote install to \"%1\"").arg(dst));
          return;
        }

        // Double-check: the DSO loaded from the staging area; confirm it still
        // loads from its final location. Catches issues like rpath/relative-path
        // assumptions that hold in pending_dir_ but break in extensions_dir_.
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

  // A core extension AT its bundled version cannot be removed — it ships with the
  // application. One updated ABOVE its bundled version can be reverted (the UI's
  // "downgrade to bundled"): allow the uninstall here, and the seed restores the
  // bundled version on the next launch. This is the backend guard mirroring the UI.
  if (isBundled(extension_id)) {
    if (comparePluginVersions(
            installedVersion(extension_id).toStdString(), bundledVersion(extension_id).toStdString()) <= 0) {
      emitUninstallFailure(
          extension_id,
          QString("Extension \"%1\" ships with the application and cannot be uninstalled").arg(extension_id));
      return;
    }
  }

  // Staged on every platform, never removed in place. Deleting the directory
  // here would free its NAME for reuse while the DSO the session loaded stays
  // mapped, and dlopen resolves by path name: a later install into that same
  // name is answered from the resident image instead of the payload on disk.
  // Deferring keeps a directory name from ever being reused inside one process,
  // which is what makes that whole class of stale reads unreachable rather than
  // merely guarded against. It also stops the UI overstating what happened —
  // the extension keeps running until the restart either way, so "removed on
  // the next launch" is the honest report. downgradeToBundled() stages for the
  // same reason; applyPendingUninstalls() drains both at startup.
  const QString dir_path = installed_[extension_id].path;

  // Order matters, and it is ordered to fail closed.
  //
  // "Removed" has to outlive a deletion that never happens: the directory goes at
  // the next launch, and that removal can lose to a locked file, antivirus, or a
  // permission the user no longer has. The loader skips exactly the ids on the
  // disabled list and knows nothing about pending removals, so an id left enabled
  // would simply load again from the directory that survived.
  //
  // The disabled entry is therefore persisted and synced FIRST, before anything
  // else is recorded, and the sync is CHECKED — an unwritten setting is not
  // persistence, and proceeding on the assumption that it was would stage a removal
  // whose safety half never reached the disk. A crash in the window that follows
  // leaves a plugin that does not load and is still installed: recoverable, and
  // never the reverse (a plugin the user removed quietly coming back).
  //
  // The entry then STAYS as a tombstone, even once the files are gone (see
  // applyPendingUninstalls), so nothing in journal processing ever needs the power
  // to enable an id. A later reinstall clears it (registerInstalledExtension).
  const bool was_enabled = isEnabled(extension_id);
  if (!writeDisabledState(extension_id, false)) {
    restoreEnabledState(extension_id, was_enabled);
    emitUninstallFailure(
        extension_id,
        QString("Could not persist the disabled state for \"%1\"; uninstall not scheduled").arg(extension_id));
    return;
  }

  if (!writeRemovalIntent(makeIdRemovalIntent(RemovalOperation::kUninstall, extensions_dir_, extension_id, dir_path))) {
    // Nothing was scheduled, so leave the user's own enable/disable choice as it
    // was rather than a plugin that silently stopped loading.
    restoreEnabledState(extension_id, was_enabled);
    emitUninstallFailure(
        extension_id, QString("Could not record the removal of \"%1\"; uninstall not scheduled").arg(dir_path));
    return;
  }

  // Keep the record: the extension leaves installed_ but stays on disk and keeps
  // running until the restart, and a caller composing rows from the installed set
  // would otherwise have nothing left to show for it (see stagedUninstalls).
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

  // Stage the removal of the updated copy — do NOT delete now: an immediate remove
  // would hot-swap the loaded DSO and the card would read "Install". Mark it for
  // restart cleanup so the card shows "Needs Restart"; on the next launch
  // applyPendingUninstalls removes it and the host seed restores the bundled
  // version (always compatible, since it ships with the app).
  const QString dir_path = installed_[extension_id].path;
  // Journalled as a downgrade: a version revert preserves the user's enable/disable
  // choice, so unlike an uninstall it leaves no tombstone behind for the restored
  // bundled version to inherit.
  if (!writeRemovalIntent(makeIdRemovalIntent(RemovalOperation::kDowngrade, extensions_dir_, extension_id, dir_path))) {
    emitUninstallFailure(extension_id, QString("Could not stage the downgrade of \"%1\"").arg(extension_id));
    return;
  }
  // Do NOT remove the record from installed_ here: the downgrade is deferred, so
  // the updated copy stays installed and loaded until the next launch. Keeping
  // it (like a staged update does) lets the row keep showing "Installed vN" with
  // the "Needs Restart" badge (driven by hasPendingUninstall), instead of the
  // "—" not-installed placeholder for a plugin that is still live this session.
  // applyPendingUninstalls promotes the removal at the next launch, and the host
  // seed restores the bundled version. Deliberately do NOT touch the disabled
  // entry either: a downgrade is a version change, not a removal, so it preserves
  // the user's enable/disable choice, exactly like update() (see
  // registerInstalledExtension's preserve_disabled_state path). uninstall() moves
  // the entry in the opposite direction because the id is meant to stop loading.
  //
  // Keeping it in installed_ only holds until the next scan, which skips a
  // journalled directory — so record it as a staged removal too, and the row
  // survives every rescan for the rest of the session (see stagedUninstalls).
  staged_uninstalls_.insert(extension_id, installed_[extension_id]);
  emit downgradePendingRestart(extension_id);
}

void ExtensionManager::update(const Extension& ext) {
  if (const QString refusal = storeWriteRefusal(); !refusal.isEmpty()) {
    emitInstallFailure(ext.id, refusal);
    return;
  }
  refreshInstalledFromDisk();

  // Updates defer to restart: the new version is staged in pending_dir_ and
  // promoted — backing up the old version first — by applyPendingInstalls() at
  // the next startup. This avoids hot-swapping a DSO the running session may
  // still have loaded; the installed version stays live until the user restarts.
  // (Fresh install() still promotes immediately, since there is no loaded
  // version to clash with.)
  if (hasNewerInstalledVersion(ext)) {
    emitInstallFailure(
        ext.id, QString("Installed version \"%1\" is newer than registry version \"%2\"; downgrade is not allowed")
                    .arg(installedVersion(ext.id), ext.version));
    return;
  }

  // An update already staged for this id — install OR uninstall — would just
  // be re-downloaded and re-staged on top of the pending marker; the installed
  // version does not change until restart, so a caller relying on hasUpdate()
  // can ask again. Refuse rather than redo the work. Both markers matter: on
  // Windows the uninstall path stages the removal, so a subsequent update()
  // during that window would resurrect an id the user just asked to remove.
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
    // Never touch the promotion target itself, the staging area, or transaction/
    // quarantine scratch dirs.
    if (clean == keep || clean == pending_clean) {
      continue;
    }
    const QString name = entry.fileName();
    if (isTransactionDirectoryName(name) || name.startsWith(kQuarantinePrefix)) {
      continue;
    }
    const DirectoryDiscovery item = discoverExtensionDirectory(root);
    if (!item.found_plugin || item.record.id != id) {
      continue;  // ids are unique per extension, so only a true prior copy matches
    }
    // Move the shadow copy into the backup area (kept for recovery); fall back to
    // outright removal if the rename fails. Either way it must leave extensions_dir_
    // so the promoted "<id>" directory becomes the sole install of this extension.
    QDir().mkpath(PlatformUtils::backupDir());
    const QString backup = QDir(PlatformUtils::backupDir())
                               .absoluteFilePath(id + "-replaced-" + QUuid::createUuid().toString(QUuid::Id128));
    if (QDir().rename(root, backup)) {
      qWarning(
          "ExtensionManager: replaced prior install of '%s' at '%s' (backed up to '%s')", qPrintable(id),
          qPrintable(root), qPrintable(backup));
    } else if (QDir(root).removeRecursively()) {
      qWarning("ExtensionManager: removed prior install of '%s' at '%s'", qPrintable(id), qPrintable(root));
    } else {
      qWarning("ExtensionManager: could not remove prior install of '%s' at '%s'", qPrintable(id), qPrintable(root));
    }
    // Drop the record only if it tracks the displaced copy: after a promotion
    // installed_[id] already points at keep_dir, and erasing it here would
    // un-register the extension applyPendingInstalls just promoted.
    const auto tracked = installed_.constFind(id);
    if (tracked != installed_.constEnd() && QDir::cleanPath(tracked->path) == clean) {
      installed_.remove(id);
    }
  }
}

void ExtensionManager::applyPendingInstalls() {
  // Promoting, quarantining and sweeping staged work all rewrite the store, and
  // the staged directories may belong to the instance that owns it.
  if (!hasStoreWriteAccess()) {
    return;
  }
  const QDir pending(pending_dir_);
  if (!pending.exists()) {
    return;
  }

  // Conflict cleanup for the promoted ids runs AFTER the whole promotion loop.
  // replaceConflictingInstallDirs() dlopens sibling directories to read their
  // embedded ids, and opening a sibling's not-yet-promoted DSO pins its old
  // image in the process (same glibc quirk as in initComponents): the rescan
  // that follows this drain would then report the pre-update version for every
  // pinned sibling, and the marketplace would re-offer updates that are
  // already on disk. Deferring the scans guarantees any DSO they open is a
  // post-promotion payload.
  QList<std::pair<QString, QString>> promoted;  // (id, promoted dir)

  for (const QFileInfo& entry :
       pending.entryInfoList(QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
    const QString staged_dir = entry.absoluteFilePath();
    const QString staged_name = entry.fileName();
    if (isTransactionDirectoryName(staged_name)) {
      // Extraction scratch an older build wrote here rather than into the
      // staging sibling; it is never a promotable stage.
      removeDirectoryIfSet(staged_dir);
      continue;
    }
    if (staged_name.startsWith(kQuarantinePrefix)) {
      // Leftover from a previous failed promotion. Skip — manual inspection only.
      continue;
    }

    auto fail_staged_install = [&](const QString& signal_id, const QString& message) {
      qWarning(
          "ExtensionManager: staged install '%s' failed validation: %s", qPrintable(staged_dir), qPrintable(message));
      QString final_message = message;
      if (!QDir(staged_dir).removeRecursively()) {
        // Removal can fail on Windows when the DSO is still locked by another
        // process. Move the broken stage aside so the next startup does not
        // re-validate the same payload and emit the same diagnostic forever.
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

    // Back up the existing install before the staged version takes its place:
    // move the current dir aside first, so promoting an update never silently
    // destroys the previous install (this is the single backup point for updates).
    pending_backup_path_.clear();
    if (QDir(dst).exists()) {
      // Name the backup WITHOUT opening the old directory. dlopening dst here to
      // read its version would pin its old image in the process (dlopen caches by
      // path name and plugin DSOs are effectively NODELETE via unique symbols),
      // so a later in-process scan of dst would keep reading the old version.
      // A UUID keeps the backup unique; the embedded manifest inside still records
      // the version for anyone inspecting the backup.
      QDir().mkpath(PlatformUtils::backupDir());
      const QString candidate = QDir(PlatformUtils::backupDir())
                                    .absoluteFilePath(intent.id + "-" + QUuid::createUuid().toString(QUuid::Id128));

      if (!QDir().rename(dst, candidate)) {
        qWarning("ExtensionManager: failed to back up '%s' before promoting staged install", qPrintable(dst));
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
          "ExtensionManager: failed to promote staged install '%s' to '%s'", qPrintable(staged_dir), qPrintable(dst));
      QString message = QString("Could not promote staged install to \"%1\"").arg(dst);

      // Best-effort rollback so the user is never left with no extension. If
      // the rollback rename also fails, leave pending_backup_path_ set so
      // emitInstallFailure appends "Previous version remains in backup". The
      // restored directory is re-registered by the refreshInstalledFromDisk()
      // that runs after this drain (the path was never opened before, so that
      // scan reads it fresh).
      if (!pending_backup_path_.isEmpty() && QDir().rename(pending_backup_path_, dst)) {
        message += " Previous version restored.";
        pending_backup_path_.clear();
      }
      emitInstallFailure(intent.id, message);
      continue;
    }

    // Validate the promoted payload in a throwaway child process. An in-process
    // scan here is unreliable: plugin DSOs export STB_GNU_UNIQUE symbols and are
    // effectively NODELETE, so if this id was already dlopened in this process the
    // re-scan resolves pj_plugin_abi_version and the embedded manifest to the copy
    // already mapped and reports its identity instead of the bytes now on disk —
    // silently admitting a staged payload that lies about its id, or rejecting a
    // legitimate update. The helper reads the real bytes every time and leaves the
    // parent's image table untouched, so the restored backup below reads cleanly.
    const DirectoryDiscovery discovered = discoverExtensionDirectoryOutOfProcess(dst);
    if (const QString validation_error = validateRegistryIntent(discovered, intent.id, intent.version);
        !validation_error.isEmpty()) {
      // Undo the promotion: drop the rejected payload and put the previous install
      // back. The out-of-process check did not map dst into this process, so the
      // restored copy reads cleanly; re-registration is left to the
      // refreshInstalledFromDisk() that runs after this drain.
      QDir(dst).removeRecursively();
      if (!pending_backup_path_.isEmpty() && QDir().rename(pending_backup_path_, dst)) {
        pending_backup_path_.clear();
      }
      emitInstallFailure(intent.id, validation_error);
      continue;
    }
    if (const HostCompatibility compat = hostCompatibility(discovered.record); !compat.ok) {
      // Same undo pattern as validation failure above: drop the rejected payload
      // and restore the previous install.
      QDir(dst).removeRecursively();
      if (!pending_backup_path_.isEmpty() && QDir().rename(pending_backup_path_, dst)) {
        pending_backup_path_.clear();
      }
      emitInstallFailure(intent.id, compat.reason);
      continue;
    }

    QFile::remove(pendingInstallIntentPath(dst));
    // Staged promotions are updates/replaces of an already-installed plugin;
    // preserve the user's disable choice instead of forcing it back to enabled.
    registerInstalledExtension(intent.id, dst, discovered.record, /*preserve_disabled_state=*/true);
    pending_backup_path_.clear();
    promoted.append({intent.id, dst});
    emit installFinished(intent.id, true);
  }

  // Replace any prior copy of a promoted id stored under a different directory
  // name (e.g. a bundled plugin) so "<id>" becomes the sole install of that id.
  for (const auto& [promoted_id, promoted_dir] : promoted) {
    replaceConflictingInstallDirs(promoted_id, promoted_dir);
  }
}

void ExtensionManager::applyPendingUninstalls() {
  // Deleting payloads is a store mutation like any other sweep: only the lease
  // holder may drain. A read-only instance leaves both the records and the
  // directories for the writer to handle.
  if (!hasStoreWriteAccess()) {
    return;
  }

  // Markers become external records BEFORE anything is deleted, so every deletion
  // in this drain is authorised by a validated record outside the payload.
  externalizeLegacyUninstallMarkers();

  QStringList rejections;
  const QList<RemovalIntent> intents = readRemovalIntents(extensions_dir_, &rejections);
  for (const QString& rejection : rejections) {
    // Never silently skipped: a record this host refuses to act on is either
    // corruption or an attempt to borrow its delete privilege, and both are worth
    // surfacing.
    reportDiagnostic({}, rejection, true);
  }

  for (const RemovalIntent& intent : intents) {
    // Re-confine at the moment of deletion. parseRemovalIntent() skips the canonical
    // check for a target that did not exist yet — there was nothing to abuse then —
    // so a record admitted on the lexical rule alone leaves a window in which a
    // matching-named symlink can be planted before this drain runs. QFileInfo::exists
    // and removeRecursively both FOLLOW such a link, which would carry the recursive
    // delete out of the store. A managed payload is a real directory, so a symlink at
    // the target is refused outright, and confinement is re-checked against what is on
    // disk NOW (this time the canonical branch fires, since the planted link exists).
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

    // A path already gone satisfies the intent; removeRecursively() reports false
    // for a directory that does not exist, which would otherwise strand the record
    // forever.
    const bool removed = !target.exists() || QDir(intent.path).removeRecursively();
    if (!removed) {
      // Keep the record so the next launch retries. This is the partial-deletion
      // case too: content already deleted stays deleted, and the record outlives it
      // because it never lived in there.
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
    // Nothing here enables anything. An uninstall's disabled entry stays as a
    // TOMBSTONE: the files are gone, so the entry costs nothing, and keeping it
    // means journal processing never needs authority over the enable state at all.
    // A corrupt or hostile record therefore cannot make a plugin loadable, and a
    // same-id copy left elsewhere on disk cannot be resurrected by this deletion.
    // A reinstall clears the tombstone (registerInstalledExtension).
    //
    // Retire the record FILE that was parsed, never a name recomputed from the id
    // it claims, so a record cannot retire a different extension's record.
    QFile::remove(intent.record_path);
  }
  // Goes with the last record (rmdir refuses a non-empty directory).
  QDir().rmdir(removalJournalRoot(extensions_dir_));
}

void ExtensionManager::externalizeLegacyUninstallMarkers() {
  // Older builds recorded the intent inside the payload, where a partially failed
  // recursive delete destroys it. Each such directory is converted into an external
  // record FIRST, so the retry survives the deletion that follows.
  //
  // The marker's CONTENT is never read. It is package-controlled filesystem content
  // — a ZIP can ship one naming any id it likes — so the derived record is
  // path-only: it carries no id and therefore no authority over any extension's
  // state. Existence schedules a deletion of the directory holding it, nothing more.

  // A marker whose deletion keeps failing survives from launch to launch. Without
  // this, each pass would file yet another cleanup-<uuid> record for the same stuck
  // directory and the journal would grow without bound. Skip a directory a live
  // cleanup record already covers.
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
    // Confined like any other record, even though this one was found by scanning
    // the store: a directory reached through a symlink still must not authorise a
    // delete outside it.
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
  // "Core" = the id ships with the application. The host computes the bundled
  // id -> version map from the bundled plugin directory and hands it in via
  // setBundledVersions(); no per-folder marker. Empty (standalone marketplace app
  // or a --plugin-dir run) means nothing is core.
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
  // The journal is keyed by id, so this no longer has to guess which directory the
  // removal was staged against — the directory NAME is not necessarily the id (it
  // comes from the embedded manifest, so a copy placed by hand can be called
  // anything), which is what made the in-payload marker awkward to find by id.
  if (QFile::exists(removalIntentPath(extensions_dir_, id))) {
    return true;
  }

  // A marker an older build left inside the payload still counts as a pending
  // removal — its EXISTENCE is a fact about this installation that the badge and
  // the install guards must respect. Its content remains untrusted (see
  // sweepLegacyUninstallMarkers).
  const auto staged = staged_uninstalls_.constFind(id);
  const QString root = staged != staged_uninstalls_.constEnd() ? staged->path : extRoot(extensions_dir_, id);
  return QFile::exists(pendingUninstallMarkerPath(root));
}

QString ExtensionManager::installedVersion(const QString& id) const {
  const auto it = installed_.constFind(id);
  if (it == installed_.constEnd()) {
    return {};
  }
  // What the seed wrote wins outright over what the scan saw, in both directions:
  // it is the later fact about the same directory, and the scan cannot be redone
  // (see the header). A restore to the bundled build lands BELOW the scanned
  // version, so anything that only ever moved the answer upwards would keep
  // reporting the incompatible copy it just replaced.
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

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

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
  // Mirror the just-promoted bytes into the content-addressed shadow. Best-effort,
  // and after the visible install is final, so it never gates the user's result.
  shadowAdoptIntoStore(id, dst);

  if (preserve_disabled_state) {
    // A staged update/replace keeps the user's enable/disable choice: promoting
    // the new version must not re-enable a plugin the user explicitly disabled.
    // Leave the persisted disabled entry untouched and mirror it into the record.
    record.enabled = !disabledExtensionIds().contains(id);
    installed_[id] = record;
    return;
  }

  // A fresh install starts enabled. Clear any stale disabled entry that may
  // linger from a previous uninstall of the same id, so the plugin loads
  // instead of being silently skipped on the next launch.
  record.enabled = true;
  installed_[id] = record;
  setEnabled(id, true);
}

void ExtensionManager::shadowAdoptIntoStore(const QString& id, const QString& dir) {
  if (artifact_store_ == nullptr || !artifact_store_->hasStoreWriteAccess()) {
    return;
  }
  // adoptDirectory hashes the tree and writes it write-once: identical bytes that
  // are already resident are a no-op, so a reinstall of the same payload adds
  // nothing. A rejected tree (a symlink, a quota, an I/O fault) leaves the visible
  // install untouched — it is what actually loads.
  const auto adopted = artifact_store_->adoptDirectory(dir);
  if (!adopted) {
    // Drop any prior mapping for this id: its store object no longer matches the
    // freshly installed bytes, so the runtime must fall back to extensions/<id>/
    // rather than load a stale version from the store.
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
  // Enumerate the managed dir from disk rather than the cached installed set: the
  // bundled seed writes extensions/<id>/ AFTER that snapshot is taken, so a cached
  // view would miss freshly-seeded plugins. The subdirectory name is the id.
  for (const QFileInfo& entry : QDir(extensions_dir_).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    // Prefer the immutable store object when it backs this id and its bytes still
    // verify against the recorded content manifest; fall back to the classic
    // directory when the store does not (yet) hold it, was rejected, has been
    // collected, or — the integrity guard — when the resident object no longer
    // matches its digest.
    if (artifact_store_ != nullptr) {
      const auto digest_it = loaded_digests_.constFind(entry.fileName());
      if (digest_it != loaded_digests_.constEnd()) {
        if (const auto digest = GenerationDigest::parse(digest_it.value().toStdString())) {
          const QString object_dir = artifact_store_->artifactRootFor(*digest);
          // Rehash the resident bytes against the object's manifest: an existence
          // check alone would happily load an object corrupted after commit.
          if (artifact_store_->verifyArtifact(*digest)) {
            dirs.push_back(object_dir);
            continue;
          }
          if (QDir(object_dir).exists()) {
            // Present but its bytes no longer match its digest (a half-written
            // commit, disk corruption). Never load it: fall back to extensions/<id>/
            // and surface why, since the store copy stays unusable until reinstall.
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
  // Drop profile entries whose extensions/<id>/ is gone: an uninstall drained at
  // startup leaves the mapping naming an install that no longer exists.
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
  // Guard the sweep: an empty profile alongside live installs means the profile
  // failed to load, and sweeping against an empty root set would erase the whole
  // store. A genuinely empty install set (nothing installed) sweeps normally.
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
    // Spare the transaction of an install still in progress: the worker is
    // writing into it in the background and its completion handler owns the
    // cleanup. Wiping it here (triggered by Refresh, or by starting an
    // install/uninstall/update of a different plugin) would race with the worker
    // and truncate a partial install into a broken final state. Both sides are
    // normalized so the guard tolerates trailing slashes, `..` segments, and
    // case-insensitive filesystems.
    if (QDir::cleanPath(entry.absoluteFilePath()) != pending_extract_dir_) {
      removeDirectoryIfSet(entry.absoluteFilePath());
    }
  }
}

void ExtensionManager::refreshInstalledFromDisk() {
  // Directories a journalled removal has claimed. They are still on disk (and still
  // loaded) until the next launch drains them, but they must not read as installs,
  // exactly as a marked directory did not.
  // Both projections of the journal are taken in ONE pass: the claimed paths gate
  // the scan below, and the claimed ids retire staged records further down.
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
      qWarning("ExtensionManager: ignoring extension directory '%s': %s", qPrintable(root), qPrintable(item.error));
      continue;
    }
    if (discovered.contains(item.record.id)) {
      // Two directories embed the same id (e.g. a promoted update left alongside
      // a differently-named prior copy). Keep the HIGHEST version rather than
      // whichever the directory scan happened to hit first, so the resolution is
      // deterministic and an update never loses to a stale lower-version copy.
      if (comparePluginVersions(item.record.version.toStdString(), discovered[item.record.id].version.toStdString()) <=
          0) {
        qWarning(
            "ExtensionManager: duplicate embedded id '%s' in '%s'; keeping higher version already found",
            qPrintable(item.record.id), qPrintable(root));
        continue;
      }
      qWarning(
          "ExtensionManager: duplicate embedded id '%s'; '%s' supersedes the lower-version copy",
          qPrintable(item.record.id), qPrintable(root));
    }
    discovered[item.record.id] = item.record;
  }
  installed_ = std::move(discovered);

  // Clear scratch a crashed or abandoned install left behind. The staging
  // sibling is where transactions live and it goes with the last of them (rmdir
  // refuses it while an in-flight transaction is still there); the in-tree pass
  // is the migration path for residue an older build wrote into the scanned dir,
  // which the recursive plugin scan would otherwise keep finding.
  //
  // Writer-only, both passes. `pending_extract_dir_` recognises this instance's
  // own live transaction and nothing else, so a read-only session sweeping here
  // would delete the extraction the lock holder is filling right now.
  if (hasStoreWriteAccess()) {
    const QString stage_root = transactionStageRoot(extensions_dir_);
    sweepTransactionRoots(stage_root);
    QDir().rmdir(stage_root);
    sweepTransactionRoots(extensions_dir_);
  }

  // Retire a staged record once nothing schedules its removal any more — drained by
  // a restart, or cleared because the directory was replaced. Read from the ids
  // collected above rather than through hasPendingUninstall(), which would consult
  // this very map while removeIf erases from it.
  staged_uninstalls_.removeIf([&](const auto& entry) {
    return !journalled_ids.contains(entry.key()) && !QFile::exists(pendingUninstallMarkerPath(entry.value().path));
  });

  // Reflect the persisted enable/disable state on each record so the UI can read
  // installedExtensions()[id].enabled without consulting QSettings itself.
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
  // ONE QSettings instance does the read, the modify, the write, the flush AND the
  // verdict. status() belongs to the object that performed the write — on Windows a
  // registry failure surfaces on that instance's flush — so syncing or reading
  // status() from a freshly constructed temporary reports on an object that wrote
  // nothing and silently swallows the failure. Reading through a second instance is
  // just as wrong in the other direction: it can miss this one's unflushed edit.
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
  // Keep the in-memory record in sync so the UI updates without a full rescan;
  // the actual load/unload happens on the next launch.
  if (installed_.contains(id)) {
    installed_[id].enabled = enabled;
  }
  return true;
}

void ExtensionManager::restoreEnabledState(const QString& id, bool enabled) {
  if (!writeDisabledState(id, enabled)) {
    // The rollback itself could not be written. Say so rather than leave the user
    // guessing why a plugin whose uninstall failed also stopped loading.
    reportDiagnostic(
        id, QString("Could not restore the enabled state of \"%1\"; it may load differently next launch").arg(id),
        true);
  }
}

void ExtensionManager::setEnabled(const QString& id, bool enabled) {
  // The disabled list is one global QSettings key shared by every instance, so a
  // read-modify-write from a second one silently discards the writer's edits.
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
