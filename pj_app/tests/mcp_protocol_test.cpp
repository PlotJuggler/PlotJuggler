// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The MCP server's HTTP and JSON-RPC gates, without a server: what a hostile or broken client can
// send must be refused with the right status, never crash or be served.

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>

#include "mcp/McpProtocol.h"

using json = nlohmann::json;  // NOLINT(readability-identifier-naming)
using PJ::mcp::HttpRequest;
using PJ::mcp::parseHttpRequest;

namespace {

QByteArray post(const QByteArray& headers, const QByteArray& body) {
  return "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n" + headers + "\r\n" + body;
}

}  // namespace

TEST(McpHttpParsing, CompleteRequestParsesAndIncompleteWaits) {
  const QByteArray full = post("Content-Length: 4\r\nAuthorization: Bearer t\r\n", "{\"a\"");
  HttpRequest request;
  EXPECT_EQ(parseHttpRequest(full.left(full.size() - 1), request), 0);  // body still arriving
  EXPECT_EQ(parseHttpRequest(full.left(20), request), 0);               // header still arriving
  ASSERT_EQ(parseHttpRequest(full, request), 200);
  EXPECT_EQ(request.method, "POST");
  EXPECT_EQ(request.path, "/mcp");
  EXPECT_EQ(request.headers.value("authorization"), "Bearer t");
  EXPECT_EQ(request.body, "{\"a\"");
}

TEST(McpHttpParsing, MissingContentLengthMeansNoBody) {
  HttpRequest request;
  ASSERT_EQ(parseHttpRequest(post("", ""), request), 200);
  EXPECT_TRUE(request.body.isEmpty());
}

TEST(McpHttpParsing, OversizedHeaderIsRefused) {
  HttpRequest request;
  const QByteArray huge = "X-Pad: " + QByteArray(PJ::mcp::kMaxHeaderBytes, 'a') + "\r\n";
  EXPECT_EQ(parseHttpRequest(post(huge, ""), request), 431);
  // Refused before the end of the header ever arrives.
  EXPECT_EQ(parseHttpRequest("POST /mcp HTTP/1.1\r\n" + huge, request), 431);
}

TEST(McpHttpParsing, OversizedBodyIsRefusedFromItsDeclaredLength) {
  HttpRequest request;
  EXPECT_EQ(
      parseHttpRequest(post("Content-Length: " + QByteArray::number(PJ::mcp::kMaxBodyBytes + 1) + "\r\n", ""), request),
      413);
}

TEST(McpHttpParsing, BadContentLengthIsRefused) {
  HttpRequest request;
  EXPECT_EQ(parseHttpRequest(post("Content-Length: -1\r\n", ""), request), 400);
  EXPECT_EQ(parseHttpRequest(post("Content-Length: +3\r\n", "abc"), request), 400);
  EXPECT_EQ(parseHttpRequest(post("Content-Length: 12abc\r\n", ""), request), 400);
  EXPECT_EQ(parseHttpRequest(post("Transfer-Encoding: chunked\r\n", ""), request), 400);
  EXPECT_EQ(parseHttpRequest("garbage\r\n\r\n", request), 400);
}

TEST(McpHttpAccess, OriginAndTokenGateEveryRequest) {
  HttpRequest request;
  ASSERT_EQ(parseHttpRequest(post("Authorization: Bearer secret\r\n", ""), request), 200);
  EXPECT_EQ(PJ::mcp::checkAccess(request, "secret"), 0);
  EXPECT_EQ(PJ::mcp::checkAccess(request, "other"), 401);
  request.headers.remove("authorization");
  EXPECT_EQ(PJ::mcp::checkAccess(request, "secret"), 401);
  // A browser page always sends Origin: refused even with the right token.
  ASSERT_EQ(parseHttpRequest(post("Authorization: Bearer secret\r\nOrigin: http://evil\r\n", ""), request), 200);
  EXPECT_EQ(PJ::mcp::checkAccess(request, "secret"), 403);
}

/// The JSON-RPC error code parsing `message` refused it with; 0 when it passed.
int refusal(const json& message) {
  const PJ::mcp::RpcMessage parsed = PJ::mcp::parseRpcMessage(message.dump());
  return parsed.error ? (*parsed.error)["error"].value("code", 0) : 0;
}

TEST(McpEnvelope, OnlyWellFormedRequestsPass) {
  EXPECT_EQ(refusal({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}}), 0);
  EXPECT_EQ(refusal({{"jsonrpc", "2.0"}, {"id", "a"}, {"method", "x"}, {"params", json::object()}}), 0);
  EXPECT_EQ(refusal({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}), 0);
  EXPECT_TRUE(PJ::mcp::parseRpcMessage(R"({"jsonrpc":"2.0","method":"n"})").notification);
  EXPECT_EQ(refusal(json::array()), -32600);
  EXPECT_EQ(refusal({{"id", 1}, {"method", "ping"}}), -32600);
  EXPECT_EQ(refusal({{"jsonrpc", "2.0"}, {"id", json::object()}, {"method", "ping"}}), -32600);
  EXPECT_EQ(refusal({{"jsonrpc", "2.0"}, {"id", 1.5}, {"method", "ping"}}), -32600);
  EXPECT_EQ(refusal({{"jsonrpc", "2.0"}, {"id", 1}, {"method", 5}}), -32600);
  EXPECT_EQ(refusal({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "x"}, {"params", json::array()}}), -32600);
  const PJ::mcp::RpcMessage bad = PJ::mcp::parseRpcMessage("{not json");
  ASSERT_TRUE(bad.error);
  EXPECT_EQ((*bad.error)["error"].value("code", 0), -32700);
}

TEST(McpEnvelope, DeepNestingIsRefusedBeforeAnyToolSeesIt) {
  const auto nested = [](int depth) {
    return R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"x":)" + std::string(depth, '[') +
           std::string(depth, ']') + "}}";
  };
  EXPECT_FALSE(PJ::mcp::parseRpcMessage(nested(PJ::mcp::kMaxJsonDepth - 2)).error);
  const PJ::mcp::RpcMessage deep = PJ::mcp::parseRpcMessage(nested(100'000));
  ASSERT_TRUE(deep.error);
  EXPECT_EQ((*deep.error)["error"].value("code", 0), -32600);
  EXPECT_NE((*deep.error)["error"].value("message", "").find("nested deeper"), std::string::npos);
}

TEST(McpOwners, EveryClientGetsAnAgentNamespace) {
  EXPECT_EQ(PJ::mcp::ownerFor("claude-code"), "agent-claude-code");
  EXPECT_EQ(PJ::mcp::ownerFor("Codex CLI!!"), "agent-codex-cli");
  EXPECT_EQ(PJ::mcp::ownerFor(""), "agent-mcp");
  EXPECT_EQ(PJ::mcp::ownerFor("///"), "agent-mcp");
  // A plugin's id cannot be claimed: the result always carries the agent prefix and no '_'.
  EXPECT_EQ(PJ::mcp::ownerFor("toolbox_assistant_agent"), "agent-toolbox-assistant-agent");
  EXPECT_LE(PJ::mcp::ownerFor(std::string(200, 'x')).size(), std::string("agent-").size() + 32);
  EXPECT_EQ(PJ::mcp::printableClientName("a\r\nX-Evil: 1"), "aX-Evil: 1");
}

TEST(McpJobs, WaitSecondsIsClampedAndOnlyLongToolsTakeIt) {
  using PJ::mcp::waitSeconds;
  EXPECT_EQ(waitSeconds(json::object()), PJ::mcp::kDefaultWaitS);
  EXPECT_EQ(waitSeconds({{"wait_s", 0}}), 0.0);
  EXPECT_EQ(waitSeconds({{"wait_s", 2.5}}), 2.5);
  EXPECT_EQ(waitSeconds({{"wait_s", -3}}), 0.0);
  EXPECT_EQ(waitSeconds({{"wait_s", 1e9}}), PJ::mcp::kMaxWaitS);
  EXPECT_EQ(waitSeconds({{"wait_s", "soon"}}), PJ::mcp::kDefaultWaitS);
  // The tool list and the server agree on which calls can become jobs.
  for (const json& tool : PJ::mcp::toolList(PJ::mcp::ToolSet::kFull)) {
    const std::string name = tool.value("name", "");
    EXPECT_EQ(tool["inputSchema"]["properties"].contains("wait_s"), PJ::mcp::isJobCapableTool(name)) << name;
  }
}

TEST(McpJobs, StoppingAJobIsNotReadOnlyButRunsUnderTheUsersDialog) {
  EXPECT_FALSE(PJ::mcp::isReadOnlyTool("job_kill"));
  EXPECT_TRUE(PJ::mcp::runsUnderUsersDialog("job_kill"));
  EXPECT_TRUE(PJ::mcp::runsUnderUsersDialog("jobs_list"));
  EXPECT_TRUE(PJ::mcp::runsUnderUsersDialog("screenshot"));
  EXPECT_TRUE(PJ::mcp::runsUnderUsersDialog("widget_state"));
  EXPECT_FALSE(PJ::mcp::runsUnderUsersDialog("set_widget_state"));
  EXPECT_FALSE(PJ::mcp::runsUnderUsersDialog("open_file"));
}

TEST(McpUiTools, ListedOnlyWhenTheRunAskedAndAlwaysAllowedUnderTheUsersDialog) {
  using PJ::mcp::ToolSet;
  const auto listed = [](ToolSet set) {
    return PJ::mcp::toolList(set).dump().find("\"ui_click\"") != std::string::npos;
  };
  EXPECT_FALSE(listed(ToolSet::kFull));
  PJ::mcp::setUiToolsEnabled(true);
  EXPECT_TRUE(listed(ToolSet::kFull));
  EXPECT_FALSE(listed(ToolSet::kAgentOnly));
  PJ::mcp::setUiToolsEnabled(false);

  EXPECT_TRUE(PJ::mcp::runsUnderUsersDialog("ui_click"));
  EXPECT_TRUE(PJ::mcp::isReadOnlyTool("ui_tree"));
  EXPECT_FALSE(PJ::mcp::isReadOnlyTool("ui_click"));
  EXPECT_FALSE(PJ::mcp::isUiTool("ui_other"));
  EXPECT_EQ(PJ::mcp::uiSettleMs("ui_tree", {{"wait_ms", 500}}), 0);
  EXPECT_EQ(PJ::mcp::uiSettleMs("ui_click", json::object()), PJ::mcp::kDefaultUiSettleMs);
  EXPECT_EQ(PJ::mcp::uiSettleMs("ui_click", {{"wait_ms", 1e9}}), PJ::mcp::kMaxUiSettleMs);
  // Read before the input is queued: a badly typed one falls back instead of failing after acting.
  EXPECT_EQ(PJ::mcp::uiSettleMs("ui_click", {{"wait_ms", "x"}}), PJ::mcp::kDefaultUiSettleMs);
  EXPECT_EQ(PJ::mcp::uiSettleMs("ui_click", {{"wait_ms", -3.5}}), 0);
  EXPECT_EQ(PJ::mcp::uiSettleMs("ui_click", {{"wait_ms", 1e300}}), PJ::mcp::kMaxUiSettleMs);
}

TEST(McpTasks, TheExtensionIsOfferedOnDiscoverAndDeclaredPerRequest) {
  using PJ::mcp::ToolSet;
  const json discovered = *PJ::mcp::lifecycleResult("server/discover", json::object(), ToolSet::kFull);
  EXPECT_EQ(discovered["capabilities"]["extensions"][PJ::mcp::kTasksExtension], json::object());
  EXPECT_TRUE(discovered["capabilities"].contains("tools"));
  // Nothing remembers an initialize, so a client of that era cannot declare the extension per request.
  const json initialized = *PJ::mcp::lifecycleResult("initialize", json::object(), ToolSet::kFull);
  EXPECT_FALSE(initialized["capabilities"].value("extensions", json::object()).contains(PJ::mcp::kTasksExtension));

  const auto meta = [](const json& capabilities) {
    return json{{"_meta", {{"io.modelcontextprotocol/clientCapabilities", capabilities}}}};
  };
  EXPECT_TRUE(PJ::mcp::declaresTasks(meta({{"extensions", {{PJ::mcp::kTasksExtension, json::object()}}}})));
  EXPECT_FALSE(PJ::mcp::declaresTasks(meta({{"extensions", {{"io.modelcontextprotocol/ui", json::object()}}}})));
  EXPECT_FALSE(PJ::mcp::declaresTasks(meta(json::object())));
  EXPECT_FALSE(PJ::mcp::declaresTasks(meta("yes")));
  EXPECT_FALSE(PJ::mcp::declaresTasks({{"_meta", 3}}));
  EXPECT_FALSE(PJ::mcp::declaresTasks(json::object()));
}

TEST(McpLifecycle, BothEras) {
  const auto init =
      PJ::mcp::lifecycleResult("initialize", {{"protocolVersion", "2025-11-25"}}, PJ::mcp::ToolSet::kFull);
  ASSERT_TRUE(init);
  EXPECT_EQ((*init)["protocolVersion"], "2025-11-25");
  const auto unknown = PJ::mcp::lifecycleResult("initialize", {{"protocolVersion", 7}}, PJ::mcp::ToolSet::kFull);
  ASSERT_TRUE(unknown);
  EXPECT_EQ((*unknown)["protocolVersion"], "2025-06-18");
  const auto discover = PJ::mcp::lifecycleResult("server/discover", json::object(), PJ::mcp::ToolSet::kFull);
  ASSERT_TRUE(discover);
  EXPECT_EQ((*discover)["supportedVersions"][0], "2026-07-28");
  EXPECT_FALSE(PJ::mcp::lifecycleResult("tools/list", json::object(), PJ::mcp::ToolSet::kFull));
}

// What an agent reads back of a plugin's configuration: no secret, at any depth; a secret passed back as the sentinel
// takes the saved value at the same key path, and one with no saved value there is named.
TEST(McpSecrets, ConfigsAreRedactedAndTheSentinelRestoresTheSavedValue) {
  using PJ::mcp::kRedacted;
  const json config = {
      {"host", "broker.local"},
      {"Password", "hunter2"},
      {"empty_password", ""},
      {"auth", {{"bearer_token", "abc"}, {"user", "me"}}},
      {"servers", json::array({{{"api_key", "k1"}}, {{"name", "b"}}})},
      {"credentials", {{"user", "me"}, {"list", json::array({"x"})}}},
      {"port", 1883}};
  const json redacted = PJ::mcp::redactSecrets(config);
  EXPECT_EQ(redacted["host"], "broker.local");
  EXPECT_EQ(redacted["Password"], kRedacted);
  EXPECT_EQ(redacted["empty_password"], "") << "an empty secret says there is none";
  EXPECT_EQ(redacted["auth"]["bearer_token"], kRedacted);
  EXPECT_EQ(redacted["auth"]["user"], "me");
  EXPECT_EQ(redacted["servers"][0]["api_key"], kRedacted);
  EXPECT_EQ(redacted["servers"][1]["name"], "b");
  EXPECT_EQ(redacted["credentials"]["user"], kRedacted) << "everything under a secret key";
  EXPECT_EQ(redacted["credentials"]["list"][0], kRedacted);
  EXPECT_EQ(redacted["port"], 1883);
  EXPECT_EQ(redacted.dump().find("hunter2"), std::string::npos);

  json given = redacted;
  EXPECT_EQ(PJ::mcp::restoreSecrets(given, config), "") << "the unchanged configuration keeps its secrets";
  EXPECT_EQ(given["Password"], "hunter2");
  EXPECT_EQ(given["auth"]["bearer_token"], "abc");
  EXPECT_EQ(given["servers"][0]["api_key"], "k1");
  EXPECT_EQ(given["credentials"], config["credentials"]);

  json new_secret = redacted;
  new_secret["auth"]["bearer_token"] = "mine";
  EXPECT_EQ(PJ::mcp::restoreSecrets(new_secret, config), "") << "a secret the agent passes itself is not a change";
  EXPECT_EQ(new_secret["auth"]["bearer_token"], "mine");
  EXPECT_EQ(new_secret["Password"], "hunter2");

  // A saved secret goes only where it was saved: another host would receive it.
  json other_host = redacted;
  other_host["host"] = "attacker.example";
  const std::string moved = PJ::mcp::restoreSecrets(other_host, config);
  EXPECT_NE(moved.find("changes /host while keeping a saved secret"), std::string::npos) << moved;
  EXPECT_EQ(other_host.dump().find("hunter2"), std::string::npos);

  json other_server = redacted;
  other_server["servers"][1]["name"] = "c";
  const std::string element = PJ::mcp::restoreSecrets(other_server, config);
  EXPECT_NE(element.find("/servers/1/name"), std::string::npos) << element;
  EXPECT_EQ(other_server.dump().find("k1"), std::string::npos);

  json added = redacted;
  added["url"] = "ws://attacker.example";
  EXPECT_NE(PJ::mcp::restoreSecrets(added, config).find("/url"), std::string::npos);

  json plain = {{"host", "attacker.example"}, {"Password", "theirs"}};
  EXPECT_EQ(PJ::mcp::restoreSecrets(plain, config), "") << "no sentinel, nothing saved is used";
  EXPECT_EQ(plain["Password"], "theirs");

  json unknown = {{"mqtt", {{"password", kRedacted}}}};
  EXPECT_NE(
      PJ::mcp::restoreSecrets(unknown, json::object()).find("no saved value for /mqtt/password"), std::string::npos);
  EXPECT_NE(
      PJ::mcp::restoreSecrets(unknown, {{"mqtt", {{"password", 3}}}}).find("no saved value for /mqtt/password"),
      std::string::npos);
}

// Secret keys are recognised however a plugin spells them: case, '_' and '-' do not matter.
TEST(McpSecrets, SecretKeysMatchWhateverTheirCaseAndSeparators) {
  const json config = {{"privateKey", "a"}, {"api-key", "b"}, {"X-API-Key", "c"}, {"private-key", "d"},
                       {"PASS_WORD", "e"},  {"keyring", "f"}, {"tokens", "g"}};
  const json redacted = PJ::mcp::redactSecrets(config);
  for (const char* key : {"privateKey", "api-key", "X-API-Key", "private-key", "PASS_WORD", "tokens"}) {
    EXPECT_EQ(redacted[key], PJ::mcp::kRedacted) << key;
  }
  EXPECT_EQ(redacted["keyring"], "f");
}

// Every tool the server and the bridge list carries a title and all four hints from one table, which the server's own
// read-only gate reads too, and a name short enough for a client's namespaced tool name.
TEST(McpAnnotations, EveryListedToolHasATitleAndAllFourHints) {
  PJ::mcp::setUiToolsEnabled(true);
  json tools = PJ::mcp::toolList(PJ::mcp::ToolSet::kFull);
  PJ::mcp::setUiToolsEnabled(false);
  std::set<std::string> names;
  for (const json& tool : tools) {
    names.insert(tool.value("name", ""));
  }
  for (const json& tool : PJ::mcp::bridgeToolEntries()) {
    names.insert(tool.value("name", ""));
    tools.push_back(tool);
  }
  for (const json& tool : PJ::mcp::toolList(PJ::mcp::ToolSet::kAgentOnly)) {
    names.insert(tool.value("name", ""));
  }
  EXPECT_TRUE(names.contains("set_widget_state"));
  for (const std::string& name : names) {
    const json entry = PJ::mcp::toolEntry(name, "", PJ::mcp::schema(json::object()));
    EXPECT_LE(name.size(), PJ::mcp::kMaxToolNameChars) << name;
    ASSERT_TRUE(entry.contains("annotations")) << name << " is missing from the annotation table";
    const json& hints = entry["annotations"];
    EXPECT_FALSE(hints.value("title", "").empty()) << name;
    EXPECT_EQ(entry.value("title", ""), hints.value("title", "")) << name;
    for (const char* hint : {"readOnlyHint", "destructiveHint", "idempotentHint", "openWorldHint"}) {
      EXPECT_TRUE(hints.contains(hint) && hints[hint].is_boolean()) << name << " " << hint;
    }
    EXPECT_EQ(hints.value("readOnlyHint", false), PJ::mcp::isReadOnlyTool(name)) << name;
    if (hints.value("readOnlyHint", false)) {
      EXPECT_FALSE(hints.value("destructiveHint", true)) << name;
    }
  }
  for (const json& tool : tools) {
    const json properties = tool["inputSchema"].value("properties", json::object());
    for (const auto& [param, property] : properties.items()) {
      EXPECT_FALSE(property.value("description", "").empty()) << tool.value("name", "") << "." << param;
    }
  }
  const auto hints = [](const char* name) { return *PJ::mcp::toolHints(name); };
  EXPECT_TRUE(hints("start_stream").open_world) << "it connects to brokers and peers";
  EXPECT_FALSE(hints("open_file").open_world);
  EXPECT_TRUE(hints("export_series").destructive) << "overwrite replaces a file";
  EXPECT_TRUE(hints("set_widget_state").destructive);
  EXPECT_TRUE(hints("add_script_series").destructive) << "the same name replaces it";
  EXPECT_FALSE(hints("create_derived_series").destructive) << "a taken name is refused";
  EXPECT_TRUE(hints("screenshot").read_only);
  EXPECT_TRUE(hints("widget_state").read_only);
  EXPECT_TRUE(hints("unload_file").idempotent);
  EXPECT_EQ(PJ::mcp::toolHints("no_such_tool"), nullptr);
}

TEST(McpAnnotations, ReportStatusIsOnlyForPluginTokens) {
  const auto listed = [](PJ::mcp::ToolSet set) {
    const json tools = PJ::mcp::toolList(set);
    return std::any_of(tools.begin(), tools.end(), [](const json& tool) { return tool["name"] == "report_status"; });
  };
  EXPECT_FALSE(listed(PJ::mcp::ToolSet::kFull)) << "get_status and the instructions cover it";
  EXPECT_TRUE(listed(PJ::mcp::ToolSet::kAgentOnly));
}
