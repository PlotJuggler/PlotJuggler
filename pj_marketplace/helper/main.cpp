// Copyright 2026 Pablo Iñigo Blasco
// SPDX-License-Identifier: MPL-2.0

// pj-plugin-check — Qt-free, one-shot admission helper.
//
// Contract:
//   pj-plugin-check <extension-directory>
//
//   Prints one JSON document (schema 1) describing the plugins the SDK could
//   discover in that directory, then exits. Loading the DSOs happens inside
//   this child process; a plugin that crashes on dlopen or during manifest
//   read never touches the caller's memory space.
//
// This is the minimum viable spike of the release-4.0 fault-boundary idea,
// designed to be cabled into the classic ExtensionManager without pulling in
// ProfileStore, ArtifactStore, LoadPlan or the generation-based catalog.

#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

#include "pj_marketplace/utf8_path.hpp"
#include "pj_plugins/host/plugin_catalog.hpp"

namespace {

constexpr int kExitAdmitted = 0;
constexpr int kExitUsage = 2;
constexpr int kExitNoValidPlugin = 4;

std::string familyToString(PJ::PluginFamily family) {
  switch (family) {
    case PJ::PluginFamily::kDataSource:
      return "data_source";
    case PJ::PluginFamily::kMessageParser:
      return "message_parser";
    case PJ::PluginFamily::kToolbox:
      return "toolbox";
    case PJ::PluginFamily::kDialog:
      return "dialog";
    case PJ::PluginFamily::kWidget:
      return "widget";
    case PJ::PluginFamily::kUnknown:
      break;
  }
  return "unknown";
}

nlohmann::json descriptorToJson(const PJ::PluginDescriptor& descriptor) {
  nlohmann::json j;
  j["id"] = descriptor.id;
  j["name"] = descriptor.name;
  j["version"] = descriptor.version;
  // description and category are display fields the caller stores in its installed
  // record; they are only reachable from the embedded manifest, which is precisely
  // what the parent must not open for itself.
  j["description"] = descriptor.description;
  j["category"] = descriptor.category;
  j["family"] = familyToString(descriptor.family);
  j["abi_major"] = descriptor.abi_major;
  j["min_sdk_required"] = descriptor.min_sdk_required;
  j["min_plotjuggler_version"] = descriptor.min_plotjuggler_version;
  j["suggested_sdk_version"] = descriptor.suggested_sdk_version;
  j["dso_path"] = PJ::pathToUtf8(descriptor.dso_path);
  return j;
}

nlohmann::json diagnosticToJson(const PJ::PluginDiagnostic& diagnostic) {
  nlohmann::json j;
  j["path"] = PJ::pathToUtf8(diagnostic.path);
  j["message"] = diagnostic.message;
  return j;
}

int printResultAndExit(const nlohmann::json& result, int exit_code) {
  // replace, not the default strict handler: a POSIX file name need not be valid UTF-8, and
  // strict dump() would throw on it and abort instead of printing the document.
  std::cout << result.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
  return exit_code;
}

// Char is char for main and wchar_t for wmain: std::filesystem::path takes either as is.
template <class Char>
int run(int argc, Char** argv) {
  if (argc != 2) {
    nlohmann::json usage;
    usage["schema"] = 1;
    usage["outcome"] = "rejected";
    usage["error"] = "usage: pj-plugin-check <extension-directory>";
    return printResultAndExit(usage, kExitUsage);
  }

  std::filesystem::path directory(argv[1]);

  auto scanned = PJ::scanPluginDsos(directory);

  nlohmann::json result;
  result["schema"] = 1;
  result["directory"] = PJ::pathToUtf8(directory);

  if (!scanned) {
    result["outcome"] = "rejected";
    result["error"] = scanned.error();
    result["plugins"] = nlohmann::json::array();
    result["diagnostics"] = nlohmann::json::array();
    return printResultAndExit(result, kExitNoValidPlugin);
  }

  nlohmann::json plugins = nlohmann::json::array();
  for (const auto& descriptor : scanned->plugins) {
    plugins.push_back(descriptorToJson(descriptor));
  }
  nlohmann::json diagnostics = nlohmann::json::array();
  for (const auto& diagnostic : scanned->diagnostics) {
    diagnostics.push_back(diagnosticToJson(diagnostic));
  }
  result["plugins"] = plugins;
  result["diagnostics"] = diagnostics;

  const bool has_plugin = !scanned->plugins.empty();
  result["outcome"] = has_plugin ? "admitted" : "rejected";
  return printResultAndExit(result, has_plugin ? kExitAdmitted : kExitNoValidPlugin);
}

}  // namespace

#ifdef _WIN32
// wmain, not main: Windows narrows main's argv to the active code page, so a directory name
// outside it would arrive with '?' in its place. wmain gets the UTF-16 command line, which
// std::filesystem::path takes as is. (MSVC selects wmainCRTStartup for it; a MinGW build
// would need -municode.)
int wmain(int argc, wchar_t** argv) {
  return run(argc, argv);
}
#else
int main(int argc, char** argv) {
  return run(argc, argv);
}
#endif
