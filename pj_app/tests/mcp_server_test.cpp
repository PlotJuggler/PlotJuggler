// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The in-app MCP server over real loopback HTTP: held replies that wait for the user (unload with
// dependent series), the user's dialogs first (the modal gate), the dialogs an agent opens for the
// user (titled as such), the socket cap, the export file, what closing public access leaves
// behind, and a toolbox plugin's private endpoint ("pj.mcp_endpoint.v1"): its owner, its tool
// profile, its lifetime and its revocation.

#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QPointer>
#include <QRectF>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "FileLoader.h"
#include "MainWindow.h"
#include "PluginPlotTabsController.h"
#include "PreferencesDialog.h"
#include "StreamingSourceManager.h"
#include "mcp/McpBrowserLink.h"
#include "mcp/McpProtocol.h"
#include "mcp/McpServer.h"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_registry.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/SessionManager.h"
#include "pj_widgets/Dialog.h"
#include "pj_widgets/MessageBox.h"
#include "pj_widgets/ToggleSwitch.h"
#include "support/loader_test_support.h"

using json = nlohmann::json;  // NOLINT(readability-identifier-naming)
using pj_app_test::pumpUntil;
using namespace Qt::StringLiterals;

namespace PJ {

/// Names the outcome in a failed comparison.
void PrintTo(LoadOutcome outcome, std::ostream* out) {
  *out << (outcome == LoadOutcome::kLoaded ? "kLoaded" : outcome == LoadOutcome::kFailed ? "kFailed" : "kCancelled");
}

/// Reaches the MCP internals the tests need: the window's server (and its destruction), the
/// public token and discovery file, and the deferred queue.
class McpTestPeer {
 public:
  /// Null until public access or a plugin's endpoint started it.
  [[nodiscard]] static McpServer* server(MainWindow& window) {
    return window.mcp_server_.get();
  }
  /// Destroys the window's server, as ~MainWindow does.
  static void destroyServer(MainWindow& window) {
    window.mcp_server_.reset();
  }
  [[nodiscard]] static std::string publicToken(const McpServer& server) {
    return server.public_token_.toStdString();
  }
  [[nodiscard]] static QString discoveryPath(const McpServer& server) {
    return server.discovery_path_;
  }
  [[nodiscard]] static std::size_t deferred(const McpServer& server) {
    return server.deferred_.size();
  }
  /// Queues `resume` as a deferred "evaluate" result of the caller holding `token` on `socket`;
  /// `reply` gets its terminal result.
  static void defer(
      McpServer& server, const QByteArray& token, QTcpSocket* socket,
      std::function<agent_tools::ToolResult(agent_tools::ToolContext&)> resume, McpServer::Reply reply) {
    server.runAgentTool(
        "evaluate", callerOf(server, token, socket), agent_tools::ToolResult::deferred(std::move(resume)),
        std::move(reply));
  }
  /// Queues, for `token` and `socket`, a deferred result that never finishes; `resumes` counts
  /// its resumes. Its reply must never run.
  static void deferForever(McpServer& server, const QByteArray& token, QTcpSocket* socket, int& resumes) {
    struct Forever {
      int* resumes;
      agent_tools::ToolResult operator()(agent_tools::ToolContext&) const {
        ++*resumes;
        return agent_tools::ToolResult::deferred(*this);
      }
    };
    defer(server, token, socket, Forever{&resumes}, [](const json&) {
      ADD_FAILURE() << "a result nobody reads was sent";
    });
  }
  /// One drain of the deferred queue, as a resume tick runs it (a reply or continuation may call it
  /// again, as a nested event loop would).
  static void resumeDeferred(McpServer& server) {
    server.resumeDeferred();
  }
  /// Calls tool `name` as the caller holding `token` on `socket`, without HTTP, so a test can act
  /// between the call and the work it queues.
  static void callTool(
      McpServer& server, const QByteArray& token, QTcpSocket* socket, const std::string& name, const json& args,
      McpServer::Reply reply) {
    server.callTool(name, args, callerOf(server, token, socket), std::move(reply));
  }

 private:
  static McpServer::Caller callerOf(const McpServer& server, const QByteArray& token, QTcpSocket* socket) {
    return {.token = token, .owner = token == server.public_token_ ? u"agent-test"_s : QString(), .connection = socket};
  }
};

}  // namespace PJ

namespace {

/// What one HTTP exchange returned: the status, the bytes after the header, and how many HTTP
/// responses the server wrote on the connection.
struct Answer {
  int status = 0;
  json body;
  int responses = 0;
};

/// Every visible top-level of type T.
template <typename T>
std::vector<T*> visible() {
  std::vector<T*> found;
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    if (auto* typed = qobject_cast<T*>(widget); typed != nullptr && typed->isVisible()) {
      found.push_back(typed);
    }
  }
  return found;
}

/// Runs `on_destroyed` when the last copy of the returned token dies: capture it in a
/// continuation to see when the server drops that continuation.
std::shared_ptr<int> lifetimeToken(std::function<void()> on_destroyed) {
  return {new int(0), [on_destroyed = std::move(on_destroyed)](const int* token) {
            delete token;
            on_destroyed();
          }};
}

class McpServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_.isValid());
    ASSERT_TRUE(data_.isValid());
    for (const char* plugin :
         {PJ_MOCK_FILE_SOURCE_PLUGIN_PATH, PJ_MOCK_LAYOUT_STREAM_PLUGIN_PATH, PJ_DIALOG_PROBE_SOURCE_PLUGIN_PATH}) {
      const QString path = QString::fromUtf8(plugin);
      ASSERT_TRUE(QFile::copy(path, extensions_.filePath(QFileInfo(path).fileName())));
    }
    window_ = std::make_unique<PJ::MainWindow>(extensions_.path());
    if (open_public_) {
      openPublic();
    }
  }

  /// The window's MCP server, or null until something started it.
  [[nodiscard]] PJ::McpServer* server() const {
    return PJ::McpTestPeer::server(*window_);
  }

  /// Opens public access and points the harness at it (token_, port_).
  void openPublic() {
    window_->openMcpPublicAccess();
    ASSERT_NE(server(), nullptr);
    ASSERT_TRUE(server()->publicAccess());
    token_ = PJ::McpTestPeer::publicToken(*server());
    port_ = server()->port();
  }

  /// Sends one JSON-RPC request with `token` on a fresh connection; the caller reads the reply.
  std::unique_ptr<QTcpSocket> request(const json& message, const std::string& token, const std::string& client) const {
    auto socket = std::make_unique<QTcpSocket>();
    socket->connectToHost(QHostAddress::LocalHost, port_);
    EXPECT_TRUE(pumpUntil([&] { return socket->state() == QAbstractSocket::ConnectedState; }));
    const QByteArray body = QByteArray::fromStdString(message.dump());
    socket->write(
        "POST /mcp HTTP/1.1\r\nAuthorization: Bearer " + QByteArray::fromStdString(token) +
        "\r\nX-PJ-Client: " + QByteArray::fromStdString(client) +
        "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
    return socket;
  }

  static json toolsCall(const std::string& tool, const json& args) {
    return {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", tool}, {"arguments", args}}}};
  }

  /// Sends one tools/call with the public token; the caller reads the reply.
  std::unique_ptr<QTcpSocket> post(const std::string& tool, const json& args) const {
    return request(toolsCall(tool, args), token_, "test");
  }

  /// Pumps the event loop until the server closes `socket`, and parses what it sent.
  static Answer exchange(QTcpSocket& socket) {
    QByteArray received;
    EXPECT_TRUE(pumpUntil([&] {
      received += socket.readAll();
      return socket.state() == QAbstractSocket::UnconnectedState;
    }));
    received += socket.readAll();
    Answer answer;
    answer.responses = static_cast<int>(received.count("HTTP/1.1 "));
    answer.status = received.startsWith("HTTP/1.1 ") ? received.mid(9, 3).toInt() : 0;
    const qsizetype header_end = received.indexOf("\r\n\r\n");
    if (header_end >= 0) {
      answer.body = json::parse(received.mid(header_end + 4).toStdString(), nullptr, false);
    }
    return answer;
  }

  /// POSTs one tools/call and pumps the event loop until the server closes the connection; while
  /// waiting, `while_waiting` runs every 50 ms (it answers the user's prompt). A timer drives it
  /// because a desktop prompt runs its own modal loop inside the server's request handler.
  Answer call(const std::string& tool, const json& args, const std::function<void()>& while_waiting = {}) const {
    QTimer answerer;
    if (while_waiting) {
      QObject::connect(&answerer, &QTimer::timeout, while_waiting);
      answerer.start(50);
    }
    const std::unique_ptr<QTcpSocket> socket = post(tool, args);
    return exchange(*socket);
  }

  /// One JSON-RPC exchange with `token`, from a client claiming the name `client`.
  Answer rpcAs(const std::string& token, const json& message, const std::string& client = "evil") const {
    const std::unique_ptr<QTcpSocket> socket = request(message, token, client);
    return exchange(*socket);
  }

  /// One tools/call with `token`, from a client claiming the name `client`.
  Answer callAs(
      const std::string& token, const std::string& tool, const json& args, const std::string& client = "evil") const {
    return rpcAs(token, toolsCall(tool, args), client);
  }

  /// The text of a tool result, and whether it is an error.
  static std::pair<std::string, bool> result(const Answer& answer) {
    const json& r = answer.body.value("result", json::object());
    const json content = r.value("content", json::array());
    return {content.empty() ? std::string() : content[0].value("text", ""), r.value("isError", false)};
  }

  /// Loads one mock file (samples at 100, 200, 300 ns); returns the dataset id.
  int loadMock() {
    const auto [text, error] =
        result(call("open_file", {{"path", pj_app_test::makeMockFile(data_, "a.mock").toStdString()}}));
    EXPECT_FALSE(error) << text;
    return json::parse(text, nullptr, false).value("dataset_id", 0);
  }

  /// A Luau series `name` doubling the mock's value.
  std::pair<std::string, bool> addDoubled(const std::string& name) {
    return result(call(
        "add_script_series",
        {{"name", name},
         {"inputs", {"mock/file_data/value"}},
         {"script",
          "return { name = \"doubled\", create = function(params) return { calculate = function(t, v) "
          "return v * 2 end } end }"}}));
  }

  /// Loads one mock file and puts a derived series on top of it; returns the dataset id.
  int loadWithDependentSeries() {
    const int dataset = loadMock();
    const auto [text, error] = addDoubled("doubled");
    EXPECT_FALSE(error) << text;
    return dataset;
  }

  /// Clicks `label` on the visible message box, once.
  static std::function<void()> clicker(const QString& label, bool& clicked) {
    return [label, &clicked] {
      for (PJ::MessageBox* box : visible<PJ::MessageBox>()) {
        for (QAbstractButton* button : box->findChildren<QAbstractButton*>()) {
          if (!clicked && button->text() == label) {
            clicked = true;
            button->click();
            return;
          }
        }
      }
    };
  }

  QTemporaryDir extensions_;
  QTemporaryDir data_;
  std::unique_ptr<PJ::MainWindow> window_;
  quint16 port_ = 0;
  /// The public token openPublic got; kept after public access closes.
  std::string token_;
  /// SetUp opens public access.
  bool open_public_ = true;
};

TEST_F(McpServerTest, UnloadWithDependentSeriesRepliesOnceAfterTheUserConfirms) {
  const int dataset = loadWithDependentSeries();
  ASSERT_NE(dataset, 0);
  bool clicked = false;
  const Answer answer = call("unload_file", {{"dataset", dataset}}, clicker(QStringLiteral("Delete"), clicked));
  EXPECT_TRUE(clicked) << "the user was never asked";
  EXPECT_EQ(answer.responses, 1);
  const auto [text, error] = result(answer);
  EXPECT_FALSE(error) << text;
  EXPECT_EQ(json::parse(text, nullptr, false).value("unloaded", 0), dataset);
}

TEST_F(McpServerTest, UnloadWithDependentSeriesRepliesOnceAfterTheUserCancels) {
  const int dataset = loadWithDependentSeries();
  ASSERT_NE(dataset, 0);
  bool clicked = false;
  const Answer answer = call("unload_file", {{"dataset", dataset}}, clicker(QStringLiteral("Cancel"), clicked));
  EXPECT_TRUE(clicked) << "the user was never asked";
  EXPECT_EQ(answer.responses, 1);
  const auto [text, error] = result(answer);
  EXPECT_TRUE(error);
  EXPECT_NE(text.find("not unloaded"), std::string::npos) << text;
}

TEST_F(McpServerTest, AnAgentTabNamesTheAgentNotAMissingPlugin) {
  const auto [text, error] = result(call("plot_tab", {{"action", "create"}, {"tab", "mine"}, {"title", "Mine"}}));
  ASSERT_FALSE(error) << text;
  const PJ::PlotDocker* docker =
      window_->pluginPlotTabs().ownedTab(QStringLiteral("agent-test"), QStringLiteral("mine"));
  ASSERT_NE(docker, nullptr);
  EXPECT_TRUE(docker->isOwnerPluginAvailable());
  EXPECT_EQ(docker->toolTip(), QString());
}

TEST_F(McpServerTest, UnderTheUsersDialogOnlyReadsRun) {
  ASSERT_NE(loadMock(), 0);
  std::optional<Answer> status;
  std::optional<Answer> change;
  bool busy = false;
  QTimer probe;
  QObject::connect(&probe, &QTimer::timeout, [&] {
    const std::vector<PJ::MessageBox*> boxes = visible<PJ::MessageBox>();
    if (busy || boxes.empty()) {
      return;
    }
    busy = true;
    status = call("get_status", json::object());
    change = call("add_filter", {{"input", "mock/file_data/value"}, {"filter", "absolute"}});
    boxes.front()->reject();
  });
  probe.start(50);
  // The user's own prompt, modal like every desktop question.
  QTimer::singleShot(0, window_.get(), [this] {
    PJ::MessageBox::questionAsync(
        window_.get(), QStringLiteral("Hold on"), QStringLiteral("The user is deciding."),
        {{QStringLiteral("OK"), PJ::MessageBox::kPrimaryRole}}, {});
  });
  ASSERT_TRUE(pumpUntil([&] { return change.has_value() && visible<PJ::MessageBox>().empty(); }));
  const auto [status_text, status_error] = result(*status);
  ASSERT_FALSE(status_error) << status_text;
  EXPECT_EQ(json::parse(status_text, nullptr, false).value("user_dialog", ""), "Hold on");
  const auto [change_text, change_error] = result(*change);
  EXPECT_TRUE(change_error);
  EXPECT_NE(change_text.find("'Hold on' open"), std::string::npos) << change_text;
}

TEST_F(McpServerTest, ADialogAnAgentOpensSaysSo) {
  QFile probe_file(data_.filePath("x.dlgprobe"));
  ASSERT_TRUE(probe_file.open(QIODevice::WriteOnly));
  probe_file.close();
  QString title;
  const auto close_it = [&title] {
    for (PJ::Dialog* dialog : visible<PJ::Dialog>()) {
      if (title.isEmpty()) {
        title = dialog->dialogTitle();
        dialog->reject();
      }
    }
  };
  const auto [text, error] =
      result(call("open_file", {{"path", probe_file.fileName().toStdString()}, {"ask_user", true}}, close_it));
  EXPECT_TRUE(error);
  EXPECT_NE(text.find("cancelled"), std::string::npos) << text;
  EXPECT_TRUE(title.startsWith(QStringLiteral("An AI agent asked you to choose: "))) << title.toStdString();

  // The stream's own dialog too, for the source named, whatever the Stream combo shows.
  title.clear();
  const auto [stream_text, stream_error] =
      result(call("start_stream", {{"source", "Mock Layout Stream"}, {"ask_user", true}}, close_it));
  EXPECT_TRUE(stream_error);
  EXPECT_NE(stream_text.find("cancelled"), std::string::npos) << stream_text;
  EXPECT_TRUE(title.startsWith(QStringLiteral("An AI agent asked you to choose: "))) << title.toStdString();
  const auto [unknown_text, unknown_error] =
      result(call("start_stream", {{"source", "No Such Source"}, {"ask_user", true}}));
  EXPECT_TRUE(unknown_error);
  EXPECT_NE(unknown_text.find("unknown stream source"), std::string::npos) << unknown_text;
}

TEST_F(McpServerTest, ClosingPublicAccessBeforeAQueuedStreamDialogOpensStartsNothing) {
  int started = 0;
  QObject::connect(
      &window_->streamingSources(), &PJ::StreamingSourceManager::streamStarted, window_.get(),
      [&started](PJ::DatasetId) { ++started; });
  bool dialog_seen = false;
  QTimer watcher;
  QObject::connect(&watcher, &QTimer::timeout, [&dialog_seen] {
    for (PJ::Dialog* dialog : visible<PJ::Dialog>()) {
      dialog_seen = true;
      dialog->reject();
    }
  });
  watcher.start(10);
  QTcpSocket client;  // stands for the caller's live connection
  // The dialog opens once the request has returned: revoke the token before that.
  PJ::McpTestPeer::callTool(
      *server(), QByteArray::fromStdString(token_), &client, "start_stream",
      {{"source", "Mock Layout Stream"}, {"ask_user", true}}, [](const json&) {});
  window_->closeMcpPublicAccess();
  static_cast<void>(pumpUntil([] { return false; }, 300));
  EXPECT_FALSE(dialog_seen) << "the agent's dialog opened after its access was revoked";
  EXPECT_EQ(started, 0);
}

TEST_F(McpServerTest, ClosingPublicAccessUnderAnAgentsDialogAppliesNothing) {
  // Each time: the user accepts the agent's dialog right after public access closed.
  bool accepted = false;
  const auto close_then_accept = [this, &accepted] {
    for (PJ::Dialog* dialog : visible<PJ::Dialog>()) {
      if (!accepted) {
        accepted = true;
        window_->closeMcpPublicAccess();
        dialog->accept();
      }
    }
  };

  std::optional<PJ::LoadOutcome> outcome;
  QObject::connect(
      &window_->fileLoader(), &PJ::FileLoader::loadFinished, window_.get(),
      [&outcome](quint64, PJ::LoadOutcome settled, const QString&, PJ::DatasetId, const QVector<PJ::DatasetId>&) {
        outcome = settled;
      });
  QFile probe_file(data_.filePath("x.dlgprobe"));
  ASSERT_TRUE(probe_file.open(QIODevice::WriteOnly));
  probe_file.close();
  EXPECT_EQ(
      call("open_file", {{"path", probe_file.fileName().toStdString()}, {"ask_user", true}}, close_then_accept)
          .responses,
      0);
  ASSERT_TRUE(accepted);
  ASSERT_TRUE(pumpUntil([&outcome] { return outcome.has_value(); }));
  EXPECT_EQ(*outcome, PJ::LoadOutcome::kCancelled) << "the file the agent asked for was loaded";

  ASSERT_NO_FATAL_FAILURE(openPublic());
  accepted = false;
  int started = 0;
  QObject::connect(
      &window_->streamingSources(), &PJ::StreamingSourceManager::streamStarted, window_.get(),
      [&started](PJ::DatasetId) { ++started; });
  const auto engine_datasets = [this] { return window_->appSession().sessionManager().dataEngine().listDatasets(); };
  const auto datasets_before = engine_datasets();
  const auto catalog_before = window_->appSession().catalogModel().datasets();
  EXPECT_EQ(
      call("start_stream", {{"source", "Mock Layout Stream"}, {"ask_user", true}}, close_then_accept).responses, 0);
  ASSERT_TRUE(accepted);
  EXPECT_EQ(started, 0) << "the stream the agent asked for started";
  EXPECT_EQ(engine_datasets(), datasets_before) << "the abandoned stream left its dataset behind";
  EXPECT_EQ(window_->appSession().catalogModel().datasets(), catalog_before);

  // The user's own prompt (derived series depend on the dataset) is the agent's dialog here.
  ASSERT_NO_FATAL_FAILURE(openPublic());
  const int dataset = loadWithDependentSeries();
  ASSERT_NE(dataset, 0);
  bool clicked = false;
  const auto close_then_delete = [this, &clicked] {
    for (PJ::MessageBox* box : visible<PJ::MessageBox>()) {
      for (QAbstractButton* button : box->findChildren<QAbstractButton*>()) {
        if (!clicked && button->text() == u"Delete"_s) {
          clicked = true;
          window_->closeMcpPublicAccess();
          button->click();
        }
      }
    }
  };
  EXPECT_EQ(call("unload_file", {{"dataset", dataset}}, close_then_delete).responses, 0);
  ASSERT_TRUE(clicked);
  const auto datasets = window_->appSession().catalogModel().datasets();
  EXPECT_TRUE(std::any_of(datasets.begin(), datasets.end(), [dataset](const auto& entry) {
    return entry.first == static_cast<PJ::DatasetId>(dataset);
  })) << "the dataset the agent asked to unload was removed";
}

TEST_F(McpServerTest, ADrainThatAReplyOrContinuationReentersRunsNothing) {
  PJ::McpServer& mcp = *server();
  const QByteArray token = QByteArray::fromStdString(token_);
  QTcpSocket client;  // stands for the caller's live connection
  int pending_resumes = 0;
  bool pending_may_finish = false;
  int pending_replies = 0;
  // A nested drain that resumed anything would also move the round on under the outer one, which
  // would then see every follow-up as due.
  const auto reenter = [&](const char* from) {
    const int before = pending_resumes;
    PJ::McpTestPeer::resumeDeferred(mcp);
    EXPECT_EQ(pending_resumes, before) << "a drain entered from a " << from << " resumed a result";
  };
  // Queued ahead of the pending result, so each runs (and re-enters) before it.
  PJ::McpTestPeer::defer(
      mcp, token, &client,
      [&](PJ::agent_tools::ToolContext&) {
        reenter("continuation");
        return PJ::agent_tools::ToolResult::success("x");
      },
      [](const json&) {});
  PJ::McpTestPeer::defer(
      mcp, token, &client, [](PJ::agent_tools::ToolContext&) { return PJ::agent_tools::ToolResult::success("y"); },
      [&](const json&) { reenter("reply"); });
  // Waits until released; the cap ends a runaway drain so a regression fails instead of hanging.
  struct Pending {
    int* resumes;
    const bool* may_finish;
    PJ::agent_tools::ToolResult operator()(PJ::agent_tools::ToolContext&) const {
      constexpr int kRunaway = 30;
      return ++*resumes >= kRunaway || *may_finish ? PJ::agent_tools::ToolResult::success("p")
                                                   : PJ::agent_tools::ToolResult::deferred(*this);
    }
  };
  PJ::McpTestPeer::defer(
      mcp, token, &client, Pending{&pending_resumes, &pending_may_finish}, [&](const json&) { ++pending_replies; });

  PJ::McpTestPeer::resumeDeferred(mcp);
  EXPECT_EQ(pending_resumes, 1) << "the pending result runs once in a round, not once per drain";
  EXPECT_EQ(PJ::McpTestPeer::deferred(mcp), 1U);
  PJ::McpTestPeer::resumeDeferred(mcp);
  EXPECT_EQ(pending_resumes, 2);
  pending_may_finish = true;
  PJ::McpTestPeer::resumeDeferred(mcp);
  EXPECT_EQ(pending_resumes, 3);
  EXPECT_EQ(pending_replies, 1);
  EXPECT_EQ(PJ::McpTestPeer::deferred(mcp), 0U);
}

TEST_F(McpServerTest, InputsThatShareNoTimestampsAreRefused) {
  ASSERT_NE(loadMock(), 0);  // samples at 100..300 ns; the stream's start at 1 s
  const auto [started, start_error] =
      result(call("start_stream", {{"source", "Mock Layout Stream"}, {"config", {{"topic", "live"}}}}));
  ASSERT_FALSE(start_error) << started;
  // Until the stream's first samples land the input is unknown; then the join is refused.
  std::string text;
  EXPECT_TRUE(pumpUntil([&] {
    const auto [answer, error] = result(call(
        "add_script_series", {{"name", "sum"},
                              {"inputs", {"mock/file_data/value", "live/value"}},
                              {"script",
                               "return { create = function() return { calculate = function(t, a, b) "
                               "return a + b end } end }"}}));
    EXPECT_TRUE(error) << answer;
    text = answer;
    return answer.find("share no timestamps") != std::string::npos;
  })) << text;
}

TEST_F(McpServerTest, AWronglyTypedArgumentIsAToolError) {
  const auto [text, error] = result(
      call("open_file", {{"path", pj_app_test::makeMockFile(data_, "b.mock").toStdString()}, {"ask_user", "yes"}}));
  EXPECT_TRUE(error);
  EXPECT_NE(text.find("bad arguments for open_file"), std::string::npos) << text;
}

// initialize and server/discover tell an agent what is loaded, with a path it can use as is, so it need not start with
// list_topics and describe_topic; with nothing loaded it says nothing about data.
TEST_F(McpServerTest, InitializeSaysWhatIsLoaded) {
  const json initialize = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", json::object()}};
  const std::string empty = rpcAs(token_, initialize).body["result"].value("instructions", "");
  EXPECT_FALSE(empty.empty());
  EXPECT_EQ(empty.find("Loaded when"), std::string::npos) << empty;

  ASSERT_NE(loadMock(), 0);
  const std::string loaded = rpcAs(token_, initialize).body["result"].value("instructions", "");
  EXPECT_EQ(loaded.rfind(empty, 0), 0U) << loaded;  // the summary follows the usual instructions
  EXPECT_NE(loaded.find("Loaded when"), std::string::npos) << loaded;
  EXPECT_NE(loaded.find("mock/file_data: 1 numeric fields: value"), std::string::npos) << loaded;
  const std::string example = "a.mock:mock/file_data/value";
  EXPECT_NE(loaded.find(example), std::string::npos) << loaded;
  const auto [read, read_error] = result(call("read_series", {{"paths", {example}}}));
  EXPECT_FALSE(read_error) << read;  // the example path resolves
  // A 2026-era client reads the instructions from server/discover alone.
  const json discover = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "server/discover"}, {"params", json::object()}};
  EXPECT_EQ(rpcAs(token_, discover).body["result"].value("instructions", ""), loaded);

  // Two files of one name: each dataset lists its own topics once, and the shared name is flagged.
  QTemporaryDir other;
  ASSERT_TRUE(other.isValid());
  const auto [opened, open_error] =
      result(call("open_file", {{"path", pj_app_test::makeMockFile(other, "a.mock").toStdString()}}));
  ASSERT_FALSE(open_error) << opened;
  const std::string twice = rpcAs(token_, initialize).body["result"].value("instructions", "");
  const auto count = [&twice](const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = twice.find(needle); at != std::string::npos; at = twice.find(needle, at + 1)) {
      ++n;
    }
    return n;
  };
  EXPECT_EQ(count("\n- a.mock"), 2U) << twice;
  EXPECT_EQ(count("mock/file_data: 1 numeric fields"), 2U) << twice;
  EXPECT_EQ(count("another loaded dataset has this name"), 2U) << twice;
}

TEST_F(McpServerTest, ExportQuotesHeadersAndRefusesTooManyRows) {
  ASSERT_NE(loadMock(), 0);
  // The first plottable path add_script_series reports.
  const auto path_of = [](const std::pair<std::string, bool>& added) {
    EXPECT_FALSE(added.second) << added.first;
    const json topics = json::parse(added.first, nullptr, false).value("topics", json::object());
    EXPECT_FALSE(topics.empty()) << added.first;
    return topics.empty() || topics.begin()->empty()
               ? std::string()
               : topics.begin().key() + "/" + topics.begin()->front().get<std::string>();
  };
  const std::string quoted = path_of(addDoubled("say \"hi\""));
  // Two names that only differ in punctuation are two series, not one replacing the other.
  const std::string plain = path_of(addDoubled("say_hi"));
  ASSERT_FALSE(quoted.empty());
  ASSERT_NE(quoted, plain);
  const auto [text, error] = result(call("export_series", {{"paths", {quoted, plain, quoted}}}));
  ASSERT_FALSE(error) << text;
  const json exported = json::parse(text, nullptr, false);
  EXPECT_EQ(exported["columns"].size(), 2U);  // the repeated path once
  QFile csv(QString::fromStdString(exported.value("path", "")));
  ASSERT_TRUE(csv.open(QIODevice::ReadOnly));
  std::string escaped;
  for (const char c : quoted) {
    escaped += c == '"' ? std::string("\"\"") : std::string(1, c);
  }
  EXPECT_EQ(csv.readLine().trimmed().toStdString(), "time,\"" + escaped + "\",\"" + plain + "\"");
  EXPECT_NE(escaped.find("say \"\"hi\"\""), std::string::npos) << escaped;
#ifndef Q_OS_WIN
  EXPECT_EQ(csv.permissions() & (QFile::ReadGroup | QFile::ReadOther), QFile::Permissions());
#endif
  csv.remove();

  // 0.2 µs of data at 1e14 Hz would be 2e7 rows.
  const auto [big, big_error] = result(call("export_series", {{"paths", {"mock/file_data/value"}}, {"rate_hz", 1e14}}));
  EXPECT_TRUE(big_error);
  EXPECT_NE(big.find("more than 10000000 rows"), std::string::npos) << big;
}

// format "npy" writes what numpy.load reads as a structured array: a 3.0 header naming "time" and each series as
// float64 fields, the data on a 64-byte boundary, one record per row.
TEST_F(McpServerTest, ExportNpyIsANumpyStructuredArray) {
  ASSERT_NE(loadMock(), 0);
  const auto [refused, refused_error] =
      result(call("export_series", {{"paths", {"mock/file_data/value"}}, {"format", "parquet"}}));
  EXPECT_TRUE(refused_error) << refused;

  // A path in a folder that does not exist yet creates the folder.
  const QString target = data_.filePath(u"new/folder/out.npy"_s);
  const auto [text, error] = result(
      call("export_series", {{"paths", {"mock/file_data/value"}}, {"format", "npy"}, {"path", target.toStdString()}}));
  ASSERT_FALSE(error) << text;
  const json exported = json::parse(text, nullptr, false);
  QFile npy(QString::fromStdString(exported.value("path", "")));
  EXPECT_EQ(npy.fileName(), target);
  ASSERT_TRUE(npy.open(QIODevice::ReadOnly));
  const QByteArray bytes = npy.readAll();
  npy.remove();
  ASSERT_GT(bytes.size(), 12);
  EXPECT_EQ(bytes.left(8), QByteArray("\x93NUMPY\x03\x00", 8));
  const auto header_size = qFromLittleEndian<quint32>(bytes.constData() + 8);
  EXPECT_EQ((12 + header_size) % 64, 0U);
  const std::string header = bytes.mid(12, static_cast<qsizetype>(header_size)).toStdString();
  const std::string column = exported["columns"][0].get<std::string>();
  EXPECT_NE(header.find("[('time', '<f8'), ('" + column + "', '<f8')]"), std::string::npos) << header;
  const std::size_t rows = exported.value("rows", std::size_t{0});
  EXPECT_NE(header.find("'shape': (" + std::to_string(rows) + ",)"), std::string::npos) << header;
  ASSERT_EQ(static_cast<std::size_t>(bytes.size()), 12 + header_size + rows * 2 * sizeof(double));
  std::array<double, 2> first{};
  std::memcpy(first.data(), bytes.constData() + 12 + header_size, sizeof first);
  EXPECT_EQ(first[1], 10.0);  // the mock's first sample
}

// A computed NaN (sign bit set on x86, which MSVC's to_chars spells "-nan(ind)") exports as plain "nan", which every
// CSV reader parses.
TEST_F(McpServerTest, ExportWritesANaNAsNan) {
  ASSERT_NE(loadMock(), 0);
  const auto [added, add_error] = result(call(
      "create_derived_series",
      {{"name", "undefined"}, {"inputs", {"mock/file_data/value"}}, {"expression", "(value - value) / 0"}}));
  ASSERT_FALSE(add_error) << added;
  const std::string series = json::parse(added, nullptr, false).value("series", "");
  ASSERT_FALSE(series.empty()) << added;
  const auto [text, error] = result(call("export_series", {{"paths", {series}}}));
  ASSERT_FALSE(error) << text;
  QFile csv(QString::fromStdString(json::parse(text, nullptr, false).value("path", "")));
  ASSERT_TRUE(csv.open(QIODevice::ReadOnly));
  const QList<QByteArray> lines = csv.readAll().trimmed().split('\n');
  csv.remove();
  ASSERT_GT(lines.size(), 1);
  for (qsizetype i = 1; i < lines.size(); ++i) {
    EXPECT_EQ(lines[i].split(',').value(1).toStdString(), "nan") << lines[i].toStdString();
  }
}

// The CSV holds every value exactly and in the C locale: a value needing more than ten significant digits reads back
// unchanged, a time is its exact display nanosecond (the mock writes a row every 100 ns), and a decimal-comma locale
// (where one is installed) does not split a number into two columns.
TEST_F(McpServerTest, ExportWritesNumbersExactlyWhateverTheLocale) {
  ASSERT_NE(loadMock(), 0);
  constexpr double kEpoch = 1731080000.123;  // a Unix time: 13 significant digits
  const auto [added, add_error] = result(call(
      "create_derived_series",
      {{"name", "epoch"}, {"inputs", {"mock/file_data/value"}}, {"expression", "value * 0 + 1731080000.123"}}));
  ASSERT_FALSE(add_error) << added;
  const std::string series = json::parse(added, nullptr, false).value("series", "");
  ASSERT_FALSE(series.empty()) << added;

  const std::string saved = std::setlocale(LC_NUMERIC, nullptr);
  bool comma_locale = false;
  for (const char* name : {"en_DK.UTF-8", "en_DK.utf8", "de_DE.UTF-8", "de_DE.utf8", "de_DE", "German_Germany.1252"}) {
    if (std::setlocale(LC_NUMERIC, name) != nullptr) {
      std::array<char, 8> probe{};
      std::snprintf(probe.data(), probe.size(), "%.1f", 1.5);
      comma_locale = std::string(probe.data()) == "1,5";
      if (comma_locale) {
        break;
      }
    }
  }
  if (!comma_locale) {
    std::setlocale(LC_NUMERIC, saved.c_str());
  }
  const auto [text, error] = result(call("export_series", {{"paths", {series}}}));
  std::setlocale(LC_NUMERIC, saved.c_str());
  ASSERT_FALSE(error) << text;

  QFile csv(QString::fromStdString(json::parse(text, nullptr, false).value("path", "")));
  ASSERT_TRUE(csv.open(QIODevice::ReadOnly));
  csv.readLine();  // header
  int rows = 0;
  std::optional<qint64> first_ns;
  qint64 row_index = 0;
  while (!csv.atEnd()) {
    const QList<QByteArray> fields = csv.readLine().trimmed().split(',');
    ASSERT_EQ(fields.size(), 2) << "comma locale: " << comma_locale << ", row: " << fields.join(',').toStdString();
    const QList<QByteArray> time = fields[0].split('.');
    ASSERT_EQ(time.size(), 2) << fields[0].toStdString();
    ASSERT_EQ(time[1].size(), 9) << fields[0].toStdString();
    bool seconds_ok = false;
    bool fraction_ok = false;
    const qint64 ns = time[0].toLongLong(&seconds_ok) * 1'000'000'000 + time[1].toLongLong(&fraction_ok);
    ASSERT_TRUE(seconds_ok && fraction_ok && !time[0].startsWith('-')) << fields[0].toStdString();
    first_ns = first_ns.value_or(ns);
    EXPECT_EQ(ns - *first_ns, 100 * row_index++) << fields[0].toStdString();
    if (!fields[1].isEmpty()) {
      EXPECT_EQ(fields[1].toDouble(), kEpoch) << fields[1].toStdString();
      ++rows;
    }
  }
  csv.remove();
  EXPECT_GT(rows, 0);
}

// An agent's removal never takes the user's work with it: with a series of the user's built on each of the agent's,
// both remove tools refuse, naming it and its owner, and remove nothing.
TEST_F(McpServerTest, AnAgentsRemovalRefusesToTakeTheUsersSeriesWithIt) {
  ASSERT_NE(loadMock(), 0);
  const auto [script_added, script_add_error] = addDoubled("doubled");
  ASSERT_FALSE(script_add_error) << script_added;
  const auto [derived_added, derived_add_error] = result(call(
      "create_derived_series",
      {{"name", "tripled"}, {"inputs", {"mock/file_data/value"}}, {"expression", "value * 3"}}));
  ASSERT_FALSE(derived_add_error) << derived_added;
  const json script_topics = json::parse(script_added, nullptr, false).value("topics", json::object());
  ASSERT_FALSE(script_topics.empty() || script_topics.begin()->empty()) << script_added;
  const std::map<std::string, std::string> agent_series = {
      {"doubled", script_topics.begin().key() + "/" + script_topics.begin()->front().get<std::string>()},
      {"tripled", json::parse(derived_added, nullptr, false).value("series", "")}};
  PJ::DataProcessorService& processors = window_->appSession().sessionManager().dataProcessorService();
  for (const auto& [name, path] : agent_series) {
    const std::string id = "on_" + name;
    const std::string script = "-- pj-script: luau\nreturn { id=\"" + id +
                               "\", name=\"Negate\", output=\"same\", create = function(p) return { calculate = "
                               "function(t, v) return -v end } end }\n";
    const auto users = processors.upsertTransform("user-editor", id, {path}, {id}, script, "{}");
    ASSERT_TRUE(users.has_value()) << path << ": " << users.error();
  }

  for (const auto& [tool, name] :
       {std::pair{"remove_script_series", "doubled"}, std::pair{"remove_derived_series", "tripled"}}) {
    const auto [text, error] = result(call(tool, {{"name", name}}));
    EXPECT_TRUE(error) << tool;
    EXPECT_NE(text.find("'on_" + std::string(name) + "'"), std::string::npos) << text;
    EXPECT_NE(text.find("user-editor"), std::string::npos) << text;
  }
  EXPECT_EQ(processors.transformRecipes().size(), 4U);
}

TEST_F(McpServerTest, TheSeventeenthConnectionIsClosed) {
  std::vector<std::unique_ptr<QTcpSocket>> held;
  for (int i = 0; i < PJ::mcp::kMaxOpenSockets; ++i) {
    auto socket = std::make_unique<QTcpSocket>();
    socket->connectToHost(QHostAddress::LocalHost, port_);
    ASSERT_TRUE(pumpUntil([&] { return socket->state() == QAbstractSocket::ConnectedState; }));
    held.push_back(std::move(socket));
  }
  // Incomplete requests hold their slots; the one past the cap is dropped at once (possibly before
  // this side ever sees it connected).
  held.push_back(std::make_unique<QTcpSocket>());
  held.back()->connectToHost(QHostAddress::LocalHost, port_);
  EXPECT_TRUE(pumpUntil([&] { return held.back()->state() == QAbstractSocket::UnconnectedState; }));
  for (std::size_t i = 0; i + 1 < held.size(); ++i) {
    EXPECT_EQ(held[i]->state(), QAbstractSocket::ConnectedState) << i;
  }
}

TEST_F(McpServerTest, ClosingPublicAccessWithAnEvaluationInFlightDropsIt) {
  ASSERT_NE(loadMock(), 0);
  // An evaluation at an instant is deferred: its result is polled while the host computes it.
  const std::unique_ptr<QTcpSocket> socket = post(
      "evaluate",
      {{"inputs", {"mock/file_data/value"}}, {"at_s", 0.0}, {"body", "return { y = 1 }"}, {"outputs", {"y:number"}}});
  ASSERT_TRUE(pumpUntil([&] { return socket->bytesToWrite() == 0; }));
  // Under one resume tick (50 ms): the host computes on its pool, so the reply is still held.
  static_cast<void>(pumpUntil([] { return false; }, 20));
  EXPECT_FALSE(socket->readAll().contains("HTTP/1.1")) << "evaluate answered at once: nothing was deferred";
  window_->closeMcpPublicAccess();
  EXPECT_EQ(PJ::McpTestPeer::deferred(*server()), 0U);
  EXPECT_EQ(exchange(*socket).responses, 0);  // the connection closes with no reply
  EXPECT_EQ(callAs(token_, "get_status", json::object()).status, 401);
}

TEST_F(McpServerTest, TurningMcpOffInPreferencesClosesPublicAccessAndItsDiscoveryFile) {
  const QString discovery = PJ::McpTestPeer::discoveryPath(*server());
  ASSERT_TRUE(QFile::exists(discovery));
  bool shown_on = false;
  QTimer::singleShot(0, window_.get(), [&] {
    QTimer::singleShot(0, window_.get(), [&] {
      for (PJ::PreferencesDialog* dialog : visible<PJ::PreferencesDialog>()) {
        auto* toggle = dialog->findChild<PJ::ToggleSwitch*>(QStringLiteral("mcpServerToggle"));
        ASSERT_NE(toggle, nullptr);
        shown_on = toggle->isChecked();  // public access is open, though the setting was never written
        toggle->setChecked(false, /*animate=*/false);
        dialog->accept();
      }
    });
    QMetaObject::invokeMethod(window_.get(), "onShowPreferencesDialog");
  });
  ASSERT_TRUE(pumpUntil([&] { return !window_->mcpPublicAccess(); }));
  EXPECT_TRUE(shown_on);
  EXPECT_FALSE(QFile::exists(discovery));
  EXPECT_FALSE(QSettings().value(PJ::MainWindow::kMcpServerKey, true).toBool());
  EXPECT_EQ(callAs(token_, "get_status", json::object()).status, 401);
}

/// An agent's loads waiting in the file loader's queue behind the user's own load while public
/// access closes. A mock import runs on a worker thread, so the loader stays busy until the event
/// loop runs.
class McpQueuedLoadTest : public McpServerTest {
 protected:
  void SetUp() override {
    McpServerTest::SetUp();
    ASSERT_NE(window_, nullptr);
    PJ::FileLoader& loader = window_->fileLoader();
    QObject::connect(&loader, &PJ::FileLoader::requestStarted, window_.get(), [this](quint64 ticket, std::uint64_t) {
      started_.push_back(ticket);
    });
    QObject::connect(
        &loader, &PJ::FileLoader::loadFinished, window_.get(),
        [this](quint64 ticket, PJ::LoadOutcome outcome, const QString&, PJ::DatasetId, const QVector<PJ::DatasetId>&) {
          terminals_[ticket].push_back(outcome);
        });
    QObject::connect(
        &loader, &PJ::FileLoader::fileLoaded, window_.get(),
        [this](const QString& path, const QString&, const QString&, const QString&) {
          loaded_.push_back(QFileInfo(path).fileName().toStdString());
        });
    QObject::connect(
        &loader, &PJ::FileLoader::fileLoadFailed, window_.get(),
        [this](const QString& path, const QString&) { failed_.push_back(QFileInfo(path).fileName().toStdString()); });
  }

  /// A load the user started: it holds the loader until the event loop runs.
  quint64 userLoad(const QString& name) {
    return window_->fileLoader().loadFileTicketed(
        PJ::LoadInput::fromNativePath(pj_app_test::makeMockFile(data_, name)), nullptr, pj_app_test::mockLoadHints());
  }

  /// Calls agent tool `name` without HTTP; its reply is recorded in replies_.
  void agentCall(const std::string& name, const json& args) {
    PJ::McpTestPeer::callTool(*server(), QByteArray::fromStdString(token_), &client_, name, args, [this](json reply) {
      replies_.push_back(std::move(reply));
    });
  }

  /// Closes public access, then runs the queue dry: returns once the load with ticket `last` settled.
  void closeAndDrain(quint64 last) {
    window_->closeMcpPublicAccess();
    ASSERT_TRUE(pumpUntil([&] { return terminals_.count(last) != 0; }));
    static_cast<void>(pumpUntil([] { return false; }, 100));  // a late duplicate terminal or legacy signal
  }

  [[nodiscard]] std::vector<PJ::DatasetId> engineDatasets() const {
    return window_->appSession().sessionManager().dataEngine().listDatasets();
  }

  /// The one cancelled reply of a revoked agent load.
  void expectOneCancelledReply() const {
    ASSERT_EQ(replies_.size(), 1U);
    const json& result = replies_.front();
    EXPECT_TRUE(result.value("isError", false)) << result.dump();
    EXPECT_NE(result.dump().find("cancelled"), std::string::npos) << result.dump();
  }

  using Terminals = std::vector<PJ::LoadOutcome>;
  QTcpSocket client_;  // stands for the agent's live connection
  std::vector<quint64> started_;
  std::map<quint64, Terminals> terminals_;
  std::vector<std::string> loaded_;
  std::vector<std::string> failed_;
  std::vector<json> replies_;
};

TEST_F(McpQueuedLoadTest, ClosingPublicAccessCancelsAnOpenFileWaitingInTheQueue) {
  const quint64 blocker = userLoad(u"blocker.mock"_s);
  agentCall("open_file", {{"path", pj_app_test::makeMockFile(data_, "agent.mock").toStdString()}});
  const quint64 unrelated = userLoad(u"next.mock"_s);
  const quint64 agent = blocker + 1;  // tickets are minted in enqueue order
  ASSERT_EQ(unrelated, agent + 1);
  ASSERT_EQ(started_, std::vector<quint64>{blocker}) << "the loader was idle: the agent's request did not wait";

  ASSERT_NO_FATAL_FAILURE(closeAndDrain(unrelated));
  EXPECT_EQ(
      terminals_, (std::map<quint64, Terminals>{
                      {blocker, {PJ::LoadOutcome::kLoaded}},
                      {agent, {PJ::LoadOutcome::kCancelled}},
                      {unrelated, {PJ::LoadOutcome::kLoaded}}}));
  EXPECT_EQ(started_, (std::vector<quint64>{blocker, agent, unrelated}));
  EXPECT_EQ(loaded_, (std::vector<std::string>{"blocker.mock", "next.mock"}));
  EXPECT_TRUE(failed_.empty());
  EXPECT_EQ(engineDatasets().size(), 2U) << "the agent's file was loaded";
  ASSERT_NO_FATAL_FAILURE(expectOneCancelledReply());
}

TEST_F(McpQueuedLoadTest, ClosingPublicAccessCancelsAReloadFileWaitingInTheQueue) {
  const int dataset = loadMock();
  ASSERT_NE(dataset, 0);
  const auto datasets_before = engineDatasets();
  const auto values_before = pj_app_test::singleTopicValues(
      window_->appSession().sessionManager().createReader(), static_cast<PJ::DatasetId>(dataset));
  ASSERT_FALSE(values_before.empty());
  terminals_.clear();
  loaded_.clear();
  started_.clear();

  const quint64 blocker = userLoad(u"blocker.mock"_s);
  agentCall("reload_file", {{"dataset", dataset}});
  const quint64 unrelated = userLoad(u"next.mock"_s);
  const quint64 agent = blocker + 1;
  ASSERT_EQ(unrelated, agent + 1);
  ASSERT_EQ(started_, std::vector<quint64>{blocker}) << "the loader was idle: the agent's request did not wait";

  ASSERT_NO_FATAL_FAILURE(closeAndDrain(unrelated));
  EXPECT_EQ(
      terminals_, (std::map<quint64, Terminals>{
                      {blocker, {PJ::LoadOutcome::kLoaded}},
                      {agent, {PJ::LoadOutcome::kCancelled}},
                      {unrelated, {PJ::LoadOutcome::kLoaded}}}));
  EXPECT_EQ(started_, (std::vector<quint64>{blocker, agent, unrelated}));
  EXPECT_EQ(loaded_, (std::vector<std::string>{"blocker.mock", "next.mock"})) << "the reload ran";
  EXPECT_TRUE(failed_.empty());
  // The loaded dataset is untouched; the user's two loads came on top of it.
  const auto datasets_after = engineDatasets();
  EXPECT_EQ(datasets_after.size(), datasets_before.size() + 2);
  EXPECT_NE(
      std::find(datasets_after.begin(), datasets_after.end(), static_cast<PJ::DatasetId>(dataset)),
      datasets_after.end());
  EXPECT_EQ(
      pj_app_test::singleTopicValues(
          window_->appSession().sessionManager().createReader(), static_cast<PJ::DatasetId>(dataset)),
      values_before);
  ASSERT_NO_FATAL_FAILURE(expectOneCancelledReply());
}

/// A toolbox plugin's endpoint, with public access closed unless a case opens it.
/// A relay as pj_mcp_relay.py serves the browser link (McpBrowserLink), one HTTP request per
/// connection, answered by the test: what the page asked, and the socket to answer on.
class FakeRelay {
 public:
  struct Request {
    PJ::mcp::HttpRequest http;
    QPointer<QTcpSocket> socket;
  };

  FakeRelay() {
    QObject::connect(&server_, &QTcpServer::newConnection, [this] {
      while (QTcpSocket* socket = server_.nextPendingConnection()) {
        auto buffer = std::make_shared<QByteArray>();
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
          *buffer += socket->readAll();
          PJ::mcp::HttpRequest request;
          if (PJ::mcp::parseHttpRequest(*buffer, request) == 200) {
            requests_.push_back({std::move(request), socket});
          }
        });
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      }
    });
    EXPECT_TRUE(server_.listen(QHostAddress::LocalHost));
  }

  /// The relay's address with its secret, as its launch hands it to the page.
  [[nodiscard]] QUrl url() const {
    return QUrl(u"http://127.0.0.1:%1/?key=s3cret"_s.arg(server_.serverPort()));
  }

  /// Pumps until the page sends a request to `path`, and takes it.
  Request next(const QByteArray& path) {
    std::optional<Request> found;
    EXPECT_TRUE(pumpUntil([&] {
      const auto it = std::find_if(
          requests_.begin(), requests_.end(), [&](const Request& r) { return r.http.path.startsWith(path); });
      if (it == requests_.end()) {
        return false;
      }
      found = std::move(*it);
      requests_.erase(it);
      return true;
    })) << "the page never asked for "
        << path.toStdString();
    return found.value_or(Request{});
  }

  static void answer(const Request& request, int status, const json& body = nullptr) {
    if (request.socket != nullptr) {
      request.socket->write(
          PJ::mcp::httpResponse(status, body.is_null() ? QByteArray() : QByteArray::fromStdString(body.dump())));
      request.socket->disconnectFromHost();
    }
  }

 private:
  QTcpServer server_;
  std::vector<Request> requests_;
};

TEST_F(McpServerTest, ARelayedCallIsAnsweredAsThePublicClientTheRelayNames) {
  FakeRelay relay;
  ASSERT_TRUE(window_->connectMcpRelay(relay.url()));
  const FakeRelay::Request hello = relay.next("/link/hello");
  EXPECT_EQ(json::parse(hello.http.body.toStdString(), nullptr, false).value("protocol", 0), 1);
  // The secret travels in the header on every request, never in a path.
  EXPECT_EQ(hello.http.path, "/link/hello");
  EXPECT_EQ(hello.http.headers.value("x-pj-link"), "s3cret");
  FakeRelay::answer(hello, 200, {{"id", "tab-1"}});

  const FakeRelay::Request poll = relay.next("/link/next");
  EXPECT_EQ(poll.http.path, "/link/next?id=tab-1");
  EXPECT_EQ(poll.http.headers.value("x-pj-link"), "s3cret");
  FakeRelay::answer(
      poll, 200, {{"seq", 7}, {"client", "claude-code"}, {"message", toolsCall("get_status", json::object())}});

  const FakeRelay::Request reply = relay.next("/link/reply");
  EXPECT_EQ(reply.http.path, "/link/reply?id=tab-1");
  EXPECT_EQ(reply.http.headers.value("x-pj-link"), "s3cret");
  const json body = json::parse(reply.http.body.toStdString(), nullptr, false);
  EXPECT_EQ(body.value("seq", 0), 7);
  const json content = body["response"]["result"].value("content", json::array());
  ASSERT_FALSE(content.empty()) << body.dump();
  const json status = json::parse(content[0].value("text", ""), nullptr, false);
  EXPECT_EQ(status.value("agent", ""), "agent-claude-code") << "owned by the client the relay names";
  FakeRelay::answer(reply, 204);
  // It keeps polling for the next message.
  FakeRelay::answer(relay.next("/link/next"), 204);
  FakeRelay::answer(relay.next("/link/next"), 204);
}

TEST_F(McpServerTest, ALinkForgottenByARestartedRelayIntroducesItselfAgain) {
  FakeRelay relay;
  ASSERT_TRUE(window_->connectMcpRelay(relay.url()));
  FakeRelay::answer(relay.next("/link/hello"), 200, {{"id", "old"}});
  FakeRelay::answer(relay.next("/link/next"), 404);
  FakeRelay::answer(relay.next("/link/hello"), 200, {{"id", "new"}});
  EXPECT_EQ(relay.next("/link/next").http.path, "/link/next?id=new");
}

TEST_F(McpServerTest, OnlyALoopbackHttpRelayIsLinked) {
  EXPECT_FALSE(window_->connectMcpRelay(QUrl(u"http://example.com:7381"_s)));
  EXPECT_FALSE(window_->connectMcpRelay(QUrl(u"https://127.0.0.1:7381"_s)));
  EXPECT_FALSE(window_->connectMcpRelay(QUrl(u"http://127.0.0.1"_s)));  // no port
  EXPECT_TRUE(PJ::McpBrowserLink::acceptableRelay(QUrl(u"http://localhost:7381"_s)));
}

class McpEndpointTest : public McpServerTest {
 protected:
  McpEndpointTest() {
    open_public_ = false;
  }

  /// The "pj.mcp_endpoint.v1" service of `session`, as the plugin sees it.
  static PJ::sdk::McpEndpointHostView endpointOf(PJ::ToolboxPanelSession& session) {
    const auto view = PJ::sdk::ServiceRegistry(PJ::MainWindow::toolboxSessionServices(session))
                          .get<PJ::sdk::McpEndpointHostService>();
    EXPECT_TRUE(view.has_value());
    return view.value_or(PJ::sdk::McpEndpointHostView{});
  }

  /// Acquires `session`'s endpoint and points the harness at its server; returns the token.
  std::string acquire(PJ::ToolboxPanelSession& session) {
    const auto endpoint = endpointOf(session).acquire();
    EXPECT_TRUE(endpoint.has_value()) << (endpoint ? std::string() : endpoint.error());
    if (!endpoint) {
      return {};
    }
    port_ = server()->port();
    EXPECT_EQ(endpoint->url, "http://127.0.0.1:" + std::to_string(port_) + "/mcp");
    return endpoint->bearer_token;
  }

  static void release(PJ::ToolboxPanelSession& session) {
    endpointOf(session).release();
  }

  /// A toolbox session as launchToolbox builds it, with no plugin loaded, and its endpoint's token.
  struct Plugin {
    std::shared_ptr<PJ::ToolboxPanelSession> session;
    std::string token;
  };
  /// Builds toolbox `id`'s session and acquires its endpoint; the token is "" when either failed.
  Plugin launch(const QString& id) {
    Plugin plugin{.session = window_->buildAgentSession(id, id), .token = {}};
    EXPECT_NE(plugin.session, nullptr);
    if (plugin.session != nullptr) {
      plugin.token = acquire(*plugin.session);
    }
    return plugin;
  }

  /// The plot tab `tab` of `owner`, or null.
  [[nodiscard]] const PJ::PlotDocker* tabOf(const char* owner, const char* tab) const {
    return window_->pluginPlotTabs().ownedTab(QString::fromLatin1(owner), QString::fromLatin1(tab));
  }

  void createTab(const std::string& token, const std::string& tab) const {
    const auto [text, error] = result(callAs(token, "plot_tab", {{"action", "create"}, {"tab", tab}}));
    EXPECT_FALSE(error) << text;
  }
};

TEST_F(McpEndpointTest, APluginTokenActsAsThePluginWhateverTheClientClaims) {
  const Plugin plugin = launch(u"toolbox-mcp-a"_s);
  ASSERT_FALSE(plugin.token.empty());
  // A name in the bridge's header, then one in the request's _meta: neither picks the owner.
  const auto [text, error] =
      result(rpcAs(plugin.token, toolsCall("plot_tab", {{"action", "create"}, {"tab", "one"}}), "evil"));
  ASSERT_FALSE(error) << text;
  json message = toolsCall("plot_tab", {{"action", "create"}, {"tab", "two"}});
  message["params"]["_meta"] = {{"io.modelcontextprotocol/clientInfo", {{"name", "other"}}}};
  const auto [meta_text, meta_error] = result(rpcAs(plugin.token, message, ""));
  ASSERT_FALSE(meta_error) << meta_text;
  EXPECT_NE(tabOf("toolbox-mcp-a", "one"), nullptr);
  EXPECT_NE(tabOf("toolbox-mcp-a", "two"), nullptr);
  EXPECT_EQ(tabOf("agent-evil", "one"), nullptr);
  EXPECT_EQ(tabOf("agent-other", "two"), nullptr);
}

TEST_F(McpEndpointTest, APluginTokenServesTheAgentToolsOnly) {
  const Plugin plugin = launch(u"toolbox-mcp-a"_s);
  ASSERT_FALSE(plugin.token.empty());
  const Answer listed = rpcAs(plugin.token, {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}});
  const json tools = listed.body["result"]["tools"];
  ASSERT_EQ(tools.size(), PJ::mcp::agentTools().toMcpToolsList().size()) << listed.body.dump();
  for (const json& tool : tools) {
    EXPECT_NE(PJ::mcp::agentTools().find(tool.value("name", "")), nullptr) << tool.value("name", "");
  }
  const auto [text, error] = result(callAs(plugin.token, "get_status", json::object()));
  EXPECT_TRUE(error);
  EXPECT_NE(text.find("unknown tool get_status"), std::string::npos) << text;
  const Answer init =
      rpcAs(plugin.token, {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", json::object()}});
  EXPECT_EQ(init.body["result"].value("instructions", "").find("get_status"), std::string::npos);
}

TEST_F(McpEndpointTest, APluginEndpointIsNotAdvertisedAndOutlivesPublicAccess) {
  const Plugin plugin = launch(u"toolbox-mcp-a"_s);
  ASSERT_FALSE(plugin.token.empty());
  const QString discovery =
      QDir(PJ::mcp::instancesDir()).filePath(u"%1.json"_s.arg(QCoreApplication::applicationPid()));
  EXPECT_FALSE(window_->mcpPublicAccess());
  EXPECT_FALSE(QFile::exists(discovery));

  const quint16 endpoint_port = port_;
  ASSERT_NO_FATAL_FAILURE(openPublic());
  EXPECT_EQ(port_, endpoint_port);  // one server for both
  EXPECT_TRUE(QFile::exists(discovery));
  QFile file(discovery);
  ASSERT_TRUE(file.open(QIODevice::ReadOnly));
  EXPECT_FALSE(file.readAll().contains(QByteArray::fromStdString(plugin.token)));
  file.close();

  window_->closeMcpPublicAccess();
  EXPECT_FALSE(QFile::exists(discovery));
  EXPECT_EQ(callAs(token_, "get_status", json::object()).status, 401);
  const auto [text, error] = result(callAs(plugin.token, "list_topics", json::object()));
  EXPECT_FALSE(error) << text;
}

TEST_F(McpEndpointTest, AcquireHoldsOneTokenUntilReleaseRevokesIt) {
  const Plugin plugin = launch(u"toolbox-mcp-a"_s);
  const std::string& first = plugin.token;
  ASSERT_FALSE(first.empty());
  EXPECT_EQ(acquire(*plugin.session), first);
  release(*plugin.session);
  EXPECT_EQ(callAs(first, "list_topics", json::object()).status, 401);
  release(*plugin.session);  // idempotent

  const std::string second = acquire(*plugin.session);
  EXPECT_NE(second, first);
  EXPECT_EQ(callAs(first, "list_topics", json::object()).status, 401);
  const auto [text, error] = result(callAs(second, "list_topics", json::object()));
  EXPECT_FALSE(error) << text;
}

TEST_F(McpEndpointTest, TwoPluginsGetTheirOwnTokensAndOwners) {
  const Plugin a = launch(u"toolbox-mcp-a"_s);
  const Plugin b = launch(u"toolbox-mcp-b"_s);
  ASSERT_FALSE(a.token.empty());
  ASSERT_FALSE(b.token.empty());
  EXPECT_NE(a.token, b.token);
  createTab(a.token, "a_tab");
  createTab(b.token, "b_tab");
  EXPECT_NE(tabOf("toolbox-mcp-a", "a_tab"), nullptr);
  EXPECT_NE(tabOf("toolbox-mcp-b", "b_tab"), nullptr);
  EXPECT_EQ(tabOf("toolbox-mcp-a", "b_tab"), nullptr);
  EXPECT_EQ(tabOf("toolbox-mcp-b", "a_tab"), nullptr);
}

// plot_tab's zoom frames the tab it names and no other: not the caller's other tabs, not another caller's. A tab the
// caller does not have is refused with the ones it does.
TEST_F(McpEndpointTest, PlotTabZoomFramesOnlyTheNamedTab) {
  openPublic();
  ASSERT_NE(loadMock(), 0);
  const Plugin plugin = launch(u"toolbox-mcp-a"_s);
  ASSERT_FALSE(plugin.token.empty());
  const auto draw = [this](const std::string& token, const std::string& client, const std::string& tab) {
    for (const json& args :
         {json{{"action", "create"}, {"tab", tab}},
          json{{"action", "add"}, {"tab", tab}, {"curves", {"mock/file_data/value"}}}}) {
      const auto [text, error] = result(callAs(token, "plot_tab", args, client));
      EXPECT_FALSE(error) << text;
    }
  };
  draw(token_, "test", "a");
  draw(token_, "test", "b");
  draw(plugin.token, "", "c");
  const auto x_range = [this](const char* owner, const char* tab) {
    PJ::PlotDocker* docker = window_->pluginPlotTabs().ownedTab(QString::fromLatin1(owner), QString::fromLatin1(tab));
    PJ::DockWidget* dock = docker == nullptr || docker->plotCount() == 0 ? nullptr : docker->plotAt(0);
    PJ::PlotWidget* plot = dock == nullptr ? nullptr : dock->plotWidget();
    EXPECT_NE(plot, nullptr) << owner << "/" << tab;
    const QRectF rect = plot == nullptr ? QRectF() : plot->currentBoundingRect();
    return std::pair{rect.left(), rect.right()};
  };
  const auto expect_x_range = [&x_range](const char* owner, const char* tab, double start, double end) {
    const auto [left, right] = x_range(owner, tab);
    EXPECT_NEAR(left, start, 1e-9) << owner << "/" << tab;
    EXPECT_NEAR(right, end, 1e-9) << owner << "/" << tab;
  };
  const auto b_before = x_range("agent-test", "b");
  const auto c_before = x_range("toolbox-mcp-a", "c");

  const auto [zoomed, zoom_error] =
      result(call("plot_tab", {{"action", "zoom"}, {"tab", "a"}, {"start_s", 10.0}, {"end_s", 20.0}}));
  ASSERT_FALSE(zoom_error) << zoomed;
  expect_x_range("agent-test", "a", 10.0, 20.0);
  EXPECT_EQ(x_range("agent-test", "b"), b_before);
  EXPECT_EQ(x_range("toolbox-mcp-a", "c"), c_before);

  const auto [plugin_zoomed, plugin_zoom_error] =
      result(callAs(plugin.token, "plot_tab", {{"action", "zoom"}, {"tab", "c"}, {"start_s", 30.0}, {"end_s", 40.0}}));
  ASSERT_FALSE(plugin_zoom_error) << plugin_zoomed;
  expect_x_range("toolbox-mcp-a", "c", 30.0, 40.0);
  expect_x_range("agent-test", "a", 10.0, 20.0);

  const auto [refused, refused_error] = result(call("plot_tab", {{"action", "zoom"}, {"tab", "c"}}));
  EXPECT_TRUE(refused_error);
  EXPECT_NE(refused.find("a, b"), std::string::npos) << refused;
}

TEST_F(McpEndpointTest, TearingDownTheSessionRevokesAndDropsItsEvaluation) {
  ASSERT_NO_FATAL_FAILURE(openPublic());
  ASSERT_NE(loadMock(), 0);
  Plugin plugin = launch(u"toolbox-mcp-a"_s);
  ASSERT_FALSE(plugin.token.empty());
  const std::unique_ptr<QTcpSocket> socket = request(
      toolsCall(
          "evaluate", {{"inputs", {"mock/file_data/value"}},
                       {"at_s", 0.0},
                       {"body", "return { y = 1 }"},
                       {"outputs", {"y:number"}}}),
      plugin.token, "assistant");
  ASSERT_TRUE(pumpUntil([&] { return PJ::McpTestPeer::deferred(*server()) == 1; }));
  EXPECT_FALSE(socket->readAll().contains("HTTP/1.1")) << "evaluate answered at once: nothing was deferred";
  plugin.session.reset();  // its evaluation releases into services that die right after
  EXPECT_EQ(PJ::McpTestPeer::deferred(*server()), 0U);
  EXPECT_EQ(exchange(*socket).responses, 0);
  EXPECT_EQ(callAs(plugin.token, "list_topics", json::object()).status, 401);
}

TEST_F(McpEndpointTest, ADeferredResultWhoseClientLeftIsDropped) {
  ASSERT_NO_FATAL_FAILURE(openPublic());
  const Plugin plugin = launch(u"toolbox-mcp-a"_s);
  ASSERT_FALSE(plugin.token.empty());
  PJ::McpServer& mcp = *server();
  for (const std::string& token : {token_, plugin.token}) {
    auto client = std::make_unique<QTcpSocket>();
    int resumes = 0;
    PJ::McpTestPeer::deferForever(mcp, QByteArray::fromStdString(token), client.get(), resumes);
    ASSERT_TRUE(pumpUntil([&] { return resumes >= 2; })) << token;
    client.reset();
    const int seen = resumes;
    static_cast<void>(pumpUntil([] { return false; }, 200));  // a few resume ticks
    EXPECT_EQ(PJ::McpTestPeer::deferred(mcp), 0U) << token;
    EXPECT_EQ(resumes, seen) << token;
  }
}

TEST_F(McpEndpointTest, TearingDownASessionWhileResumingDropsItsQueuedResultsBeforeItsServices) {
  Plugin a = launch(u"toolbox-mcp-a"_s);
  Plugin b = launch(u"toolbox-mcp-b"_s);
  Plugin c = launch(u"toolbox-mcp-c"_s);
  ASSERT_FALSE(a.token.empty());
  ASSERT_FALSE(b.token.empty());
  ASSERT_FALSE(c.token.empty());
  PJ::McpServer& mcp = *server();
  QTcpSocket client;  // stands for each caller's live connection
  // Set only while a session is being destroyed: its token is revoked first, its services after.
  bool tearing_down = false;
  const auto tear_down = [&tearing_down](Plugin& plugin) {
    tearing_down = true;
    plugin.session.reset();
    tearing_down = false;
  };
  struct Queued {
    int resumes = 0;
    bool dropped_in_teardown = false;
  };
  const auto queue = [&](const Plugin& plugin, Queued& queued) {
    const auto token = lifetimeToken([&queued, &tearing_down] { queued.dropped_in_teardown = tearing_down; });
    PJ::McpTestPeer::defer(
        mcp, QByteArray::fromStdString(plugin.token), &client,
        [&queued, token](PJ::agent_tools::ToolContext&) {
          ++queued.resumes;
          return PJ::agent_tools::ToolResult::success("ran");
        },
        [](const json&) { ADD_FAILURE() << "a revoked token's result was sent"; });
  };
  // a's continuation tears b's session down, a's reply c's; b and c are queued behind a.
  bool replied = false;
  PJ::McpTestPeer::defer(
      mcp, QByteArray::fromStdString(a.token), &client,
      [&](PJ::agent_tools::ToolContext&) {
        tear_down(b);
        return PJ::agent_tools::ToolResult::success("done");
      },
      [&](const json&) {
        replied = true;
        tear_down(c);
      });
  Queued queued_b;
  Queued queued_c;
  queue(b, queued_b);
  queue(c, queued_c);
  ASSERT_TRUE(pumpUntil([&replied] { return replied; }));
  static_cast<void>(pumpUntil([] { return false; }, 200));  // a few more resume ticks
  EXPECT_TRUE(queued_b.dropped_in_teardown) << "b's result outlived its token, into its services' death";
  EXPECT_TRUE(queued_c.dropped_in_teardown) << "c's result outlived its token, into its services' death";
  EXPECT_EQ(queued_b.resumes, 0);
  EXPECT_EQ(queued_c.resumes, 0);
  EXPECT_EQ(PJ::McpTestPeer::deferred(mcp), 0U);
}

TEST_F(McpEndpointTest, AFollowUpOfARevokedTokenIsNotQueued) {
  Plugin plugin = launch(u"toolbox-mcp-a"_s);
  ASSERT_FALSE(plugin.token.empty());
  PJ::McpServer& mcp = *server();
  QTcpSocket client;  // stands for the caller's live connection
  bool resumed = false;
  bool follow_up_dropped = false;
  int follow_up_resumes = 0;
  PJ::McpTestPeer::defer(
      mcp, QByteArray::fromStdString(plugin.token), &client,
      [&](PJ::agent_tools::ToolContext&) {
        resumed = true;
        release(*plugin.session);  // the plugin lets its endpoint go mid-call; its services stay
        return PJ::agent_tools::ToolResult::deferred([&follow_up_resumes, token = lifetimeToken([&follow_up_dropped] {
                                                                            follow_up_dropped = true;
                                                                          })](PJ::agent_tools::ToolContext&) {
          ++follow_up_resumes;
          return PJ::agent_tools::ToolResult::success("late");
        });
      },
      [](const json&) { ADD_FAILURE() << "a revoked token's result was sent"; });
  ASSERT_TRUE(pumpUntil([&resumed] { return resumed; }));
  EXPECT_TRUE(follow_up_dropped);
  EXPECT_EQ(PJ::McpTestPeer::deferred(mcp), 0U);
  static_cast<void>(pumpUntil([] { return false; }, 200));  // a few more resume ticks
  EXPECT_EQ(follow_up_resumes, 0);
}

TEST_F(McpEndpointTest, AnEndpointWhoseServerIsGoneReleasesSafely) {
  const Plugin plugin = launch(u"toolbox-mcp-a"_s);
  ASSERT_FALSE(plugin.token.empty());
  PJ::McpTestPeer::destroyServer(*window_);
  release(*plugin.session);
  release(*plugin.session);
}

}  // namespace
