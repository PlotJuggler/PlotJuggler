// Copyright 2026 Pablo Iñigo Blasco
// SPDX-License-Identifier: MPL-2.0

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLatin1StringView>
#include <QLibraryInfo>
#include <QProcess>
#include <string_view>

#include "pj_marketplace/plugin_check_runner.hpp"

namespace PJ {

namespace {

#if defined(_WIN32)
constexpr std::string_view kHelperExecutableName = "pj-plugin-check.exe";
#else
constexpr std::string_view kHelperExecutableName = "pj-plugin-check";
#endif

QString helperExecutableName() {
  return QString::fromLatin1(kHelperExecutableName.data(), static_cast<qsizetype>(kHelperExecutableName.size()));
}

QString autoLocateHelper() {
  QString exec_name = helperExecutableName();
  const QString app_dir = QCoreApplication::applicationDirPath();
  const QFileInfo alongside_app(QDir(app_dir).absoluteFilePath(exec_name));
  if (alongside_app.isExecutable()) {
    return alongside_app.absoluteFilePath();
  }
  // Development fallback: helper may live in PATH (found via QProcess) when the
  // app is being run from a build directory. Kept narrow — production packaging
  // always ships the helper next to the app.
  return exec_name;
}

PluginCheckDescriptor descriptorFromJson(const QJsonObject& obj) {
  PluginCheckDescriptor d;
  d.id = obj.value("id").toString();
  d.name = obj.value("name").toString();
  d.version = obj.value("version").toString();
  d.description = obj.value("description").toString();
  d.category = obj.value("category").toString();
  d.family = obj.value("family").toString();
  d.abi_major = static_cast<quint32>(obj.value("abi_major").toInt());
  d.min_sdk_required = obj.value("min_sdk_required").toString();
  d.min_plotjuggler_version = obj.value("min_plotjuggler_version").toString();
  d.suggested_sdk_version = obj.value("suggested_sdk_version").toString();
  d.dso_path = obj.value("dso_path").toString();
  return d;
}

PluginCheckDiagnostic diagnosticFromJson(const QJsonObject& obj) {
  PluginCheckDiagnostic diag;
  diag.path = obj.value("path").toString();
  diag.message = obj.value("message").toString();
  return diag;
}

}  // namespace

PluginCheckRunner::PluginCheckRunner(PluginCheckOptions options) : options_(std::move(options)) {}

QString PluginCheckRunner::locateHelper(const QString& override_path) {
  return override_path.isEmpty() ? autoLocateHelper() : override_path;
}

QProcessEnvironment PluginCheckRunner::childEnvironment(
    const QProcessEnvironment& base, const QString& qt_libraries_dir) {
  QProcessEnvironment env = base;
#if defined(_WIN32)
  constexpr auto kSearchPathVariable = "PATH";
#elif defined(__linux__)
  constexpr auto kSearchPathVariable = "LD_LIBRARY_PATH";
#else
  // macOS: the helper finds Qt through its rpath; nothing to prepend.
  Q_UNUSED(qt_libraries_dir);
  return env;
#endif
#if defined(_WIN32) || defined(__linux__)
  if (qt_libraries_dir.isEmpty()) {
    return env;
  }
  const QString existing = env.value(QLatin1StringView(kSearchPathVariable));
  env.insert(
      QLatin1StringView(kSearchPathVariable),
      existing.isEmpty() ? qt_libraries_dir : qt_libraries_dir + QDir::listSeparator() + existing);
  return env;
#endif
}

PluginCheckResult PluginCheckRunner::run(const QString& extension_directory) const {
  PluginCheckResult result;

  const QString helper = locateHelper(options_.helper_path);
  QProcess proc;
  proc.setProgram(helper);
  proc.setArguments({extension_directory});
#if defined(_WIN32)
  const QString qt_libraries_dir = QLibraryInfo::path(QLibraryInfo::BinariesPath);
#else
  const QString qt_libraries_dir = QLibraryInfo::path(QLibraryInfo::LibrariesPath);
#endif
  proc.setProcessEnvironment(childEnvironment(QProcessEnvironment::systemEnvironment(), qt_libraries_dir));
  proc.setProcessChannelMode(QProcess::SeparateChannels);
  proc.start();

  if (!proc.waitForStarted(2000)) {
    result.parent_error = QString("Could not start helper '%1': %2").arg(helper, proc.errorString());
    return result;
  }
  if (!proc.waitForFinished(static_cast<int>(options_.timeout.count()))) {
    proc.kill();
    proc.waitForFinished(2000);
    result.exit_code = proc.exitCode();
    result.exit_status = proc.exitStatus();
    result.parent_error = QString("Helper timed out after %1 ms; killed").arg(options_.timeout.count());
    return result;
  }

  result.exit_code = proc.exitCode();
  result.exit_status = proc.exitStatus();
  if (result.exit_status == QProcess::CrashExit) {
    result.parent_error = QString("Helper crashed (signal / abnormal exit); exit code %1").arg(result.exit_code);
    return result;
  }

  const QByteArray stdout_bytes = proc.readAllStandardOutput();
  QJsonParseError parse_err{};
  const QJsonDocument doc = QJsonDocument::fromJson(stdout_bytes, &parse_err);
  if (parse_err.error != QJsonParseError::NoError || !doc.isObject()) {
    result.parent_error = QString("Helper produced malformed JSON: %1").arg(parse_err.errorString());
    return result;
  }
  const QJsonObject root = doc.object();
  const QJsonArray plugins = root.value("plugins").toArray();
  for (const auto& v : plugins) {
    result.plugins.push_back(descriptorFromJson(v.toObject()));
  }
  const QJsonArray diagnostics = root.value("diagnostics").toArray();
  for (const auto& v : diagnostics) {
    result.diagnostics.push_back(diagnosticFromJson(v.toObject()));
  }
  const QString outcome = root.value("outcome").toString();
  result.ok = (outcome == QLatin1String("admitted")) && !result.plugins.isEmpty();
  if (!result.ok && root.contains("error")) {
    result.parent_error = root.value("error").toString();
  }
  return result;
}

}  // namespace PJ
