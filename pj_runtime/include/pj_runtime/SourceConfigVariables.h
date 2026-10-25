#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

/**
 * @file SourceConfigVariables.h
 * @brief Deployment variables a saved source configuration may reference.
 *
 * A layout saved for a deployment cannot know the address it will be served from, so a source
 * configuration may name it through a variable that is expanded just before the source starts.
 * The saved layout always keeps the template; the expansion is never persisted.
 */

#include <QString>

class QUrl;

namespace PJ {

/// Expands `${deployment.ws_origin}` inside the JSON string values of `json`: the WebSocket origin of
/// `page_url` (`http` becomes `ws`, `https` becomes `wss`; host, bracketed IPv6 and port kept; no user
/// info, path or trailing slash). Keys are never expanded, and neither is text outside a JSON string:
/// such a configuration is not JSON and is refused. `${deployment.` is a reserved namespace: any other
/// name in it, or a token without its `}`, is an error naming the token (checked before the deployment
/// context, so a typo is reported as a typo); any other `${...}` is the plugin's own and stays as is.
/// A configuration that uses no deployment variable comes back unchanged, byte for byte, so an explicit
/// `ws://` URL works on every platform. One that does is re-serialized compactly, which also sorts its
/// keys and does not preserve integers outside qint64 or float formatting. Returns an empty string and
/// describes the problem in `*error` (may be null) on any error, including a variable used while
/// `page_url` is not http(s), which covers the desktop's empty URL. Pure function; any thread.
[[nodiscard]] QString expandSourceConfigVariables(const QString& json, const QUrl& page_url, QString* error);

}  // namespace PJ
