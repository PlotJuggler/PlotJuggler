// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QHash>
#include <QStringList>
#include <string>
#include <vector>

#include "pj_base/sdk/semver.hpp"
#include "pj_marketplace/plugin_compatibility.hpp"
#include "pj_marketplace/registry_resolver.hpp"
#include "pj_marketplace/version_compare.hpp"

namespace PJ {
namespace {

QString invalidCandidateReason(const Extension& extension) {
  const std::string version = extension.version.toStdString();
  if (!SemVer::isValid(version)) {
    return QString("Registry extension \"%1\" has invalid version \"%2\"").arg(extension.id, extension.version);
  }

  const auto validate_floor = [&extension](const QString& floor, const QString& label) -> QString {
    if (!floor.isEmpty() && !SemVer::isValid(floor.toStdString())) {
      return QString("Registry extension \"%1\" has invalid %2 \"%3\"").arg(extension.id, label, floor);
    }
    return {};
  };
  return validate_floor(extension.min_sdk_required, "minimum SDK version");
}

bool hostCompatible(const Extension& extension, const RegistryResolutionContext& context) {
  const std::string version = extension.version.toStdString();
  const std::string minimum_sdk = extension.min_sdk_required.toStdString();
  const std::string sdk_version = context.sdk_version.toStdString();
  return evaluatePluginCompatibility(
             {
                 .version = version,
                 .abi_major = 0,
                 .min_sdk_required = minimum_sdk,
             },
             {
                 .abi_major = PJ_ABI_VERSION,
                 .sdk_version = sdk_version,
             })
      .ok;
}

const Extension* highestVersion(const std::vector<const Extension*>& candidates) {
  const Extension* winner = candidates.front();
  for (const Extension* candidate : candidates) {
    if (comparePluginVersions(candidate->version.toStdString(), winner->version.toStdString()) > 0) {
      winner = candidate;
    }
  }
  return winner;
}

}  // namespace

Expected<QList<Extension>, QString> resolveRegistryCandidates(
    const QList<Extension>& candidates, const RegistryResolutionContext& context) {
  QHash<QString, std::vector<const Extension*>> candidates_by_id;
  QStringList id_order;

  for (const Extension& candidate : candidates) {
    if (const QString error = invalidCandidateReason(candidate); !error.isEmpty()) {
      return unexpected(error);
    }
    auto& group = candidates_by_id[candidate.id];
    if (group.empty()) {
      id_order.append(candidate.id);
    }
    for (const Extension* incumbent : group) {
      if (comparePluginVersions(candidate.version.toStdString(), incumbent->version.toStdString()) == 0) {
        return unexpected(
            QString(
                "Registry reuses semantic version slot \"%1\" for extension \"%2\"; publish one immutable "
                "record containing every platform artifact")
                .arg(candidate.version, candidate.id));
      }
    }
    group.push_back(&candidate);
  }

  QList<Extension> resolved;
  resolved.reserve(id_order.size());
  for (const QString& id : id_order) {
    const auto& group = candidates_by_id[id];
    std::vector<const Extension*> platform_candidates;
    std::vector<const Extension*> compatible_candidates;
    for (const Extension* candidate : group) {
      if (!context.platform.isEmpty() && candidate->platforms.contains(context.platform)) {
        platform_candidates.push_back(candidate);
        if (hostCompatible(*candidate, context)) {
          compatible_candidates.push_back(candidate);
        }
      }
    }

    const Extension* winner = nullptr;
    if (!compatible_candidates.empty()) {
      winner = highestVersion(compatible_candidates);
    } else if (!platform_candidates.empty()) {
      winner = highestVersion(platform_candidates);
    } else {
      winner = highestVersion(group);
    }
    resolved.append(*winner);
  }
  return resolved;
}

}  // namespace PJ
