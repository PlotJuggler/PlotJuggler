#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <QtGlobal>
#include <functional>
#include <utility>
#include <vector>

namespace PJ {

/// On-disk record of the Custom Widget panels whose plugin code is live in this process.
///
/// A Widget plugin runs in the GUI thread of PlotJuggler: a crash in its code takes the app down,
/// and restoring the same layout crashes it again. The record breaks that loop. Each panel adds
/// itself before the first call into its plugin and removes itself after the plugin instance was
/// destroyed; a clean exit deletes the file. A record whose process is gone therefore names the
/// widgets that were live when that process died, and the shell can offer to disable them before
/// it restores any layout.
///
/// One file per process, `active_widgets.<pid>.json` in the record directory, so instances running
/// side by side never read or erase each other's record: the startup review (take()) only consumes
/// the files of processes that are no longer alive. A pid the system reused for an unrelated live
/// process hides that file until the process ends; the record is then reviewed at a later start.
///
/// Format: `{"version":1,"widgets":[{"plugin":"<id>","type":"<type>"}, ...]}`, one entry per
/// distinct (plugin, type). Every change rewrites the file atomically (QSaveFile); with no live
/// panel the file is removed. GUI thread only.
class ActiveWidgetsMarker {
 public:
  struct Entry {
    QString plugin_id;
    QString type;
    bool operator==(const Entry&) const = default;
  };

  struct ReadResult {
    std::vector<Entry> entries;  ///< distinct (plugin, type), file order (files in name order)
    QString error;               ///< non-empty when a file exists but could not be used
  };

  /// Whether the process `pid` is alive; take() asks it per file. Replaceable for tests.
  using ProcessProbe = std::function<bool(qint64 pid)>;

  /// The record of process `pid` (by default this one) inside `directory`.
  explicit ActiveWidgetsMarker(QString directory, qint64 pid = currentPid());

  /// `<AppLocalDataLocation>`, where every instance of the user keeps its record.
  [[nodiscard]] static QString defaultDirectory();
  /// `active_widgets.<pid>.json`.
  [[nodiscard]] static QString fileNameFor(qint64 pid);
  [[nodiscard]] static qint64 currentPid();
  /// Linux/macOS: `kill(pid, 0)` succeeds or fails with EPERM. Windows: the process opens and has
  /// not exited. False for a pid <= 0.
  [[nodiscard]] static bool isProcessAlive(qint64 pid);

  [[nodiscard]] const QString& directory() const {
    return directory_;
  }
  /// This process's file.
  [[nodiscard]] const QString& path() const {
    return path_;
  }

  /// Records that the panel `owner` is about to run widget `type` of plugin `plugin_id`, and
  /// writes the file before returning. Replaces what `owner` held before.
  void add(const void* owner, const QString& plugin_id, const QString& type);
  /// Forgets `owner` and rewrites the file (removes it when nothing is left). No-op when `owner`
  /// held nothing.
  void remove(const void* owner);
  /// Clean exit: forgets every panel and deletes this process's file (no other).
  void clear();

  /// Reads one record file. A missing file is no entries and no error; an unreadable or malformed
  /// one is no entries and an error saying why.
  [[nodiscard]] static ReadResult read(const QString& path);
  /// The startup review: reads and deletes every record in `directory` whose process `alive` says
  /// is gone, and merges their entries (distinct, in file-name order) and errors. Files of live
  /// processes, this one included, are left alone. What a dead run left is consumed exactly once.
  /// `alive` must not be empty.
  [[nodiscard]] static ReadResult take(const QString& directory, const ProcessProbe& alive = &isProcessAlive);

 private:
  void write() const;

  QString directory_;
  QString path_;
  std::vector<std::pair<const void*, Entry>> live_;
};

}  // namespace PJ
