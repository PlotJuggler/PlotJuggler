// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// The MCP Skills extension (SEP-2640): PlotJuggler serves its own agent guide, compiled into the
// binary from docs/agents/, so the guide an agent reads always matches the build answering it.

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace PJ::mcp {

/// The identifier advertised under `capabilities.extensions`.
inline constexpr char kSkillsExtension[] = "io.modelcontextprotocol/skills";

/// The URI scheme of every skill file: `skill://<skill name>/<path below the skill's folder>`.
inline constexpr char kSkillUriScheme[] = "skill://";

/// The Qt resource folder holding one sub-folder per skill, each with its SKILL.md (pj_app/CMakeLists.txt fills it
/// from docs/agents/).
inline constexpr char kSkillsResourceRoot[] = ":/pj_mcp_skills";

/// The JSON-RPC error of a `resources/read` naming no file this server serves (MCP's "resource not found").
inline constexpr int kResourceNotFound = -32002;

/// Whether `method` is answered by skillsResponse: `skills/list`, `skills/get`, `resources/list`,
/// `resources/templates/list` and `resources/read`.
[[nodiscard]] bool isSkillsMethod(const std::string& method);

/// The complete JSON-RPC response to a skills method (result or error), nullopt for any other method.
/// - `skills/list`: every skill, one page; `skills/get` {uri}: one skill by its SKILL.md URI. An entry is
///   `{uri, frontmatter, resources: [{uri, digest: "sha256:<hex>", size}]}`, files sorted by URI.
/// - `resources/list`: every skill file, so a client without the extension still finds the guide;
///   `resources/templates/list`: none. `resources/read` {uri}: one file's text.
/// The catalog is read from kSkillsResourceRoot once, on first use; it never changes while the process runs.
[[nodiscard]] std::optional<nlohmann::json> skillsResponse(
    const nlohmann::json& id, const std::string& method, const nlohmann::json& params);

}  // namespace PJ::mcp
