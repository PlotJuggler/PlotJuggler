#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDate>
#include <QString>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace PJ {

/// The closed set of app features whose use is counted. Only these reach a report, so a counter can
/// never carry free text (a topic, a path). Adding one means a wire key in usageFeatureKey() and a
/// row in docs/TELEMETRY.md.
enum class UsageFeature {
  kLayoutLoad,      ///< the user opened a layout file
  kLayoutSave,      ///< the user saved a layout file
  kRecordingStart,  ///< a session recording started
  kPanelScene3D,    ///< a Scene3D panel was created (by the user or by a layout)
  kPanelScene2D,    ///< a Scene2D panel was created (by the user or by a layout)
};

/// One way a plugin gets used; each implies the plugin family (usagePluginKind()).
enum class PluginUse {
  kFileLoad,      ///< a DataSource plugin loaded a file
  kStreamStart,   ///< a DataSource plugin started streaming
  kParserBind,    ///< a MessageParser plugin was bound to decode one topic
  kToolboxOpen,   ///< a Toolbox plugin was launched
  kWidgetCreate,  ///< a Widget plugin created a Custom Widget panel
};

/// Wire key of a feature ("layout.load", ...), stable across releases: the server allowlists it.
[[nodiscard]] std::string_view usageFeatureKey(UsageFeature feature);
/// Plugin family of a use: "data_source", "parser", "toolbox" or "widget".
[[nodiscard]] std::string_view usagePluginKind(PluginUse use);
/// Wire action of a use: "file", "stream", "decode", "open" or "create".
[[nodiscard]] std::string_view usagePluginAction(PluginUse use);

/// One plugin present in the catalog, for the daily installed inventory.
struct InstalledPluginRecord {
  std::string id;
  std::string version;
  std::string kind;  ///< usagePluginKind() vocabulary
};

/// Everything counted on one UTC day. Plugin ids are raw here (they never leave the machine in this
/// form): UsageReporter decides which ones may be named in a report.
struct DayUsage {
  QDate day;
  QString report_id;    ///< random, minted with the day: lets the server drop a retried upload
  QString app_version;  ///< the version that did the counting, which an upgrade before upload must not change
  std::map<std::string, int64_t> features;  ///< usageFeatureKey() -> uses
  /// (kind, action, plugin id) -> uses
  std::map<std::tuple<std::string, std::string, std::string>, int64_t> plugin_uses;
  std::vector<InstalledPluginRecord> installed;  ///< the last inventory taken that day
};

/// Local, per-UTC-day usage counters behind the anonymous usage report (docs/TELEMETRY.md).
///
/// Counting is cheap, thread-safe and never touches the disk (parser binds count from stream
/// threads); flush() persists. The app counts through the process-wide usageCounters(), which does
/// nothing until enabled, so call sites never check consent themselves. Disabling deletes everything
/// stored: an opt-out also discards what was counted before it.
///
/// Days are kept until acknowledged (the upload got a 2xx) and at most kMaxStoredDays completed days
/// are kept, so an offline machine neither grows the file nor uploads a stale backlog.
class UsageCounters {
 public:
  /// Completed days kept for upload; older ones are dropped unsent.
  static constexpr int kMaxStoredDays = 7;
  /// Distinct plugin-use rows kept per day; further distinct rows are dropped (counts still grow).
  static constexpr size_t kMaxPluginRowsPerDay = 256;

  /// `storage_file` is the JSON file the counters persist to (empty: memory only). `today` returns the
  /// current UTC date (injectable for tests); `app_version` stamps each new day.
  UsageCounters(QString storage_file, QString app_version, std::function<QDate()> today = {});

  UsageCounters(const UsageCounters&) = delete;
  UsageCounters& operator=(const UsageCounters&) = delete;

  /// Enabling loads what an earlier run stored; disabling clears memory and deletes the storage file.
  void setEnabled(bool enabled);
  [[nodiscard]] bool enabled() const;

  /// Re-target storage (the shell does this once at startup, before enabling).
  void configure(QString storage_file, QString app_version);

  void count(UsageFeature feature);
  /// `plugin_id` is the manifest id; an empty id is ignored.
  void countPlugin(PluginUse use, std::string_view plugin_id);
  /// Replaces today's installed inventory.
  void setInstalledPlugins(std::vector<InstalledPluginRecord> plugins);

  /// Writes the counters to the storage file when anything changed since the last write.
  void flush();

  /// Completed days (before today), oldest first, at most kMaxStoredDays.
  [[nodiscard]] std::vector<DayUsage> completedDays() const;
  /// Today's counters so far (what tomorrow's upload will carry for it).
  [[nodiscard]] DayUsage today() const;
  /// Forgets the days whose reports were accepted, and persists.
  void acknowledge(const std::vector<QString>& report_ids);

 private:
  DayUsage& todayLocked();
  void pruneLocked();
  void loadLocked();
  void saveLocked();

  mutable std::mutex mutex_;
  QString storage_file_;
  QString app_version_;
  std::function<QDate()> today_;
  bool enabled_ = false;
  bool dirty_ = false;
  std::map<QDate, DayUsage> days_;
};

/// The process-wide counters the app counts into; disabled (every call a no-op) until the shell
/// configures and enables it.
[[nodiscard]] UsageCounters& usageCounters();

}  // namespace PJ
