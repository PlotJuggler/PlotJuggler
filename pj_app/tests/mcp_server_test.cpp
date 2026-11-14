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
#include <QPushButton>
#include <QRectF>
#include <QRegularExpression>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <thread>
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
#include "pj_base/sdk/data_source_plugin_base.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_registry.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/PluginRuntimeCatalog.h"
#include "pj_runtime/SessionManager.h"
#include "pj_widgets/Dialog.h"
#include "pj_widgets/MessageBox.h"
#include "pj_widgets/ToggleSwitch.h"
#include "support/loader_test_support.h"
#include "ui/McpJobsDialog.h"

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
  /// Agent continuations still queued (jobs with a pending resume).
  [[nodiscard]] static std::size_t deferred(const McpServer& server) {
    return static_cast<std::size_t>(std::count_if(
        server.jobs_.begin(), server.jobs_.end(), [](const auto& entry) { return bool(entry.second.resume); }));
  }
  /// Queues `resume` as the deferred result of agent tool `tool`, called by the holder of `token` on
  /// `socket` with `args` (its wait_s); `reply` gets its terminal result, or the job id once the
  /// call outlasts its wait_s.
  static void defer(
      McpServer& server, const QByteArray& token, QTcpSocket* socket,
      std::function<agent_tools::ToolResult(agent_tools::ToolContext&)> resume, McpServer::Reply reply,
      const json& args = json::object(), const json& request_id = {}, const std::string& tool = "evaluate",
      bool declares_tasks = false) {
    McpServer::Caller caller = callerOf(server, token, socket);
    caller.request_id = request_id;
    caller.tasks = declares_tasks;
    if (const quint64 job = server.openJob(tool, args, caller, std::move(reply)); job != 0) {
      server.continueJob(job, agent_tools::ToolResult::deferred(std::move(resume)));
    }
  }
  /// Opens a job as app tool open_file does for its load: stopped through `kill`, ended by endJob.
  static quint64 openAppJob(
      McpServer& server, const QByteArray& token, QTcpSocket* socket, const json& args, McpServer::Reply reply,
      std::function<void(bool)> kill) {
    const quint64 job = server.openJob("open_file", args, callerOf(server, token, socket), std::move(reply));
    if (job != 0) {
      server.jobs_.at(job).kill = std::move(kill);
    }
    return job;
  }
  /// The terminal result of an app tool's work, as its reply delivers it.
  static void endJob(McpServer& server, quint64 job, json result) {
    server.finishJob(job, std::move(result));
  }
  /// Every job the server holds, running or ended.
  [[nodiscard]] static std::size_t jobs(const McpServer& server) {
    return server.jobs_.size();
  }
  /// Moves every ended job's end `age` into the past, as if that much time went by.
  static void ageEndedJobs(McpServer& server, std::chrono::steady_clock::duration age) {
    for (auto& [id, job] : server.jobs_) {
      if (McpServer::ended(job)) {
        job.ended_at -= age;
      }
    }
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
    return {
        .token = token,
        .owner = token == server.public_token_ ? u"agent-test"_s : QString(),
        .connection = socket,
        .request_id = {},
        .job = 0,
        .tasks = false};
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

  /// Opens a prompt of the user's own, modal like every desktop question, from the event loop.
  void openUsersDialog() {
    QTimer::singleShot(0, window_.get(), [this] {
      PJ::MessageBox::questionAsync(
          window_.get(), QStringLiteral("Hold on"), QStringLiteral("The user is deciding."),
          {{QStringLiteral("OK"), PJ::MessageBox::kPrimaryRole}}, {});
    });
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
  openUsersDialog();
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

// A null sample (an empty CSV cell, an absent optional field) is no value: the store reads it as
// NaN everywhere else, and so must the two tools that read through the SDK's series view, whose
// value buffer holds a zero in that slot.
TEST_F(McpServerTest, ANullSampleIsNotReadAsZero) {
  const int dataset = loadMock();
  ASSERT_NE(dataset, 0);
  PJ::SessionManager& manager = window_->appSession().sessionManager();
  PJ::DataWriter writer = manager.dataEngine().createWriter();
  const auto gaps = writer.registerScalarSeries(static_cast<PJ::DatasetId>(dataset), "gaps", PJ::NumericType::kFloat64);
  ASSERT_TRUE(gaps.has_value()) << gaps.error();
  for (const auto& [stamp, value] :
       std::vector<std::pair<PJ::Timestamp, std::optional<double>>>{{100, 5.0}, {200, std::nullopt}, {300, 7.0}}) {
    ASSERT_TRUE(writer.beginRow(gaps->topic_id, stamp).has_value());
    value.has_value() ? writer.set<double>(gaps->topic_id, 0, *value) : writer.setNull(gaps->topic_id, 0);
    ASSERT_TRUE(writer.finishRow(gaps->topic_id).has_value());
  }
  ASSERT_FALSE(manager.commitChunks(writer.flushAll()).empty());
  window_->appSession().catalogModel().rebuildFromDatastore();

  const auto [stats, stats_error] = result(call("read_series", {{"paths", {"gaps/value"}}, {"mode", "stats"}}));
  ASSERT_FALSE(stats_error) << stats;
  EXPECT_NE(stats.find("\"min\":5"), std::string::npos) << "the null was read as a sample of 0: " << stats;

  const auto [text, error] = result(call("export_series", {{"paths", {"gaps/value"}}}));
  ASSERT_FALSE(error) << text;
  QFile csv(QString::fromStdString(json::parse(text, nullptr, false).value("path", "")));
  ASSERT_TRUE(csv.open(QIODevice::ReadOnly));
  const QList<QByteArray> lines = csv.readAll().trimmed().split('\n');
  csv.remove();
  ASSERT_EQ(lines.size(), 4) << lines.join('|').toStdString();
  EXPECT_EQ(lines[1].split(',').value(1), "5");
  EXPECT_EQ(lines[2].split(',').value(1), "nan") << "the null was exported as a value";
  EXPECT_EQ(lines[3].split(',').value(1), "7");
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

TEST_F(McpServerTest, AKilledExportLeavesNothingBehindNorTouchesTheFileThatWasThere) {
  ASSERT_NE(loadMock(), 0);  // samples at 100, 200 and 300 ns
  QTemporaryDir out;
  ASSERT_TRUE(out.isValid());
  const QString destination = out.filePath(u"keep.csv"_s);
  {
    QFile existing(destination);
    ASSERT_TRUE(existing.open(QIODevice::WriteOnly));
    existing.write("the user's file");
  }
  // Close to the row limit, so the writer is still at it when the stop arrives.
  const json started = json::parse(
      result(call(
                 "export_series", {{"paths", {"mock/file_data/value"}},
                                   {"path", destination.toStdString()},
                                   {"rate_hz", 4.9e13},
                                   {"wait_s", 0}}))
          .first,
      nullptr, false);
  ASSERT_TRUE(started.contains("job_id")) << started.dump();
  const auto [killed, kill_error] = result(call("job_kill", {{"job_id", started["job_id"]}}));
  EXPECT_FALSE(kill_error) << killed;
  ASSERT_EQ(json::parse(killed, nullptr, false).value("state", ""), "killed") << killed;
  QFile kept(destination);
  ASSERT_TRUE(kept.open(QIODevice::ReadOnly));
  EXPECT_EQ(kept.readAll(), "the user's file");
  EXPECT_EQ(QDir(out.path()).entryList(QDir::Files).size(), 1) << "the stopped export left a partial file";
}

TEST_F(McpServerTest, AnExportThatOutlastsItsWaitIsAJobWhoseResultIsTheFile) {
  ASSERT_NE(loadMock(), 0);
  const json started = json::parse(
      result(call("export_series", {{"paths", {"mock/file_data/value"}}, {"rate_hz", 1e13}, {"wait_s", 0}})).first,
      nullptr, false);
  ASSERT_TRUE(started.contains("job_id")) << started.dump();
  const auto [text, error] = result(call("job_wait", {{"job_id", started["job_id"]}, {"timeout_s", 60}}));
  ASSERT_FALSE(error) << text;
  const json done = json::parse(text, nullptr, false);
  EXPECT_EQ(done.value("rows", 0), 2000001);
  const QString path = QString::fromStdString(done.value("path", ""));
  EXPECT_TRUE(QFile::exists(path)) << text;
  QFile::remove(path);
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

/// The JSON a tool result's text holds.
json parsedText(const std::pair<std::string, bool>& text) {
  return json::parse(text.first, nullptr, false);
}

/// The JSON in the text of `reply`, a tool result as a Reply gets it.
json textOf(const json& reply) {
  return json::parse(reply["content"][0].value("text", ""), nullptr, false);
}

/// An object evaluation at an instant: deferred for a tick or two, then answered.
json instantEvaluation() {
  return {{"inputs", {"mock/file_data/value"}}, {"at_s", 0.0}, {"body", "return { y = 1 }"}, {"outputs", {"y:number"}}};
}

/// A continuation that never finishes; `alive`, when given, dies with its last copy, and `resumes`
/// counts how often it ran.
struct Endless {
  std::shared_ptr<int> alive = {};
  int* resumes = nullptr;
  PJ::agent_tools::ToolResult operator()(PJ::agent_tools::ToolContext&) const {
    if (resumes != nullptr) {
      ++*resumes;
    }
    return PJ::agent_tools::ToolResult::deferred(*this);
  }
};

/// Calls made without HTTP as the public client "test" (owner agent-test, as the HTTP helpers'
/// default client), so a case can hold a call open and act around it.
class McpJobTest : public McpServerTest {
 protected:
  /// A call of `client_` to agent tool `tool` whose result is `resume`, made with `args` (its wait_s);
  /// replies land in replies_.
  void startCall(
      std::function<PJ::agent_tools::ToolResult(PJ::agent_tools::ToolContext&)> resume, const json& args,
      const json& request_id = {}, const std::string& tool = "evaluate", bool declares_tasks = false) {
    PJ::McpTestPeer::defer(
        *server(), QByteArray::fromStdString(token_), client_.get(), std::move(resume),
        [this](json reply) { replies_.push_back(std::move(reply)); }, args, request_id, tool, declares_tasks);
  }

  /// A call of `client_` standing for an import: `kills` counts its stop requests, and it ends only
  /// through McpTestPeer::endJob. Returns its job.
  quint64 startImport(const json& args, int& kills) {
    return PJ::McpTestPeer::openAppJob(
        *server(), QByteArray::fromStdString(token_), client_.get(), args,
        [this](json reply) { replies_.push_back(std::move(reply)); }, [&kills](bool) { ++kills; });
  }

  /// A call of `client_` that never ends.
  void startEndless(const json& args, const json& request_id = {}, std::shared_ptr<int> alive = {}) {
    startCall(Endless{std::move(alive)}, args, request_id);
  }

  /// Starts an endless call that goes to the background at once; returns its job id.
  json startBackgroundJob(std::shared_ptr<int> alive = {}) {
    const std::size_t before = replies_.size();
    startEndless({{"wait_s", 0}}, {}, std::move(alive));
    PJ::McpTestPeer::resumeDeferred(*server());
    EXPECT_EQ(replies_.size(), before + 1) << "the call did not get its job id";
    return replies_.size() > before ? textOf(replies_.back()).value("job_id", json()) : json();
  }

  std::unique_ptr<QTcpSocket> client_ = std::make_unique<QTcpSocket>();  // the caller's live connection
  std::vector<json> replies_;
};

TEST_F(McpServerTest, TheJobCapableToolsTakeWaitSAndEveryClientGetsTheJobTools) {
  const Answer listed = rpcAs(token_, {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}, "test");
  std::map<std::string, json> tools;
  for (const json& tool : listed.body["result"]["tools"]) {
    tools[tool.value("name", "")] = tool;
  }
  for (const char* name : {"evaluate", "create_derived_object", "open_file", "reload_file"}) {
    EXPECT_TRUE(tools[name]["inputSchema"]["properties"].contains("wait_s")) << name;
  }
  EXPECT_FALSE(tools["read_series"]["inputSchema"]["properties"].contains("wait_s"));
  EXPECT_TRUE(tools["jobs_list"]["annotations"].value("readOnlyHint", false));
  EXPECT_TRUE(tools["job_wait"]["annotations"].value("readOnlyHint", false));
  EXPECT_FALSE(tools["job_kill"]["annotations"].value("readOnlyHint", true)) << "a kill changes state";
}

TEST_F(McpJobTest, ACallThatEndsWithinItsWaitRepliesAsBefore) {
  ASSERT_NE(loadMock(), 0);
  const auto [text, error] = result(call("evaluate", instantEvaluation()));
  ASSERT_FALSE(error) << text;
  EXPECT_EQ(text.find("job_id"), std::string::npos) << text;
  EXPECT_EQ(PJ::McpTestPeer::jobs(*server()), 0U) << "a call that ended in the foreground left a job";
}

TEST_F(McpJobTest, ACallPastItsWaitBecomesAJobThatJobWaitAndJobKillReach) {
  const json id = startBackgroundJob();
  ASSERT_TRUE(id.is_number_integer());
  EXPECT_EQ(textOf(replies_[0]).value("state", ""), "running");

  const json listed = parsedText(result(call("jobs_list", json::object())));
  ASSERT_EQ(listed.size(), 1U) << listed.dump();
  EXPECT_EQ(listed[0]["job_id"], id);
  EXPECT_EQ(listed[0].value("tool", ""), "evaluate");
  EXPECT_EQ(parsedText(result(call("job_wait", {{"job_id", id}, {"timeout_s", 0}}))).value("state", ""), "running");
  // A wait that times out says the job still runs, without ending it.
  EXPECT_EQ(parsedText(result(call("job_wait", {{"job_id", id}, {"timeout_s", 0.1}}))).value("state", ""), "running");

  const auto killed = result(call("job_kill", {{"job_id", id}}));
  EXPECT_FALSE(killed.second) << killed.first;
  EXPECT_EQ(parsedText(killed).value("state", ""), "killed") << killed.first;
  // The result stays readable, again and again, until it expires.
  for (int i = 0; i < 2; ++i) {
    const auto [text, error] = result(call("job_wait", {{"job_id", id}}));
    EXPECT_TRUE(error);
    EXPECT_NE(text.find("killed"), std::string::npos) << text;
  }
  EXPECT_EQ(parsedText(result(call("job_kill", {{"job_id", id}}))).value("state", ""), "killed") << "a repeated kill";
  EXPECT_EQ(replies_.size(), 1U) << "the call that got its job id was answered again";
  PJ::McpTestPeer::ageEndedJobs(*server(), std::chrono::minutes(11));
  const auto [expired, expired_error] = result(call("job_wait", {{"job_id", id}}));
  EXPECT_TRUE(expired_error);
  EXPECT_NE(expired.find("no such job"), std::string::npos) << expired;
}

TEST_F(McpJobTest, WronglyTypedJobArgumentsAreToolErrorsThatStopNothing) {
  const json id = startBackgroundJob();
  ASSERT_TRUE(id.is_number_integer());
  for (const json& args : {json{{"job_id", id}, {"keep_partial", "yes"}}, json{{"job_id", "one"}}, json::object()}) {
    const auto [text, error] = result(call("job_kill", args));
    EXPECT_TRUE(error) << args.dump() << ": " << text;
  }
  EXPECT_TRUE(result(call("job_wait", {{"job_id", "one"}})).second);
  // A timeout that is no number falls back to the default wait; the job is untouched either way.
  const std::unique_ptr<QTcpSocket> waiting = post("job_wait", {{"job_id", id}, {"timeout_s", "soon"}});
  static_cast<void>(pumpUntil([] { return false; }, 150));
  EXPECT_FALSE(waiting->readAll().contains("HTTP/1.1"));
  EXPECT_EQ(parsedText(result(call("jobs_list", json::object())))[0].value("state", ""), "running");
}

TEST_F(McpJobTest, AJobWaitGetsTheResultWhenTheJobEnds) {
  bool may_finish = false;
  struct Pending {
    const bool* may_finish;
    PJ::agent_tools::ToolResult operator()(PJ::agent_tools::ToolContext&) const {
      return *may_finish ? PJ::agent_tools::ToolResult::success("finished")
                         : PJ::agent_tools::ToolResult::deferred(*this);
    }
  };
  startCall(Pending{&may_finish}, {{"wait_s", 0}});
  PJ::McpTestPeer::resumeDeferred(*server());
  ASSERT_EQ(replies_.size(), 1U);
  const json id = textOf(replies_[0])["job_id"];
  // The agent's client times out and drops the request: a background job does not care.
  client_.reset();
  const std::unique_ptr<QTcpSocket> waiting = post("job_wait", {{"job_id", id}, {"timeout_s", 30}});
  static_cast<void>(pumpUntil([] { return false; }, 150));
  EXPECT_FALSE(waiting->readAll().contains("HTTP/1.1")) << "job_wait answered while the job ran";
  may_finish = true;
  const auto [text, error] = result(exchange(*waiting));
  EXPECT_FALSE(error) << text;
  EXPECT_EQ(text, "finished");
  EXPECT_EQ(result(call("job_wait", {{"job_id", id}})).first, "finished") << "the result is repeatable";
  EXPECT_EQ(replies_.size(), 1U) << "the call's reply was sent twice";
}

TEST_F(McpJobTest, AnotherOwnersJobReadsAsNoSuchJob) {
  const json id = startBackgroundJob();
  ASSERT_TRUE(id.is_number_integer());
  for (const char* tool : {"job_wait", "job_kill"}) {
    const auto [text, error] = result(callAs(token_, tool, {{"job_id", id}}, "other"));
    EXPECT_TRUE(error) << tool;
    EXPECT_NE(text.find("no such job"), std::string::npos) << text;
  }
  EXPECT_EQ(parsedText(result(callAs(token_, "jobs_list", json::object(), "other"))), json::array());
  const json mine = parsedText(result(call("jobs_list", json::object())));
  ASSERT_EQ(mine.size(), 1U);
  EXPECT_EQ(mine[0].value("state", ""), "running") << "the other owner stopped it";
}

TEST_F(McpJobTest, KillingAJobReleasesItsContinuationAtOnce) {
  bool released = false;
  const json id = startBackgroundJob(lifetimeToken([&released] { released = true; }));
  ASSERT_TRUE(id.is_number_integer());
  EXPECT_FALSE(released);
  PJ::McpTestPeer::callTool(
      *server(), QByteArray::fromStdString(token_), client_.get(), "job_kill", {{"job_id", id}},
      [this](json reply) { replies_.push_back(std::move(reply)); });
  EXPECT_TRUE(released) << "the kill kept the continuation (and its evaluation) alive";
  ASSERT_EQ(replies_.size(), 2U) << "job_kill answers once the stop took effect";
  EXPECT_EQ(textOf(replies_[1]).value("state", ""), "killed") << replies_[1].dump();
}

TEST_F(McpJobTest, AForegroundCallDiesWithItsConnection) {
  bool released = false;
  startEndless({{"wait_s", 60}}, {}, lifetimeToken([&released] { released = true; }));
  PJ::McpTestPeer::resumeDeferred(*server());
  ASSERT_EQ(PJ::McpTestPeer::deferred(*server()), 1U);
  client_.reset();
  PJ::McpTestPeer::resumeDeferred(*server());
  EXPECT_TRUE(released);
  EXPECT_EQ(PJ::McpTestPeer::jobs(*server()), 0U);
  EXPECT_TRUE(replies_.empty());
}

TEST_F(McpJobTest, AHangUpAnswersAnotherSessionWaitingOnTheJob) {
  startEndless({{"wait_s", 60}});
  // A second session of the same client learns the id and waits on it; then the first hangs up.
  const json id = parsedText(result(call("jobs_list", json::object())))[0]["job_id"];
  const std::unique_ptr<QTcpSocket> waiting = post("job_wait", {{"job_id", id}, {"timeout_s", 30}});
  static_cast<void>(pumpUntil([] { return false; }, 150));
  ASSERT_FALSE(waiting->readAll().contains("HTTP/1.1"));
  client_.reset();
  const auto [text, error] = result(exchange(*waiting));
  EXPECT_TRUE(error);
  EXPECT_NE(text.find("killed"), std::string::npos) << text;
  EXPECT_EQ(parsedText(result(call("jobs_list", json::object())))[0].value("state", ""), "killed");
  EXPECT_TRUE(replies_.empty());
}

TEST_F(McpJobTest, AnOwnerRunsAtMostEightJobs) {
  ASSERT_NE(loadMock(), 0);
  for (int i = 0; i < 8; ++i) {
    startEndless({{"wait_s", 0}});
  }
  EXPECT_FALSE(result(call("get_status", json::object())).second) << "a call that is no job still runs";
  const json evaluate = instantEvaluation();
  const auto [text, error] = result(call("evaluate", evaluate));
  EXPECT_TRUE(error);
  EXPECT_NE(text.find("jobs_list"), std::string::npos) << text;
  EXPECT_EQ(PJ::McpTestPeer::jobs(*server()), 8U);
  const auto [refused, refused_error] =
      result(call("open_file", {{"path", pj_app_test::makeMockFile(data_, "b.mock").toStdString()}}));
  EXPECT_TRUE(refused_error) << refused;
  // Another client of the same server has its own allowance.
  const auto [other, other_error] = result(callAs(token_, "evaluate", evaluate, "other"));
  EXPECT_FALSE(other_error) << other;
}

TEST_F(McpJobTest, OnlyTheLatestEndedJobsOfAnOwnerAreKept) {
  std::vector<json> ids;
  for (int i = 0; i < 34; ++i) {
    ids.push_back(startBackgroundJob());
    ASSERT_FALSE(result(call("job_kill", {{"job_id", ids.back()}})).second);
  }
  EXPECT_EQ(PJ::McpTestPeer::jobs(*server()), 32U);
  EXPECT_TRUE(result(call("job_wait", {{"job_id", ids.front()}})).second) << "the oldest result was kept";
  EXPECT_NE(result(call("job_wait", {{"job_id", ids.back()}})).first.find("killed"), std::string::npos);
}

TEST_F(McpJobTest, TheResultsKeptAreTheLatestToEnd) {
  const json first = startBackgroundJob();  // started first, ended last
  for (int i = 0; i < 32; ++i) {
    ASSERT_FALSE(result(call("job_kill", {{"job_id", startBackgroundJob()}})).second);
  }
  ASSERT_FALSE(result(call("job_kill", {{"job_id", first}})).second);
  EXPECT_NE(result(call("job_wait", {{"job_id", first}})).first.find("killed"), std::string::npos)
      << "the job that just ended lost its result to older ones";
}

TEST_F(McpJobTest, AStopThatCameTooLateLeavesTheJobCompleted) {
  int kills = 0;
  const quint64 job = startImport({{"wait_s", 0}}, kills);
  PJ::McpTestPeer::resumeDeferred(*server());
  ASSERT_EQ(replies_.size(), 1U);
  // The import had already finished when the stop arrived: its terminal is on its way.
  const std::unique_ptr<QTcpSocket> killing = post("job_kill", {{"job_id", job}, {"keep_partial", false}});
  ASSERT_TRUE(pumpUntil([&] { return kills == 1; }));
  EXPECT_TRUE(parsedText(result(call("jobs_list", json::object())))[0].value("stopping", false));
  PJ::McpTestPeer::endJob(*server(), job, PJ::mcp::textResult("loaded"));
  EXPECT_EQ(parsedText(result(exchange(*killing))).value("state", ""), "completed");
  const auto [text, error] = result(call("job_wait", {{"job_id", job}}));
  EXPECT_FALSE(error);
  EXPECT_EQ(text, "loaded");
  // A second terminal (the load's own, after a stop already ended the job) changes nothing.
  PJ::McpTestPeer::endJob(*server(), job, PJ::mcp::textResult("again", true));
  EXPECT_EQ(result(call("job_wait", {{"job_id", job}})).first, "loaded");
  EXPECT_EQ(kills, 1);
}

TEST_F(McpJobTest, AnImportWhoseCallHungUpKeepsItsResult) {
  int kills = 0;
  // Before and after the tick that notices the hang-up: the client finds the outcome either way.
  for (const bool ticked : {false, true}) {
    client_ = std::make_unique<QTcpSocket>();
    const quint64 job = startImport({{"wait_s", 60}}, kills);
    client_.reset();
    if (ticked) {
      PJ::McpTestPeer::resumeDeferred(*server());
    }
    PJ::McpTestPeer::endJob(*server(), job, PJ::mcp::textResult("loaded"));
    EXPECT_EQ(result(call("job_wait", {{"job_id", job}})).first, "loaded") << ticked;
  }
  EXPECT_EQ(kills, 0) << "an import was stopped because its client left";
  EXPECT_TRUE(replies_.empty());
}

TEST_F(McpJobTest, AForegroundJobSeenInJobsListKeepsItsResult) {
  int kills = 0;
  const quint64 job = startImport({{"wait_s", 60}}, kills);
  // Another session of the same client learns the id while the call still waits.
  ASSERT_EQ(parsedText(result(call("jobs_list", json::object())))[0].value("job_id", 0U), job);
  PJ::McpTestPeer::endJob(*server(), job, PJ::mcp::textResult("loaded"));
  ASSERT_EQ(replies_.size(), 1U) << "the waiting call was not answered";
  EXPECT_EQ(replies_[0]["content"][0].value("text", ""), "loaded");
  EXPECT_EQ(result(call("job_wait", {{"job_id", job}})).first, "loaded");
}

TEST_F(McpJobTest, AMutatingJobWaitsWhileTheUserHasADialogOpen) {
  int resumes = 0;
  startCall(Endless{.alive = {}, .resumes = &resumes}, {{"wait_s", 0}}, {}, "create_derived_object");
  ASSERT_TRUE(pumpUntil([&] { return resumes >= 2; }));
  std::optional<json> listed;
  int resumes_under_dialog = -1;
  QTimer probe;
  QObject::connect(&probe, &QTimer::timeout, [&] {
    const std::vector<PJ::MessageBox*> boxes = visible<PJ::MessageBox>();
    if (listed.has_value() || boxes.empty()) {
      return;
    }
    const int before = resumes;
    static_cast<void>(pumpUntil([] { return false; }, 200));  // a few resume ticks
    resumes_under_dialog = resumes - before;
    listed = parsedText(result(call("jobs_list", json::object())));
    boxes.front()->reject();
  });
  probe.start(50);
  openUsersDialog();
  ASSERT_TRUE(pumpUntil([&] { return listed.has_value() && visible<PJ::MessageBox>().empty(); }));
  EXPECT_EQ(resumes_under_dialog, 0) << "a mutating continuation ran under the user's dialog";
  EXPECT_EQ((*listed)[0].value("state", ""), "waiting_for_user") << listed->dump();
  const int after = resumes;
  ASSERT_TRUE(pumpUntil([&] { return resumes > after; })) << "the job did not carry on once the dialog closed";
  EXPECT_EQ(parsedText(result(call("jobs_list", json::object())))[0].value("state", ""), "running");
}

TEST_F(McpJobTest, TheUserSeesRunningJobsAndStopsOne) {
  bool released = false;
  const json id = startBackgroundJob(lifetimeToken([&released] { released = true; }));
  ASSERT_TRUE(id.is_number_integer());
  // The status bar says something runs, even though no client has called for a while.
  auto* indicator = window_->findChild<QToolButton*>(u"mcpAgentButton"_s);
  ASSERT_NE(indicator, nullptr);
  EXPECT_TRUE(indicator->isVisibleTo(window_.get()));
  EXPECT_NE(indicator->text().indexOf(u"1 running"_s), -1) << indicator->text().toStdString();

  indicator->click();
  const std::vector<PJ::McpJobsDialog*> dialogs = visible<PJ::McpJobsDialog>();
  ASSERT_EQ(dialogs.size(), 1U);
  EXPECT_EQ(QApplication::activeModalWidget(), nullptr) << "the user's view must not hold the agents up";
  auto* stop = dialogs.front()->findChild<QPushButton*>(u"stopJob%1"_s.arg(id.get<quint64>()));
  ASSERT_NE(stop, nullptr);
  stop->click();
  EXPECT_TRUE(released) << "Stop left the job's work running";
  EXPECT_NE(result(call("job_wait", {{"job_id", id}})).first.find("killed"), std::string::npos);
  // The row is gone from view (its widgets are deleted when the event loop next runs).
  const QList<QPushButton*> buttons = dialogs.front()->findChildren<QPushButton*>(QRegularExpression(u"^stopJob"_s));
  EXPECT_TRUE(std::none_of(buttons.begin(), buttons.end(), [&](const QPushButton* button) {
    return button->isVisibleTo(dialogs.front());
  }));
  EXPECT_EQ(indicator->text().indexOf(u"running"_s), -1) << indicator->text().toStdString();
}

/// A request's params declaring the MCP Tasks extension, as a 2026-07-28 client sends them.
json declaringTasks(json params = json::object()) {
  params["_meta"]["io.modelcontextprotocol/clientCapabilities"]["extensions"][PJ::mcp::kTasksExtension] =
      json::object();
  return params;
}

TEST_F(McpJobTest, AClientThatDeclaresTasksGetsATaskHandleAndReachesItsJobThroughTasksMethods) {
  bool may_finish = false;
  struct Pending {
    const bool* may_finish;
    PJ::agent_tools::ToolResult operator()(PJ::agent_tools::ToolContext&) const {
      // A tool error, which the extension reports as a completed task.
      return *may_finish ? PJ::agent_tools::ToolResult::failure("the tool says no")
                         : PJ::agent_tools::ToolResult::deferred(*this);
    }
  };
  startCall(Pending{&may_finish}, {{"wait_s", 0}}, {}, "evaluate", /*declares_tasks=*/true);
  PJ::McpTestPeer::resumeDeferred(*server());
  ASSERT_EQ(replies_.size(), 1U);
  const json handle = replies_[0];
  EXPECT_EQ(handle.value("resultType", ""), "task") << handle.dump();
  EXPECT_EQ(handle.value("status", ""), "working");
  EXPECT_FALSE(handle.contains("content")) << "a task handle is not a tool result";
  const std::string task_id = handle.value("taskId", "");
  EXPECT_EQ(task_id.size(), 64U) << "a task id must not be guessable";
  EXPECT_TRUE(handle["ttlMs"].is_null());
  EXPECT_FALSE(handle.value("createdAt", "").empty());

  const auto rpc = [&](const char* method, const json& params, const std::string& client = "test") {
    return rpcAs(token_, {{"jsonrpc", "2.0"}, {"id", 5}, {"method", method}, {"params", params}}, client).body;
  };
  const json get = declaringTasks({{"taskId", task_id}});
  EXPECT_EQ(rpc("tasks/get", get)["result"].value("status", ""), "working");
  EXPECT_EQ(rpc("tasks/get", get)["result"].value("resultType", ""), "complete");
  // Without the capability, and for anyone else's client, there is no such method or task.
  const json undeclared = rpc("tasks/get", {{"taskId", task_id}});
  EXPECT_EQ(undeclared["error"].value("code", 0), PJ::mcp::kMissingClientCapability) << undeclared.dump();
  EXPECT_TRUE(undeclared["error"]["data"]["requiredCapabilities"]["extensions"].contains(PJ::mcp::kTasksExtension));
  EXPECT_EQ(rpc("tasks/get", get, "other")["error"].value("code", 0), -32602);
  EXPECT_EQ(rpc("tasks/get", declaringTasks({{"taskId", "nope"}}))["error"].value("code", 0), -32602);
  EXPECT_EQ(
      rpc("tasks/update", declaringTasks({{"taskId", task_id}, {"inputResponses", json::object()}}))["result"],
      json({{"resultType", "complete"}}));
  // The job tools see the same job.
  EXPECT_EQ(parsedText(result(call("jobs_list", json::object()))).size(), 1U);

  may_finish = true;
  PJ::McpTestPeer::resumeDeferred(*server());
  const json done = rpc("tasks/get", get)["result"];
  EXPECT_EQ(done.value("status", ""), "completed") << done.dump();
  EXPECT_TRUE(done["result"].value("isError", false)) << done.dump();
  EXPECT_EQ(done["result"]["content"][0].value("text", ""), "the tool says no");
  EXPECT_TRUE(done["ttlMs"].is_number_integer()) << done.dump();
  EXPECT_EQ(replies_.size(), 1U) << "the call that got its task handle was answered again";
}

TEST_F(McpJobTest, TasksCancelStopsTheJobAndAClientThatDoesNotDeclareTasksGetsTheJobText) {
  bool released = false;
  startCall(
      Endless{lifetimeToken([&released] { released = true; })}, {{"wait_s", 0}}, {}, "evaluate",
      /*declares_tasks=*/true);
  startEndless({{"wait_s", 0}});  // the same client without the capability on this call
  PJ::McpTestPeer::resumeDeferred(*server());
  ASSERT_EQ(replies_.size(), 2U);
  ASSERT_EQ(replies_[0].value("resultType", ""), "task");
  EXPECT_TRUE(textOf(replies_[1]).contains("job_id")) << "a task handle went to a client that did not ask for one";

  const json cancel = declaringTasks({{"taskId", replies_[0].value("taskId", "")}});
  const json ack =
      rpcAs(token_, {{"jsonrpc", "2.0"}, {"id", 6}, {"method", "tasks/cancel"}, {"params", cancel}}, "test").body;
  EXPECT_EQ(ack["result"], json({{"resultType", "complete"}})) << ack.dump();
  EXPECT_TRUE(released) << "tasks/cancel left the work running";
  const json after =
      rpcAs(token_, {{"jsonrpc", "2.0"}, {"id", 7}, {"method", "tasks/get"}, {"params", cancel}}, "test").body;
  EXPECT_EQ(after["result"].value("status", ""), "cancelled") << after.dump();
  EXPECT_FALSE(after["result"].contains("result"));
}

TEST_F(McpJobTest, ATaskIsNotEvictedByLaterJobsBeforeItsPromisedExpiry) {
  startCall(Endless{}, {{"wait_s", 0}}, {}, "evaluate", /*declares_tasks=*/true);
  PJ::McpTestPeer::resumeDeferred(*server());
  ASSERT_EQ(replies_.size(), 1U);
  const json task = declaringTasks({{"taskId", replies_[0].value("taskId", "")}});
  const auto get = [&] {
    return rpcAs(token_, {{"jsonrpc", "2.0"}, {"id", 5}, {"method", "tasks/get"}, {"params", task}}, "test").body;
  };
  ASSERT_TRUE(rpcAs(token_, {{"jsonrpc", "2.0"}, {"id", 6}, {"method", "tasks/cancel"}, {"params", task}}, "test")
                  .body.contains("result"));
  ASSERT_TRUE(get()["result"]["ttlMs"].is_number_integer());
  // More ended jobs than the plain results a client keeps.
  for (int i = 0; i < 34; ++i) {
    ASSERT_FALSE(result(call("job_kill", {{"job_id", startBackgroundJob()}})).second);
  }
  EXPECT_EQ(get()["result"].value("status", ""), "cancelled") << "the task went before the expiry it advertised";
  PJ::McpTestPeer::ageEndedJobs(*server(), std::chrono::minutes(11));
  EXPECT_EQ(get()["error"].value("code", 0), -32602);
}

TEST_F(McpJobTest, ClosingPublicAccessKillsABackgroundJob) {
  bool released = false;
  const json id = startBackgroundJob(lifetimeToken([&released] { released = true; }));
  ASSERT_TRUE(id.is_number_integer());
  ASSERT_EQ(PJ::McpTestPeer::deferred(*server()), 1U) << "the job did not survive its call";
  window_->closeMcpPublicAccess();
  EXPECT_TRUE(released);
  EXPECT_EQ(PJ::McpTestPeer::jobs(*server()), 0U);
}

TEST_F(McpJobTest, ACancelledNotificationStopsThatForegroundCall) {
  bool released = false;
  startEndless({{"wait_s", 60}}, "call-7", lifetimeToken([&released] { released = true; }));
  const json cancel = {
      {"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", "call-7"}}}};
  // Another client's cancel of its own call with that id stops nothing of this one, nor does another id.
  EXPECT_EQ(rpcAs(token_, cancel, "other").status, 202);
  json other_id = cancel;
  other_id["params"]["requestId"] = 7;
  EXPECT_EQ(rpcAs(token_, other_id, "test").status, 202);
  EXPECT_FALSE(released);
  EXPECT_EQ(rpcAs(token_, cancel, "test").status, 202);
  EXPECT_TRUE(released);
  ASSERT_EQ(replies_.size(), 1U);
  EXPECT_TRUE(replies_[0].value("isError", false));
  EXPECT_NE(replies_[0].dump().find("killed"), std::string::npos) << replies_[0].dump();
  EXPECT_EQ(PJ::McpTestPeer::jobs(*server()), 0U);
}

// A direct client may name itself on its calls only: its cancel then carries no name to match.
TEST_F(McpJobTest, ACancelThatNamesNoClientStopsTheCallWithThatId) {
  bool released = false;
  startEndless({{"wait_s", 60}}, "call-9", lifetimeToken([&released] { released = true; }));
  const json cancel = {
      {"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", "call-9"}}}};
  EXPECT_EQ(rpcAs(token_, cancel, "").status, 202);
  EXPECT_TRUE(released);
}

TEST_F(McpJobTest, ACancelNamingTwoCallsOrABackgroundJobStopsNothing) {
  startEndless({{"wait_s", 60}}, 7);
  startEndless({{"wait_s", 60}}, 7);
  startEndless({{"wait_s", 0}}, 8);
  PJ::McpTestPeer::resumeDeferred(*server());  // the third call goes to the background
  for (const int id : {7, 8}) {
    EXPECT_EQ(
        rpcAs(
            token_, {{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", id}}}},
            "test")
            .status,
        202);
  }
  EXPECT_EQ(PJ::McpTestPeer::deferred(*server()), 3U);
}

TEST_F(McpJobTest, UnderTheUsersDialogTheJobToolsRun) {
  const json id = startBackgroundJob();
  ASSERT_TRUE(id.is_number_integer());
  std::optional<Answer> listed;
  std::optional<Answer> killed;
  std::optional<Answer> change;
  bool busy = false;
  QTimer probe;
  QObject::connect(&probe, &QTimer::timeout, [&] {
    const std::vector<PJ::MessageBox*> boxes = visible<PJ::MessageBox>();
    if (busy || boxes.empty()) {
      return;
    }
    busy = true;
    change = call("remove_markers", json::object());
    listed = call("jobs_list", json::object());
    killed = call("job_kill", {{"job_id", id}});
    boxes.front()->reject();
  });
  probe.start(50);
  openUsersDialog();
  ASSERT_TRUE(pumpUntil([&] { return killed.has_value() && visible<PJ::MessageBox>().empty(); }));
  EXPECT_NE(result(*change).first.find("'Hold on' open"), std::string::npos) << "the dialog gated nothing";
  EXPECT_EQ(parsedText(result(*listed)).size(), 1U) << result(*listed).first;
  EXPECT_EQ(parsedText(result(*killed)).value("state", ""), "killed") << result(*killed).first;
}

/// Streams and the dialogs an agent's call opens, as jobs over real HTTP.
class McpStreamAndDialogJobTest : public McpServerTest {
 protected:
  /// Starts the mock stream with no dialog; returns what start_stream answered.
  json startStream() {
    const auto [text, error] =
        result(call("start_stream", {{"source", "Mock Layout Stream"}, {"config", {{"topic", "live"}}}}));
    EXPECT_FALSE(error) << text;
    json started = json::parse(text, nullptr, false);
    // The dataset is listed with its first sample: a stream stopped before one leaves nothing to find.
    EXPECT_TRUE(pumpUntil([&] { return started.contains("dataset_id") && loaded(started["dataset_id"]); }));
    return started;
  }

  [[nodiscard]] bool streaming(const json& dataset) const {
    return !window_->streamingSources().layoutSourceId(dataset.get<PJ::DatasetId>()).isEmpty();
  }

  [[nodiscard]] bool loaded(const json& dataset) const {
    const auto datasets = window_->appSession().catalogModel().datasets();
    return std::any_of(datasets.begin(), datasets.end(), [&](const auto& entry) {
      return entry.first == dataset.get<PJ::DatasetId>();
    });
  }

  /// Runs while a call waits: once a dialog of type T is up, records how the caller's one job
  /// reads, then sends its kill. The dialog is never touched: only the kill may close it. The
  /// kill's answer is read later (killAnswer): the dialog's own modal loop, which this runs in,
  /// must unwind first.
  template <typename T>
  std::function<void()> killOnceShown() {
    return [this] {
      if (kill_ != nullptr || visible<T>().empty() || busy_) {
        return;
      }
      busy_ = true;
      const json jobs = json::parse(result(call("jobs_list", json::object())).first, nullptr, false);
      if (jobs.size() == 1) {
        state_under_dialog_ = jobs[0].value("state", "");
        kill_ = post("job_kill", {{"job_id", jobs[0]["job_id"]}});
      }
      busy_ = false;
    };
  }

  /// What the kill sent by killOnceShown answered.
  json killAnswer() const {
    return json::parse(result(exchange(*kill_)).first, nullptr, false);
  }

  std::unique_ptr<QTcpSocket> kill_;
  std::string state_under_dialog_;
  bool busy_ = false;
};

TEST_F(McpStreamAndDialogJobTest, AStreamIsAJobThatJobKillStops) {
  const json started = startStream();
  ASSERT_TRUE(started.value("dataset_id", 0) != 0 && started.contains("job_id")) << started.dump();
  ASSERT_TRUE(streaming(started["dataset_id"]));
  const json jobs = json::parse(result(call("jobs_list", json::object())).first, nullptr, false);
  ASSERT_EQ(jobs.size(), 1U) << jobs.dump();
  EXPECT_EQ(jobs[0].value("state", ""), "running");
  EXPECT_EQ(jobs[0]["progress"]["dataset_id"], started["dataset_id"]);

  const auto [killed, kill_error] = result(call("job_kill", {{"job_id", started["job_id"]}}));
  EXPECT_FALSE(kill_error) << killed;
  EXPECT_EQ(json::parse(killed, nullptr, false).value("state", ""), "killed") << killed;
  EXPECT_FALSE(streaming(started["dataset_id"]));
  EXPECT_TRUE(loaded(started["dataset_id"])) << "stopping the stream removed its data";
}

TEST_F(McpStreamAndDialogJobTest, WaitForEndsWithASampleThatArrivesOnTheLiveStream) {
  // The mock stream keeps appending samples of value 5 to live/value.
  const auto [stream, stream_error] =
      result(call("start_stream", {{"source", "Mock Layout Stream"}, {"config", {{"topic", "live"}, {"value", 5}}}}));
  ASSERT_FALSE(stream_error) << stream;
  ASSERT_TRUE(pumpUntil([&] { return !result(call("describe_topic", {{"topic", "live"}})).second; }));
  const auto [found, found_error] =
      result(call("wait_for", {{"series", "live/value"}, {"comparison", ">"}, {"threshold", 4}, {"timeout_s", 30}}));
  ASSERT_FALSE(found_error) << found;
  const json sample = json::parse(found, nullptr, false);
  EXPECT_EQ(sample.value("value", 0.0), 5.0) << found;
  EXPECT_TRUE(sample.contains("time_s")) << found;
  // A value the stream never reaches: the wait gives up, naming what it last saw.
  const auto [missed, missed_error] =
      result(call("wait_for", {{"series", "live/value"}, {"comparison", ">"}, {"threshold", 6}, {"timeout_s", 0.3}}));
  EXPECT_TRUE(missed_error);
  EXPECT_NE(missed.find("the latest was 5"), std::string::npos) << missed;
  // And it is a job like the others: in the background past wait_s, stopped by job_kill.
  const json waiting = json::parse(
      result(call(
                 "wait_for",
                 {{"series", "live/value"}, {"comparison", "<"}, {"threshold", 0}, {"timeout_s", 600}, {"wait_s", 0}}))
          .first,
      nullptr, false);
  ASSERT_TRUE(waiting.contains("job_id")) << waiting.dump();
  EXPECT_EQ(
      json::parse(result(call("job_kill", {{"job_id", waiting["job_id"]}})).first, nullptr, false).value("state", ""),
      "killed");
}

/// A tools/call whose client declares the MCP Tasks extension on it, as a 2026-07-28 client does.
json declaringToolsCall(const std::string& tool, const json& args) {
  return {
      {"jsonrpc", "2.0"},
      {"id", 9},
      {"method", "tools/call"},
      {"params", declaringTasks({{"name", tool}, {"arguments", args}})}};
}

TEST_F(McpStreamAndDialogJobTest, ADeclaringCallThatOutlastsItsWaitIsATaskOverTheWire) {
  ASSERT_FALSE(
      result(call("start_stream", {{"source", "Mock Layout Stream"}, {"config", {{"topic", "live"}, {"value", 5}}}}))
          .second);
  ASSERT_TRUE(pumpUntil([&] { return !result(call("describe_topic", {{"topic", "live"}})).second; }));
  // The stream never goes below 0: the wait is still on when its call is handed off.
  const json never = {
      {"series", "live/value"}, {"comparison", "<"}, {"threshold", 0}, {"timeout_s", 600}, {"wait_s", 0}};
  const json handle = rpcAs(token_, declaringToolsCall("wait_for", never), "test").body["result"];
  ASSERT_EQ(handle.value("resultType", ""), "task") << handle.dump();
  const json task = declaringTasks({{"taskId", handle.value("taskId", "")}});
  const auto rpc = [&](const char* method) {
    return rpcAs(token_, {{"jsonrpc", "2.0"}, {"id", 5}, {"method", method}, {"params", task}}, "test").body;
  };
  EXPECT_EQ(rpc("tasks/get")["result"].value("status", ""), "working");
  EXPECT_EQ(rpc("tasks/cancel")["result"], json({{"resultType", "complete"}}));
  EXPECT_EQ(rpc("tasks/get")["result"].value("status", ""), "cancelled");
  // The same call without the declaration is the job text, and one that ends in time is its own result.
  const json plain = rpcAs(token_, toolsCall("wait_for", never), "test").body["result"];
  EXPECT_TRUE(plain.contains("content")) << plain.dump();
  EXPECT_FALSE(plain.contains("resultType"));
  json soon = never;
  soon["comparison"] = ">";
  soon["wait_s"] = 30;
  const json direct = rpcAs(token_, declaringToolsCall("wait_for", soon), "test").body["result"];
  EXPECT_TRUE(direct.contains("content")) << "a call that ended within its wait became a task: " << direct.dump();
}

TEST_F(McpStreamAndDialogJobTest, AStreamIsNeverATaskEvenForAClientThatDeclaresThem) {
  // Handed off while the user is at the dialog: a task would end with the stream's stop, not its start.
  const std::unique_ptr<QTcpSocket> starting = request(
      declaringToolsCall("start_stream", {{"source", "Mock Layout Stream"}, {"ask_user", true}, {"wait_s", 0}}), token_,
      "test");
  QByteArray received;
  bool rejected = false;
  QTimer user;
  QObject::connect(&user, &QTimer::timeout, [&] {
    received += starting->readAll();
    if (!rejected && received.contains("\r\n\r\n") && !visible<PJ::Dialog>().empty()) {
      rejected = true;
      visible<PJ::Dialog>().front()->reject();
    }
  });
  user.start(20);
  ASSERT_TRUE(pumpUntil([&] { return rejected && visible<PJ::Dialog>().empty(); }));
  user.stop();
  const json answer = json::parse(received.mid(received.indexOf("\r\n\r\n") + 4).toStdString(), nullptr, false);
  EXPECT_FALSE(answer["result"].contains("resultType")) << answer.dump();
  EXPECT_NE(answer["result"]["content"][0].value("text", "").find("job_id"), std::string::npos) << answer.dump();
}

TEST_F(McpStreamAndDialogJobTest, AStreamThatSomeoneElseStopsEndsItsJobCompleted) {
  const json started = startStream();
  ASSERT_TRUE(started.contains("job_id")) << started.dump();
  const std::unique_ptr<QTcpSocket> waiting = post("job_wait", {{"job_id", started["job_id"]}, {"timeout_s", 30}});
  static_cast<void>(pumpUntil([] { return false; }, 150));
  ASSERT_FALSE(waiting->readAll().contains("HTTP/1.1")) << "job_wait answered while the stream ran";
  ASSERT_FALSE(result(call("stop_stream", {{"dataset", started["dataset_id"]}})).second);
  const auto [text, error] = result(exchange(*waiting));
  EXPECT_FALSE(error) << text;
  EXPECT_EQ(json::parse(text, nullptr, false)["stopped"], started["dataset_id"]) << text;
}

TEST_F(McpStreamAndDialogJobTest, ClosingPublicAccessStopsTheStreamsItsAgentsStarted) {
  const json started = startStream();
  ASSERT_TRUE(streaming(started["dataset_id"]));
  window_->closeMcpPublicAccess();
  EXPECT_TRUE(pumpUntil([&] { return !streaming(started["dataset_id"]); }));
  EXPECT_TRUE(loaded(started["dataset_id"]));
  EXPECT_EQ(PJ::McpTestPeer::jobs(*server()), 0U);
}

TEST_F(McpStreamAndDialogJobTest, JobKillClosesTheLoaderDialogTheAgentOpened) {
  QFile probe_file(data_.filePath("x.dlgprobe"));
  ASSERT_TRUE(probe_file.open(QIODevice::WriteOnly));
  probe_file.close();
  const auto [text, error] = result(call(
      "open_file", {{"path", probe_file.fileName().toStdString()}, {"ask_user", true}}, killOnceShown<PJ::Dialog>()));
  ASSERT_NE(kill_, nullptr) << "the agent's dialog never showed";
  EXPECT_EQ(state_under_dialog_, "waiting_for_user");
  EXPECT_TRUE(error);
  EXPECT_NE(text.find("cancelled"), std::string::npos) << text;
  EXPECT_EQ(killAnswer().value("state", ""), "killed");
  EXPECT_TRUE(visible<PJ::Dialog>().empty()) << "the dialog outlived its job";
  EXPECT_TRUE(window_->appSession().catalogModel().datasets().empty());
}

TEST_F(McpStreamAndDialogJobTest, ALoadAfterAKillAtTheLoaderDialogStillLoads) {
  QFile probe_file(data_.filePath("x.dlgprobe"));
  ASSERT_TRUE(probe_file.open(QIODevice::WriteOnly));
  probe_file.close();
  ASSERT_TRUE(result(call(
                         "open_file", {{"path", probe_file.fileName().toStdString()}, {"ask_user", true}},
                         killOnceShown<PJ::Dialog>()))
                  .second);
  ASSERT_NE(kill_, nullptr);
  EXPECT_EQ(killAnswer().value("state", ""), "killed");
  EXPECT_NE(loadMock(), 0) << "the stop of one load cancelled the next";
}

TEST_F(McpStreamAndDialogJobTest, ClosingPublicAccessClosesTheDialogsItsAgentsOpened) {
  // The stream's dialog, then the unload prompt: revoked while up, never touched by the test.
  const int dataset = loadWithDependentSeries();
  ASSERT_NE(dataset, 0);
  for (const auto& [tool, args] : std::vector<std::pair<std::string, json>>{
           {"start_stream", {{"source", "Mock Layout Stream"}, {"ask_user", true}}},
           {"unload_file", {{"dataset", dataset}}}}) {
    if (!server()->publicAccess()) {
      ASSERT_NO_FATAL_FAILURE(openPublic());
    }
    bool revoked = false;
    const Answer answer = call(tool, args, [&] {
      if (!revoked && (!visible<PJ::Dialog>().empty() || !visible<PJ::MessageBox>().empty())) {
        revoked = true;
        window_->closeMcpPublicAccess();
      }
    });
    EXPECT_TRUE(revoked) << tool << ": the dialog never showed";
    EXPECT_EQ(answer.responses, 0) << tool;
    EXPECT_TRUE(visible<PJ::Dialog>().empty() && visible<PJ::MessageBox>().empty())
        << tool << ": the dialog of a revoked agent stayed up";
  }
  EXPECT_TRUE(loaded(dataset));
  EXPECT_FALSE(window_->streamingSources().hasActiveSession());
}

TEST_F(McpStreamAndDialogJobTest, AStreamAcceptedAfterItsCallWasAnsweredRunsAsThatJob) {
  // wait_s 0: the call gets its job id while the user is still at the dialog.
  const std::unique_ptr<QTcpSocket> starting =
      post("start_stream", {{"source", "Mock Layout Stream"}, {"ask_user", true}, {"wait_s", 0}});
  QByteArray received;
  bool accepted = false;
  QTimer user;
  QObject::connect(&user, &QTimer::timeout, [&] {
    received += starting->readAll();
    if (!accepted && received.contains("job_id") && !visible<PJ::Dialog>().empty()) {
      accepted = true;
      visible<PJ::Dialog>().front()->accept();
    }
  });
  user.start(20);
  ASSERT_TRUE(pumpUntil([&] { return accepted && window_->streamingSources().hasActiveSession(); }));
  user.stop();
  EXPECT_EQ(received.count("HTTP/1.1 "), 1) << "the call was answered twice";
  const json jobs = json::parse(result(call("jobs_list", json::object())).first, nullptr, false);
  ASSERT_EQ(jobs.size(), 1U) << jobs.dump();
  EXPECT_EQ(jobs[0].value("state", ""), "running");
  ASSERT_TRUE(jobs[0]["progress"].contains("dataset_id")) << "the stream's dataset is nowhere to be found";
  EXPECT_TRUE(streaming(jobs[0]["progress"]["dataset_id"]));
  const auto [killed, kill_error] = result(call("job_kill", {{"job_id", jobs[0]["job_id"]}}));
  EXPECT_EQ(json::parse(killed, nullptr, false).value("state", ""), "killed") << killed;
  EXPECT_FALSE(window_->streamingSources().hasActiveSession());
}

TEST_F(McpStreamAndDialogJobTest, JobKillClosesTheStreamDialogTheAgentOpened) {
  const auto [text, error] =
      result(call("start_stream", {{"source", "Mock Layout Stream"}, {"ask_user", true}}, killOnceShown<PJ::Dialog>()));
  ASSERT_NE(kill_, nullptr) << "the agent's dialog never showed";
  EXPECT_EQ(state_under_dialog_, "waiting_for_user");
  EXPECT_TRUE(error);
  EXPECT_NE(text.find("cancelled"), std::string::npos) << text;
  EXPECT_EQ(killAnswer().value("state", ""), "killed");
  EXPECT_TRUE(visible<PJ::Dialog>().empty());
  EXPECT_FALSE(window_->streamingSources().hasActiveSession());
  EXPECT_TRUE(window_->appSession().catalogModel().datasets().empty()) << "the abandoned stream left a dataset";
}

TEST_F(McpStreamAndDialogJobTest, JobKillClosesTheUnloadPromptAndUnloadsNothing) {
  const int dataset = loadWithDependentSeries();
  ASSERT_NE(dataset, 0);
  const auto [text, error] = result(call("unload_file", {{"dataset", dataset}}, killOnceShown<PJ::MessageBox>()));
  ASSERT_NE(kill_, nullptr) << "the user was never asked";
  EXPECT_EQ(state_under_dialog_, "waiting_for_user");
  EXPECT_TRUE(error);
  EXPECT_NE(text.find("not unloaded"), std::string::npos) << text;
  EXPECT_EQ(killAnswer().value("state", ""), "killed");
  EXPECT_TRUE(visible<PJ::MessageBox>().empty());
  EXPECT_TRUE(loaded(dataset));
}

TEST_F(McpStreamAndDialogJobTest, AClientThatDisconnectsWhileTheUserIsAskedIsHandledSafely) {
  // Over real HTTP: the prompt's modal loop runs while the request's own socket goes away under it.
  const int dataset = loadWithDependentSeries();
  ASSERT_NE(dataset, 0);
  std::unique_ptr<QTcpSocket> client = post("unload_file", {{"dataset", dataset}});
  bool asked = false;
  QTimer leaver;
  QObject::connect(&leaver, &QTimer::timeout, [&] {
    if (!asked && !visible<PJ::MessageBox>().empty()) {
      asked = true;
      client->abort();
    }
  });
  leaver.start(20);
  ASSERT_TRUE(pumpUntil([&] { return asked && visible<PJ::MessageBox>().empty(); }))
      << "the question stayed up for a client that left";
  EXPECT_TRUE(loaded(dataset));
  EXPECT_FALSE(result(call("get_status", json::object())).second) << "the server no longer answers";
}

TEST_F(McpStreamAndDialogJobTest, ADeleteClickedRightAfterTheCallerHungUpDeletesNothing) {
  const int dataset = loadWithDependentSeries();
  ASSERT_NE(dataset, 0);
  auto client = std::make_unique<QTcpSocket>();
  bool clicked = false;
  QTimer user;
  QObject::connect(&user, &QTimer::timeout, [&] {
    for (PJ::MessageBox* box : visible<PJ::MessageBox>()) {
      for (QAbstractButton* button : box->findChildren<QAbstractButton*>()) {
        if (!clicked && button->text() == u"Delete"_s) {
          clicked = true;
          client.reset();   // the client leaves...
          button->click();  // ...and the user confirms before the server's next tick notices
        }
      }
    }
  });
  user.start(20);
  PJ::McpTestPeer::callTool(
      *server(), QByteArray::fromStdString(token_), client.get(), "unload_file", {{"dataset", dataset}},
      [](const json&) { ADD_FAILURE() << "a client that left was answered"; });
  EXPECT_TRUE(clicked);
  EXPECT_TRUE(loaded(dataset)) << "a client that left still had its dataset deleted";
}

TEST_F(McpStreamAndDialogJobTest, TheUnloadPromptGoesWhenItsCallerHangsUp) {
  const int dataset = loadWithDependentSeries();
  ASSERT_NE(dataset, 0);
  auto client = std::make_unique<QTcpSocket>();
  bool asked = false;
  QTimer leaver;
  QObject::connect(&leaver, &QTimer::timeout, [&] {
    if (!visible<PJ::MessageBox>().empty()) {
      asked = true;
      client.reset();  // the agent's client gives up while the user is being asked
    }
  });
  leaver.start(20);
  // Blocks in the prompt's modal loop until the hang-up withdraws the question.
  PJ::McpTestPeer::callTool(
      *server(), QByteArray::fromStdString(token_), client.get(), "unload_file", {{"dataset", dataset}},
      [](const json&) { ADD_FAILURE() << "a client that left was answered"; });
  EXPECT_TRUE(asked);
  EXPECT_TRUE(visible<PJ::MessageBox>().empty());
  EXPECT_TRUE(loaded(dataset)) << "a client that left still had its dataset deleted";
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
  // Revoking the token kills its jobs: the agent's load leaves the queue before it starts.
  EXPECT_EQ(started_, (std::vector<quint64>{blocker, unrelated}));
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
  EXPECT_EQ(started_, (std::vector<quint64>{blocker, unrelated}));
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

TEST_F(McpQueuedLoadTest, JobKillTakesAnOpenFileOutOfTheQueue) {
  const quint64 blocker = userLoad(u"blocker.mock"_s);
  agentCall("open_file", {{"path", pj_app_test::makeMockFile(data_, "agent.mock").toStdString()}, {"wait_s", 0}});
  const quint64 agent = blocker + 1;
  PJ::McpTestPeer::resumeDeferred(*server());  // the call is past its wait_s: it gets the job id
  ASSERT_EQ(replies_.size(), 1U);
  const json status = textOf(replies_[0]);
  ASSERT_TRUE(status.contains("job_id")) << status.dump();
  EXPECT_TRUE(status["progress"].value("queued", false)) << status.dump();

  agentCall("job_kill", {{"job_id", status["job_id"]}});
  ASSERT_TRUE(pumpUntil([&] { return replies_.size() == 2 && terminals_.count(blocker) != 0; }));
  EXPECT_EQ(textOf(replies_[1]).value("state", ""), "killed") << replies_[1].dump();
  EXPECT_EQ(terminals_[agent], Terminals{PJ::LoadOutcome::kCancelled});
  EXPECT_EQ(started_, std::vector<quint64>{blocker}) << "the killed load started";
  EXPECT_EQ(loaded_, std::vector<std::string>{"blocker.mock"});
}

/// Released by the test (or after 30 s) to let GateSource's import end.
std::atomic<bool> g_gate_released{false};

/// A file import held active until the test releases it: it writes one row, then polls the host's
/// progress callback, which is how a real import notices a stop, until it is released or stopped.
class GateSource final : public PJ::DataSourcePluginBase {
 public:
  [[nodiscard]] uint64_t capabilities() const override {
    return PJ::kCapabilityFiniteImport | PJ::kCapabilityDirectIngest;
  }
  [[nodiscard]] std::string saveConfig() const override {
    return "{}";
  }
  PJ::Status loadConfig(std::string_view) override {
    return PJ::okStatus();
  }
  PJ::Status start() override {
    const auto fail = [this](std::string reason) -> PJ::Status {
      runtimeHost().progressFinish();
      state_ = PJ::DataSourceState::kFailed;
      runtimeHost().notifyState(state_);
      return PJ::unexpected(std::move(reason));
    };
    state_ = PJ::DataSourceState::kStarting;
    runtimeHost().notifyState(state_);
    if (auto status = runtimeHost().progressStart("gate", 2, true); !status) {
      return fail(status.error());
    }
    auto topic = writeHost().ensureTopic("gate/value");
    if (!topic) {
      return fail(topic.error());
    }
    const auto append = [&](int64_t t, double v) {
      return writeHost().appendRecord(*topic, PJ::Timestamp{t}, {{.name = "value", .value = v}});
    };
    if (auto status = append(100, 1.0); !status) {
      return fail(status.error());
    }
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!g_gate_released) {
      if (!runtimeHost().progressUpdate(1)) {
        return fail("cancelled via progress");
      }
      if (std::chrono::steady_clock::now() > give_up) {
        return fail("the test never released the gate");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (auto status = append(200, 2.0); !status) {
      return fail(status.error());
    }
    runtimeHost().progressFinish();
    state_ = PJ::DataSourceState::kStopped;
    runtimeHost().notifyState(state_);
    runtimeHost().requestStop(PJ::DataSourceState::kStopped, "import complete");
    return PJ::okStatus();
  }
  void stop() override {
    state_ = PJ::DataSourceState::kStopped;
  }
  [[nodiscard]] PJ::DataSourceState currentState() const override {
    return state_;
  }

 private:
  PJ::DataSourceState state_ = PJ::DataSourceState::kIdle;
};

const PJ_data_source_vtable_t* gateSourceVtable() {
  // registerStaticDataSource keeps this pointer for the catalog's lifetime.
  static const PJ_data_source_vtable_t* vtable = PJ::DataSourcePluginBase::vtableWithCreate(
      []() noexcept -> void* {
        try {
          return new GateSource();
        } catch (...) {
          return nullptr;
        }
      },
      R"({"id":"gate-source","name":"Gate Source","version":"1.0.0","file_extensions":[".gate"]})");
  return vtable;
}

/// An agent's import held active by GateSource, then killed.
class McpActiveLoadTest : public McpQueuedLoadTest {
 protected:
  void SetUp() override {
    McpQueuedLoadTest::SetUp();
    ASSERT_NE(window_, nullptr);
    g_gate_released = false;
    ASSERT_TRUE(window_->appSession().extensionCatalog().pluginCatalog().registerStaticDataSource(gateSourceVtable()));
    QObject::connect(
        &window_->fileLoader(), &PJ::FileLoader::loadProgress, window_.get(),
        [this](quint64, std::uint64_t current, std::uint64_t) { progressed_ = progressed_ || current >= 1; });
  }
  void TearDown() override {
    g_gate_released = true;
    McpQueuedLoadTest::TearDown();
  }

  /// Starts an agent import of a .gate file in the background and returns its job id once it has
  /// ingested a row.
  json startHeldImport() {
    QFile file(data_.filePath(u"held.gate"_s));
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("gate");
    file.close();
    agentCall("open_file", {{"path", file.fileName().toStdString()}, {"wait_s", 0}});
    EXPECT_TRUE(pumpUntil([&] { return progressed_ && !replies_.empty(); }));
    if (replies_.empty()) {
      return {};
    }
    const json status = textOf(replies_[0]);
    EXPECT_EQ(status.value("state", ""), "running") << status.dump();
    return status.value("job_id", json());
  }

  bool progressed_ = false;
};

TEST_F(McpActiveLoadTest, JobKillStopsAnActiveImportKeepingItsRows) {
  const json id = startHeldImport();
  ASSERT_TRUE(id.is_number_integer());
  const json listed = [&] {
    agentCall("jobs_list", json::object());
    return textOf(replies_.back());
  }();
  ASSERT_EQ(listed.size(), 1U) << listed.dump();
  EXPECT_GE(listed[0]["progress"].value("done", 0), 1) << listed.dump();

  agentCall("job_kill", {{"job_id", id}});
  ASSERT_TRUE(pumpUntil([&] { return replies_.size() == 3; }));
  const json killed = textOf(replies_[2]);
  EXPECT_EQ(killed.value("state", ""), "killed") << killed.dump();
  EXPECT_NE(killed.value("result", "").find("stay as dataset"), std::string::npos) << killed.dump();
  ASSERT_EQ(engineDatasets().size(), 1U);
  EXPECT_EQ(
      pj_app_test::singleTopicValues(window_->appSession().sessionManager().createReader(), engineDatasets().front()),
      std::vector<double>{1.0});
}

TEST_F(McpActiveLoadTest, JobKillWithoutKeepPartialRemovesTheImportsRows) {
  const json id = startHeldImport();
  ASSERT_TRUE(id.is_number_integer());
  agentCall("job_kill", {{"job_id", id}, {"keep_partial", false}});
  ASSERT_TRUE(pumpUntil([&] { return replies_.size() == 2; }));
  EXPECT_EQ(textOf(replies_[1]).value("state", ""), "killed") << replies_[1].dump();
  EXPECT_TRUE(engineDatasets().empty()) << "the discarded import's dataset is still loaded";
}

// Half of a replacement is never what a stop means: whatever stops a reload (here a job_kill left at
// its keep_partial default, as a client's cancel or a revoked token would) restores the data it replaced.
TEST_F(McpActiveLoadTest, AStoppedReloadRestoresTheDataItWasReplacing) {
  ASSERT_TRUE(startHeldImport().is_number_integer());
  g_gate_released = true;
  ASSERT_TRUE(pumpUntil([&] { return !terminals_.empty(); }));
  ASSERT_EQ(engineDatasets().size(), 1U);
  const PJ::DatasetId dataset = engineDatasets().front();
  const auto values = [&] {
    return pj_app_test::singleTopicValues(window_->appSession().sessionManager().createReader(), dataset);
  };
  ASSERT_EQ(values(), (std::vector<double>{1.0, 2.0}));

  g_gate_released = false;
  progressed_ = false;
  agentCall("reload_file", {{"dataset", static_cast<int>(dataset)}, {"wait_s", 0}});
  ASSERT_TRUE(pumpUntil([&] { return progressed_ && replies_.size() == 2; }));
  const json reload = textOf(replies_[1]);
  ASSERT_EQ(reload.value("state", ""), "running") << reload.dump();

  agentCall("job_kill", {{"job_id", reload["job_id"]}});
  ASSERT_TRUE(pumpUntil([&] { return replies_.size() == 3; }));
  const json killed = textOf(replies_[2]);
  EXPECT_EQ(killed.value("state", ""), "killed") << killed.dump();
  EXPECT_NE(killed.value("result", "").find("keeps the data it had"), std::string::npos) << killed.dump();
  EXPECT_EQ(values(), (std::vector<double>{1.0, 2.0})) << "the stopped reload left its partial rows in place";
}

TEST_F(McpActiveLoadTest, ClosingPublicAccessStopsAnActiveImport) {
  const json id = startHeldImport();
  ASSERT_TRUE(id.is_number_integer());
  window_->closeMcpPublicAccess();
  ASSERT_TRUE(pumpUntil([&] { return !terminals_.empty(); }));
  EXPECT_EQ(terminals_.begin()->second, Terminals{PJ::LoadOutcome::kCancelled});
  static_cast<void>(pumpUntil([] { return false; }, 100));
  EXPECT_EQ(PJ::McpTestPeer::jobs(*server()), 0U) << "a revoked token's job was kept";
  EXPECT_EQ(replies_.size(), 1U) << "the revoked client was handed something after its job id";
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

/// A page whose relay has relayed a call that is still waiting.
class McpRelayLossTest : public McpServerTest {
 protected:
  /// Links the window to `relay` as page `id` and has it relay a call that waits for a threshold
  /// the mock data never reaches; returns once that call's job runs.
  void relayAWaitingCall(FakeRelay& relay, const char* id) {
    ASSERT_NE(loadMock(), 0);
    ASSERT_TRUE(window_->connectMcpRelay(relay.url()));
    FakeRelay::answer(relay.next("/link/hello"), 200, {{"id", id}});
    const json waiting = toolsCall(
        "wait_for", {{"series", "mock/file_data/value"},
                     {"comparison", ">"},
                     {"threshold", 1e9},
                     {"timeout_s", 600},
                     {"wait_s", 60}});
    FakeRelay::answer(relay.next("/link/next"), 200, {{"seq", 1}, {"client", "claude-code"}, {"message", waiting}});
    ASSERT_TRUE(pumpUntil([&] { return PJ::McpTestPeer::jobs(*server()) == 1; })) << "the relayed call never started";
  }
};

// One relay serves one agent session. A relay that forgot this page restarted: the calls its old
// session relayed can never be answered, so what waited on them stops, as when a desktop client
// hangs up.
TEST_F(McpRelayLossTest, ARestartedRelayEndsTheCallsItsOldSessionRelayed) {
  FakeRelay relay;
  ASSERT_NO_FATAL_FAILURE(relayAWaitingCall(relay, "old"));

  FakeRelay::answer(relay.next("/link/next"), 404);
  FakeRelay::answer(relay.next("/link/hello"), 200, {{"id", "new"}});
  EXPECT_TRUE(pumpUntil([&] { return PJ::McpTestPeer::jobs(*server()) == 0; }))
      << "the old session's call went on with nobody to answer";
}

// One failed poll may be a hiccup: the relay answers the next one and still remembers this page,
// so the calls it relayed go on.
TEST_F(McpRelayLossTest, AFailedPollAloneKeepsTheSession) {
  FakeRelay relay;
  ASSERT_NO_FATAL_FAILURE(relayAWaitingCall(relay, "tab"));
  // An error answer, not a dropped connection: Qt sends a GET again by itself when its connection
  // closes before any answer, and the page never hears of that.
  FakeRelay::answer(relay.next("/link/next"), 500);

  const FakeRelay::Request retried = relay.next("/link/next");
  EXPECT_EQ(retried.http.path, "/link/next?id=tab") << "the page introduced itself again";
  FakeRelay::answer(retried, 204);
  static_cast<void>(relay.next("/link/next"));  // the 204 was taken in: the page polls on
  EXPECT_EQ(PJ::McpTestPeer::jobs(*server()), 1U) << "one failed poll ended the session";
}

// A relay that stops answering is an agent session that ended.
TEST_F(McpRelayLossTest, ARelayThatWentAwayEndsTheCallsItRelayed) {
  auto relay = std::make_unique<FakeRelay>();
  ASSERT_NO_FATAL_FAILURE(relayAWaitingCall(*relay, "tab"));
  static_cast<void>(relay->next("/link/next"));  // the poll in flight dies with the relay

  relay.reset();
  EXPECT_TRUE(pumpUntil([&] { return PJ::McpTestPeer::jobs(*server()) == 0; }))
      << "the call went on after its relay was gone";
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
  // The agent tools, and the job tools that stop or wait on the plugin's own long calls.
  const std::set<std::string> job_tools = {"jobs_list", "job_wait", "job_kill"};
  ASSERT_EQ(tools.size(), PJ::mcp::agentTools().toMcpToolsList().size() + job_tools.size()) << listed.body.dump();
  for (const json& tool : tools) {
    const std::string name = tool.value("name", "");
    EXPECT_TRUE(PJ::mcp::agentTools().find(name) != nullptr || job_tools.contains(name)) << name;
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

TEST_F(McpEndpointTest, APluginSeesAndStopsOnlyItsOwnJobs) {
  ASSERT_NO_FATAL_FAILURE(openPublic());
  const Plugin a = launch(u"toolbox-mcp-a"_s);
  const Plugin b = launch(u"toolbox-mcp-b"_s);
  ASSERT_FALSE(a.token.empty());
  ASSERT_FALSE(b.token.empty());
  QTcpSocket client;
  int resumes = 0;
  PJ::McpTestPeer::deferForever(*server(), QByteArray::fromStdString(a.token), &client, resumes);
  const json mine = json::parse(result(callAs(a.token, "jobs_list", json::object())).first, nullptr, false);
  ASSERT_EQ(mine.size(), 1U) << mine.dump();
  const json id = mine[0]["job_id"];
  for (const std::string& other : {b.token, token_}) {
    EXPECT_EQ(result(callAs(other, "jobs_list", json::object())).first, "[]");
    for (const char* tool : {"job_wait", "job_kill"}) {
      const auto [text, error] = result(callAs(other, tool, {{"job_id", id}}));
      EXPECT_TRUE(error) << tool;
      EXPECT_NE(text.find("no such job"), std::string::npos) << text;
    }
    // Nor does another client's cancellation reach it.
    EXPECT_EQ(
        rpcAs(other, {{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", 1}}}})
            .status,
        202);
  }
  EXPECT_EQ(PJ::McpTestPeer::deferred(*server()), 1U) << "another client stopped the plugin's job";
}

TEST_F(McpEndpointTest, AnEndpointWhoseServerIsGoneReleasesSafely) {
  const Plugin plugin = launch(u"toolbox-mcp-a"_s);
  ASSERT_FALSE(plugin.token.empty());
  PJ::McpTestPeer::destroyServer(*window_);
  release(*plugin.session);
  release(*plugin.session);
}

}  // namespace
