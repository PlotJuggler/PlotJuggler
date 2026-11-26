// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "mcp/McpSkills.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <algorithm>
#include <map>
#include <vector>

#include "mcp/McpProtocol.h"

using json = nlohmann::json;  // NOLINT(readability-identifier-naming)
using namespace Qt::StringLiterals;

namespace PJ::mcp {

namespace {

struct SkillFile {
  std::string uri;
  QByteArray bytes;
};

struct Skill {
  json entry;                    ///< what skills/list and skills/get return
  std::vector<SkillFile> files;  ///< sorted by URI, SKILL.md among them
};

/// The `key: value` lines between a leading `---` line and the next one. Values are scalars on one line, quotes
/// around them dropped; anything else (lists, nested maps) is left out, so only plain strings are published.
json parseFrontmatter(const QByteArray& text) {
  json out = json::object();
  const QList<QByteArray> lines = text.split('\n');
  if (lines.isEmpty() || lines.front().trimmed() != "---") {
    return out;
  }
  for (qsizetype i = 1; i < lines.size(); ++i) {
    const QByteArray line = lines[i].endsWith('\r') ? lines[i].chopped(1) : lines[i];
    if (line.trimmed() == "---") {
      return out;
    }
    const qsizetype colon = line.indexOf(':');
    if (colon <= 0 || line.startsWith(' ') || line.startsWith('\t')) {
      continue;
    }
    const std::string key = line.left(colon).trimmed().toStdString();
    QByteArray value = line.mid(colon + 1).trimmed();
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
      value = value.mid(1, value.size() - 2);
    }
    if (!value.isEmpty()) {
      out[key] = value.toStdString();
    }
  }
  return json::object();  // no closing `---`: not a frontmatter
}

std::string mimeTypeOf(const std::string& uri) {
  return uri.ends_with(".md") ? "text/markdown" : "text/plain";
}

/// Every skill under kSkillsResourceRoot, by its SKILL.md URI. A folder whose SKILL.md lacks a name equal to the
/// folder's or a description is skipped with a warning: a client would drop the entry anyway.
const std::map<std::string, Skill>& catalog() {
  static const std::map<std::string, Skill> skills = [] {
    std::map<std::string, Skill> out;
    const QDir root(QString::fromLatin1(kSkillsResourceRoot));
    for (const QString& folder : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
      const QString base = root.filePath(folder);
      QFile manifest(base + u"/SKILL.md"_s);
      if (!manifest.open(QIODevice::ReadOnly)) {
        continue;
      }
      const json frontmatter = parseFrontmatter(manifest.readAll());
      if (frontmatter.value("name", "") != folder.toStdString() || frontmatter.value("description", "").empty()) {
        qCWarning(lcMcp).noquote() << "skill" << folder << "skipped: its SKILL.md needs name:" << folder
                                   << "and a description";
        continue;
      }
      Skill skill;
      QDirIterator it(base, QDir::Files, QDirIterator::Subdirectories);
      while (it.hasNext()) {
        const QString path = it.next();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
          continue;
        }
        const std::string relative = path.mid(base.size() + 1).toStdString();
        skill.files.push_back({kSkillUriScheme + folder.toStdString() + "/" + relative, file.readAll()});
      }
      std::sort(skill.files.begin(), skill.files.end(), [](const auto& a, const auto& b) { return a.uri < b.uri; });
      json resources = json::array();
      for (const SkillFile& file : skill.files) {
        const QByteArray digest = QCryptographicHash::hash(file.bytes, QCryptographicHash::Sha256).toHex();
        resources.push_back(
            {{"uri", file.uri}, {"digest", "sha256:" + digest.toStdString()}, {"size", file.bytes.size()}});
      }
      const std::string uri = kSkillUriScheme + folder.toStdString() + "/SKILL.md";
      skill.entry = {{"uri", uri}, {"frontmatter", frontmatter}, {"resources", std::move(resources)}};
      out.emplace(uri, std::move(skill));
    }
    return out;
  }();
  return skills;
}

const SkillFile* findFile(const std::string& uri) {
  for (const auto& [_, skill] : catalog()) {
    for (const SkillFile& file : skill.files) {
      if (file.uri == uri) {
        return &file;
      }
    }
  }
  return nullptr;
}

/// The SDK's cache hint: the files change only with the binary, and are the same for every user.
void addCacheHint(json& result) {
  result["ttlMs"] = 0;
  result["cacheScope"] = "public";
}

std::optional<std::string> stringParam(const json& params, const char* key) {
  const auto it = params.find(key);
  return it != params.end() && it->is_string() ? std::optional(it->get<std::string>()) : std::nullopt;
}

}  // namespace

bool isSkillsMethod(const std::string& method) {
  return method == "skills/list" || method == "skills/get" || method == "resources/list" ||
         method == "resources/templates/list" || method == "resources/read";
}

std::optional<json> skillsResponse(const json& id, const std::string& method, const json& params) {
  if (!isSkillsMethod(method)) {
    return std::nullopt;
  }
  const auto ok = [&id](json result) { return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}; };
  // Everything fits one page: a cursor can only be one this server never issued.
  if (params.contains("cursor") && (method == "skills/list" || method == "resources/list")) {
    return errorResponse(id, -32602, "invalid cursor: every item is on the first page");
  }
  if (method == "skills/list") {
    json skills = json::array();
    for (const auto& [_, skill] : catalog()) {
      skills.push_back(skill.entry);
    }
    json result = {{"skills", std::move(skills)}};
    addCacheHint(result);
    return ok(std::move(result));
  }
  if (method == "skills/get") {
    const std::optional<std::string> uri = stringParam(params, "uri");
    if (!uri) {
      return errorResponse(id, -32602, "skills/get needs a string uri");
    }
    const auto it = catalog().find(*uri);
    if (it == catalog().end()) {
      return errorResponse(id, -32602, "No skill is served at " + *uri);
    }
    json result = {{"skill", it->second.entry}};
    addCacheHint(result);
    return ok(std::move(result));
  }
  if (method == "resources/list") {
    json resources = json::array();
    for (const auto& [skill_uri, skill] : catalog()) {
      for (const SkillFile& file : skill.files) {
        json resource = {
            {"uri", file.uri},
            {"name", file.uri.substr(std::char_traits<char>::length(kSkillUriScheme))},
            {"mimeType", mimeTypeOf(file.uri)},
            {"size", file.bytes.size()}};
        if (file.uri == skill_uri) {
          resource["description"] = skill.entry["frontmatter"]["description"];
        }
        resources.push_back(std::move(resource));
      }
    }
    return ok({{"resources", std::move(resources)}});
  }
  if (method == "resources/templates/list") {
    return ok({{"resourceTemplates", json::array()}});
  }
  // resources/read
  const std::optional<std::string> uri = stringParam(params, "uri");
  if (!uri) {
    return errorResponse(id, -32602, "resources/read needs a string uri");
  }
  const SkillFile* file = findFile(*uri);
  if (file == nullptr) {
    return errorResponse(id, kResourceNotFound, "Resource not found: " + *uri, {{"uri", *uri}});
  }
  return ok(
      {{"contents",
        json::array(
            {{{"uri", file->uri}, {"mimeType", mimeTypeOf(file->uri)}, {"text", file->bytes.toStdString()}}})}});
}

}  // namespace PJ::mcp
