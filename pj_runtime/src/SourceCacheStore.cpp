// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/SourceCacheStore.h"

#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <system_error>

namespace PJ {
namespace {

namespace di = sdk::descriptor_import;

// "\x89MCAP0\r\n" — present at both ends of every well-formed MCAP file.
constexpr std::array<char, 8> kMcapMagic = {'\x89', 'M', 'C', 'A', 'P', '0', '\r', '\n'};
// Footer record: opcode 0x02, uint64 length (always 20), 20-byte payload
// whose first field is summary_start. Frame + trailing magic = the last 37
// bytes; with the leading magic the smallest well-formed file is 45 bytes.
constexpr std::size_t kFooterFrame = 1 + 8 + 20;
constexpr std::size_t kMinMcapBytes = kMcapMagic.size() * 2 + kFooterFrame;

std::uint64_t readU64Le(const char* bytes) {
  std::uint64_t value = 0;
  for (int i = 7; i >= 0; --i) {
    value = (value << 8) | static_cast<unsigned char>(bytes[i]);
  }
  return value;
}

// C4's cheap validation: plausibly sized, magic at both ends, and a sane
// footer frame (opcode 0x02, length 20, summary_start inside the file) so
// foreign bytes that merely end in the magic still fail cheaply. The digest
// in the filename is over the identity STRING, not the content, so this
// deliberately does not hash the body; a corrupt body that keeps this shape
// reaches the loader once and is then quarantined.
bool looksLikeMcap(const std::filesystem::path& file, const std::string&, std::string* error) {
  std::ifstream in(file, std::ios::binary);  // fs::path overload: wide-safe on Windows
  if (!in) {
    if (error) {
      *error = "unreadable";
    }
    return false;
  }
  in.seekg(0, std::ios::end);
  const auto size = static_cast<std::uint64_t>(in.tellg());
  std::array<char, 8> head{};
  std::array<char, kFooterFrame + 8> tail{};
  bool ok = size >= kMinMcapBytes && in.seekg(0).read(head.data(), head.size()).good() &&
            in.seekg(-static_cast<std::streamoff>(tail.size()), std::ios::end).read(tail.data(), tail.size()).good();
  ok = ok && std::equal(kMcapMagic.begin(), kMcapMagic.end(), head.begin()) &&
       std::equal(kMcapMagic.begin(), kMcapMagic.end(), tail.begin() + kFooterFrame) && tail[0] == '\x02' &&
       readU64Le(tail.data() + 1) == 20 && readU64Le(tail.data() + 9) < size;
  if (!ok && error) {
    *error = "not a complete MCAP file (bad size, magic or footer frame)";
  }
  return ok;
}

di::CacheSpec makeSpec(std::filesystem::path root) {
  di::CacheSpec spec;
  spec.root = std::move(root);
  spec.artifact_suffix = ".mcap";
  // The host digests the PROVIDER'S identity string: identities are opaque
  // here (C1), and hashing the string lets one cache serve every provider
  // without knowing any prefix. The digest width is named in the prefix (SDK
  // convention) so a future width change cannot orphan existing files.
  spec.identity.prefix = "pj:v1:sha256/128:";
  spec.identity.digest_hex_chars = 32;
  return spec;
}

// Provider identity → host identity under the store's own scheme.
std::string hostIdentity(const di::RequestArtifactCache& cache, std::string_view provider_identity) {
  if (provider_identity.empty()) {
    return {};
  }
  return cache.spec().identity.identityFor(provider_identity);
}

}  // namespace

SourceCacheStore::SourceCacheStore(std::filesystem::path root, std::uintmax_t budget_bytes)
    : cache_(makeSpec(std::move(root)), &looksLikeMcap), budget_bytes_(budget_bytes) {}

std::filesystem::path SourceCacheStore::defaultRoot() {
  const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  // Empty or relative (no QCoreApplication, misconfigured platform): fall
  // back to a temp-rooted cache rather than resolving against the cwd.
  std::filesystem::path base_path(base.toStdU16String());
  if (base_path.empty() || base_path.is_relative()) {
    base_path = std::filesystem::temp_directory_path() / "plotjuggler";
  }
  return base_path / "source_cache";
}

namespace {
constexpr auto kCacheDirectoryKey = "Preferences::source_cache_directory";
constexpr auto kCacheBudgetKey = "Preferences::source_cache_budget_gb";
// Preferences scrubber range; a hand-edited .ini cannot arm a petabyte cache.
constexpr int kMinBudgetGb = 1;
constexpr int kMaxBudgetGb = 1000;
}  // namespace

SourceCacheStore::Settings SourceCacheStore::loadSettings() {
  QSettings settings;
  Settings loaded;
  loaded.directory = settings.value(QString::fromLatin1(kCacheDirectoryKey)).toString();
  loaded.budget_gb = std::clamp(
      settings.value(QString::fromLatin1(kCacheBudgetKey), loaded.budget_gb).toInt(), kMinBudgetGb, kMaxBudgetGb);
  return loaded;
}

void SourceCacheStore::saveSettings(const Settings& settings) {
  QSettings store;
  store.setValue(QString::fromLatin1(kCacheDirectoryKey), settings.directory);
  store.setValue(QString::fromLatin1(kCacheBudgetKey), settings.budget_gb);
}

std::unique_ptr<SourceCacheStore> SourceCacheStore::fromSettings(const Settings& settings) {
  const std::filesystem::path root = settings.directory.trimmed().isEmpty()
                                         ? defaultRoot()
                                         : std::filesystem::path(settings.directory.toStdU16String());
  const auto budget =
      static_cast<std::uintmax_t>(std::clamp(settings.budget_gb, kMinBudgetGb, kMaxBudgetGb)) * 1'000'000'000ull;
  return std::make_unique<SourceCacheStore>(root, budget);
}

std::filesystem::path SourceCacheStore::pathFor(std::string_view identity) const {
  const std::string host_id = hostIdentity(cache_, identity);
  return host_id.empty() ? std::filesystem::path{} : cache_.pathFor(host_id);
}

std::optional<SourceCacheStore::Pinned> SourceCacheStore::lookup(std::string_view identity, std::string* miss_reason) {
  const std::string host_id = hostIdentity(cache_, identity);
  if (host_id.empty()) {
    if (miss_reason) {
      *miss_reason = "empty identity";
    }
    return std::nullopt;
  }
  auto hit = cache_.lookup(host_id, miss_reason);
  if (!hit) {
    return std::nullopt;
  }
  return Pinned{std::move(hit->path), std::move(hit->lease)};
}

Expected<SourceCacheStore::WriteTransaction, SourceCacheStore::CacheError> SourceCacheStore::beginPublish(
    std::string_view identity) {
  const std::string host_id = hostIdentity(cache_, identity);
  if (host_id.empty()) {
    return PJ::unexpected(CacheError{"empty identity", false});
  }
  return cache_.beginWrite(host_id);
}

Expected<SourceCacheStore::Pinned, SourceCacheStore::CacheError> SourceCacheStore::publish(
    std::string_view identity, WriteTransaction&& txn) {
  auto committed = txn.commit();
  if (committed) {
    return Pinned{std::move(committed->path), std::move(committed->lease)};
  }
  if (!committed.error().retryable) {
    return PJ::unexpected(committed.error());
  }
  // Retryable commit failure = the artifact IS published but the lock handoff
  // lost a race; the SDK contract prescribes exactly one lookup to pin it.
  std::string miss_reason;
  if (auto pinned = lookup(identity, &miss_reason)) {
    return std::move(*pinned);
  }
  return PJ::unexpected(CacheError{"published but not pinnable: " + miss_reason, true});
}

bool SourceCacheStore::quarantine(std::string_view identity, std::string* reason) {
  const std::string host_id = hostIdentity(cache_, identity);
  if (host_id.empty()) {
    if (reason) {
      *reason = "empty identity";
    }
    return false;
  }
  // The exclusive lock keeps a pinned or in-publish artifact untouched:
  // reported, never forced.
  auto txn = cache_.beginWrite(host_id);
  if (!txn) {
    if (reason) {
      *reason = txn.error().message;
    }
    return false;
  }
  const std::filesystem::path artifact = cache_.pathFor(host_id);
  std::filesystem::path touch = artifact;
  touch += ".touch";
  std::error_code ec;
  std::filesystem::remove(artifact, ec);
  std::error_code touch_ec;
  std::filesystem::remove(touch, touch_ec);  // stale LRU stamp goes with the file
  txn->abort();
  if (ec && reason) {
    *reason = ec.message();
  }
  return !ec;
}

SourceCacheStore::CleanupResult SourceCacheStore::cleanup() {
  di::CleanupPolicy policy;
  policy.max_total_bytes = budget_bytes_;
  return cache_.cleanup(policy);
}

const std::filesystem::path& SourceCacheStore::root() const noexcept {
  return cache_.spec().root;
}

}  // namespace PJ
