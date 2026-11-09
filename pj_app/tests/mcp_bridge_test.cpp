// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// End to end, the way an agent sees PlotJuggler: `plotjuggler4 --mcp-bridge` over stdio in front of
// an offscreen `plotjuggler4 --mcp`, both under a private HOME so neither touches the user's
// settings or discovery folder.

#include <gtest/gtest.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QProcess>
#include <QSettings>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <nlohmann/json.hpp>
#include <set>
#include <string>

using json = nlohmann::json;  // NOLINT(readability-identifier-naming)

namespace {

class McpBridgeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(home_.isValid());
    env_ = QProcessEnvironment::systemEnvironment();
    env_.insert("HOME", home_.path());
    env_.insert("XDG_CONFIG_HOME", home_.filePath(".config"));
    env_.insert("XDG_DATA_HOME", home_.filePath(".local/share"));
    env_.insert("XDG_CACHE_HOME", home_.filePath(".cache"));
    env_.insert("QT_QPA_PLATFORM", "offscreen");
    env_.insert("PJ_DISABLE_TELEMETRY", "1");
    env_.remove("WAYLAND_DISPLAY");  // nothing may reach the session's display
    {
      QSettings settings(home_.filePath(".config/PlotJuggler/PlotJuggler4.conf"), QSettings::IniFormat);
      settings.setValue("Preferences::check_updates_on_startup", false);
    }
    const QString plugins = home_.filePath("plugins");
    ASSERT_TRUE(QDir().mkpath(plugins));
    const QString plugin = QString::fromUtf8(PJ_MOCK_FILE_SOURCE_PLUGIN_PATH);
    ASSERT_TRUE(QFile::copy(plugin, plugins + "/" + QFileInfo(plugin).fileName()));
    data_file_ = home_.filePath("a.mock");
    QFile data(data_file_);
    ASSERT_TRUE(data.open(QIODevice::WriteOnly));

    app_.setProcessEnvironment(env_);
    app_.setProcessChannelMode(QProcess::MergedChannels);
    app_.start(QString::fromUtf8(PJ_APP_PATH), {"--nosplash", "--mcp", "--plugin-dir", plugins});
    ASSERT_TRUE(app_.waitForStarted());
    const QString discovery =
        home_.filePath(".local/share/PlotJuggler/PlotJuggler4/mcp-instances/%1.json").arg(app_.processId());
    QElapsedTimer waited;
    waited.start();
    while (!QFile::exists(discovery) && waited.elapsed() < 60000 && app_.state() == QProcess::Running) {
      app_.waitForReadyRead(100);
    }
    ASSERT_TRUE(QFile::exists(discovery)) << app_.readAll().toStdString();
    QFile file(discovery);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    instance_ = json::parse(file.readAll().toStdString(), nullptr, false);
    ASSERT_TRUE(instance_.is_object());
    EXPECT_FALSE(instance_.contains("exe"));
    EXPECT_EQ(file.permissions() & (QFile::ReadGroup | QFile::ReadOther), QFile::Permissions());

    bridge_.setProcessEnvironment(env_);
    bridge_.start(QString::fromUtf8(PJ_APP_PATH), {"--mcp-bridge"});
    ASSERT_TRUE(bridge_.waitForStarted());
  }

  void TearDown() override {
    bridge_.closeWriteChannel();
    if (!bridge_.waitForFinished(5000)) {
      bridge_.kill();
      bridge_.waitForFinished();
    }
    app_.terminate();
    if (!app_.waitForFinished(10000)) {
      app_.kill();
      app_.waitForFinished();
    }
  }

  /// Sends one line on the bridge's stdin and returns its one-line reply.
  json send(const std::string& line) {
    bridge_.write(QByteArray::fromStdString(line) + "\n");
    QElapsedTimer waited;
    waited.start();
    while (!bridge_.canReadLine() && waited.elapsed() < 30000 && bridge_.state() == QProcess::Running) {
      bridge_.waitForReadyRead(100);
    }
    return json::parse(bridge_.readLine().toStdString(), nullptr, false);
  }

  json request(const std::string& method, const json& params) {
    return send(json({{"jsonrpc", "2.0"}, {"id", ++id_}, {"method", method}, {"params", params}}).dump());
  }

  /// A tools/call result's text as JSON, failing the test on a tool error.
  json call(const std::string& tool, const json& args) {
    const json reply = request("tools/call", {{"name", tool}, {"arguments", args}});
    const json& result = reply.value("result", json::object());
    const json content = result.value("content", json::array());
    const std::string text = content.empty() ? std::string() : content[0].value("text", "");
    EXPECT_FALSE(result.value("isError", false)) << tool << ": " << text;
    return json::parse(text, nullptr, false);
  }

  QTemporaryDir home_;
  QProcessEnvironment env_;
  QString data_file_;
  QProcess app_;
  QProcess bridge_;
  json instance_;
  int id_ = 0;
};

TEST_F(McpBridgeTest, AnAgentSessionOverStdio) {
  // 2025 era: initialize names the client, which becomes the owner.
  const json init =
      request("initialize", {{"protocolVersion", "2025-06-18"}, {"clientInfo", {{"name", "toolbox_assistant_agent"}}}});
  EXPECT_EQ(init["result"].value("protocolVersion", ""), "2025-06-18");
  EXPECT_TRUE(init["result"].contains("instructions"));
  // 2026 era: stateless discovery.
  const json discover = request("server/discover", json::object());
  EXPECT_EQ(discover["result"]["supportedVersions"][0], "2026-07-28");

  // Forwarded to the live instance, with the bridge's own tools added.
  const json list = request("tools/list", json::object());
  std::set<std::string> names;
  for (const json& tool : list["result"]["tools"]) {
    names.insert(tool.value("name", ""));
  }
  for (const char* expected : {"open_file", "read_series", "export_series", "plot_tab", "launch", "list_instances"}) {
    EXPECT_TRUE(names.contains(expected)) << expected;
  }
  for (const char* dropped : {"list_catalog", "get_values", "statistics"}) {
    EXPECT_FALSE(names.contains(dropped)) << dropped;
  }

  const json opened = call("open_file", {{"path", data_file_.toStdString()}});
  EXPECT_TRUE(opened.value("loaded", false));
  // The instance's own instructions come through, with what it has loaded.
  const json reinit = request("initialize", {{"protocolVersion", "2025-06-18"}});
  EXPECT_NE(reinit["result"].value("instructions", "").find("mock/file_data"), std::string::npos) << reinit.dump();
  const json rediscover = request("server/discover", json::object());
  EXPECT_NE(rediscover["result"].value("instructions", "").find("mock/file_data"), std::string::npos)
      << rediscover.dump();
  const json read = call(
      "read_series", {{"paths", {"mock/file_data/value"}}, {"mode", "raw"}, {"t_start_s", -1.0}, {"t_end_s", 1.0e9}});
  EXPECT_NE(read.dump().find("30"), std::string::npos) << read.dump();  // the mock's samples: 10, 20, 30

  // The client named itself after a plugin id; it still acts in an agent namespace.
  EXPECT_EQ(call("get_status", json::object()).value("agent", ""), "agent-toolbox-assistant-agent");
}

TEST_F(McpBridgeTest, MalformedEnvelopesAreRefusedNotCrashed) {
  const json garbage = send("not json");
  EXPECT_EQ(garbage["error"].value("code", 0), -32700);
  const json bad_id = send(R"({"jsonrpc":"2.0","id":{},"method":"ping"})");
  EXPECT_EQ(bad_id["error"].value("code", 0), -32600);
  EXPECT_TRUE(bad_id["id"].is_null());
  const json bad_params = send(R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":[1]})");
  EXPECT_EQ(bad_params["error"].value("code", 0), -32600);
  EXPECT_EQ(bad_params["id"], 7);
  const json bad_args =
      send(R"({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"open_file","arguments":{"path":5}}})");
  EXPECT_TRUE(bad_args["result"].value("isError", false)) << bad_args.dump();
  const json deep = send(
      R"({"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"get_status","arguments":{"x":)" +
      std::string(1000, '[') + std::string(1000, ']') + "}}}");
  EXPECT_EQ(deep["error"].value("code", 0), -32600) << deep.dump();
  EXPECT_EQ(deep["id"], 9);
  const json huge =
      send(R"({"jsonrpc":"2.0","id":10,"method":"ping","params":{"pad":")" + std::string(1200 * 1024, 'x') + R"("}})");
  EXPECT_EQ(huge["error"].value("code", 0), -32600) << huge.dump().substr(0, 200);
  // Still serving.
  EXPECT_EQ(request("ping", json::object())["result"], json::object());
}

TEST_F(McpBridgeTest, DirectHttpCannotPickAnOwnerOrSkipTheToken) {
  const auto post = [this](const QByteArray& headers, const json& message) {
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, instance_.value("port", quint16{0}));
    EXPECT_TRUE(socket.waitForConnected(5000));
    const QByteArray body = QByteArray::fromStdString(message.dump());
    socket.write(
        "POST /mcp HTTP/1.1\r\n" + headers + "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
    QByteArray response;
    while (socket.waitForReadyRead(10000)) {
      response += socket.readAll();
    }
    return response;
  };
  const QByteArray auth = "Authorization: Bearer " + QByteArray::fromStdString(instance_.value("token", "")) + "\r\n";
  const json status = {
      {"jsonrpc", "2.0"},
      {"id", 1},
      {"method", "tools/call"},
      {"params", {{"name", "get_status"}, {"arguments", json::object()}}}};
  EXPECT_TRUE(post({}, status).startsWith("HTTP/1.1 401"));
  EXPECT_TRUE(post(auth + "Origin: http://example.com\r\n", status).startsWith("HTTP/1.1 403"));
  // The spike's raw owner header is ignored; the client header is mapped into the agent namespace.
  const QByteArray reply =
      post(auth + "X-PJ-Owner: toolbox_assistant_agent\r\nX-PJ-Client: toolbox_assistant_agent\r\n", status);
  ASSERT_TRUE(reply.startsWith("HTTP/1.1 200")) << reply.toStdString();
  EXPECT_TRUE(reply.contains("agent-toolbox-assistant-agent")) << reply.toStdString();
  EXPECT_FALSE(reply.contains("\\\"agent\\\":\\\"toolbox_assistant_agent")) << reply.toStdString();
}

}  // namespace
