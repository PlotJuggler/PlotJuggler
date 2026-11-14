#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>

namespace PJ {

class McpServer;

/// Connects this window's MCP server to an agent through a relay on the user's machine (the
/// plotjuggler-agent plugin's `pj_mcp_relay.py`, which speaks MCP over stdio to the agent). The
/// browser build can neither listen nor be spawned, so the page connects OUT and long-polls; the
/// relay never reaches in. Plain QNetworkAccessManager, so desktop runs it too (`--mcp-relay`,
/// tests). Protocol (relay base URL `R`, JSON bodies, docs/MCP_SERVER.md "Browser relay"):
///
/// - `POST R/link/hello {"title", "protocol": 1, "browser"}` → `{"id"}`: this page is one relay
///   instance; the id then names it on every request.
/// - `GET R/link/next?id=` → `{"seq", "client", "message"}` (one JSON-RPC message from the
///   agent), or 204 when none came within the relay's poll window; 404 when the relay forgot the
///   id (it restarted): hello again.
/// - `POST R/link/reply?id= {"seq", "response"}`: the answer to message `seq` (null for a
///   notification), sent whenever the server answers, possibly much later (an import, a dialog).
///
/// Every request carries `X-PJ-Link: <key>`, the secret the relay put in its own URL (`?key=`, which
/// only the agent's launch hands out): without it nothing joins, so neither another local process
/// nor a website can pose as this page. A custom header also makes a browser ask the relay first
/// (CORS preflight), which admits only its allowlisted origins.
/// Messages are answered as a public client (McpServer::handleRelayed); one poll is in flight at a
/// time, answers go out as they come. A relay that is down or restarting is retried, backing off to
/// 10 s, for as long as this lives. GUI thread.
class McpBrowserLink : public QObject {
  Q_OBJECT
 public:
  /// Starts at once. `relay` must pass acceptableRelay; `title` names this page in the agent's
  /// list_instances.
  McpBrowserLink(McpServer& server, QUrl relay, QString title, QObject* parent = nullptr);

  /// Whether a page may talk to `url` as its relay: plain http on a loopback host with a port (its
  /// `key` query item is the relay's secret). The relay runs on the user's own machine; anything
  /// else would hand a remote party the session.
  [[nodiscard]] static bool acceptableRelay(const QUrl& url);

  /// The id the relay gave this page; empty until the first hello succeeded.
  [[nodiscard]] QString id() const {
    return id_;
  }

 private:
  void hello();
  void poll();
  /// Hangs up every call the current relay session relayed, and starts the next session.
  void endSession();
  /// Answers message `seq` with `response` (empty: a notification, no answer).
  void reply(qint64 seq, const QByteArray& response);
  /// Runs `next` after the current backoff, then doubles it (up to 10 s).
  void retryLater(void (McpBrowserLink::*next)());
  /// `path` on the relay, naming this page once it has an id; never the relay URL's own query.
  [[nodiscard]] QUrl endpoint(const QString& path) const;
  [[nodiscard]] QNetworkRequest request(const QUrl& url, int timeout_ms) const;

  McpServer& server_;
  QUrl relay_;
  /// The relay's secret (its URL's `key`), sent on every request.
  QByteArray key_;
  QString title_;
  // ponytail: in the browser a QNetworkReply never frees its fetch buffer (docs/TECHNICAL_NOTES.md);
  // relay messages are tool calls of a few hundred bytes, so a session leaks kilobytes. Move to
  // BrowserFetch-style page fetches if a session ever relays megabytes.
  QNetworkAccessManager network_;
  QString id_;
  int backoff_ms_ = 0;
  /// The connection of the calls the current relay session relayed (McpServer::handleRelayed). One
  /// relay serves one agent session, so a relay that forgot this page or stopped answering has no
  /// way left to deliver their answers: deleting this is that session's hang-up, as a closed socket
  /// is a desktop client's.
  QObject* session_ = new QObject(this);
  /// Polls that failed in a row.
  int failed_polls_ = 0;
};

}  // namespace PJ
