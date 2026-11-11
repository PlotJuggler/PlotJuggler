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
/// - `POST R/link/hello {"title", "protocol": 1}` → `{"id"}`: this page is one relay instance.
/// - `GET R/link/next?id=` → `{"seq", "client", "message"}` (one JSON-RPC message from the
///   agent), or 204 when none came within the relay's poll window; 404 when the relay forgot the
///   id (it restarted): hello again.
/// - `POST R/link/reply?id= {"seq", "response"}`: the answer to message `seq` (null for a
///   notification), sent whenever the server answers, possibly much later (an import, a dialog).
///
/// Every request carries `X-PJ-Link: 1`: a custom header makes a browser ask the relay first (CORS
/// preflight), and the relay admits only its allowlisted origins (the hosted app, local servers).
/// Messages are answered as a public client (McpServer::handleRelayed); one poll is in flight at a
/// time, answers go out as they come. A relay that is down or restarting is retried, backing off to
/// 10 s, for as long as this lives. GUI thread.
class McpBrowserLink : public QObject {
  Q_OBJECT
 public:
  /// Starts at once. `relay` must pass acceptableRelay; `title` names this page in the agent's
  /// list_instances.
  McpBrowserLink(McpServer& server, QUrl relay, QString title, QObject* parent = nullptr);

  /// Whether a page may talk to `url` as its relay: plain http on a loopback host with a port. The
  /// relay runs on the user's own machine; anything else would hand a remote party the session.
  [[nodiscard]] static bool acceptableRelay(const QUrl& url);

  /// The id the relay gave this page; empty until the first hello succeeded.
  [[nodiscard]] QString id() const {
    return id_;
  }

 private:
  void hello();
  void poll();
  /// Answers message `seq` with `response` (empty: a notification, no answer).
  void reply(qint64 seq, const QByteArray& response);
  /// Runs `next` after the current backoff, then doubles it (up to 10 s).
  void retryLater(void (McpBrowserLink::*next)());
  [[nodiscard]] QUrl endpoint(const QString& path) const;

  McpServer& server_;
  QUrl relay_;
  QString title_;
  // ponytail: in the browser a QNetworkReply never frees its fetch buffer (docs/TECHNICAL_NOTES.md);
  // relay messages are tool calls of a few hundred bytes, so a session leaks kilobytes. Move to
  // BrowserFetch-style page fetches if a session ever relays megabytes.
  QNetworkAccessManager network_;
  QString id_;
  int backoff_ms_ = 0;
};

}  // namespace PJ
