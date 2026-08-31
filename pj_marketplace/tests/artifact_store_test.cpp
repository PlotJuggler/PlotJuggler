// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_marketplace/artifact_store.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "pj_marketplace/sha256.hpp"
#include "pj_marketplace/store_session.hpp"

namespace PJ {
namespace {

using ArchiveMode = decltype(archive_entry_filetype(nullptr));

struct ZipEntry {
  ZipEntry(
      std::string entry_path, QByteArray entry_contents, ArchiveMode entry_type = AE_IFREG,
      std::string entry_link_target = {})
      : path(std::move(entry_path)),
        contents(std::move(entry_contents)),
        file_type(entry_type),
        link_target(std::move(entry_link_target)) {}

  std::string path;
  QByteArray contents;
  ArchiveMode file_type = AE_IFREG;
  std::string link_target;
};

QByteArray buildZip(const std::vector<ZipEntry>& entries) {
  std::vector<char> buffer(4U * 1024U * 1024U);
  size_t used = 0;

  auto archive_deleter = [](struct archive* archive_handle) { archive_write_free(archive_handle); };
  std::unique_ptr<struct archive, decltype(archive_deleter)> archive_handle(archive_write_new(), archive_deleter);
  EXPECT_EQ(archive_write_set_format_zip(archive_handle.get()), ARCHIVE_OK);
  EXPECT_EQ(archive_write_add_filter_none(archive_handle.get()), ARCHIVE_OK);
  EXPECT_EQ(archive_write_open_memory(archive_handle.get(), buffer.data(), buffer.size(), &used), ARCHIVE_OK);

  auto entry_deleter = [](struct archive_entry* entry) { archive_entry_free(entry); };
  std::unique_ptr<struct archive_entry, decltype(entry_deleter)> archive_entry_handle(
      archive_entry_new(), entry_deleter);

  for (const ZipEntry& fixture : entries) {
    archive_entry_clear(archive_entry_handle.get());
    archive_entry_set_pathname(archive_entry_handle.get(), fixture.path.c_str());
    // libarchive's ZIP writer refuses device nodes even though readers can
    // encounter them through Unix external attributes. Write that fixture as
    // an empty regular entry, then patch its central-directory mode below.
    const bool patch_as_device = fixture.file_type == AE_IFCHR || fixture.file_type == AE_IFBLK;
    archive_entry_set_filetype(archive_entry_handle.get(), patch_as_device ? AE_IFREG : fixture.file_type);
    archive_entry_set_perm(archive_entry_handle.get(), fixture.file_type == AE_IFDIR ? 0755 : 0644);
    if (fixture.file_type == AE_IFLNK) {
      archive_entry_set_symlink(archive_entry_handle.get(), fixture.link_target.c_str());
      archive_entry_set_size(archive_entry_handle.get(), 0);
    } else if (fixture.file_type == AE_IFREG) {
      archive_entry_set_size(archive_entry_handle.get(), fixture.contents.size());
    } else {
      archive_entry_set_size(archive_entry_handle.get(), 0);
      archive_entry_set_rdevmajor(archive_entry_handle.get(), 1);
      archive_entry_set_rdevminor(archive_entry_handle.get(), 3);
    }
    EXPECT_EQ(archive_write_header(archive_handle.get(), archive_entry_handle.get()), ARCHIVE_OK) << fixture.path;
    if (fixture.file_type == AE_IFREG && !fixture.contents.isEmpty()) {
      EXPECT_EQ(
          archive_write_data(
              archive_handle.get(), fixture.contents.constData(), static_cast<size_t>(fixture.contents.size())),
          fixture.contents.size());
    }
  }

  EXPECT_EQ(archive_write_close(archive_handle.get()), ARCHIVE_OK);
  QByteArray result(buffer.data(), static_cast<qsizetype>(used));
  for (const ZipEntry& fixture : entries) {
    if (fixture.file_type != AE_IFCHR && fixture.file_type != AE_IFBLK) {
      continue;
    }
    const qsizetype central_header = result.indexOf(QByteArray::fromHex("504b0102"));
    EXPECT_GE(central_header, 0);
    if (central_header < 0 || central_header + 42 >= result.size()) {
      continue;
    }
    result[central_header + 5] = 3;  // "version made by" host: Unix
    const uint32_t external_attributes = static_cast<uint32_t>(fixture.file_type | 0644) << 16U;
    for (size_t byte_index = 0; byte_index < sizeof(external_attributes); ++byte_index) {
      result[central_header + 38 + static_cast<qsizetype>(byte_index)] =
          static_cast<char>(external_attributes >> (byte_index * 8U));
    }
  }
  return result;
}

QString writeArchive(QTemporaryDir& directory, const QByteArray& bytes, const QString& name = "fixture.zip") {
  const QString path = QDir(directory.path()).absoluteFilePath(name);
  QFile output(path);
  EXPECT_TRUE(output.open(QIODevice::WriteOnly));
  EXPECT_EQ(output.write(bytes), bytes.size());
  output.close();
  return path;
}

StoreQuotas smallQuotas() {
  StoreQuotas quotas;
  quotas.maximum_compressed_bytes = 1024U * 1024U;
  quotas.maximum_entry_expanded_bytes = 64U * 1024U;
  quotas.maximum_total_expanded_bytes = 128U * 1024U;
  quotas.maximum_file_count = 16;
  quotas.maximum_path_segments = 6;
  return quotas;
}

QStringList artifactDirectories(const ArtifactStore& store) {
  return QDir(store.artifactsRoot()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
}

QStringList extractionResidue(const ArtifactStore& store) {
  return QDir(store.extractionRoot()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
}

class ArtifactStoreTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(root_.isValid());
    ASSERT_TRUE(source_.isValid());
    auto opened = StoreSession::open(root_.path());
    ASSERT_TRUE(opened) << opened.error().message;
    session_ = std::make_unique<StoreSession>(std::move(*opened));
    ASSERT_TRUE(session_->hasStoreWriteAccess());
  }

  VerifiedArtifact makeArtifact(const QByteArray& bytes, const QString& name = "fixture.zip") {
    const std::string digest_text = sha256Hex(std::string_view(bytes.constData(), static_cast<size_t>(bytes.size())));
    auto digest = GenerationDigest::parse(digest_text);
    EXPECT_TRUE(digest) << digest.error().message;
    return {writeArchive(source_, bytes, name), *digest};
  }

  QTemporaryDir root_;
  QTemporaryDir source_;
  std::unique_ptr<StoreSession> session_;
};

/// The store's content-manifest identity for a single-file tree, mirrored here
/// so tests can predict object directory names.
QString manifestDigestFor(std::string_view relative_path, std::string_view content) {
  Sha256 aggregate;
  aggregate.update(relative_path);
  aggregate.update(std::string_view("\0", 1));
  aggregate.update("sha256:" + sha256Hex(content));
  aggregate.update("\n");
  return QString::fromStdString(aggregate.finishHex());
}

void writeFile(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  ASSERT_EQ(file.write(bytes), bytes.size());
}

TEST_F(ArtifactStoreTest, ExpansionBombRejectedBeforeCommit) {
  StoreQuotas quotas = smallQuotas();
  quotas.maximum_entry_expanded_bytes = 512;
  quotas.maximum_total_expanded_bytes = 512;
  const VerifiedArtifact artifact = makeArtifact(buildZip({{"bomb.bin", QByteArray(4096, '\0')}}));

  ArtifactStore store(*session_, quotas);
  const auto result = store.commitArchive(artifact);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StoreRejectionCode::kLimitExceeded);
  EXPECT_TRUE(artifactDirectories(store).isEmpty());
  EXPECT_TRUE(extractionResidue(store).isEmpty());
}

TEST_F(ArtifactStoreTest, RunningExpandedSizeCapRejectedBeforeCommit) {
  StoreQuotas quotas = smallQuotas();
  quotas.maximum_entry_expanded_bytes = 4096;
  quotas.maximum_total_expanded_bytes = 700;
  const VerifiedArtifact artifact =
      makeArtifact(buildZip({{"first.bin", QByteArray(400, 'a')}, {"second.bin", QByteArray(400, 'b')}}));

  ArtifactStore store(*session_, quotas);
  const auto result = store.commitArchive(artifact);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StoreRejectionCode::kLimitExceeded);
  EXPECT_TRUE(artifactDirectories(store).isEmpty());
  EXPECT_TRUE(extractionResidue(store).isEmpty());
}

TEST_F(ArtifactStoreTest, SymlinkAndDeviceEntriesAreRejectedByTypePolicy) {
  for (const ZipEntry& rejected : {
           ZipEntry{"plugin/link", {}, AE_IFLNK, "target"},
           ZipEntry{"plugin/device", {}, AE_IFCHR, {}},
       }) {
    const VerifiedArtifact artifact = makeArtifact(buildZip({rejected}));
    ArtifactStore store(*session_, smallQuotas());

    const auto result = store.commitArchive(artifact);

    ASSERT_FALSE(result) << rejected.path;
    EXPECT_EQ(result.error().code, StoreRejectionCode::kUnsupportedEntryType) << rejected.path;
    EXPECT_TRUE(artifactDirectories(store).isEmpty()) << rejected.path;
  }
}

TEST_F(ArtifactStoreTest, HardlinkPolicyFailsClosed) {
  const auto result = validateArchiveEntryType(ArchiveEntryType::kRegularFile, true);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StoreRejectionCode::kUnsupportedEntryType);
}

TEST_F(ArtifactStoreTest, PathTraversalAbsoluteAndDotDotEntriesAreRejected) {
  for (const char* path_text : {"../evil", "dir/../../evil", "/absolute", "C:/absolute", "dir\\..\\evil"}) {
    const std::string path(path_text);
    const VerifiedArtifact artifact = makeArtifact(buildZip({{path, "payload"}}));
    ArtifactStore store(*session_, smallQuotas());

    const auto result = store.commitArchive(artifact);

    ASSERT_FALSE(result) << path;
    EXPECT_EQ(result.error().code, StoreRejectionCode::kUnsafePath) << path;
    EXPECT_TRUE(artifactDirectories(store).isEmpty()) << path;
  }
}

TEST_F(ArtifactStoreTest, ReservedManifestExposedByRootStrippingIsRejectedBeforeCommit) {
  const VerifiedArtifact artifact =
      makeArtifact(buildZip({{"plugin/.pj-artifact.json", "payload-controlled manifest"}}));
  ArtifactStore store(*session_, smallQuotas());

  const auto result = store.commitArchive(artifact);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StoreRejectionCode::kUnsafePath);
  EXPECT_TRUE(artifactDirectories(store).isEmpty());
  EXPECT_TRUE(extractionResidue(store).isEmpty());
}

TEST_F(ArtifactStoreTest, FileCountCapRejectedBeforeCommit) {
  StoreQuotas quotas = smallQuotas();
  quotas.maximum_file_count = 2;
  const VerifiedArtifact artifact = makeArtifact(buildZip({{"one", "1"}, {"two", "2"}, {"three", "3"}}));
  ArtifactStore store(*session_, quotas);

  const auto result = store.commitArchive(artifact);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StoreRejectionCode::kLimitExceeded);
  EXPECT_TRUE(artifactDirectories(store).isEmpty());
}

TEST_F(ArtifactStoreTest, PathDepthCapRejectedBeforeCommit) {
  StoreQuotas quotas = smallQuotas();
  quotas.maximum_path_segments = 3;
  const VerifiedArtifact artifact = makeArtifact(buildZip({{"one/two/three/four.txt", "deep"}}));
  ArtifactStore store(*session_, quotas);

  const auto result = store.commitArchive(artifact);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StoreRejectionCode::kLimitExceeded);
  EXPECT_TRUE(artifactDirectories(store).isEmpty());
}

TEST_F(ArtifactStoreTest, CompressedSizeCapRejectedBeforeCommit) {
  const QByteArray bytes = buildZip({{"payload", QByteArray(2048, 'x')}});
  const VerifiedArtifact artifact = makeArtifact(bytes);
  StoreQuotas quotas = smallQuotas();
  quotas.maximum_compressed_bytes = static_cast<uint64_t>(bytes.size() - 1);
  ArtifactStore store(*session_, quotas);

  const auto result = store.commitArchive(artifact);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StoreRejectionCode::kLimitExceeded);
  EXPECT_TRUE(artifactDirectories(store).isEmpty());
  EXPECT_TRUE(extractionResidue(store).isEmpty());
}

TEST_F(ArtifactStoreTest, DiskFullDuringExtractionLeavesNoPartialArtifactAndCleansStaging) {
  const VerifiedArtifact artifact = makeArtifact(buildZip({{"plugin/payload", QByteArray(4096, 'x')}}));
  ArtifactStore store(*session_, smallQuotas());
  store.setExtractionWriteHookForTesting([](const std::filesystem::path&, const char*, size_t) { return false; });

  const auto result = store.commitArchive(artifact);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StoreRejectionCode::kIoFailure);
  EXPECT_TRUE(artifactDirectories(store).isEmpty());
  EXPECT_TRUE(extractionResidue(store).isEmpty());
}

TEST_F(ArtifactStoreTest, PostExtractionMutationDetectedBeforeCommit) {
  const VerifiedArtifact artifact = makeArtifact(buildZip({{"plugin/payload", "original"}}));
  ArtifactStore store(*session_, smallQuotas());
  store.setBeforeCommitHookForTesting([](const std::filesystem::path& staging_directory) {
    QFile payload(QString::fromStdString((staging_directory / "payload").string()));
    ASSERT_TRUE(payload.open(QIODevice::Append));
    ASSERT_EQ(payload.write("mutation"), 8);
  });

  const auto result = store.commitArchive(artifact);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StoreRejectionCode::kPostExtractionMutation);
  EXPECT_TRUE(artifactDirectories(store).isEmpty());
  EXPECT_TRUE(extractionResidue(store).isEmpty());
}

TEST_F(ArtifactStoreTest, SameDigestReCommitIsIdempotent) {
  const QByteArray bytes = buildZip({{"plugin/payload", "same"}});
  const VerifiedArtifact first_artifact = makeArtifact(bytes, "first.zip");
  const VerifiedArtifact second_artifact = makeArtifact(bytes, "second.zip");
  ArtifactStore store(*session_, smallQuotas());

  const auto first = store.commitArchive(first_artifact);
  const auto second = store.commitArchive(second_artifact);

  ASSERT_TRUE(first) << first.error().message;
  ASSERT_TRUE(second) << second.error().message;
  EXPECT_FALSE(first->already_present);
  EXPECT_TRUE(second->already_present);
  EXPECT_EQ(first->artifact_digest, second->artifact_digest);
  EXPECT_EQ(first->artifact_path, second->artifact_path);
  ASSERT_EQ(first->files.size(), 1);
  EXPECT_EQ(first->files.front().relative_path, "payload");
  EXPECT_EQ(first->files.front().sha256, sha256Hex("same"));
  EXPECT_EQ(first->artifact_digest, manifestDigestFor("payload", "same").toStdString());
  ASSERT_TRUE(first->stripped_archive_root.has_value());
  EXPECT_EQ(*first->stripped_archive_root, "plugin");
  EXPECT_TRUE(QFile::exists(QDir(first->artifact_path).absoluteFilePath("payload")));
  EXPECT_FALSE(QFileInfo::exists(QDir(first->artifact_path).absoluteFilePath("plugin")));
  EXPECT_EQ(artifactDirectories(store).size(), 1);
}

TEST_F(ArtifactStoreTest, DedupedArchiveCommitRewrapsMaterialization) {
  const QByteArray bytes = buildZip({{"plugin/payload", "same"}});
  ArtifactStore store(*session_, smallQuotas());

  const auto first = store.commitArchive(makeArtifact(bytes, "first.zip"));
  const auto second = store.commitArchive(makeArtifact(bytes, "second.zip"));

  ASSERT_TRUE(first) << first.error().message;
  ASSERT_TRUE(second) << second.error().message;
  EXPECT_FALSE(first->already_present);
  EXPECT_TRUE(second->already_present);
  EXPECT_EQ(first->artifact_digest, second->artifact_digest);
  EXPECT_EQ(artifactDirectories(store).size(), 1);
  ASSERT_TRUE(second->stripped_archive_root.has_value());
  EXPECT_EQ(*second->stripped_archive_root, "plugin");

  QTemporaryDir materialized;
  ASSERT_TRUE(materialized.isValid());
  const auto copied = store.materializeArtifact(*second, materialized.path());

  ASSERT_TRUE(copied) << copied.error().message;
  EXPECT_TRUE(QFile::exists(QDir(materialized.path()).absoluteFilePath("plugin/payload")));
  EXPECT_FALSE(QFileInfo::exists(QDir(materialized.path()).absoluteFilePath("payload")));
}

TEST_F(ArtifactStoreTest, SimilarTopLevelNamesAreNotMistakenForOneWrapper) {
  const VerifiedArtifact artifact = makeArtifact(buildZip({{"foo/payload", "one"}, {"foo-bar/payload", "two"}}));
  ArtifactStore store(*session_, smallQuotas());

  const auto result = store.commitArchive(artifact);

  ASSERT_TRUE(result) << result.error().message;
  EXPECT_FALSE(result->stripped_archive_root.has_value());
  EXPECT_TRUE(
      std::ranges::any_of(result->files, [](const StoredFile& file) { return file.relative_path == "foo/payload"; }));
  EXPECT_TRUE(std::ranges::any_of(result->files, [](const StoredFile& file) {
    return file.relative_path == "foo-bar/payload";
  }));
  EXPECT_TRUE(QFile::exists(QDir(result->artifact_path).absoluteFilePath("foo/payload")));
  EXPECT_TRUE(QFile::exists(QDir(result->artifact_path).absoluteFilePath("foo-bar/payload")));
}

TEST_F(ArtifactStoreTest, MultipleTopLevelDirectoriesRemainUnrootedForDownstreamRejection) {
  const VerifiedArtifact artifact = makeArtifact(buildZip({{"first/payload", "one"}, {"second/payload", "two"}}));
  ArtifactStore store(*session_, smallQuotas());

  const auto result = store.commitArchive(artifact);

  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->files.size(), 2U);
  EXPECT_EQ(result->files[0].relative_path, "first/payload");
  EXPECT_EQ(result->files[1].relative_path, "second/payload");
  EXPECT_FALSE(result->stripped_archive_root.has_value());
  EXPECT_TRUE(QFile::exists(QDir(result->artifact_path).absoluteFilePath("first/payload")));
  EXPECT_TRUE(QFile::exists(QDir(result->artifact_path).absoluteFilePath("second/payload")));
}

TEST_F(ArtifactStoreTest, ResidentDirectoryIsAdoptedThroughTheWriteOnceArtifactPath) {
  const QString resident = QDir(source_.path()).absoluteFilePath("legacy-extension");
  ASSERT_TRUE(QDir().mkpath(QDir(resident).absoluteFilePath("lib")));
  QFile payload(QDir(resident).absoluteFilePath("lib/plugin.bin"));
  ASSERT_TRUE(payload.open(QIODevice::WriteOnly));
  ASSERT_EQ(payload.write("resident bytes"), 14);
  payload.close();
  ArtifactStore store(*session_, smallQuotas());

  const auto first = store.adoptDirectory(resident);
  const auto second = store.adoptDirectory(resident);

  ASSERT_TRUE(first) << first.error().message;
  ASSERT_TRUE(second) << second.error().message;
  EXPECT_FALSE(first->already_present);
  EXPECT_TRUE(second->already_present);
  EXPECT_EQ(first->artifact_digest, second->artifact_digest);
  EXPECT_EQ(first->files, second->files);
  ASSERT_EQ(first->files.size(), 1U);
  EXPECT_EQ(first->files.front().relative_path, "lib/plugin.bin");
  EXPECT_EQ(first->files.front().sha256, sha256Hex("resident bytes"));
  EXPECT_TRUE(QFile::exists(QDir(first->artifact_path).absoluteFilePath("lib/plugin.bin")));
  EXPECT_EQ(artifactDirectories(store).size(), 1);
}

TEST_F(ArtifactStoreTest, CorruptResidentArtifactIsRejectedOnReCommit) {
  const QByteArray bytes = buildZip({{"plugin/payload", "same"}});
  const VerifiedArtifact first_artifact = makeArtifact(bytes, "first.zip");
  const VerifiedArtifact second_artifact = makeArtifact(bytes, "second.zip");
  ArtifactStore store(*session_, smallQuotas());

  const auto first = store.commitArchive(first_artifact);
  ASSERT_TRUE(first) << first.error().message;
  QFile resident_payload(QDir(first->artifact_path).absoluteFilePath("payload"));
  ASSERT_TRUE(resident_payload.open(QIODevice::Append));
  ASSERT_EQ(resident_payload.write("corruption"), 10);
  resident_payload.close();

  const auto second = store.commitArchive(second_artifact);

  ASSERT_FALSE(second);
  EXPECT_EQ(second.error().code, StoreRejectionCode::kCorruptResidentArtifact);
  EXPECT_EQ(artifactDirectories(store).size(), 1);
}

TEST_F(ArtifactStoreTest, ByteDistinctArtifactGetsDistinctPath) {
  const VerifiedArtifact first_artifact = makeArtifact(buildZip({{"plugin/payload", "first"}}), "first.zip");
  const VerifiedArtifact second_artifact = makeArtifact(buildZip({{"plugin/payload", "second"}}), "second.zip");
  ArtifactStore store(*session_, smallQuotas());

  const auto first = store.commitArchive(first_artifact);
  const auto second = store.commitArchive(second_artifact);

  ASSERT_TRUE(first) << first.error().message;
  ASSERT_TRUE(second) << second.error().message;
  EXPECT_NE(first->artifact_digest, second->artifact_digest);
  EXPECT_NE(first->artifact_path, second->artifact_path);
  EXPECT_EQ(artifactDirectories(store).size(), 2);
}

TEST_F(ArtifactStoreTest, CollectUnreferencedRemovesOnlyOrphanObjects) {
  ArtifactStore store(*session_, smallQuotas());
  const auto kept = store.commitArchive(makeArtifact(buildZip({{"plugin/payload", "kept"}}), "kept.zip"));
  const auto orphan = store.commitArchive(makeArtifact(buildZip({{"plugin/payload", "orphan"}}), "orphan.zip"));
  ASSERT_TRUE(kept) << kept.error().message;
  ASSERT_TRUE(orphan) << orphan.error().message;
  ASSERT_EQ(artifactDirectories(store).size(), 2);

  // Keep only the first object's digest as live; the second is the orphan.
  const auto live = GenerationDigest::parse(kept->artifact_digest);
  ASSERT_TRUE(live) << live.error().message;
  EXPECT_EQ(store.collectUnreferenced({*live}), 1u);

  const QStringList remaining = artifactDirectories(store);
  ASSERT_EQ(remaining.size(), 1);
  EXPECT_EQ(remaining.front(), QString::fromStdString(objectDirectoryNameFor(*live)));
}

TEST_F(ArtifactStoreTest, CollectUnreferencedLeavesScratchAndStrayEntriesUntouched) {
  ArtifactStore store(*session_, smallQuotas());
  ASSERT_TRUE(store.commitArchive(makeArtifact(buildZip({{"plugin/payload", "bytes"}}), "a.zip")));

  // An in-flight scratch directory and a stray non-digest name must survive even a
  // sweep with an empty root set that removes every real object.
  const QString scratch = QDir(store.artifactsRoot()).absoluteFilePath(QString(kArtifactScratchPrefix) + "inflight");
  const QString stray = QDir(store.artifactsRoot()).absoluteFilePath(QStringLiteral("not-a-digest"));
  ASSERT_TRUE(QDir().mkpath(scratch));
  ASSERT_TRUE(QDir().mkpath(stray));

  EXPECT_EQ(store.collectUnreferenced({}), 1u);
  EXPECT_TRUE(QDir(scratch).exists());
  EXPECT_TRUE(QDir(stray).exists());
}

TEST_F(ArtifactStoreTest, ComponentsShareOneLeaseAndContendedSessionIsReadOnly) {
  const QByteArray bytes = buildZip({{"plugin/payload", "bytes"}});
  const VerifiedArtifact artifact = makeArtifact(bytes);
  ArtifactStore first(*session_, smallQuotas());
  ArtifactStore second(*session_, smallQuotas());

  const auto first_result = first.commitArchive(artifact);
  const auto second_result = second.commitArchive(artifact);

  ASSERT_TRUE(first_result) << first_result.error().message;
  ASSERT_TRUE(second_result) << second_result.error().message;
  EXPECT_TRUE(second_result->already_present);

  auto contended = StoreSession::open(root_.path());
  ASSERT_TRUE(contended) << contended.error().message;
  EXPECT_FALSE(contended->hasStoreWriteAccess());
  ArtifactStore read_only(*contended, smallQuotas());
  const auto denied = read_only.commitArchive(artifact);
  ASSERT_FALSE(denied);
  EXPECT_EQ(denied.error().code, StoreRejectionCode::kWriterLeaseUnavailable);
}

TEST_F(ArtifactStoreTest, CancelledCommitPublishesNoArtifact) {
  const QByteArray bytes = buildZip({{"plugin/payload", "bytes"}});
  const VerifiedArtifact artifact = makeArtifact(bytes);
  session_.reset();
  auto acquired = ArtifactStore::acquire(root_.path(), smallQuotas());
  ASSERT_TRUE(acquired) << acquired.error().message;
  const std::atomic<bool> cancelled = true;

  const auto result = (*acquired)->commitArchive(artifact, &cancelled);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StoreRejectionCode::kCancelled);
  EXPECT_TRUE(artifactDirectories(**acquired).isEmpty());
}

TEST_F(ArtifactStoreTest, CrashBeforeRenameLeavesNoArtifactVisible) {
  const VerifiedArtifact artifact = makeArtifact(buildZip({{"plugin/payload", "bytes"}}));
  ArtifactStore store(*session_, smallQuotas());
  store.setCommitFaultForTesting(ArtifactStoreCommitFault::kBeforeRename);

  const auto result = store.commitArchive(artifact);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StoreRejectionCode::kIoFailure);
  EXPECT_TRUE(artifactDirectories(store).isEmpty());
  EXPECT_TRUE(extractionResidue(store).isEmpty());
}

TEST_F(ArtifactStoreTest, CrashAfterRenameLeavesCompleteArtifact) {
  const QByteArray bytes = buildZip({{"plugin/payload", "bytes"}});
  const VerifiedArtifact artifact = makeArtifact(bytes);
  ArtifactStore store(*session_, smallQuotas());
  store.setCommitFaultForTesting(ArtifactStoreCommitFault::kAfterRename);

  const auto result = store.commitArchive(artifact);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StoreRejectionCode::kIoFailure);
  const QString artifact_path = QDir(store.artifactsRoot()).absoluteFilePath(manifestDigestFor("payload", "bytes"));
  EXPECT_TRUE(QFile::exists(QDir(artifact_path).absoluteFilePath("payload")));
  EXPECT_TRUE(QFile::exists(QDir(artifact_path).absoluteFilePath(".pj-artifact.json")));
  EXPECT_EQ(artifactDirectories(store).size(), 1);
  EXPECT_TRUE(extractionResidue(store).isEmpty());
}

TEST_F(ArtifactStoreTest, AdoptedAndDownloadedIdenticalBytesShareOneDigest) {
  // The published identity is the content manifest, so the same bytes have the
  // same identity no matter how they arrived — adopt-then-install (or the
  // reverse) dedupes instead of colliding as a release mutation.
  const QByteArray bytes = buildZip({{"plugin/payload", "bytes"}});
  ArtifactStore store(*session_, smallQuotas());
  const auto committed = store.commitArchive(makeArtifact(bytes));
  ASSERT_TRUE(committed) << committed.error().message;
  EXPECT_FALSE(committed->transport_digest.empty());

  QTemporaryDir resident;
  ASSERT_TRUE(resident.isValid());
  writeFile(QDir(resident.path()).absoluteFilePath("payload"), "bytes");
  const auto adopted = store.adoptDirectory(resident.path());
  ASSERT_TRUE(adopted) << adopted.error().message;

  EXPECT_EQ(adopted->artifact_digest, committed->artifact_digest);
  EXPECT_TRUE(adopted->already_present);
  EXPECT_TRUE(adopted->transport_digest.empty());
  EXPECT_EQ(artifactDirectories(store).size(), 1);
}

}  // namespace
}  // namespace PJ
