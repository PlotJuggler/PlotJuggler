// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// End to end, the way an agent sees PlotJuggler: `plotjuggler4 --mcp-bridge` over stdio in front of
// an offscreen `plotjuggler4 --mcp`, both under a private HOME so neither touches the user's
// settings or discovery folder.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QProcess>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <clocale>
#include <memory>
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
    if (!numeric_locale_.isEmpty()) {
      env_.remove("LC_ALL");  // it would win over LC_NUMERIC
      env_.insert("LC_NUMERIC", numeric_locale_);
    }
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
    const QString discovery = instancesDir() + QStringLiteral("/%1.json").arg(app_.processId());
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

  /// The discovery folder under the private HOME.
  [[nodiscard]] QString instancesDir() const {
    return home_.filePath(".local/share/PlotJuggler/PlotJuggler4/mcp-instances");
  }

  /// The bridge's next one-line reply, once it has written the whole line.
  json readReply() {
    QElapsedTimer waited;
    waited.start();
    while (!bridge_.canReadLine() && waited.elapsed() < 30000 && bridge_.state() == QProcess::Running) {
      bridge_.waitForReadyRead(100);
    }
    return json::parse(bridge_.readLine().toStdString(), nullptr, false);
  }

  /// Writes one message on the bridge's stdin, flushed at once: for a case that then waits on the
  /// instance's side, not on the bridge's output.
  void write(const json& message) {
    bridge_.write(QByteArray::fromStdString(message.dump()) + "\n");
    bridge_.waitForBytesWritten(5000);
  }

  /// Sends one line on the bridge's stdin and returns its one-line reply.
  json send(const std::string& line) {
    bridge_.write(QByteArray::fromStdString(line) + "\n");
    return readReply();
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
  QString numeric_locale_;  ///< LC_NUMERIC of the app and the bridge; empty inherits the test's own.
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

/// A second "instance" played by this process: a discovery file naming a server the test answers by
/// hand, so a case sees what the bridge sends and decides when (and whether) each call is answered.
class FakeInstance {
 public:
  /// Listens and writes the discovery file into `instances_dir`.
  explicit FakeInstance(const QString& instances_dir) {
    EXPECT_TRUE(server_.listen(QHostAddress::LocalHost));
    QFile file(instances_dir + QStringLiteral("/%1.json").arg(QCoreApplication::applicationPid()));
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(
        QByteArray::fromStdString(json({{"pid", QCoreApplication::applicationPid()},
                                        {"port", server_.serverPort()},
                                        {"token", "fake-token"},
                                        {"started_ms", 1}})
                                      .dump()));
  }

  /// One request the bridge sent, and the connection to answer it on.
  struct Received {
    json message;
    std::unique_ptr<QTcpSocket> socket;
  };

  /// The next request, waiting up to `timeout_ms` for it; a null socket when none came.
  Received next(int timeout_ms = 10000) {
    if (!server_.hasPendingConnections() && !server_.waitForNewConnection(timeout_ms)) {
      return {};
    }
    std::unique_ptr<QTcpSocket> socket(server_.nextPendingConnection());
    QByteArray bytes;
    qsizetype header_end = -1;
    while ((header_end = bytes.indexOf("\r\n\r\n")) < 0 && socket->waitForReadyRead(5000)) {
      bytes += socket->readAll();
    }
    const qsizetype at = bytes.toLower().indexOf("content-length:");
    const qsizetype length = at < 0 ? 0 : bytes.mid(at + 15, bytes.indexOf('\r', at) - at - 15).trimmed().toLongLong();
    while (bytes.size() - header_end - 4 < length && socket->waitForReadyRead(5000)) {
      bytes += socket->readAll();
    }
    return {json::parse(bytes.mid(header_end + 4).toStdString(), nullptr, false), std::move(socket)};
  }

  /// Answers `received` with `result` (a JSON-RPC result), or with 202 and no body when null.
  static void answer(Received& received, const json& result) {
    send(
        received, result.is_null()
                      ? QByteArray()
                      : QByteArray::fromStdString(
                            json({{"jsonrpc", "2.0"}, {"id", received.message["id"]}, {"result", result}}).dump()));
  }

  /// Answers `received` with the JSON-RPC error `error`.
  static void refuse(Received& received, const json& error) {
    send(
        received,
        QByteArray::fromStdString(json({{"jsonrpc", "2.0"}, {"id", received.message["id"]}, {"error", error}}).dump()));
  }

 private:
  /// One HTTP response carrying `body` (202 when empty), flushed before the connection closes.
  static void send(Received& received, const QByteArray& body) {
    received.socket->write(
        (body.isEmpty() ? QByteArray("HTTP/1.1 202 Accepted\r\n") : QByteArray("HTTP/1.1 200 OK\r\n")) +
        "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    received.socket->disconnectFromHost();
    if (received.socket->state() != QAbstractSocket::UnconnectedState) {
      received.socket->waitForDisconnected(5000);
    }
  }

  QTcpServer server_;
};

/// A bridge session that uses a FakeInstance. Two instances are live (the app the base fixture
/// started, and this process), so the session picks this one, which the bridge pings first.
class McpBridgeFakeInstanceTest : public McpBridgeTest {
 protected:
  void SetUp() override {
    McpBridgeTest::SetUp();
    fake_ = std::make_unique<FakeInstance>(instancesDir());
    write(toolsCall(1, "select_instance", {{"id", QCoreApplication::applicationPid()}}));
    FakeInstance::Received ping = fake_->next();
    ASSERT_NE(ping.socket, nullptr);
    ASSERT_EQ(ping.message.value("method", ""), "ping");
    FakeInstance::answer(ping, json::object());
    ASSERT_FALSE(readReply()["result"].value("isError", false));
  }

  static json toolsCall(int id, const char* tool, const json& args) {
    return {
        {"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"}, {"params", {{"name", tool}, {"arguments", args}}}};
  }

  std::unique_ptr<FakeInstance> fake_;
};

TEST_F(McpBridgeFakeInstanceTest, ACancellationReachesTheInstanceWhileItsCallIsInFlight) {
  const auto cancel = [](int id) {
    return json{{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", id}}}};
  };
  // A call in flight, then its cancellation, then the next call, all before the instance answers.
  write(toolsCall(2, "open_file", {{"path", "/big.mcap"}}));
  FakeInstance::Received held = fake_->next();
  ASSERT_NE(held.socket, nullptr);
  ASSERT_EQ(held.message["params"].value("name", ""), "open_file");
  const json forwarded_id = held.message["id"];
  EXPECT_TRUE(forwarded_id.is_string()) << "the bridge's own id, unique across bridges: " << forwarded_id.dump();
  // The method spelled with a JSON escape, as a client's serializer may: still that notification.
  bridge_.write(
      R"({"jsonrpc":"2.0","method":"notifications\/cancelled","params":{"requestId":2}})"
      "\n");
  write(toolsCall(3, "get_status", json::object()));
  write(cancel(3));  // cancelled while it waits behind call 2: it is never sent
  write(toolsCall(4, "get_status", json::object()));

  FakeInstance::Received cancelled = fake_->next();
  ASSERT_NE(cancelled.socket, nullptr) << "the cancellation waited for the call to end";
  EXPECT_EQ(cancelled.message.value("method", ""), "notifications/cancelled");
  EXPECT_EQ(cancelled.message["params"]["requestId"], forwarded_id);
  EXPECT_FALSE(cancelled.message.contains("id"));
  FakeInstance::answer(cancelled, nullptr);
  // The instance ends the cancelled call; the client gets no response for it.
  FakeInstance::answer(held, {{"content", json::array({{{"type", "text"}, {"text", "killed"}}})}, {"isError", true}});

  FakeInstance::Received status = fake_->next();
  ASSERT_NE(status.socket, nullptr);
  EXPECT_EQ(status.message["params"].value("name", ""), "get_status");
  EXPECT_NE(status.message["id"], forwarded_id);
  FakeInstance::answer(status, {{"content", json::array({{{"type", "text"}, {"text", "fine"}}})}});
  const json reply = readReply();
  EXPECT_EQ(reply.value("id", 0), 4) << "a cancelled call was answered: " << reply.dump();
  EXPECT_EQ(reply["result"]["content"][0].value("text", ""), "fine");
  EXPECT_EQ(fake_->next(300).socket, nullptr) << "the call cancelled in the queue was sent";
}

// The MCP Tasks extension through the bridge: its methods and a task handle pass through whole.
TEST_F(McpBridgeFakeInstanceTest, TheTasksMethodsAndATaskHandlePassThroughWhole) {
  const json handle = {{"resultType", "task"}, {"taskId", "abc"}, {"status", "working"}};
  write(toolsCall(2, "open_file", {{"path", "/big.mcap"}}));
  FakeInstance::Received call = fake_->next();
  ASSERT_NE(call.socket, nullptr);
  FakeInstance::answer(call, handle);
  EXPECT_EQ(readReply()["result"], handle) << "the bridge rewrote a task handle";

  const json get_task = {
      {"jsonrpc", "2.0"},
      {"id", "t-9"},
      {"method", "tasks/get"},
      {"params", {{"taskId", "abc"}, {"_meta", {{"k", 1}}}}}};
  write(get_task);
  FakeInstance::Received passed = fake_->next();
  ASSERT_NE(passed.socket, nullptr) << "tasks/get was answered by the bridge itself";
  EXPECT_EQ(passed.message.value("method", ""), "tasks/get");
  EXPECT_EQ(passed.message["params"], get_task["params"]);
  FakeInstance::refuse(passed, {{"code", -32602}, {"message", "Failed to retrieve task: Task not found"}});
  const json relayed = readReply();
  EXPECT_EQ(relayed.value("id", ""), "t-9") << relayed.dump();
  EXPECT_EQ(relayed["error"].value("code", 0), -32602) << relayed.dump();
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

/// An installed locale that writes 1.5 as "1,5", or empty when the machine has none.
QString decimalCommaLocale() {
  const std::string saved = std::setlocale(LC_NUMERIC, nullptr);
  QString found;
  for (const char* name : {"sv_SE.UTF-8", "de_DE.UTF-8", "fr_FR.UTF-8", "en_DK.UTF-8"}) {
    if (std::setlocale(LC_NUMERIC, name) != nullptr && *std::localeconv()->decimal_point == ',') {
      found = QString::fromLatin1(name);
      break;
    }
  }
  std::setlocale(LC_NUMERIC, saved.c_str());
  return found;
}

class McpBridgeDecimalCommaTest : public McpBridgeTest {
 protected:
  void SetUp() override {
    numeric_locale_ = decimalCommaLocale();
    if (numeric_locale_.isEmpty()) {
      GTEST_SKIP() << "no decimal-comma locale installed";
    }
    McpBridgeTest::SetUp();
  }
};

// Luau reads number literals with the C library's strtod, so under the user's decimal-comma LC_NUMERIC neither the
// bundled filters nor a script would compile.
TEST_F(McpBridgeDecimalCommaTest, FiltersAndScriptsCompile) {
  ASSERT_TRUE(call("open_file", {{"path", data_file_.toStdString()}}).value("loaded", false));
  call("add_filter", {{"input", "mock/file_data/value"}, {"filter", "absolute"}});
  const json evaluated = call("evaluate", {{"inputs", {"mock/file_data/value"}}, {"expression", "value * 0 + 1.5"}});
  ASSERT_TRUE(evaluated.is_object());
  EXPECT_DOUBLE_EQ(evaluated.value("stats", json::object()).value("max", 0.0), 1.5) << evaluated.dump();
}

}  // namespace
