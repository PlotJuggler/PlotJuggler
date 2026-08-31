// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

#include "ArchiveExtractor.hpp"
#include "Durability.hpp"
#include "pj_marketplace/artifact_store.hpp"
#include "pj_marketplace/content_manifest.hpp"
#include "pj_marketplace/store_lease.hpp"

using namespace Qt::StringLiterals;

namespace PJ {
namespace {

constexpr int kArtifactManifestSchema = 1;
constexpr qsizetype kMaximumArtifactManifestBytes = 4 * 1024 * 1024;
constexpr auto kArtifactManifestName = kArtifactManifestFileName;
constexpr std::string_view kSha256Prefix = "sha256:";

std::filesystem::path filesystemPath(const QFileInfo& file_info) {
  return file_info.filesystemAbsoluteFilePath();
}

bool cancellationRequested(const std::atomic<bool>* cancel_requested) {
  return cancel_requested != nullptr && cancel_requested->load(std::memory_order_relaxed);
}

class StagingCleanup {
 public:
  explicit StagingCleanup(QString path) : path_(std::move(path)) {}

  ~StagingCleanup() {
    if (armed_) {
      QDir(path_).removeRecursively();
    }
  }

  void disarm() noexcept {
    armed_ = false;
  }

 private:
  QString path_;
  bool armed_ = true;
};

Expected<std::vector<StoredFile>, StoreRejection> inspectArtifactFiles(
    const QString& directory, bool hash_contents, bool ignore_manifest) {
  const std::filesystem::path root = QFileInfo(directory).filesystemAbsoluteFilePath();
  auto observed = inspectContentManifest(
      root, {.hash_contents = hash_contents,
             .skipped_relative_path = ignore_manifest ? std::string_view(kArtifactManifestName) : std::string_view{}});
  if (!observed) {
    const StoreRejectionCode code = observed.error().code == ContentManifestRejectionCode::kUnsafeTree
                                        ? StoreRejectionCode::kPostExtractionMutation
                                        : StoreRejectionCode::kIoFailure;
    return unexpected(storeRejection(code, std::move(observed.error().message)));
  }
  return std::move(*observed);
}

Expected<void, StoreRejection> validateStagingTree(
    const QString& directory, const std::vector<StoredFile>& expected_files) {
  auto observed = inspectArtifactFiles(directory, false, false);
  if (!observed) {
    return unexpected(observed.error());
  }
  if (observed->size() != expected_files.size()) {
    return unexpected(
        storeRejection(StoreRejectionCode::kPostExtractionMutation, "staged artifact file set changed before commit"));
  }
  for (size_t index = 0; index < observed->size(); ++index) {
    const StoredFile& expected = expected_files[index];
    const StoredFile& actual = (*observed)[index];
    if (actual.relative_path != expected.relative_path || actual.size != expected.size) {
      return unexpected(storeRejection(
          StoreRejectionCode::kPostExtractionMutation,
          "staged artifact file set or size changed before commit: " + actual.relative_path));
    }
  }
  return {};
}

Expected<void, StoreRejection> writeManifest(
    const QString& staging_directory, std::string_view artifact_digest, const std::vector<StoredFile>& files) {
  QJsonArray file_array;
  for (const StoredFile& file : files) {
    QJsonObject file_object;
    file_object[u"path"_s] = QString::fromUtf8(file.relative_path);
    file_object[u"sha256"_s] = QString::fromLatin1(file.sha256);
    file_object[u"size"_s] = static_cast<qint64>(file.size);
    file_array.append(file_object);
  }
  QJsonObject manifest_object;
  manifest_object[u"schema"_s] = kArtifactManifestSchema;
  manifest_object[u"artifact_sha256"_s] = QString::fromLatin1(artifact_digest);
  manifest_object[u"files"_s] = file_array;
  const QByteArray payload = QJsonDocument(manifest_object).toJson(QJsonDocument::Compact);

  QSaveFile manifest(QDir(staging_directory).absoluteFilePath(QLatin1String(kArtifactManifestName)));
  if (!manifest.open(QIODevice::WriteOnly | QIODevice::Truncate) || manifest.write(payload) != payload.size()) {
    manifest.cancelWriting();
    return unexpected(storeRejection(StoreRejectionCode::kIoFailure, "cannot write artifact manifest"));
  }
  if (!manifest.commit()) {
    return unexpected(storeRejection(StoreRejectionCode::kIoFailure, "cannot commit artifact manifest"));
  }
  return {};
}

Expected<StoredArtifact, StoreRejection> readResidentArtifact(
    const QString& artifact_path, const std::string& expected_artifact_digest) {
  QFile manifest(QDir(artifact_path).absoluteFilePath(QLatin1String(kArtifactManifestName)));
  if (!manifest.open(QIODevice::ReadOnly) || manifest.size() < 0 || manifest.size() > kMaximumArtifactManifestBytes) {
    return unexpected(storeRejection(
        StoreRejectionCode::kCorruptResidentArtifact, "resident artifact manifest is missing or exceeds its bound"));
  }
  const QByteArray payload = manifest.read(kMaximumArtifactManifestBytes + 1);
  if (payload.size() != manifest.size()) {
    return unexpected(
        storeRejection(StoreRejectionCode::kCorruptResidentArtifact, "resident artifact manifest read failed"));
  }
  QJsonParseError parse_error;
  const QJsonDocument document = QJsonDocument::fromJson(payload, &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    return unexpected(
        storeRejection(StoreRejectionCode::kCorruptResidentArtifact, "resident artifact manifest is invalid"));
  }
  const QJsonObject object = document.object();
  if (object.size() != 3 || object.value(u"schema"_s).toInt(-1) != kArtifactManifestSchema ||
      object.value(u"artifact_sha256"_s).toString().toStdString() != expected_artifact_digest ||
      !object.value(u"files"_s).isArray()) {
    return unexpected(storeRejection(
        StoreRejectionCode::kCorruptResidentArtifact, "resident artifact manifest identity is inconsistent"));
  }

  std::vector<StoredFile> files;
  for (const QJsonValue& value : object.value(u"files"_s).toArray()) {
    if (!value.isObject()) {
      return unexpected(
          storeRejection(StoreRejectionCode::kCorruptResidentArtifact, "resident file record is invalid"));
    }
    const QJsonObject file_object = value.toObject();
    const QString path = file_object.value(u"path"_s).toString();
    const QString digest = file_object.value(u"sha256"_s).toString();
    const qint64 size = file_object.value(u"size"_s).toInteger(-1);
    if (file_object.size() != 3 || path.isEmpty() || digest.size() != 64 || size < 0) {
      return unexpected(
          storeRejection(StoreRejectionCode::kCorruptResidentArtifact, "resident file record is invalid"));
    }
    auto valid_path = validateArchivePath(path.toUtf8().toStdString(), kDefaultMaximumPathSegments);
    if (!valid_path) {
      return unexpected(storeRejection(StoreRejectionCode::kCorruptResidentArtifact, "resident file path is invalid"));
    }
    files.push_back({*valid_path, digest.toStdString(), static_cast<uint64_t>(size)});
  }
  std::ranges::sort(files, {}, &StoredFile::relative_path);

  // Re-reading resident bytes is intentional: idempotent re-commit is also a rare integrity audit.
  auto observed = inspectArtifactFiles(artifact_path, true, true);
  if (!observed || *observed != files) {
    const std::string detail =
        observed ? "resident artifact bytes do not match its manifest" : observed.error().message;
    return unexpected(storeRejection(StoreRejectionCode::kCorruptResidentArtifact, detail));
  }
  return StoredArtifact{
      .artifact_digest = expected_artifact_digest,
      .transport_digest = {},
      .artifact_path = artifact_path,
      .files = files,
      .stripped_archive_root = std::nullopt,
      .already_present = true,
  };
}

Expected<StoredArtifact, StoreRejection> publishStaging(
    const QString& staging_path, const QString& extraction_root, const QString& artifacts_root,
    const std::string& artifact_digest, const std::vector<StoredFile>& files, ArtifactStoreCommitFault fault) {
  if (auto structure = validateStagingTree(staging_path, files); !structure) {
    return unexpected(structure.error());
  }
  if (auto manifest = writeManifest(staging_path, artifact_digest, files); !manifest) {
    return unexpected(manifest.error());
  }
  if (fault == ArtifactStoreCommitFault::kBeforeRename) {
    return unexpected(storeRejection(StoreRejectionCode::kIoFailure, "injected I/O fault before artifact rename"));
  }
  if (!sameFilesystem(extraction_root, artifacts_root)) {
    return unexpected(storeRejection(
        StoreRejectionCode::kFilesystemMismatch, "object staging and destination are not on one filesystem"));
  }
  const QString artifact_path = QDir(artifacts_root).absoluteFilePath(QString::fromLatin1(artifact_digest));
  if (!QDir().rename(staging_path, artifact_path)) {
    return unexpected(storeRejection(StoreRejectionCode::kIoFailure, "atomic artifact directory rename failed"));
  }
  if (auto sync_result = syncDirectoryToDisk(artifacts_root); !sync_result) {
    return unexpected(storeRejection(
        StoreRejectionCode::kIoFailure, "cannot make artifact rename durable: " + sync_result.error().toStdString()));
  }
  if (fault == ArtifactStoreCommitFault::kAfterRename) {
    return unexpected(storeRejection(StoreRejectionCode::kIoFailure, "injected I/O fault after artifact rename"));
  }
  return StoredArtifact{
      .artifact_digest = artifact_digest,
      .transport_digest = {},
      .artifact_path = artifact_path,
      .files = files,
      .stripped_archive_root = std::nullopt,
      .already_present = false,
  };
}

Expected<void, StoreRejection> validateResidentFiles(const std::vector<StoredFile>& files, const StoreQuotas& quotas) {
  if (files.size() > quotas.maximum_file_count) {
    return unexpected(storeRejection(StoreRejectionCode::kLimitExceeded, "resident file count exceeds quota"));
  }
  uint64_t total_size = 0;
  for (const StoredFile& file : files) {
    if (file.relative_path == kArtifactManifestName) {
      return unexpected(
          storeRejection(StoreRejectionCode::kUnsafePath, "resident directory uses the reserved object-manifest path"));
    }
    if (auto path = validateArchivePath(file.relative_path, quotas.maximum_path_segments); !path) {
      return unexpected(path.error());
    }
    if (file.size > quotas.maximum_entry_expanded_bytes || total_size > quotas.maximum_total_expanded_bytes ||
        file.size > quotas.maximum_total_expanded_bytes - total_size) {
      return unexpected(storeRejection(StoreRejectionCode::kLimitExceeded, "resident expanded size exceeds quota"));
    }
    total_size += file.size;
  }
  return {};
}

Expected<void, StoreRejection> copyResidentFiles(
    const QString& source_root, const QString& staging_root, const std::vector<StoredFile>& expected_files,
    const std::atomic<bool>* cancel_requested, bool verify_copy_hashes = true) {
  constexpr qint64 kCopyChunkBytes = 64 * 1024;
  QByteArray buffer(kCopyChunkBytes, Qt::Uninitialized);
  for (const StoredFile& file : expected_files) {
    if (cancellationRequested(cancel_requested)) {
      return unexpected(storeRejection(StoreRejectionCode::kCancelled, "directory adoption was cancelled"));
    }
    const QString relative = QString::fromUtf8(file.relative_path);
    const QString source_path = QDir(source_root).absoluteFilePath(relative);
    const QString destination_path = QDir(staging_root).absoluteFilePath(relative);
    if (!QDir().mkpath(QFileInfo(destination_path).absolutePath())) {
      return unexpected(storeRejection(StoreRejectionCode::kIoFailure, "cannot create resident staging path"));
    }
    QFile input(source_path);
    QFile output(destination_path);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
      return unexpected(storeRejection(StoreRejectionCode::kIoFailure, "cannot copy resident file into staging"));
    }
    while (!input.atEnd()) {
      if (cancellationRequested(cancel_requested)) {
        return unexpected(storeRejection(StoreRejectionCode::kCancelled, "directory adoption was cancelled"));
      }
      const qint64 read = input.read(buffer.data(), buffer.size());
      if (read <= 0 || output.write(buffer.constData(), read) != read) {
        return unexpected(storeRejection(StoreRejectionCode::kIoFailure, "cannot copy resident file into staging"));
      }
    }
    if (auto synced = syncFileToDisk(output); !synced) {
      return unexpected(storeRejection(StoreRejectionCode::kIoFailure, synced.error().toStdString()));
    }
  }
  auto observed = inspectArtifactFiles(staging_root, verify_copy_hashes, false);
  const auto matches_without_hashes = [&]() {
    if (observed->size() != expected_files.size()) {
      return false;
    }
    for (size_t index = 0; index < observed->size(); ++index) {
      if ((*observed)[index].relative_path != expected_files[index].relative_path ||
          (*observed)[index].size != expected_files[index].size) {
        return false;
      }
    }
    return true;
  };
  if (!observed || (verify_copy_hashes ? *observed != expected_files : !matches_without_hashes())) {
    return unexpected(storeRejection(
        StoreRejectionCode::kPostExtractionMutation,
        observed ? "resident bytes changed while being adopted" : observed.error().message));
  }
  return {};
}

// Returns the common first path segment only when every file is nested below it.
std::optional<std::string> archiveSoleTopLevelDirectory(const std::vector<StoredFile>& files) {
  if (files.empty()) {
    return std::nullopt;
  }
  const size_t separator = files.front().relative_path.find('/');
  if (separator == std::string::npos || separator == 0) {
    return std::nullopt;
  }
  const std::string directory = files.front().relative_path.substr(0, separator);
  const std::string prefix = directory + '/';
  if (!std::ranges::all_of(files, [&](const StoredFile& file) { return file.relative_path.starts_with(prefix); })) {
    return std::nullopt;
  }
  return directory;
}

}  // namespace

ArtifactStore::ArtifactStore(StoreSession& session, StoreQuotas quotas) : session_(session), quotas_(quotas) {}

ArtifactStore::ArtifactStore(std::unique_ptr<StoreSession> session, StoreQuotas quotas)
    : owned_session_(std::move(session)), session_(*owned_session_), quotas_(quotas) {}

Expected<std::unique_ptr<ArtifactStore>, StoreRejection> ArtifactStore::acquire(
    const QString& store_root, StoreQuotas quotas) {
  auto session = StoreSession::open(store_root);
  if (!session) {
    return unexpected(session.error());
  }
  auto owned_session = std::make_unique<StoreSession>(std::move(*session));
  return std::unique_ptr<ArtifactStore>(new ArtifactStore(std::move(owned_session), quotas));
}

ArtifactStore::~ArtifactStore() = default;

bool ArtifactStore::hasStoreWriteAccess() const noexcept {
  return session_.hasStoreWriteAccess();
}

const StoreRejection* ArtifactStore::writeRefusal() const noexcept {
  return session_.writeRefusal();
}

const QString& ArtifactStore::storeRoot() const noexcept {
  return session_.layout().storeRoot();
}

const QString& ArtifactStore::artifactsRoot() const noexcept {
  return session_.layout().artifactsRoot();
}

QString ArtifactStore::artifactRootFor(const GenerationDigest& digest) const {
  return QDir(artifactsRoot()).absoluteFilePath(QString::fromStdString(objectDirectoryNameFor(digest)));
}

size_t ArtifactStore::collectUnreferenced(const std::vector<GenerationDigest>& live) {
  if (!hasStoreWriteAccess()) {
    return 0;
  }
  std::unordered_set<std::string> keep;
  keep.reserve(live.size());
  for (const GenerationDigest& digest : live) {
    keep.insert(objectDirectoryNameFor(digest));
  }
  size_t removed = 0;
  const QDir root(artifactsRoot());
  for (const QFileInfo& entry : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    const QString name = entry.fileName();
    // A candidate is only a canonical object directory: parsing the name back to a
    // digest and re-deriving the directory name rejects scratch dirs
    // (kArtifactScratchPrefix) and any stray entry without a hand-rolled hex check.
    const auto digest = GenerationDigest::parse(name.toStdString());
    if (!digest || QString::fromStdString(objectDirectoryNameFor(*digest)) != name) {
      continue;
    }
    if (keep.count(name.toStdString()) != 0) {
      continue;
    }
    if (QDir(entry.absoluteFilePath()).removeRecursively()) {
      ++removed;
    }
  }
  return removed;
}

const QString& ArtifactStore::extractionRoot() const noexcept {
  return session_.layout().extractionRoot();
}

Expected<StoredArtifact, StoreRejection> ArtifactStore::commitArchive(
    const VerifiedArtifact& artifact, const std::atomic<bool>* cancel_requested,
    const ArtifactDigestKnownHook& digest_known) {
  if (cancellationRequested(cancel_requested)) {
    return unexpected(storeRejection(StoreRejectionCode::kCancelled, "artifact commit was cancelled"));
  }
  if (!hasStoreWriteAccess()) {
    const StoreRejection* refusal = writeRefusal();
    return unexpected(
        refusal != nullptr ? *refusal
                           : storeRejection(StoreRejectionCode::kWriterLeaseUnavailable, "store is read-only"));
  }
  const QFileInfo archive_info(artifact.path);
  if (!archive_info.isFile() || archive_info.size() < 0) {
    return unexpected(storeRejection(StoreRejectionCode::kIoFailure, "artifact is not a readable regular file"));
  }
  if (static_cast<uint64_t>(archive_info.size()) > quotas_.maximum_compressed_bytes) {
    return unexpected(storeRejection(StoreRejectionCode::kLimitExceeded, "artifact compressed size exceeds quota"));
  }

  // The published identity is the content manifest of the extracted files, so
  // it cannot be known before extraction; the dedupe check runs afterwards.
  const QString staging_path =
      QDir(extractionRoot())
          .absoluteFilePath(
              QLatin1String(kArtifactScratchPrefix) + u"%1"_s.arg(QUuid::createUuid().toString(QUuid::Id128)));
  if (!QDir().mkpath(staging_path)) {
    return unexpected(
        storeRejection(StoreRejectionCode::kIoFailure, "cannot create artifact extraction staging directory"));
  }
  StagingCleanup cleanup(staging_path);

  ArchiveExtractor extractor;
  auto extracted = extractor.extract(
      filesystemPath(archive_info), QFileInfo(staging_path).filesystemAbsoluteFilePath(), quotas_, cancel_requested,
      extraction_write_hook_);
  if (!extracted) {
    return unexpected(extracted.error());
  }

  const std::optional<std::string> top_level = archiveSoleTopLevelDirectory(extracted->files);
  QString published_staging_path = staging_path;
  if (top_level.has_value()) {
    published_staging_path =
        QDir(staging_path)
            .absoluteFilePath(QString::fromUtf8(top_level->data(), static_cast<qsizetype>(top_level->size())));
    const size_t prefix_size = top_level->size() + 1;
    for (StoredFile& file : extracted->files) {
      file.relative_path.erase(0, prefix_size);
    }
  }
  if (auto resident_files = validateResidentFiles(extracted->files, quotas_); !resident_files) {
    return unexpected(resident_files.error());
  }
  const std::string artifact_digest = aggregateContentManifestDigest(extracted->files);
  if (digest_known) {
    if (auto parsed = GenerationDigest::parse(std::string(kSha256Prefix) + artifact_digest); parsed) {
      digest_known(*parsed);
    }
  }
  const QString artifact_path = QDir(artifactsRoot()).absoluteFilePath(QString::fromLatin1(artifact_digest));
  if (QFileInfo::exists(artifact_path)) {
    auto resident = readResidentArtifact(artifact_path, artifact_digest);
    if (resident) {
      resident->transport_digest = artifact.digest.str();
      resident->stripped_archive_root = top_level;
    }
    return resident;
  }
  if (before_commit_hook_) {
    before_commit_hook_(QFileInfo(published_staging_path).filesystemAbsoluteFilePath());
  }
  if (cancellationRequested(cancel_requested)) {
    return unexpected(storeRejection(StoreRejectionCode::kCancelled, "artifact commit was cancelled"));
  }
  auto published = publishStaging(
      published_staging_path, extractionRoot(), artifactsRoot(), artifact_digest, extracted->files, commit_fault_);
  if ((published || QFileInfo::exists(artifact_path)) && published_staging_path == staging_path) {
    cleanup.disarm();
  }
  if (published) {
    published->transport_digest = artifact.digest.str();
    published->stripped_archive_root = top_level;
  }
  return published;
}

Expected<StoredArtifact, StoreRejection> ArtifactStore::adoptDirectory(
    const QString& directory, const std::atomic<bool>* cancel_requested, const ArtifactDigestKnownHook& digest_known) {
  if (cancellationRequested(cancel_requested)) {
    return unexpected(storeRejection(StoreRejectionCode::kCancelled, "directory adoption was cancelled"));
  }
  if (!hasStoreWriteAccess()) {
    const StoreRejection* refusal = writeRefusal();
    return unexpected(
        refusal != nullptr ? *refusal
                           : storeRejection(StoreRejectionCode::kWriterLeaseUnavailable, "store is read-only"));
  }
  const QFileInfo source_info(directory);
  if (!source_info.isDir() || source_info.isSymLink()) {
    return unexpected(storeRejection(StoreRejectionCode::kIoFailure, "resident source is not a real directory"));
  }
  auto files = inspectArtifactFiles(source_info.absoluteFilePath(), true, false);
  if (!files) {
    return unexpected(files.error());
  }
  if (auto quotas = validateResidentFiles(*files, quotas_); !quotas) {
    return unexpected(quotas.error());
  }
  const std::string artifact_digest = aggregateContentManifestDigest(*files);
  const std::string canonical_digest = std::string(kSha256Prefix) + artifact_digest;
  if (digest_known) {
    if (auto parsed = GenerationDigest::parse(std::string(kSha256Prefix) + artifact_digest); parsed) {
      digest_known(*parsed);
    }
  }
  const QString artifact_path = QDir(artifactsRoot()).absoluteFilePath(QString::fromLatin1(artifact_digest));
  if (QFileInfo::exists(artifact_path)) {
    return readResidentArtifact(artifact_path, artifact_digest);
  }

  const QString staging_path =
      QDir(extractionRoot())
          .absoluteFilePath(
              QLatin1String(kArtifactScratchPrefix) + u"%1"_s.arg(QUuid::createUuid().toString(QUuid::Id128)));
  if (!QDir().mkpath(staging_path)) {
    return unexpected(storeRejection(StoreRejectionCode::kIoFailure, "cannot create artifact adoption staging"));
  }
  StagingCleanup cleanup(staging_path);
  if (auto copied = copyResidentFiles(source_info.absoluteFilePath(), staging_path, *files, cancel_requested);
      !copied) {
    return unexpected(copied.error());
  }
  if (before_commit_hook_) {
    before_commit_hook_(QFileInfo(staging_path).filesystemAbsoluteFilePath());
  }
  if (cancellationRequested(cancel_requested)) {
    return unexpected(storeRejection(StoreRejectionCode::kCancelled, "directory adoption was cancelled"));
  }
  auto published =
      publishStaging(staging_path, extractionRoot(), artifactsRoot(), artifact_digest, *files, commit_fault_);
  if (published || QFileInfo::exists(artifact_path)) {
    cleanup.disarm();
  }
  return published;
}

Expected<StoredArtifact, StoreRejection> ArtifactStore::verifyArtifact(const GenerationDigest& digest) const {
  const std::string& canonical_digest = digest.str();
  const std::string artifact_digest = canonical_digest.substr(kSha256Prefix.size());
  const QString artifact_path = QDir(artifactsRoot()).absoluteFilePath(QString::fromLatin1(artifact_digest));
  if (!QFileInfo::exists(artifact_path)) {
    return unexpected(storeRejection(StoreRejectionCode::kMissingArtifact, "resident artifact is missing"));
  }
  return readResidentArtifact(artifact_path, artifact_digest);
}

Expected<StoredArtifact, StoreRejection> ArtifactStore::materializeArtifact(
    const StoredArtifact& artifact, const QString& destination, const std::atomic<bool>* cancel_requested,
    bool verify_copy_hashes) const {
  if (cancellationRequested(cancel_requested)) {
    return unexpected(storeRejection(StoreRejectionCode::kCancelled, "artifact materialization was cancelled"));
  }
  const QFileInfoList existing =
      QDir(destination).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
  if ((!QFileInfo::exists(destination) && !QDir().mkpath(destination)) || !existing.isEmpty()) {
    return unexpected(
        storeRejection(StoreRejectionCode::kIoFailure, "artifact materialization destination is not empty"));
  }
  // No pre-copy re-verification: this overload is for an object the caller just
  // committed under this session's exclusive lease, whose file list is already
  // known good. copyResidentFiles still rehashes the copy, catching real I/O
  // corruption on the way out.
  QString materialization_root = destination;
  if (artifact.stripped_archive_root.has_value()) {
    materialization_root = QDir(destination)
                               .absoluteFilePath(
                                   QString::fromUtf8(
                                       artifact.stripped_archive_root->data(),
                                       static_cast<qsizetype>(artifact.stripped_archive_root->size())));
  }
  if (auto copied = copyResidentFiles(
          artifact.artifact_path, materialization_root, artifact.files, cancel_requested, verify_copy_hashes);
      !copied) {
    return unexpected(copied.error());
  }
  return artifact;
}

void ArtifactStore::setExtractionWriteHookForTesting(
    std::function<bool(const std::filesystem::path&, const char*, size_t)> hook) {
  extraction_write_hook_ = std::move(hook);
}

void ArtifactStore::setBeforeCommitHookForTesting(std::function<void(const std::filesystem::path&)> hook) {
  before_commit_hook_ = std::move(hook);
}

void ArtifactStore::setCommitFaultForTesting(ArtifactStoreCommitFault fault) noexcept {
  commit_fault_ = fault;
}

}  // namespace PJ
