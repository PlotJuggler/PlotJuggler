// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The agent guide served over the MCP Skills extension: the entries a client checks (name, digests, the files
// inside the skill), and that every link between the guide's pages resolves inside the skill.

#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <regex>
#include <set>
#include <string>

#include "mcp/McpProtocol.h"
#include "mcp/McpSkills.h"

using json = nlohmann::json;  // NOLINT(readability-identifier-naming)
using PJ::mcp::skillsResponse;

namespace {

constexpr char kSkillUri[] = "skill://plotjuggler/SKILL.md";

json ask(const std::string& method, const json& params = json::object()) {
  const std::optional<json> response = skillsResponse(7, method, params);
  EXPECT_TRUE(response.has_value()) << method;
  EXPECT_EQ(response.value_or(json()).value("id", json()), 7);
  return response.value_or(json());
}

json theSkill() {
  const json listed = ask("skills/list");
  for (const json& entry : listed["result"]["skills"]) {
    if (entry["uri"] == kSkillUri) {
      return entry;
    }
  }
  ADD_FAILURE() << "no " << kSkillUri << " in " << listed.dump();
  return json::object();
}

std::string readText(const std::string& uri) {
  const json read = ask("resources/read", {{"uri", uri}});
  return read["result"]["contents"][0].value("text", "");
}

}  // namespace

TEST(McpSkills, ListsThePlotjugglerSkillWithItsPages) {
  const json skill = theSkill();
  EXPECT_EQ(skill["frontmatter"]["name"], "plotjuggler");  // the folder's name, or a client drops the entry
  const std::string description = skill["frontmatter"].value("description", "");
  EXPECT_FALSE(description.empty());
  EXPECT_LE(description.size(), 1024U);
  std::set<std::string> uris;
  std::string previous;
  for (const json& file : skill["resources"]) {
    const std::string uri = file["uri"];
    EXPECT_LT(previous, uri) << "files are sorted by URI, once each";
    previous = uri;
    EXPECT_TRUE(uri.starts_with("skill://plotjuggler/")) << uri;
    EXPECT_TRUE(std::regex_match(file["digest"].get<std::string>(), std::regex("sha256:[0-9a-f]{64}"))) << uri;
    uris.insert(uri);
  }
  EXPECT_TRUE(uris.contains(kSkillUri));
  EXPECT_TRUE(uris.contains("skill://plotjuggler/scripting.md"));
  EXPECT_TRUE(uris.contains("skill://plotjuggler/ui_tasks/plots.md"));
  EXPECT_FALSE(uris.contains("skill://plotjuggler/llms.txt"));  // the web index is not part of the skill
  const json listed = ask("skills/list")["result"];
  EXPECT_EQ(listed["cacheScope"], "public");
  EXPECT_TRUE(listed["ttlMs"].is_number_integer());
}

TEST(McpSkills, EveryFileReadsBackAsItsDigestAndSize) {
  const json skill = theSkill();
  for (const json& file : skill["resources"]) {
    const json read = ask("resources/read", {{"uri", file["uri"]}});
    const json& content = read["result"]["contents"][0];
    EXPECT_EQ(content["uri"], file["uri"]);
    EXPECT_EQ(content["mimeType"], "text/markdown");
    const std::string text = content.value("text", "");
    EXPECT_EQ(text.size(), file["size"].get<std::size_t>()) << file["uri"];
    const QByteArray digest =
        QCryptographicHash::hash(QByteArray::fromStdString(text), QCryptographicHash::Sha256).toHex();
    EXPECT_EQ(file["digest"], "sha256:" + digest.toStdString()) << file["uri"];
  }
}

TEST(McpSkills, GetFindsTheSkillAndRefusesOtherUris) {
  EXPECT_EQ(ask("skills/get", {{"uri", kSkillUri}})["result"]["skill"], theSkill());
  EXPECT_EQ(ask("skills/get", {{"uri", "skill://plotjuggler/scripting.md"}})["error"]["code"], -32602);
  EXPECT_EQ(ask("skills/get", json::object())["error"]["code"], -32602);
  EXPECT_EQ(
      ask("resources/read", {{"uri", "skill://plotjuggler/../CLAUDE.md"}})["error"]["code"],
      PJ::mcp::kResourceNotFound);
  EXPECT_EQ(ask("resources/read", {{"uri", "file:///etc/passwd"}})["error"]["code"], PJ::mcp::kResourceNotFound);
  EXPECT_EQ(ask("skills/list", {{"cursor", "x"}})["error"]["code"], -32602);
  EXPECT_FALSE(skillsResponse(1, "tools/list", json::object()).has_value());
}

TEST(McpSkills, ResourcesListShowsTheGuideToClientsWithoutTheExtension) {
  const json skill = theSkill();
  const json resources = ask("resources/list")["result"]["resources"];
  ASSERT_EQ(resources.size(), skill["resources"].size());
  bool described = false;
  for (const json& resource : resources) {
    if (resource["uri"] == kSkillUri) {
      described = resource.value("description", json()) == skill["frontmatter"]["description"];
    }
  }
  EXPECT_TRUE(described);
  EXPECT_EQ(ask("resources/templates/list")["result"]["resourceTemplates"], json::array());
}

TEST(McpSkills, EveryRelativeLinkBetweenPagesResolvesInsideTheSkill) {
  const json skill = theSkill();
  std::set<std::string> uris;
  for (const json& file : skill["resources"]) {
    uris.insert(file["uri"]);
  }
  // [text](target) with a relative target: no scheme, no anchor-only link.
  const std::regex link(R"(\]\(([^)#:\s]+)(#[^)]*)?\))");
  for (const std::string& uri : uris) {
    const std::string text = readText(uri);
    const std::string folder = uri.substr(0, uri.rfind('/') + 1);
    for (auto it = std::sregex_iterator(text.begin(), text.end(), link); it != std::sregex_iterator(); ++it) {
      std::string target = folder + (*it)[1].str();
      // Resolve "./" and "../" segments as a client resolves the link against the page's URI.
      for (std::size_t here; (here = target.find("/./")) != std::string::npos;) {
        target.erase(here, 2);
      }
      for (std::size_t up; (up = target.find("/../")) != std::string::npos;) {
        const std::size_t parent = target.rfind('/', up - 1);
        target.erase(parent, up + 3 - parent);
      }
      EXPECT_TRUE(uris.contains(target)) << uri << " links to " << (*it)[1].str() << ", not in the skill";
    }
  }
}

TEST(McpSkills, OnlyTheAppToolsTokenAdvertisesTheSkill) {
  for (const char* method : {"initialize", "server/discover"}) {
    const json full = *PJ::mcp::lifecycleResult(method, json::object(), PJ::mcp::ToolSet::kFull);
    EXPECT_TRUE(full["capabilities"]["extensions"].contains(PJ::mcp::kSkillsExtension)) << method;
    EXPECT_TRUE(full["capabilities"].contains("resources")) << method;
    EXPECT_NE(full["instructions"].get<std::string>().find(kSkillUri), std::string::npos) << method;
    const json plugin = *PJ::mcp::lifecycleResult(method, json::object(), PJ::mcp::ToolSet::kAgentOnly);
    EXPECT_FALSE(plugin["capabilities"].value("extensions", json::object()).contains(PJ::mcp::kSkillsExtension));
    EXPECT_FALSE(plugin["capabilities"].contains("resources")) << method;
  }
  const json discover = *PJ::mcp::lifecycleResult("server/discover", json::object(), PJ::mcp::ToolSet::kFull);
  EXPECT_TRUE(discover["capabilities"]["extensions"].contains(PJ::mcp::kTasksExtension));
}
