#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QLatin1StringView>
#include <QString>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PJ::detail {

// Multi-instance fanout extension: a DataSource plugin's accepted config may
// declare a top-level `__pj_fanout` array (each entry a complete config JSON
// string) to ask the host to spawn N separate plugin instances — one per
// entry, each with its own DatasetId. The doubled-underscore prefix marks the
// key as host-private so it cannot collide with plugin-native fields.
//
// `__pj_saved_filepath` is a second host-private key. During layout replay the
// host writes it with the absolute filepath it replaced (on the outer object
// and on each fanout child, each carrying its own old value), only when the old
// value is an absolute filesystem path (no "://") that differs from the new one
// after QDir::cleanPath. Plugins may read it to rebase
// auxiliary files that lived beside the old source. It is transient: the host
// strips it where it could enter host state (a plugin's saveConfig, a layout
// file), and plugins must never write it from saveConfig.
//
// Returns the list of per-instance config strings. If the key is absent or the
// config is not the expected shape, returns `{ config }` so the host runs a
// single-instance import — this preserves back-compat with configs persisted
// before fanout existed. Non-string array entries are skipped (and logged); if
// that leaves nothing usable, the single-instance fallback applies.
std::vector<std::string> extractFanout(std::string_view config);

// Per-fanout entry hint for the dataset display name. The plugin emits this key
// (`display_suffix`) on each fanout config so the host can build a unique
// catalog label like "<basename>/<suffix>" without having to understand the
// plugin's domain. Falls back to `fallback` on any miss.
QString parseDisplaySuffix(std::string_view cfg, const QString& fallback);

// Top-level dataset display-name hint (issue #98). A DataSource plugin emits the
// `display_name` string key in its accepted/saved config to override the
// file-derived catalog/tree root label — the part the host would otherwise take
// from the selected filename (e.g. selecting `info.json` shows `info`). The host
// uses it as the base name (single-instance label, or the prefix combined with
// `display_suffix` in fanout). Sibling of `display_suffix`: plugin-owned, so no
// host-private `__pj_` prefix. Returns an empty QString if the key is absent,
// not a string, or empty — callers then keep the filename default, so plugins
// that don't emit it are unaffected (back-compat).
QString parseDisplayName(std::string_view cfg);

inline constexpr QLatin1StringView kSavedFilepathKey{"__pj_saved_filepath"};

enum class SavedFilepath { kOmit, kRecord };

// Layout replay (desktop and browser) must point every filepath that belongs to
// a source at the path actually loaded, so a moved or copied layout folder
// replays the data beside it. Besides the top-level field, fan-out configs carry
// complete child configs as JSON strings inside `__pj_fanout`; rewrite those
// recursively while leaving unrelated nested objects and malformed or
// non-string fan-out entries as they are. An empty config becomes
// `{"filepath":fresh}`. A non-empty config that is not a JSON object yields
// nullopt: the caller fails the source rather than replace what the user saved.
// `saved` selects whether the replaced path is recorded under kSavedFilepathKey
// (see its rule above); any incoming value of that key is always dropped.
[[nodiscard]] std::optional<std::string> rewriteReplayFilepaths(
    std::string_view config, const QString& fresh_path, SavedFilepath saved);

// Removes kSavedFilepathKey from the outer object and from each fan-out child.
// Configs that do not contain the key, or are not JSON objects, come back
// byte-for-byte.
[[nodiscard]] std::string stripHostReplayKeys(std::string_view config);

}  // namespace PJ::detail
