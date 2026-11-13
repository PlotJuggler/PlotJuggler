# AI agents: the MCP server

PlotJuggler lets AI agents read and drive a running instance over the
[Model Context Protocol](https://modelcontextprotocol.io). Two kinds of client use the same in-app server:

- **External agents** (Claude Code, Codex, any MCP client) reach it through a stdio bridge. This is on by default,
  and the user can turn it off.
- **Toolbox plugins** (the Assistant) lease a private endpoint through the SDK service `pj.mcp_endpoint.v1`. The
  plugin then points its own agent CLI at that endpoint. There is no opt-in and no discovery file.
- **Agents of a browser page** (the WebAssembly build) reach it through a relay the page connects out to (see
  [Browser relay](#browser-relay)): a page can neither listen on a socket nor be spawned. The browser build has
  no stdio bridge and no plugin endpoint.

This document is the design as built. Code-level detail lives in the header doc-comments it names.

## Picture

```
external agent ──stdio──> plotjuggler4 --mcp-bridge ──HTTP 127.0.0.1:<port>, Bearer <public token>──┐
                          (one per agent session)       reads <AppLocalData>/mcp-instances/<pid>.json │
                                                                                                     ▼
                                                                                     McpServer (GUI thread)
                                                                                                     ▲
Assistant plugin ──spawns──> claude / codex CLI ──HTTP 127.0.0.1:<port>, Bearer <plugin token>───────┘
      │
      └── pj.mcp_endpoint.v1 acquire() ──> {url, bearer_token}   (McpEndpointHost, one per toolbox session)

browser page:
external agent ──stdio──> pj_mcp_relay.py (127.0.0.1:<port>) <──long-poll HTTP── McpBrowserLink ──> McpServer
                          (plotjuggler-agent plugin)            (the page connects out)            (handleRelayed)
```

## Components

| Piece | Where | What it does |
|---|---|---|
| `McpProtocol` | `pj_app/src/mcp/` (QtCore only) | Vocabulary both ends share: JSON-RPC envelope checks (`parseRpcMessage`), lifecycle replies, the tool lists (`ToolSet`), the HTTP/1.1 subset, the discovery folder, `ownerFor`, `checkAccess` |
| `McpBridge.cpp` | `pj_app/src/mcp/` | `plotjuggler4 --mcp-bridge`, handled in `main()` before any `QApplication`: stdio ↔ HTTP, instance discovery, `list_instances` / `select_instance` |
| `McpServer` | `pj_app/src/mcp/` | The loopback endpoint on the GUI thread, the token table, deferred results, revocation. The HTTP is hand-written because QtHttpServer is GPL-3-only. Its transport-free half (`dispatch`) also answers what a relay link brings in (`handleRelayed`); in the browser only that half is built |
| `McpBrowserLink` | `pj_app/src/mcp/` | The page side of the browser relay: long-polls the relay and feeds each message to `handleRelayed`. Plain `QNetworkAccessManager`, so the desktop runs it too (`--mcp-relay`, tests) |
| `pj_mcp_relay.py` | `plotjuggler-agent/relay/` | The agent side of the browser relay: MCP over stdio, the page protocol over loopback HTTP, standard-library Python |
| `McpServerTools.cpp` | `pj_app/src/mcp/` | The app tools, each a thin call into an existing service or the public "agent control" block of `MainWindow.h` |
| `McpEndpointHost` | `pj_app/src/mcp/McpServer.h` | The host side of `pj.mcp_endpoint.v1`, one per toolbox session |
| `pj_agent_tools` | `pj_runtime/{include,src}/agent_tools/` (static lib, MIT) | The agent tools (`ToolRegistry`) over a `ToolContext` of SDK host views; no Qt Widgets, no app types |
| `pj.mcp_endpoint.v1` | `plotjuggler_sdk` `pj_base/include/pj_base/plugin_data_api.h`, C++ view in `sdk/plugin_data_api.hpp` | The C-ABI service a toolbox plugin acquires an endpoint from (since SDK 0.40.0) |
| The Assistant | `pj-official-plugins/toolbox_assistant_agent` | The first plugin consumer: chat panel, Claude and Codex backends, catalog digest |

## Tool sets

The tool set depends on which token a request carries (`mcp::ToolSet`):

- **Agent tools** (`pj_agent_tools`, served to every token): `list_topics`, `describe_topic`, `read_series`,
  `evaluate`, `create_derived_series`, `create_derived_object`, `create_markers`, `remove_markers`, `list_created`,
  `remove_derived_series`, `playback`, `plot_tab`, `scene_view`, `report_status`.
- **App tools** (`McpServerTools.cpp`, public token only): `get_status`, `about`, `open_file`, `unload_file`,
  `reload_file`, `start_stream`, `stop_stream`, `export_series`, `add_script_series`, `remove_script_series`,
  `add_filter`, `screenshot`.
- **Bridge tools** (answered by the bridge itself): `list_instances`, `select_instance`.

`ToolSet::kFull` serves both lists. `ToolSet::kAgentOnly` is used for plugin tokens: `tools/list` and the
`initialize` instructions leave the app tools out, and calling one gets the unknown-tool error. `isReadOnlyTool` names the tools that
may run while a modal dialog is open.

The instructions (`initialize` and `server/discover` both carry them) end with what the caller's token finds loaded
(`McpServer::loadedSummary`): each dataset by its source name (the tools' `dataset:` qualifier) with its display time
range, its topics and their numeric fields (`isSignalField`, as `read_series` counts them), read from one catalog
snapshot and capped at 60 topics, 40 fields each and about 6 kB. Names are kept to one line, since they reach the
agent's system prompt, and a source name two datasets share is flagged (a `dataset:` prefix cannot pick either).
Claude Code puts the instructions in its system prompt, so an agent goes straight to the tool that answers instead of
spending a turn on each discovery call: half the turns and 26-32% less time on lookup tasks. It is a picture
of when the session connected; `list_topics` and `get_status` show later loads. The bridge asks the current instance
for its instructions on both calls, and keeps its own when no instance answers: Claude Code 2.1 sends only
`server/discover`.

The agent tools run against a `ToolContext` built from a service registry. That is the same set of SDK host views a
toolbox plugin gets (`ToolboxHostView`, `PlotTabsHostView`, `DataProcessorHostView`, `PlaybackHostView`, ...). The
registry decides whose tabs and derived series the tools see and create. Series names follow `read_series` everywhere
(`resolveSeriesPath`: unique suffixes, `dataset:topic/field`, near-miss candidates); a transform input whose bare path
several datasets share reaches the host dataset-qualified. An agent's removal (`remove_derived_series`,
`remove_script_series`) goes through `DataProcessorService::removeOwnTransform`: when the cascade would also delete a
series another owner built on it (the user's included), it is refused, naming each one and its owner. The user's own
deletes ask first and keep cascading. `plot_tab`'s zoom frames only the tab it names: `pj.viewport.v1` frames all of a
caller's tabs at once, so the server sets the context's `zoom_tab` hook (`PluginPlotTabsController` with the caller's
owner, which a plugin token records at `addPluginToken`). Windowed reads never build the whole series' Arrow array:
the server sets the context's `read_window` and `scan_values` hooks over the session's `DataEngine`
(`readSeriesArrowWindow`, `scanSeriesValues`, `pj_datastore/plugin_data_host.hpp`). `export_series` then decodes the
window only; `read_series` also scans the whole series' values once, for its whole-series flat facts, which is still
linear in the series but several times cheaper than the full read. `export_series` keeps time as integer display
nanoseconds, so its CSV time column is exact; `format: "npy"` writes a numpy structured array (`time` and one float64
field per series path, NaN before a series starts) that `numpy.load` reads ~30-100x faster than `numpy.loadtxt` reads
the CSV, with no package beyond numpy. `add_filter` takes its class from `pj_scripting`'s
`FilterCatalogue::standaloneClassScript`. `screenshot` composites each visible QRhi view's next presented frame
(`grabPresentedFrame`) into the window grab, and replies when the frames arrive.

## Tokens and identity

`McpServer` holds at most one public token and a table of plugin tokens (`plugin_tokens_`, token → that session's
service registry). Every request's bearer is compared against each token in constant time (`mcp::checkAccess`). The token that
matches decides the tool set, the owner and where the `ToolContext` comes from.

| | Public token | Plugin token |
|---|---|---|
| Minted by | `openPublicAccess` (Preferences toggle or `--mcp`) | `McpEndpointHost::acquire` → `addPluginToken` |
| Advertised | In the discovery file | Never; only the acquiring plugin gets it |
| Tool set | `kFull` | `kAgentOnly` |
| Acts as | `agent-<client>` per request (`mcp::ownerFor`) | The plugin id its session was built for, whatever the client claims |
| Services | One assembly per owner (`MainWindow::buildAgentSession`, in `agent_sessions_`) | The plugin's own `ToolboxPanelSession` registry, so its existing tabs and series stay reachable |
| Status bar | Shows the calling agent's name | Nothing (the plugin has its own UI) |

**Public owners.** `mcp::ownerFor` builds the owner from the bridge's `X-PJ-Client` header or the request's `_meta`
client name: `agent-` plus that name reduced to `[a-z0-9-]`, at most 32 characters. A request can never name a
plugin's id, and a plugin can never take an agent's: the plugin catalog refuses ids starting with `agent-`
(`PJ::kAgentIdPrefix`). Two sessions of one client share a namespace. On first use the server registers the owner
with `ExtensionCatalogService::registerAdHocOwner`, so its tabs' watermark and its rows read "AI agent (<owner>)".
Agents draw only in their own tabs. Their derived series are history-exempt plugin transforms under Custom Topics,
with an injective transform id (bytes other than `[A-Za-z0-9]` escaped).

## Lifetimes and revocation

- **Server.** `MainWindow` keeps one server from its first need until `~MainWindow` (`MainWindow::ensureMcpServer`,
  random free port). The first need is either public access opening or a plugin's `acquire`. With no token left the
  server keeps listening and refuses everything (401). `~MainWindow` destroys it right after `teardownAllToolboxes`:
  each toolbox session revoked its token as it died, and no plugin is left to start the server again. This happens
  before the services the agent sessions point into are destroyed.
- **Endpoint.** `acquire` returns the endpoint already held until `release`; after a `release`, the next `acquire`
  mints a new token. `McpEndpointHost` holds the server by `QPointer`, so a `release` after the server is gone is a
  no-op, and a later `acquire` either opens an endpoint on a new server or fails with `PJ_ERROR_CODE_INTERNAL`. A
  run of failed starts is reported to the diagnostics once, not once per `acquire`. `acquire` builds everything that
  can throw before it mints the token, so a failed call holds none.
- **Revocation is synchronous.** It runs on `release`, on the plugin's session dying (`McpServer::revoke`), and on
  `closePublicAccess` for the public token. When it returns:
  - the token is forgotten;
  - its deferred results are dropped, each releasing into services that are still alive;
  - its open sockets are aborted, so a held reply then writes nothing.

  `~ToolboxPanelSession` resets `mcp_endpoint` right after `handle` (the plugin may call `release` from its
  destructor) and before every service the token reaches. Closing public access drops `agent_sessions_` after
  revoking.
- **Revocation from inside a result.** A reply or continuation can itself revoke a token, for example by tearing down
  another plugin's session. `resumeDeferred` therefore takes queued results out one at a time, so the revocation
  still finds and drops the others before their services die. Each result carries the round that queued it, so a
  round never resumes its own follow-ups. The token is checked before a result runs and before its follow-up is
  queued, and each continuation is destroyed right after it ran. The services of the result that is running stay
  alive because agent tools never spin a nested event loop (`agent_tools::ToolResult::resume`). Should a reply spin
  one anyway, the nested `resumeDeferred` returns at once (`draining_`), leaving the queue to the outer loop: a nested
  call would move the round on under it, and the outer loop would then resume the follow-ups it queued again and again.
- **App work left for later** asks `McpServer::stillAuthorized(caller)` before it runs and again once the user has
  answered its dialog. Once the token is gone it applies nothing. This covers:
  - `start_stream`'s queued dialog (`StreamingSourceManager::startSession`'s `still_allowed`; a stream abandoned
    after its dialog leaves no dataset behind);
  - `open_file` and `reload_file` loads waiting in the queue or at their dialog (`LoadHints::still_allowed`, checked
    in `FileLoader::beginLoad`);
  - `unload_file`'s prompt (`confirmAndRemoveDependentTransforms`'s `still_allowed`).
- **Deferred results.** A tool may hold its reply: an import in progress, a dialog the user answers, a screenshot
  frame. Held results are resumed every 50 ms, as the Assistant's GUI tick does. A mutating one waits, and is not
  failed, while the user has a modal dialog open. One whose client disconnected is dropped at its next resume. A tool
  replies exactly once.

## Public access (external agents)

- **On by default.** An attended run opens public access at startup unless the user turned off the Preferences →
  Appearance switch "Let AI agents control PlotJuggler (MCP server)" (`MainWindow::kMcpServerKey`). An unattended
  run (`--exit-after-layout`, `--screenshot`, the offscreen and minimal platforms, so every test) opens it only with
  `--mcp`, so it never shows up as an instance an agent could pick. The switch shows whether public access is open
  now; a plugin's endpoint does not count. OK applies the switch when it differs from the current state: it writes
  the setting and either opens public access with a random token (discovery file written) or closes it (file
  removed; plugin tokens keep working). `--mcp` opens it for one run without touching the setting.
- **Indicator.** While a public client calls, a status-bar label names it. The label hides two minutes after the last
  request, or when public access closes.
- **Discovery.** `<AppLocalData>/mcp-instances/<pid>.json` holds pid, port, token and start time, owner-only like its
  folder. On Windows `setPermissions` only toggles read-only, so privacy there rests on `%LOCALAPPDATA%`'s per-user
  ACL. `AppLocalDataLocation` needs only HOME, because agents start the bridge with a stripped environment. A starting
  server deletes the files of dead pids (`ActiveWidgetsMarker::isProcessAlive`). The bridge deletes a file only when
  its pid is gone or its port refuses; a slow or odd reply leaves it. Attended starts record the executable path
  (`mcp-instances/executable`) for agent plugins that launch PlotJuggler.
- **Bridge.** Its waits are bounded: 2 s connect, 5 s probes, 300 s per tool call. Past the client's own tool
  timeout, a long import carries on and `get_status` shows the dataset once it lands. A connection that closes with no
  reply tells the agent PlotJuggler quit or turned MCP off, and to call `list_instances`. `tools/list` is the live
  instance's list when exactly one instance answers (or one is selected), and the bridge binary's own list otherwise.

## Browser relay

The browser build's server does not listen; the page links to a relay instead. The plotjuggler-agent plugin starts
`relay/pj_mcp_relay.py` when no desktop PlotJuggler is installed: the agent speaks MCP to it over stdio, and it
listens on `127.0.0.1` (first free port of 7381-7390). Its `launch` tool opens the hosted app with
`?mcp=http://127.0.0.1:<port>/?key=<secret>`; `main.cpp` hands that to `MainWindow::connectMcpRelay`, which opens public access and
starts an `McpBrowserLink` (only a plain http, loopback relay with a port: `McpBrowserLink::acceptableRelay`). The
desktop does the same with `--mcp-relay <url>`, the key copied from the relay's cache
(`~/.cache/plotjuggler-agent/relay-key`).

- **Protocol** (relay base `R`, JSON bodies, every request with `X-PJ-Link: <the url's key>`):
  - `POST R/link/hello {"title", "protocol": 1, "browser"}` → `{"id"}`: the page is one relay instance
    (`list_instances`, `select_instance`; ids are strings, unlike the bridge's pids). `browser` says whether its tool
    list is the browser build's, which the relay keeps for its next sessions.
  - `GET R/link/next?id=` → `{"seq", "client", "message"}` or, after 20 s, 204; 404 when the relay forgot the id
    (it restarted), and the page says hello again.
  - `POST R/link/reply?id= {"seq", "response"}`, whenever the server answers (held replies included).
  - `GET R/file/<token>/<name>`: a file `open_file` named. The relay keeps `path` and adds `url` and `name`; the
    browser's `open_file` streams the url into a `BrowserFileStore` file (at most `kMaxPresetDataBytes`) and loads it
    like a picked file, the desktop's uses the path.
- **Who gets in.** Each relayed message is answered as the public client the relay names (its MCP client's name,
  so `agent-<client>`). The relay answers `/link/*` only with its secret, random and kept in the user's cache (mode
  0600, so a tab survives a relay restart: the new run answers 404 and the page says hello again), which `launch` hands
  out in the URL it opens; pages and processes without it cannot join as PlotJuggler and take the agent's calls. The
  secret is as private as that URL: browser history, the browser's command line, the app server's request log (it is
  a query item), and it is only good against the loopback relay. A browser sends the custom `X-PJ-Link` header only after a CORS preflight, which the relay answers only for
  its allowed origins (the hosted app, plus `PJ_RELAY_ORIGINS`, e.g. a local `serve_wasm.mjs`), with
  `Access-Control-Allow-Private-Network` for Chrome's local network access. The page in turn trusts whatever relay
  its `?mcp=` names, so it accepts loopback relays only: a link to `?mcp=` is a request to be driven by that relay,
  as `--mcp-relay` is on the desktop.
- **Tools.** The browser has no `about` (no feature catalog) and no `export_series` (the file would stay in the
  page); `toolList` and the instructions leave them out there. MCP clients read `tools/list` once at startup, before
  any page connects, and do not reliably refresh on `list_changed`, so the relay serves its bundled
  `browser-tools.json` (regenerated from a build, see its README) until a page has connected, then that page's list,
  cached for the next sessions.
- **Lifetime.** The link is a child of the server, deleted with it (in `~QObject`, after the server's members); its
  in-flight replies are deleted without emitting `finished`, so no callback reaches the server then, and replies the
  server still holds check the link through a `QPointer`. A relay that is down is retried, backing off to 10 s, while
  the page lives; the relay forgets a page that has not polled for 120 s. In the browser a
  `QNetworkReply` never frees its body (`pj_runtime/docs/TECHNICAL_NOTES.md`): relay messages are small, so a session leaks
  kilobytes.

## The plugin endpoint (`pj.mcp_endpoint.v1`)

The SDK contract, with the authoritative wording in `plugin_data_api.h`:

```c
typedef struct { PJ_string_view_t url; PJ_string_view_t bearer_token; } PJ_mcp_endpoint_t;
typedef struct PJ_mcp_endpoint_host_vtable_t {
  uint32_t protocol_version; uint32_t struct_size;
  bool (*acquire_endpoint)(void* ctx, PJ_mcp_endpoint_t* out_endpoint, PJ_error_t* out_error);
  void (*release_endpoint)(void* ctx);
} PJ_mcp_endpoint_host_vtable_t;
```

- **Main thread only.** Both strings are host-owned and stay valid until `release_endpoint` or the end of the
  service's lifetime; a plugin copies them. The service itself stays valid until the plugin instance is destroyed,
  so the plugin may release from its destructor.
- **Credentials.** The url and token do not expire, and the host does not revoke them unasked while the plugin
  instance and the server live. If the client is refused, the plugin recovers by calling `release_endpoint` and then
  `acquire_endpoint`, which mints a new token. The token is the endpoint's only credential. It goes to the client the
  plugin launches and nowhere else: no logs, no persisted config.
- **Optional.** The host registers the service in every `buildToolboxPanelSession` (desktop only). A registration
  failure warns and does not fail the session. A plugin asks for it with `McpEndpointHostView` (`valid()` false when
  the host lacks it) and tests against `PJ::sdk::testing::FakeMcpEndpointHost`.
- **Versioning.** The service is additive in SDK 0.40.0 (feature floor group in `pj_base/feature_floors.json`). A
  plugin that needs it declares `min_sdk_required: "0.40.0"`. That field is what the marketplace gates on;
  `min_plotjuggler_version` is display-only.

## The Assistant as a consumer

- **Per-turn acquire.** On every turn, on the GUI thread, `AssistantDialog::sendCurrentInput` acquires the endpoint
  (the same one while held) and hands it to the backend in `TurnTools::mcp`. A failure ends the turn with a chat note,
  "This PlotJuggler cannot serve the Assistant's tools: …", which for a host without the service reads "it offers no
  MCP endpoint to plugins." A PlotJuggler older than SDK 0.40.0 never gets that far: it refuses to install or load the
  plugin (`min_sdk_required`). The plugin never releases; its session teardown revokes the token.
- **Claude backend.** The CLI runs with `--tools ""` (no built-in tool: no shell, no filesystem), `--restricted`
  (none of the user's settings or CLAUDE.md), and `--mcp-config <0600 temp file>` with `--strict-mcp-config` and
  `--allowedTools mcp__pj`. The config file holds the url and the token. It is written through `writePrivateTempFile`
  and reused per endpoint.
- **Codex backend.** The url goes on argv (`mcp_servers.pj.url`). The token goes only through the environment
  (`bearer_token_env_var`, `PJ_ASSISTANT_MCP_TOKEN`), never on the command line.
- **Context.** The catalog digest (`catalog_digest.cpp`) summarises the loaded topics into the system prompt. It is
  rebuilt every turn and re-sent only when its hash changes. The tools themselves all come from the host.

## The user comes first

- While a modal dialog is open, only read-only tools run (and `screenshot` without `tab`); the rest are told to wait.
- `unload_file` with dependent derived series holds its reply until the user answers the prompt (the `cancelled`
  hook of `confirmAndRemoveDependentTransforms`).
- A dialog opened because an agent passed `ask_user` is titled "An AI agent asked you to choose: ...". The prefix
  travels `LoadHints::dialog_title_prefix` → `DataSourceRequest::title_prefix` → `DialogEngineConfig::title_prefix`
  (for streams, `StreamingSourceManager::startSession`).
- `start_stream` with `ask_user` opens the named source (checked against `streamSources()`) once its request has
  returned, and only if no modal dialog came up meanwhile.

## Protocol and security

- **Both protocol eras** on one endpoint and over stdio: `initialize` (2025-06-18 and kin) and the stateless
  2026-07-28 `server/discover`. Every response is plain JSON: no session id, no SSE.
- **Every token kind:**
  - Requests need the bearer token (401 otherwise, compared in constant time), and any `Origin` is refused (403).
  - `mcp::parseRpcMessage` validates the envelope on both ends. Malformed input, or JSON nested deeper than 64 levels,
    gets a JSON-RPC error.
  - Size caps: 16 KiB of headers and a 1 MiB body (also the bridge's stdin line cap).
  - At most 16 sockets; an incomplete request is closed after 10 s.
- **`export_series`** by default writes a new owner-only, randomly named file (`QTemporaryFile`). It refuses more
  than 10 million rows and reports a failed write, flush or close.
- **Logging.** No token is ever logged, and server logging goes to the `pj.mcp` category.
- **Trust boundary.** It is the same user. The bridge is a convenience, not a security layer. With public access on
  by default, any process of that user can read the discovery file and drive the window; the Preferences switch
  closes it. A plugin token is capability-scoped (agent tools only, the plugin's own identity), but any local process
  of that user that learns it can use it until it is revoked.

## Tests

- `pj_app/tests/mcp_server_test.cpp`:
  - `McpServerTest`: protocol, security, owners, deferred results, public access.
  - `McpEndpointTest`: plugin tokens. Covers the forced owner, refused app tools, the `tools/list` profile, no
    discovery file, release and teardown revocation, disconnects, distinct tokens per plugin, and the same endpoint
    on a repeated acquire. Also a session torn down from inside another's resume, and a revoked token's follow-up.
  - Revocation of public access before or under an agent's queued dialog: nothing is applied.
  - `ARelayedCall…`, `ALinkForgotten…`, `OnlyALoopbackHttpRelay…`: the browser link against a fake relay.
- `plotjuggler-agent/tests/test_relay.py`: the relay against a fake page (routing, `open_file` serving, origins).
- `pj_runtime/tests/agent_tools_*` (runner `pj_agent_tools_tests`) for the agent tools, against the `PJ::testing`
  fakes.
- SDK: `pj_base/tests/mcp_endpoint_host_test.cpp` plus the ABI sentinels.
- Assistant: its own suite in `toolbox_assistant_agent/tests` (`tests/support/test_endpoint.hpp`). Live tests against
  a running instance are opt-in.

## Known limits and follow-ups

- Plugin tokens get the agent tools only, so the Assistant cannot open files or start streams. The planned follow-up
  adds `open_file`, `start_stream` and `stop_stream` to the plugin profile, with `ask_user` on by default.
- A plugin token is not bound to the process the plugin spawned (see the trust boundary above).
- No call can be cancelled on request (`notifications/cancelled` is ignored) and the bridge runs one call at a time.
  The plan to turn long calls into jobs an agent can list, wait on and kill is [mcp_jobs_plan.md](./mcp_jobs_plan.md).
