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
| `McpBridge.cpp` | `pj_app/src/mcp/` | `plotjuggler4 --mcp-bridge`, handled in `main()` before any `QApplication`: stdio ↔ HTTP, instance discovery, `list_instances` / `select_instance`, forwarding a cancellation of the call in flight |
| `McpServer` | `pj_app/src/mcp/` | The loopback endpoint on the GUI thread, the token table, the job table, revocation. The HTTP is hand-written because QtHttpServer is GPL-3-only. Its transport-free half (`dispatch`) also answers what a relay link brings in (`handleRelayed`); in the browser only that half is built |
| `McpBrowserLink` | `pj_app/src/mcp/` | The page side of the browser relay: long-polls the relay and feeds each message to `handleRelayed`. Plain `QNetworkAccessManager`, so the desktop runs it too (`--mcp-relay`, tests) |
| `pj_mcp_relay.py` | `plotjuggler-agent/relay/` | The agent side of the browser relay: MCP over stdio, the page protocol over loopback HTTP, standard-library Python |
| `McpServerTools.cpp` | `pj_app/src/mcp/` | The app tools: calls into existing services and the public "agent control" block of `MainWindow.h`, plus `export_series`' file writer, which runs on a worker |
| `McpEndpointHost` | `pj_app/src/mcp/McpServer.h` | The host side of `pj.mcp_endpoint.v1`, one per toolbox session |
| `pj_agent_tools` | `pj_runtime/{include,src}/agent_tools/` (static lib, MIT) | The agent tools (`ToolRegistry`) over a `ToolContext` of SDK host views; no Qt Widgets, no app types |
| `pj.mcp_endpoint.v1` | `plotjuggler_sdk` `pj_base/include/pj_base/plugin_data_api.h`, C++ view in `sdk/plugin_data_api.hpp` | The C-ABI service a toolbox plugin acquires an endpoint from (since SDK 0.40.0) |
| The Assistant | `pj-official-plugins/toolbox_assistant_agent` | The first plugin consumer: chat panel, Claude and Codex backends, catalog digest |

## Tool sets

The tool set depends on which token a request carries (`mcp::ToolSet`):

- **Agent tools** (`pj_agent_tools`, served to every token): `list_topics`, `describe_topic`, `read_series`,
  `wait_for`, `evaluate`, `create_derived_series`, `create_derived_object`, `create_markers`, `remove_markers`, `list_created`,
  `remove_derived_series`, `playback`, `plot_tab`, `scene_view`, `report_status`.
- **App tools** (`McpServerTools.cpp`, public token only): `get_status`, `about`, `open_file`, `unload_file`,
  `reload_file`, `start_stream`, `stop_stream`, `export_series`, `add_script_series`, `remove_script_series`,
  `add_filter`, `screenshot`.
- **Job tools** (`McpServer`, served to every token): `jobs_list`, `job_wait`, `job_kill` (see [Jobs](#jobs)).
- **Bridge tools** (answered by the bridge itself): `list_instances`, `select_instance`.

`ToolSet::kFull` serves the agent, app and job tools. `ToolSet::kAgentOnly` is used for plugin tokens: `tools/list`
and the `initialize` instructions leave the app tools out, and calling one gets the unknown-tool error.
`isReadOnlyTool` names the tools that may run while a modal dialog is open; `job_kill` runs there too, though it is
not read-only (it stops only the agent's own work).

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
| Status bar | Names the calling agent, and counts its running jobs | Its running jobs are counted and listed; its name is not shown (the plugin has its own UI) |

**Public owners.** `mcp::ownerFor` builds the owner from the bridge's `X-PJ-Client` header or the request's `_meta`
client name: `agent-` plus that name reduced to `[a-z0-9-]`, at most 32 characters. A request can never name a
plugin's id, and a plugin can never take an agent's: the plugin catalog refuses ids starting with `agent-`
(`PJ::kAgentIdPrefix`). Two sessions of one client share a namespace. On first use the server registers the owner
with `ExtensionCatalogService::registerAdHocOwner`, so its tabs' watermark and its rows read "AI agent (<owner>)".
Agents draw only in their own tabs. Their derived series are history-exempt plugin transforms under Custom Topics,
with an injective transform id (bytes other than `[A-Za-z0-9]` escaped).

## Jobs

A call that can take long does not hold its client until it ends. The job-capable tools (`mcp::isJobCapableTool`:
`evaluate`, `create_derived_object`, `wait_for`, `open_file`, `reload_file`, `unload_file`, `start_stream`,
`export_series`)
take `wait_s` (`mcp::waitSeconds`: default `kDefaultWaitS`, at most `kMaxWaitS`), added to their schemas by
`mcp::toolList`.

```
call ──► running ──(ends within wait_s)──► the ordinary reply                  (foreground; no job id surfaces)
            │
            └──(still running at wait_s)──► {job_id, state, progress, ...}      (background)

states: running · waiting_for_user · completed · failed · killed
```

- **One table.** `McpServer::jobs_` holds every such call from before its work starts (`openJob`): an agent tool's
  continuation (`ToolResult::resume`, resumed on the 50 ms tick) or an app tool's work (a load, which reports through
  its reply and stops through the job's `kill` hook). A job the call still waits on is *foreground*. Past `wait_s`
  the call gets the job id and the job is *background*. A job whose id nobody knows is forgotten when it ends; once
  its id is known (`Job::retained`: the call got it, the call hung up on an import, or `jobs_list` showed it) its
  result is kept for `job_wait`, repeatably, until it expires (`kJobExpiry` after it ended, the `kMaxEndedJobs` latest
  to end per client). An agent tool outside `isJobCapableTool` that defers is answered with an
  error: only a job is resumed.
- **Waiters.** The original call, a `job_wait` (`timeout_s` up to `kMaxJobWaitS`) and a `job_kill` are waiters on a
  job, each with a deadline and its own connection. A waiter is taken out of the job before its reply runs, so a
  call that got its job id is never answered again. A waiter whose client left is dropped.
- **Ownership.** A job belongs to the client that started it (`McpServer::sameClient`: the same token and, on the
  public token, the same `agent-<client>` owner), so a reconnecting client of the same name finds its jobs again and
  a plugin sees only its own. A foreign or unknown id reads "no such job". A client runs at most `kMaxRunningJobs`;
  the next job-capable call is refused before its work starts, naming `jobs_list` and `job_kill`.
- **Kill stops real work**, and `job_kill` answers once the stop took effect (the final state, with the tool's last
  result), or after `kKillWaitS` with `stopping: true` when the work is still winding down. What happened decides the
  final state, not what was asked: a load that had completed when its stop arrived ends `completed`.
  - an evaluation: its continuation is destroyed, which releases the evaluation handle
    (`OnDemandRuntimeHost::onRelease` cancels the `EnginePool` job). The pool worker may still be winding down; it
    publishes nothing. A `create_derived_object` killed during its first read-back stays installed
    (`remove_derived_series` removes it);
  - a load: `FileLoader::cancelLoad(ticket, keep_partial)`. A queued one leaves the queue; an active one stops at
    the producer's next progress callback, keeping the rows loaded so far (`keep_partial`, the default) or removing
    them. The job ends when `loadFinished` arrives. The first kill decides `keep_partial`. A `reload_file` replaces
    data the user already has, so every stop of it discards (`McpServer::awaitLoad`): the dataset goes back to what
    it held, whatever `keep_partial` said. `McpServer::stillAuthorized` is false once the caller's job was asked to
    stop, so a load killed while it waited never starts;
  - in the browser, `open_file`'s download from the relay: `cancelBrowserFetch`, until the load is queued;
  - a stream: `StreamingSourceManager::requestStopDataset`, which only asks (a kill hook must not block on the
    worker's join); the job ends when `streamStopped` arrives, and the data stays loaded;
  - an export: its `EnginePool` token is cancelled; nothing is written (see below);
  - a dialog the job's work has open for the user: it is closed as their Cancel would (see below).
- **A stream is a job that outlives its call.** `start_stream` answers as soon as the stream runs, with `dataset_id`
  and the `job_id` of a job that lives as long as the stream (`McpServer::answerCall` answers the call and leaves
  the job running; `followStream` ties the two). The job ends `completed` when the stream ends on its own or anyone
  stops it (`stop_stream`, the user removing the dataset), `failed` when it fails, `killed` on `job_kill`. Streams
  count toward `kMaxRunningJobs`, and revoking a token stops the streams its agents started.
- **Dialogs an agent caused.** `open_file` and `start_stream` with `ask_user`, and `unload_file`'s confirmation, put
  a dialog in front of the user. Each hands the job the closer of exactly that dialog while it is up
  (`McpServer::dialogWatch`, a `dialog_presenter::DialogWatch` that the file loader, the stream manager and the
  unload confirmation each pass to what opens their dialog): the job then reads
  `waiting_for_user`, and a kill closes that dialog from the event loop, as the user's click would arrive. The
  rejection is the tool's ordinary "cancelled" outcome, so nothing is applied, and it is the whole stop: a load at
  its dialog is not also cancelled through `FileLoader::cancelLoad`. Revoking a token closes the dialogs its jobs
  have open. No dialog is ever found by looking at
  what is on screen: one the user opened is never touched.
- **`export_series` writes off the GUI thread.** The handler stages the series on the GUI thread (the datastore's
  readers belong to it) and hands the copy to `SessionManager::enginePool()`; grid building and writing run on a
  worker, which polls its cancel token. A new temporary file is removed unless the export completes; a named
  destination is written through `QSaveFile`, so a stopped or failed export never truncates a file that was there.
  Only the staging read still runs on the GUI thread.
- **Hang-up.** Whether a foreground job dies with its call's connection is per tool (`diesWithItsCall`). An agent
  tool's does: it ends as killed, which also answers another session of the same client waiting on it. So does
  `unload_file`'s, whose prompt is withdrawn: a client that left must not have data deleted later
  (`stillAuthorized` already refuses from the moment the call hung up, before the tick notices). An import, a
  stream and an export carry on as background jobs their client finds with `jobs_list`. A background job ignores
  the connection of the call that started it.
- **`notifications/cancelled`** kills the one foreground call of the same token and owner whose JSON-RPC id matches
  `requestId` (`McpServer::cancelRequest`); none or several matches stop nothing, since ids are per client session.
  A cancel that names no client (some name themselves on requests only) is matched on the token alone. The bridge
  sends ids unique across bridges.
- **Progress** is what the work reports, never a nested event loop: a load gives `{done, total}` in the producer's
  own units (`FileLoader::loadProgress`, total 0 when unknown) or `{queued}` before its first tick; an evaluation
  has none.
- **Under the user's modal dialog** the three job tools run; a mutating job's continuation waits (state
  `waiting_for_user`) rather than fail.
- **The user's view.** `McpServer::unfinishedJobs` lists every client's running jobs (unlike `jobs_list`, which is
  per client), `stopJob` stops one as its own client's `job_kill` would, and `jobsChanged` says the set changed.
  `MainWindow` shows them in its status-bar indicator, which names the active client, counts the running jobs and
  stays visible while anything runs; clicking it opens `McpJobsDialog` (not modal, so agents are not held up), one
  row and one Stop button per job.

- **Watching a stream.** `wait_for` (an agent tool, so the Assistant has it too) is a job that ends with the first
  sample of one series, newer than the latest at the call, that satisfies a comparison, or fails at its timeout:
  with `start_stream` and `job_kill` it covers "start a stream, watch it, stop it". It polls through the context's
  `scan_values` hook from its cursor (`scanSeriesValues`, where a null is a NaN), not through an Arrow read.

### MCP Tasks

The server offers the MCP Tasks extension of protocol 2026-07-28 (`mcp::kTasksExtension`,
`io.modelcontextprotocol/tasks`; not the 2025-11-25 core utility it replaced) as a second way to reach the same job
table:

- **Negotiation.** `server/discover` advertises it under `capabilities.extensions`; a client declares it per request
  in `_meta["io.modelcontextprotocol/clientCapabilities"].extensions` (`mcp::declaresTasks`, kept in `Caller::tasks`).
  Nothing here remembers an `initialize`, so a client of that era cannot opt in.
- **Handle.** A job-capable call still answers directly when it ends within `wait_s`. At the handoff a declaring
  client gets a `CreateTaskResult` (`resultType: "task"`) instead of the job-id text; a client that did not declare
  the extension on that call never gets one. The task id is random (64 hex digits), minted at the handoff beside the
  small job id, which the job tools keep using. `start_stream` is the exception (`answersBeforeItEnds`): its job
  outlives the answer its call is owed, so it is never a task, only a job.
- **Methods.** `tasks/get` returns the task (`McpServer::taskOf`) and, once it ended, the stored tool result;
  `tasks/cancel` is `killJob` and acknowledges at once (cooperative: `tasks/get` says when it took effect);
  `tasks/update` acknowledges, since a job never asks its client for input. A task of another client, an unknown or an
  expired one is JSON-RPC `-32602`; a client that did not declare the extension gets `-32021`
  (`mcp::kMissingClientCapability`) naming it. The bridge passes the three methods through whole.
- **Status mapping.** Running and waiting for the user are `working` (with a `statusMessage`: the job's progress, or
  that the user is being asked in PlotJuggler); completed *and failed* are `completed`, the tool result carrying
  `isError`, because the extension keeps `failed` for protocol errors; killed is `cancelled`. `ttlMs` is null while
  the job runs, then the time from its creation to its result's expiry; a task that advertised it is not evicted by
  the `kMaxEndedJobs` count (its own bound is `kMaxEndedTasks`).
- **Not built:** `notifications/tasks` and its subscription (optional for a server), and `input_required`.
- Only this repository's tests exercise it until a client implements the extension.

### Adding a job-capable tool

The tool joins `mcp::isJobCapableTool`, gets its job id in `Caller::job`, and gives the job its stop and progress hooks
as `McpServer::awaitLoad` does (through `liveJob`: the job may have been stopped meanwhile). Neither hook may block or
spin a nested event loop. A tool that answers before its work ends uses `McpServer::answerCall`, as `start_stream`
does; one that asks the user passes `McpServer::dialogWatch` to whatever opens the dialog. Whether the job dies with
its call is decided in `diesWithItsCall`, and whether it can be an MCP task in `answersBeforeItEnds`. A test must not
wait synchronously for a `job_kill` answer from inside the dialog's own modal loop: the loop cannot unwind until that
wait returns.

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
  - its jobs are killed: each continuation is destroyed (releasing into services that are still alive) and its
    record forgotten; a load is cancelled, keeping its partial rows, and its record goes when the load winds down;
  - its open sockets are aborted, so a held reply then writes nothing.

  `~ToolboxPanelSession` resets `mcp_endpoint` right after `handle` (the plugin may call `release` from its
  destructor) and before every service the token reaches. Closing public access drops `agent_sessions_` after
  revoking.
- **Revocation from inside a result.** A reply or continuation can itself revoke a token, for example by tearing down
  another plugin's session. `resumeDeferred` therefore takes one continuation out of its job at a time, so the
  revocation still finds and drops the other jobs before their services die. Each job carries the round that last
  resumed it, so a round never resumes its own follow-ups. The token is checked before a continuation runs and
  before its follow-up is kept, and each continuation is destroyed right after it ran. The services of the result that is running stay
  alive because agent tools never spin a nested event loop (`agent_tools::ToolResult::resume`). Should a reply spin
  one anyway, the nested `resumeDeferred` returns at once (`draining_`), leaving the queue to the outer loop: a nested
  call would move the round on under it, and the outer loop would then resume the follow-ups it queued again and again.
- **App work left for later** asks `McpServer::stillAuthorized(caller)` before it runs and again once the user has
  answered its dialog. Once the token is gone, or the job the call runs as was killed, it applies nothing. This
  covers:
  - `start_stream`'s queued dialog (`StreamingSourceManager::startSession`'s `still_allowed`; a stream abandoned
    after its dialog leaves no dataset behind);
  - `open_file` and `reload_file` loads waiting in the queue or at their dialog (`LoadHints::still_allowed`, checked
    in `FileLoader::beginLoad`);
  - `unload_file`'s prompt (`confirmAndRemoveDependentTransforms`'s `still_allowed`).
- **A tool runs from the event loop**, never from inside its connection's own signal (`McpServer::dispatch` queues
  the call): a tool may run a modal loop (the unload prompt, a stream's dialog), during which that connection can
  close and be deleted.
- **Held replies.** A tool that is not job-capable may still hold its reply: a dialog the user answers, a screenshot
  frame. A job-capable one becomes a job ([Jobs](#jobs)), whose continuation is resumed every 50 ms, as the
  Assistant's GUI tick does. Every request gets exactly one response; a call that went to the background got it
  (the job id) and its result waits for `job_wait`.

## Public access (external agents)

- **On by default.** An attended run opens public access at startup unless the user turned off the Preferences →
  Appearance switch "Let AI agents control PlotJuggler (MCP server)" (`MainWindow::kMcpServerKey`). An unattended
  run (`--exit-after-layout`, `--screenshot`, the offscreen and minimal platforms, so every test) opens it only with
  `--mcp`, so it never shows up as an instance an agent could pick. The switch shows whether public access is open
  now; a plugin's endpoint does not count. OK applies the switch when it differs from the current state: it writes
  the setting and either opens public access with a random token (discovery file written) or closes it (file
  removed; plugin tokens keep working). `--mcp` opens it for one run without touching the setting.
- **Indicator.** A status-bar button names the public client that called last (for two minutes after its last
  request, or until public access closes), counts the jobs agents have running and stays while any runs; clicking it
  lists them, each with a Stop button ([Jobs](#jobs), "The user's view").
- **Discovery.** `<AppLocalData>/mcp-instances/<pid>.json` holds pid, port, token and start time, owner-only like its
  folder. On Windows `setPermissions` only toggles read-only, so privacy there rests on `%LOCALAPPDATA%`'s per-user
  ACL. `AppLocalDataLocation` needs only HOME, because agents start the bridge with a stripped environment. A starting
  server deletes the files of dead pids (`ActiveWidgetsMarker::isProcessAlive`). The bridge deletes a file only when
  its pid is gone or its port refuses; a slow or odd reply leaves it. Attended starts record the executable path
  (`mcp-instances/executable`) for agent plugins that launch PlotJuggler.
- **Bridge.** Its waits are bounded: 2 s connect, 5 s probes, 300 s per tool call (a job-capable call answers
  within its `wait_s`; what can still take long is a dialog the user answers). It forwards one call at a time, but
  reads stdin on its own thread: a `notifications/cancelled` naming the call in flight is forwarded to the instance
  under the id the bridge gave that call; the bridge then drops that call's connection without waiting (the instance
  may be unable to stop it) and the cancelled request gets no response. One naming a call still waiting behind it
  drops that call unsent. A connection that closes with no
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
- **Files.** The page cannot read the agent's disk: the relay serves the file `open_file` names and passes its url,
  which the page downloads into a staged file (`McpServer::fetchAndLoad`). The server remembers what each dataset
  was downloaded from, so `reload_file` asks the relay for the file again, as it is now, and loads it over the
  dataset in place (`LoadHints::replace_dataset_id` with `require_replacement`). A dataset the user picked by hand
  has no such url and is refused, as the Reload button treats it; so is one the user has since replaced with a file
  of their own (the url is kept only while the dataset has the source identity it was loaded with). Not covered: a
  file that loaded as several datasets (a strict replacement refuses a fan-out), and an import stopped with its
  partial rows kept (it never recorded its url).
- **Lifetime.** The link is a child of the server, deleted with it (in `~QObject`, after the server's members); its
  in-flight replies are deleted without emitting `finished`, so no callback reaches the server then, and replies the
  server still holds check the link through a `QPointer`. A relay that is down is retried, backing off to 10 s, while
  the page lives; the relay forgets a page that has not polled for 120 s. One relay serves one agent session, so a
  relay that forgot the page (404) or failed two polls in a row is a session that ended: the link deletes the object
  it gave that session's calls as their connection, which the server treats as a desktop client hanging up (a job
  that dies with its call is killed, the others carry on for their client to find), and no answer of the old
  session is posted into the next one, whose message numbering starts over. In the browser a
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
- `unload_file` with dependent derived series waits for the user's answer to the prompt (the `cancelled` hook of
  `confirmAndRemoveDependentTransforms`), as a job: past `wait_s` the agent gets its job id.
- A dialog opened because an agent passed `ask_user` is titled "An AI agent asked you to choose: ...". The prefix
  travels `LoadHints::dialog_title_prefix` → `DataSourceRequest::title_prefix` → `DialogEngineConfig::title_prefix`
  (for streams, `StreamingSourceManager::startSession`).
- `start_stream` with `ask_user` opens the named source (checked against `streamSources()`) once its request has
  returned, and only if no modal dialog came up meanwhile. The start does not wait for its dialog
  (`StreamingSourceManager::startSession` reports through its `done`): a browser cannot block on one.
- The user sees what agents are running and stops any of it ([Jobs](#jobs), "The user's view").

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
  - `McpJobTest`: the job table through synthetic continuations: the handoff at `wait_s`, `job_wait` and `job_kill`,
    ownership, limits and expiry, hang-up, revocation, `notifications/cancelled`, the modal gate, the user's view,
    and the MCP Tasks methods.
  - `McpQueuedLoadTest`, `McpActiveLoadTest`: killing and revoking a load in the queue and one held active by an
    in-test source, with and without `keep_partial`.
  - `McpStreamAndDialogJobTest`: a stream as a job (kill, someone else's stop, revocation) and `job_kill` closing
    each of the three dialogs an agent's call opens; a hung-up `unload_file` withdrawing its prompt.
  - `tests/wasm/mcp_relay.spec.js`: the browser page driven through an in-test relay
    (`tests/wasm/support/mcp_relay.js`, the page protocol of `pj_mcp_relay.py`): `reload_file` of a file the relay
    served, and a stream dialog an agent asked for, opened and withdrawn.
  - `McpEndpointTest`: plugin tokens. Covers the forced owner, refused app tools, the `tools/list` profile, no
    discovery file, release and teardown revocation, disconnects, distinct tokens per plugin, and the same endpoint
    on a repeated acquire. Also a session torn down from inside another's resume, and a revoked token's follow-up.
  - Revocation of public access before or under an agent's queued dialog: nothing is applied.
  - `ARelayedCall…`, `ALinkForgotten…`, `OnlyALoopbackHttpRelay…`: the browser link against a fake relay.
- `pj_app/tests/mcp_bridge_test.cpp`: the bridge over stdio against the built app, and against a fake instance the
  test answers by hand (a cancellation forwarded while its call is in flight).
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
- The bridge forwards one call at a time, so two calls of one session overlap only as a background job plus a
  foreground call, and a `tasks/get` or `tasks/cancel` waits behind a call still in its `wait_s`.
- The browser relay (`pj_mcp_relay.py`, in the `plotjuggler-agent` repository) must forward notifications and the
  `tasks/*` methods and pass on the page's extensions for `notifications/cancelled` and MCP Tasks to reach a page;
  until it does, only the job tools do.
- The Assistant's Stop ends its agent CLI; for it to stop the jobs that turn left running too, the plugin
  (`pj-official-plugins/toolbox_assistant_agent`) must release its endpoint on Stop, which revokes its token.
- Protocol 2026-07-28 is spoken only as far as agents have needed: ordinary results carry no
  `resultType: "complete"`, and `server/discover` keeps the `serverInfo` field of the earlier era.
- The server is stateless, so it cannot tell two sessions of one client apart: a cancellation that arrives after its
  call ended can stop another session's foreground call that reuses the same JSON-RPC id. Bridge traffic is immune
  (unique ids); it concerns clients that speak HTTP to the server directly.
- Killing an evaluation is acknowledged when its handle is released, not when its pool worker has exited.
- Held calls and waits share the 16-socket cap: many clients each holding calls for their full `wait_s` can keep a
  `job_kill` from connecting until one returns.
- A plugin's message box raised while an import runs (`FileLoader`'s plugin message-box gate) is not a dialog a job
  can close: the import's kill stops the load at the producer's next progress callback instead.
- `export_series` still stages its series on the GUI thread, and the row cap is per export: several wide exports at
  once hold several copies. Its write runs on `EnginePool`'s shared lane, which a machine with two or three cores
  sizes at one thread: derived evaluations wait there until the file is written.
- A named export is written beside its destination and replaces it only once complete, so it needs a folder it can
  write to: exporting over a writable file in a folder that is not fails ("cannot create"), where a stopped export
  written in place would have truncated the file.
- A `create_derived_object` stopped during its first read-back answers `killed` although the object is installed,
  and the read-back's removal of an object whose script fails is skipped: `list_created` shows it and
  `remove_derived_series` removes it.
