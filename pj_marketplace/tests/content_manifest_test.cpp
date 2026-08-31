// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_marketplace/content_manifest.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include "pj_marketplace/sha256.hpp"

namespace {

// Writes `content` at `relative` below `root`, creating parents as needed.
void writeFile(const std::filesystem::path& root, const std::string& relative, const std::string& content) {
  const std::filesystem::path target = root / relative;
  std::filesystem::create_directories(target.parent_path());
  std::ofstream out(target, std::ios::binary);
  out << content;
}

// A directory that removes itself, so a failing test does not leave a tree behind.
class TempTree {
 public:
  TempTree() : root_(std::filesystem::temp_directory_path() / uniqueName()) {
    std::filesystem::create_directories(root_);
  }
  ~TempTree() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }
  TempTree(const TempTree&) = delete;
  TempTree& operator=(const TempTree&) = delete;

  const std::filesystem::path& path() const {
    return root_;
  }

 private:
  // No clock or RNG: a monotonic counter is enough to keep concurrent cases apart
  // and keeps the name reproducible when a failure has to be chased.
  static std::string uniqueName() {
    static int counter = 0;
    return "pj-content-manifest-" + std::to_string(++counter);
  }

  std::filesystem::path root_;
};

std::string digestOf(const std::filesystem::path& root, PJ::ContentManifestOptions options = {}) {
  const auto files = PJ::inspectContentManifest(root, options);
  EXPECT_TRUE(files.has_value());
  if (!files) {
    return {};
  }
  return PJ::aggregateContentManifestDigest(*files);
}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

TEST(ContentManifestTest, SameBytesInTwoTreesYieldTheSameDigest) {
  // The whole point of the identity: two independently produced copies of the
  // same extension have to be recognised as the same thing.
  TempTree first;
  TempTree second;
  for (const auto* root : {&first, &second}) {
    writeFile(root->path(), "manifest.json", R"({"id":"probe"})");
    writeFile(root->path(), "lib/plugin.so", "binary-payload");
  }

  EXPECT_EQ(digestOf(first.path()), digestOf(second.path()));
  EXPECT_FALSE(digestOf(first.path()).empty());
}

TEST(ContentManifestTest, OneChangedByteChangesTheDigest) {
  TempTree before;
  writeFile(before.path(), "lib/plugin.so", "binary-payload");
  const std::string original = digestOf(before.path());

  TempTree after;
  writeFile(after.path(), "lib/plugin.so", "binary-payloaD");

  EXPECT_NE(original, digestOf(after.path()));
}

TEST(ContentManifestTest, TheSameBytesUnderADifferentNameChangeTheDigest) {
  // Paths take part in the identity, so moving a file is not a no-op. Without
  // that, two extensions holding identical payloads at different layouts would
  // collide.
  TempTree first;
  writeFile(first.path(), "lib/plugin.so", "payload");
  TempTree second;
  writeFile(second.path(), "lib/other.so", "payload");

  EXPECT_NE(digestOf(first.path()), digestOf(second.path()));
}

TEST(ContentManifestTest, DigestIsIndependentOfCreationOrder) {
  // The walk order a filesystem reports is not guaranteed, so the aggregation
  // sorts. Creating the same set in the opposite order must still agree.
  TempTree first;
  writeFile(first.path(), "a.txt", "one");
  writeFile(first.path(), "z.txt", "two");

  TempTree second;
  writeFile(second.path(), "z.txt", "two");
  writeFile(second.path(), "a.txt", "one");

  EXPECT_EQ(digestOf(first.path()), digestOf(second.path()));
}

TEST(ContentManifestTest, TheFileListComesBackSorted) {
  // The digest can only be stable if the list is ordered before it is aggregated,
  // and the order a filesystem reports its entries in is not guaranteed. Asserted
  // on the postcondition rather than by creating files in a different order: on
  // any given filesystem both orders may well be walked the same way, which makes
  // such a test agree with a build that does not sort at all.
  TempTree tree;
  for (const char* name : {"z.txt", "a.txt", "m/nested.txt", "b.txt", "m/aaa.txt"}) {
    writeFile(tree.path(), name, "payload");
  }

  const auto files = PJ::inspectContentManifest(tree.path());
  ASSERT_TRUE(files.has_value());
  EXPECT_TRUE(std::ranges::is_sorted(*files, {}, &PJ::StoredFile::relative_path));
}

// ---------------------------------------------------------------------------
// Self-exclusion
// ---------------------------------------------------------------------------

TEST(ContentManifestTest, TheSkippedPathIsLeftOutOfTheIdentity) {
  // The file that records the digest lives inside the tree it describes, so it
  // has to be excluded from the walk. Otherwise recording it would change the
  // very value being recorded and no later check could ever agree.
  TempTree tree;
  writeFile(tree.path(), "lib/plugin.so", "payload");
  const std::string before_recording = digestOf(tree.path());

  writeFile(tree.path(), ".pj-content-manifest.json", "whatever-gets-written-here");

  PJ::ContentManifestOptions options;
  options.skipped_relative_path = ".pj-content-manifest.json";
  EXPECT_EQ(before_recording, digestOf(tree.path(), options));

  // And without the exclusion the recorded file does count, which is exactly the
  // trap: the digest of a tree changes the moment its own digest is stored.
  EXPECT_NE(before_recording, digestOf(tree.path()));
}

// ---------------------------------------------------------------------------
// Fail-closed cases
// ---------------------------------------------------------------------------

TEST(ContentManifestTest, ASymbolicLinkIsRefused) {
  // An identity computed over a link would describe the target, which is outside
  // the tree and can change underneath. Refused rather than followed.
  TempTree tree;
  writeFile(tree.path(), "lib/plugin.so", "payload");
  std::error_code ec;
  std::filesystem::create_symlink("/etc/passwd", tree.path() / "link", ec);
  ASSERT_FALSE(ec) << "could not create the symlink this case needs";

  const auto files = PJ::inspectContentManifest(tree.path());
  ASSERT_FALSE(files.has_value());
  EXPECT_EQ(files.error().code, PJ::ContentManifestRejectionCode::kUnsafeTree);
}

TEST(ContentManifestTest, AMissingTreeIsReportedNotAssumedEmpty) {
  const auto files = PJ::inspectContentManifest(std::filesystem::temp_directory_path() / "pj-does-not-exist");
  EXPECT_FALSE(files.has_value());
}

}  // namespace
