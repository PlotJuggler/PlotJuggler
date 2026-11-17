#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QHash>
#include <QLatin1StringView>
#include <QString>
#include <QStringList>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PJ::detail {

// Plugin config contract. `__pj_fanout`: a top-level array of complete config JSON strings, one plugin instance and
// DatasetId per entry. `__pj_saved_filepath`: written by the host on layout replay (outer object and each fan-out
// child, each with its own old value) with the filepath it replaced, only when that was an absolute filesystem path
// (no "://") different from the new one after QDir::cleanPath; plugins may read it to rebase sidecar files. It is
// transient: the host strips it from saveConfig output and layouts, and plugins must not write it. The `__pj_`
// prefix keeps both apart from plugin fields.
//
// The per-instance configs; `{ config }` when the key is absent or malformed. Non-string entries are logged and
// skipped; none left means `{ config }`.
std::vector<std::string> extractFanout(std::string_view config);

// A fan-out entry's `display_suffix`, which names it "<base>/<suffix>"; `fallback` when missing or empty.
QString parseDisplaySuffix(std::string_view cfg, const QString& fallback);

// The config's top-level `display_name`, which replaces the file basename as the dataset's tree root (and as the
// fan-out prefix). Empty when absent or not a string: the caller keeps the basename.
QString parseDisplayName(std::string_view cfg);

inline constexpr QLatin1StringView kSavedFilepathKey{"__pj_saved_filepath"};

enum class SavedFilepath { kOmit, kRecord };

// Layout replay: points the top-level `filepath` and each fan-out child's at `fresh_path`, so a moved layout replays
// the data beside it; other nested objects and malformed children stay as they are. An empty config becomes
// `{"filepath":fresh}`; a non-empty non-object yields nullopt (the caller fails the source rather than replace what
// the user saved). `saved` decides whether the replaced path is recorded under kSavedFilepathKey; an incoming value
// of that key is always dropped.
[[nodiscard]] std::optional<std::string> rewriteReplayFilepaths(
    std::string_view config, const QString& fresh_path, SavedFilepath saved);

// Removes kSavedFilepathKey from the outer object and each fan-out child. A config without the key, or not an
// object, comes back byte-for-byte.
[[nodiscard]] std::string stripHostReplayKeys(std::string_view config);

// Every string value (never a key) at any depth, including inside each fan-out child that parses as an object (not
// the child's JSON text itself). Each value once, in a stable order: keys sorted, fan-out children last. Empty for a
// non-object config.
[[nodiscard]] QStringList configStringValues(std::string_view config);

// Replaces every string value equal to a key of `replacements` (configStringValues' scope); substrings are left
// alone. An empty config stays empty; a non-empty non-object yields nullopt.
[[nodiscard]] std::optional<std::string> replaceConfigStringValues(
    std::string_view config, const QHash<QString, QString>& replacements);

}  // namespace PJ::detail
