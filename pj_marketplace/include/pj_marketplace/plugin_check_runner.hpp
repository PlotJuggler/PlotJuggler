#pragma once
// Copyright 2026 Pablo Iñigo Blasco
// SPDX-License-Identifier: MPL-2.0

// Parent-side supervisor for the pj-plugin-check helper.
//
// Launches the helper as a child process against one extension directory,
// enforces a wall-clock timeout, parses the JSON descriptor list it produces,
// and returns a typed result. The child does the dlopen; a plugin that
// crashes on load never reaches the caller's process image.

#include <QProcess>
#include <QString>
#include <QVector>
#include <chrono>

namespace PJ {

/// Descriptor of one plugin the helper could discover in the candidate directory.
/// Mirrors the SDK PluginDescriptor projection without dragging its header
/// into every consumer of the runner.
struct PluginCheckDescriptor {
  QString id;
  QString name;
  QString version;
  /// Display fields the caller copies into its installed record. Carried here
  /// because the embedded manifest is the only source and reading it in-process
  /// is what the helper exists to avoid.
  QString description;
  QString category;
  QString family;
  quint32 abi_major = 0;
  QString min_sdk_required;
  QString min_plotjuggler_version;
  QString suggested_sdk_version;
  QString dso_path;
};

/// Per-candidate diagnostic emitted by the SDK scanner for DSOs that could
/// not produce a valid descriptor.
struct PluginCheckDiagnostic {
  QString path;
  QString message;
};

/// Typed result of one helper invocation. `ok == true` means the helper exited
/// cleanly and produced at least one valid descriptor; every other outcome
/// (bad usage, no valid plugin, timeout, crash, malformed JSON) sets `ok`
/// to false with a populated `parent_error`.
struct PluginCheckResult {
  bool ok = false;
  QVector<PluginCheckDescriptor> plugins;
  QVector<PluginCheckDiagnostic> diagnostics;
  int exit_code = -1;
  QProcess::ExitStatus exit_status = QProcess::CrashExit;
  QString parent_error;
};

/// Runs one pj-plugin-check invocation against `extension_directory`. Blocks
/// until the child exits or the timeout expires. The default timeout matches
/// the release-4.0 helper's default so integrating callers see similar wall
/// clocks either way.
struct PluginCheckOptions {
  std::chrono::milliseconds timeout{30'000};
  QString helper_path;  ///< empty ⇒ auto-locate next to the app executable
};

class PluginCheckRunner {
 public:
  explicit PluginCheckRunner(PluginCheckOptions options = {});

  [[nodiscard]] PluginCheckResult run(const QString& extension_directory) const;

  /// Resolve the path of the helper binary the runner would invoke. Exposed for
  /// tests and diagnostics; production callers rely on the auto-locate default.
  [[nodiscard]] static QString locateHelper(const QString& override_path = QString());

 private:
  PluginCheckOptions options_;
};

}  // namespace PJ
