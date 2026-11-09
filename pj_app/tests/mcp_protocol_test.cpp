// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The MCP server's HTTP and JSON-RPC gates, without a server: what a hostile or broken client can
// send must be refused with the right status, never crash or be served.

#include <gtest/gtest.h>

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
