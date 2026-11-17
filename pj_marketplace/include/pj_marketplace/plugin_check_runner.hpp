#pragma once
// Copyright 2026 Pablo Iñigo Blasco
// SPDX-License-Identifier: MPL-2.0

// Parent-side supervisor for the pj-plugin-check helper: launches it as a child process against one extension
// directory, enforces a wall-clock timeout and parses the JSON descriptor list it produces. The child does the dlopen,
// so a plugin that crashes on load never reaches the caller's process image.

#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QVector>
#include <chrono>

namespace PJ {

/// Descriptor of one plugin the helper discovered in the candidate directory; mirrors the SDK PluginDescriptor
/// projection without dragging its header into every consumer.
struct PluginCheckDescriptor {
  QString id;
  QString name;
  QString version;
  /// Display fields the caller copies into its installed record: the embedded manifest is the only source, and reading
  /// it in-process is what the helper exists to avoid.
  QString description;
  QString category;
  QString family;
  quint32 abi_major = 0;
  QString min_sdk_required;
  QString min_plotjuggler_version;
  QString suggested_sdk_version;
  QString dso_path;
};

/// Per-candidate diagnostic from the SDK scanner for a DSO that could not produce a valid descriptor.
struct PluginCheckDiagnostic {
  QString path;
  QString message;
};

/// Typed result of one helper run. `ok` means the helper reported outcome "admitted" with at least one descriptor;
/// every other outcome (bad usage, no valid plugin, timeout, crash, malformed JSON) leaves it false with `parent_error`
/// populated.
struct PluginCheckResult {
  bool ok = false;
  QVector<PluginCheckDescriptor> plugins;
  QVector<PluginCheckDiagnostic> diagnostics;
  int exit_code = -1;
  QProcess::ExitStatus exit_status = QProcess::CrashExit;
  QString parent_error;
};

/// `timeout` is the wall-clock limit of one run, after which the child is killed.
struct PluginCheckOptions {
  std::chrono::milliseconds timeout{30'000};
  QString helper_path;  ///< Empty: auto-locate next to the app executable (else a PATH lookup).
};

class PluginCheckRunner {
 public:
  explicit PluginCheckRunner(PluginCheckOptions options = {});

  /// Runs the helper against `extension_directory`. Blocks the calling thread until the child exits or the timeout
  /// expires.
  [[nodiscard]] PluginCheckResult run(const QString& extension_directory) const;

  /// The path of the helper binary the runner would invoke. Exposed for tests and diagnostics.
  [[nodiscard]] static QString locateHelper(const QString& override_path = QString());

  /// Environment the helper child runs with: `base` with `qt_libraries_dir` first on the dynamic-library search path
  /// (LD_LIBRARY_PATH on Linux, PATH on Windows; untouched elsewhere). The helper is Qt-free but dlopen()s every
  /// candidate, and a Widget plugin links Qt: outside the AppImage (a development tree, a .deb) nothing else tells the
  /// loader where libQt6Widgets lives, and a failed load reads as a rejected plugin. Exposed for tests.
  [[nodiscard]] static QProcessEnvironment childEnvironment(
      const QProcessEnvironment& base, const QString& qt_libraries_dir);

 private:
  PluginCheckOptions options_;
};

}  // namespace PJ
