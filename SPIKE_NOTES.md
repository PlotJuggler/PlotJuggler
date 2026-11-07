# Spike: an MCP server inside PlotJuggler 4 (2026-11-07)

Throwaway code, not the production shape: `pj_app/src/spike/` (`McpSpikeServer`, `McpProtocol`, `McpBridge`),
`--mcp` / `--mcp-port` / `--mcp-token` / `--mcp-bridge` in `main.cpp`, `friend class McpSpikeServer` in
`MainWindow.h`.

```bash
# for users: the agent plugin in packaging/agent-plugin/ (MCP server + skill), see its README:
claude plugin marketplace add <repo or path> && claude plugin install plotjuggler@plotjuggler
codex plugin marketplace add <repo or path> && codex plugin add plotjuggler@plotjuggler
# by hand, per agent (stdio; no URL or token in the config):
claude mcp add pj -- /path/to/run.sh --mcp-bridge
codex mcp add pj -- /path/to/run.sh --mcp-bridge
#   plus, in ~/.codex/config.toml [mcp_servers.pj]: env_vars = ["DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR"]
#   (Codex strips them; without them `launch` refuses, an already running PlotJuggler still works)
# PlotJuggler with MCP on (the agent can also start one itself with the `launch` tool):
./run.sh --mcp
# direct HTTP, for scripts and debugging:
./run.sh --mcp-port 7777 --mcp-token spike             # POST /mcp, GET /screenshot.png, Bearer token
```

## Shape

```
agent ──stdio──> plotjuggler4 --mcp-bridge ──HTTP 127.0.0.1:<port>, Bearer──> PlotJuggler (--mcp)
                 (one per agent session:          discovery: <AppLocalData>/mcp-instances/<pid>.json
                  picks/launches the instance)     (owner-only: pid, port, token)
```

- In the app: hand-written HTTP/1.1 on `QTcpServer` on the GUI thread (QtHttpServer is GPL-3-only), JSON-RPC,
  bearer token, `Origin` refused, `Connection: close`. Both MCP eras on one endpoint.
- `McpProtocol`: the tool list (static, so the bridge lists tools while nothing runs), lifecycle replies with
  server `instructions`, client name to tab owner, the discovery folder.
- Every tool is a thin call into an existing service; the spike adds no business logic.

## Tools (32)

| Group | Tools | Backed by |
|---|---|---|
| Session | `get_status`, `about`, `report_status`* | `CatalogModel`, `SessionManager::loadedSources`, `PlaybackEngine`; `FeatureCatalogHost::document()` (#783) |
| Data | `open_file` (default / `config` / `ask_user`), `unload_file`, `reload_file`, `start_stream` (same three modes), `stop_stream`, `list_catalog`, `list_topics`*, `describe_topic`* | `FileLoader::loadFileTicketed`, `MainWindow` remove/reload bodies; `StreamingSourceManager::startFromLayout` / `onStartRequested` / `stopDatasetAndWait`; catalog snapshot v2 (#766) |
| Values | `statistics`, `get_values`, `export_series` (aligned CSV), `read_series`* (stats / min-max buckets / raw), `evaluate`* (Luau over series or objects, leaves nothing) | `sdk::ToolboxHostView::readSeries`; `pj.data_processors.v1` ephemeral transforms and on-demand evaluations |
| Derived | `add_filter`, `add_script_series`, `remove_script_series`, `create_derived_series`*, `create_derived_object`* (incl. `pin_at_s` findings), `create_markers`*, `remove_markers`*, `list_created`*, `remove_derived_series`* | `upsertTransform(owner, …)`; `pj.data_processors.v1` under the agent's plugin id |
| Views | `plot_tab`* (create/add/remove/zoom/close/list), `scene_view`* (create/attach/detach/focus/close/list), `playback`* (state/play/pause/seek/rate), `screenshot` (`tab` brings one of the agent's to the front) | `pj.plot_tabs.v1`, `pj.viewport.v1`, `pj.playback.v1` under the agent's plugin id; `QWidget::grab` + `QRhiWidget::grabFramebuffer` |
| Bridge | `list_instances`, `select_instance`, `launch` | discovery files, `QProcess::startDetached` |

\* The Assistant Agent's tools (`toolbox_assistant_agent` 0.4.0), compiled unchanged from a snapshot in
`pj_app/src/spike/assistant_tools/`. The server builds, per agent owner, the same service assembly a toolbox
plugin gets (`MainWindow::buildToolboxPanelSession` with the owner as plugin id) and runs the plugin's
`ToolRegistry` against it; deferred results (object evaluations) are polled every 50 ms as the plugin's GUI
tick does. The spike's own view tools (`create_tab`, `add_curve`, `remove_curve`, `close_tab`,
`attach_topic`, `tab_config`, `focus_tab`, `zoom_fit`, `seek`) are retired in their favour.

## Findings

| Question | Result |
|---|---|
| Protocol era of real clients | Claude Code 2.1.319: **2026-07-28** (`server/discover`, stateless, `_meta` client info, `progressToken`). Codex 0.162.1: **2025-06-18** (`initialize`, session id, GET SSE attempt accepts 405, DELETE on exit). **Both eras are required**, over HTTP and stdio. |
| Image results | Both clients pass `image` content to the model. |
| Screenshot | `QWidget::grab()` + each visible `QRhiWidget::grabFramebuffer()` composited: plots **and Scene3D**, synchronously, 50–70 ms (OpenGL, Linux; D3D11/Metal untested). An open user dialog is a separate top-level: composited centred over a dimmed window (Wayland hides real positions). `GET /screenshot.png?tab=&max_width=` serves scripts in 82 ms. |
| Responsiveness | GUI-thread server responsive during a 17 GB import (40 polls, 40–150 ms); `open_file` held its reply 10.3 s, tolerated by both clients. |
| Catalog | Authoritative field paths (`pose/pose/position/x`, no leading `/`). A topic can be an object (`/odom` = `kPosesInFrame`) and scalar fields. Scalar topics carry no count/time range in snapshot v2 (objects do): `statistics` fills that. |
| Fresh curves | Drawn in the 0..1 default window until fitted: `add_curve` fits. |
| Loader dialogs | `open_file` default `kNever` (loader defaults, e.g. every MCAP topic); `config` (agent-chosen loader settings, `get_status` shows each dataset's as a template; a one-topic subset verified); `ask_user: true` (the user's own dialog, reply held until OK/cancel, cancel reported as such). The reply carries `dataset_id` and the effective `config` (the MCAP loader adds `/tf`, `/tf_static` itself). |
| CLI launch (the old skill idea) | `plotjuggler4 -d bag.mcap -l layout.xml` is an attended load: the loader dialog blocks until the user answers, and the layout's saved loader config is not used for an explicit `-d` file. MCP `open_file` has no such step. |
| User dialogs vs agent | A modal dialog runs a nested event loop, so the server keeps answering during it. Guard: with `QApplication::activeModalWidget()` set only read-only tools run; the rest return "the user has '<title>' open", `get_status.user_dialog` names it. |
| Unload / reload | `unload_file` 30 ms; `reload_file` in place 491 ms, same dataset id, curves stay bound, recorded loader config, dialogs forbidden. UI actions mix confirmation with action: the spike duplicates the post-confirmation bodies; production should split confirm from act. |
| Same topic in two datasets | Value, view and filter tools refuse with "exists in several datasets [1,3]: pass dataset" and work with `dataset` (id, name or path). Mixed clocks show (3.79e7 s next to a 0–100 s bag): time ranges per dataset are needed. |
| Several instances | A fixed port lets one instance win (the second logs `listen failed`). `--mcp` takes an OS-assigned port and writes an owner-only discovery file; the bridge forwards. |
| Stdio bridge | Handled before any `QApplication` (like `--selftest-python`). None running: "call launch" (2.4 s to a ready instance). One: used and pinned for the session. Several: refuses to guess; `list_instances` (datasets, focused, open dialog) + `select_instance`. Crash (SIGKILL): the next call says so and forgets it. Leftover files (SIGKILL, SIGTERM, a planted fake) are deleted when they stop answering. Forwarding is sequential: a held reply holds the session. |
| Tab namespaces | Owner = `agent-<client name>` (`initialize` clientInfo or 2026 `_meta`), sent by the bridge as `X-PJ-Owner`: Claude Code's and Codex's `speed` tabs are separate. Two sessions of one client share a namespace. Agent tabs are history-exempt (outside undo/redo) like any plugin tab. |
| Agent-written series | `add_script_series` = a plugin-style transform owned by the agent, `history_exempt`: listed under **Custom Topics** (an `agent` group), saved in layouts with its script, untouched by the user's undo, replaced by the same name. The transform id must equal the Luau class id: the server wraps the script and stamps it. A syntax error comes back with the Luau message and line. Several inputs and outputs: see below. |
| Plugin introspection reused | `about` is the #783 document (core features with UI entry points, limitations, plugins; 21.6 KB). `list_catalog` and the value tools read the #766 snapshot and `readSeries` through `sdk::ToolboxHostView`: the MCP and an in-app plugin agent see one data contract. |
| Export | `export_series`: 3 series, 12,868 aligned rows (union of sample times, sample-and-hold) in 0.09 s; `rate_hz: 10` over 10–20 s gives 100 rows. |
| Agent environments | Codex starts MCP servers with a filtered environment (HOME, PATH, LANG, USER, ...; no `DISPLAY`, `WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR`). Two failures followed: `launch` started a GUI with no display (it died; the bridge waited 60 s), and Qt's `RuntimeLocation` fell back to `/tmp/runtime-<user>` in the bridge while the GUIs wrote `/run/user/<uid>`, so the bridge saw no instance at all. Fixed: discovery under `AppLocalDataLocation` (needs only HOME); `launch` refuses at once without a display and names the fix (`mcp_servers.<name>.env_vars`); a child that dies during startup is reported in ~1 s with the head of its output (the cause precedes the crash handler's backtrace). Production: the discovery path must depend on nothing an agent may strip. |
| Bypassing the bridge | With the bridge blind, Codex read the token from the discovery file and called HTTP from Python: same-user access is the trust boundary (owner-only file), and an agent with a shell can always cross it. Not a leak, but the bridge is a convenience, not a security layer. |
| Multi-input / multi-output scripts | `inputs` of 5 fields of one message (accel + quaternion) joined on their shared timestamps, 3 outputs, 9,879 samples in 74 ms. Outputs are named `<name>/<output>` (bare names would collide across scripts); `remove_script_series` removes all of them. |
| Assistant tools over MCP | All 14 run unchanged through the host's toolbox services (scripted end to end on the nissan and lidar bags): suffix path resolution, min/max buckets, `evaluate` on series and at an instant (deferred poll, 57 ms), a derived series with its point count, the empty-join refusal (99 Hz IMU + 30 Hz speed), a region marker with `covered_s`, derived objects over a point cloud (crop box, live and pinned), a 3D scene view with the cloud attached, playback, `remove_*` refusing what the agent did not create. The spike's `add_script_series` output appears in `list_created`: both tool sets share the agent's namespace. Ownership is the host's per-plugin identity, but the identity itself is the unauthenticated `X-PJ-Owner` string: any token holder can claim any namespace, including a real plugin id. |
| Streams | `get_status` lists the loaded stream sources and marks live datasets with `stream` and their config. `start_stream` reuses the layout-replay start (no dialog): the agent's `config`, else the settings the user last confirmed in that source's dialog. `ask_user` selects the source in the Stream combo and presses Start for the user (the reply waits for the dialog, a cancel or the plugin's error box). `stop_stream` keeps the data, like a stream that ends by itself. Verified: Dummy Streamer started with defaults (5 topics streaming in 2 s), stopped (15 s of data kept), a second start through `ask_user` (no dialog: starts at once); ROS 2 through `ask_user` without ROS sourced: the app's own error box, the agent held off meanwhile. Not reachable yet: toolboxes, marketplace installs, Custom Widget panels; a loader's options are learnt from `get_status` examples only (no schema). |
| Python | `add_script_series` takes Python (`# pj-script: python`, one class `T`, whose `id` the server stamps, imports limited to math/cmath/statistics; the embedded CPython is in every desktop build). Verified live: `py_kmh` at 30 s = 52.27 (14.52 m/s × 3.6), and a Claude run wrote a correct class from the skill alone. The Assistant's 14 tools are Luau-only. |
| Agent plugin | `packaging/agent-plugin/`: one folder that is both marketplace and plugin, for Claude Code and Codex (`.claude-plugin/`, `.codex-plugin/`), a skill, and `bin/plotjuggler-mcp` (`.cmd` on Windows) that finds `plotjuggler4`: `$PLOTJUGGLER4`, PATH (deb, conda, nix), the path the app records at every start (`mcp-instances/executable`; the AppImage file via `$APPIMAGE`, which `launch` also starts: the mount dies with the bridge; attended runs only, so tests never repoint it), default install folders (macOS `.app`, Windows installer). Claude Code, empty folder, prompt naming no tool: skill auto-loaded, 21 turns, 73 s, $0.31, Python series + own tab + screenshot check. Codex: installs from the same marketplace; `codex exec` refuses every non-read-only tool ("approval policy is never") unless the plugin sets `default_tools_approval_mode` (now `approve`; `export_series` and `unload_file` still prompt). The Codex config also forwards `XAUTHORITY` (GDM keeps the X cookie under `/run/user`) and `DBUS_SESSION_BUS_ADDRESS`. |
| Browser (WASM) | Not as is: a page cannot listen on a socket, write the discovery folder or be spawned. The tool layer is transport-free; a browser transport would be the page connecting out to a local bridge's WebSocket (pairing + Chrome's local-network prompt), WebMCP later. `main.cpp` now creates the server only outside WASM, but `pj_app/CMakeLists.txt` still adds the spike sources (`QProcess` is not in Qt for WebAssembly): production must leave them out of the browser build. |
| Origin / auth | Missing or wrong token 401; any `Origin` header 403; discovery files and folder owner-only. |

## Real agents on one task

Task: "Open the nissan bag, compare vehicle speed with the integrated IMU forward acceleration, show both, check the
view, explain any discrepancy with numbers." Bash and Read were available.

| Run | Calls | Time | Outcome |
|---|---|---|---|
| Claude Sonnet, HTTP, first 12 tools | 16 | 76 s | Correct view; decoded the bag in Python for every number. |
| Codex, HTTP, first 12 tools | 22 | 105 s | Same, shell + Python. |
| Claude Sonnet, stdio bridge, + values/catalog v2 | 35 (7 Bash) | 196 s, $0.54 | Numbers from `statistics` (6 calls) instead of decoding for the basics. Found the forward axis y (correlation 0.987) and explained the drift as gravity through pitch (fit R² 0.974 → 0.992 with g·sin(pitch), coefficient −0.955 vs −1 expected). Bash only for multi-signal numpy (fits, lag, quaternion pitch), and said why: "the MCP tools only expose the built-in filters", so the corrected curve could not be plotted. Asked for: legend placement, removing its unused filter output. Found a real bug: re-titling a tab does not update the tab strip. |
| Codex, stdio bridge, full toolset, nothing running, env stripped | 7 MCP + 36 shell | 357 s | Bridge blind (see "Agent environments"): `launch` timed out. Codex started PlotJuggler from its shell and called HTTP with the token from the discovery file. Right answer anyway (y axis, gravity through pitch, RMS 2.86 → 0.29 m/s with a fitted offset). |
| Codex, same, display variables forwarded (`env_vars`) | 32 MCP + 9 shell (4 numpy on the `export_series` CSV) | 212 s, 0.73 M input tokens | Everything through the bridge: `launch` → `open_file` → `about` → `list_catalog` → 7 `statistics` → `export_series` → `add_script_series` (trapezoidal integral in PlotJuggler) → tab, curves, `zoom_fit`, `screenshot` (checked it) → `get_values` at 30/60/99.6 s. Used the parser's own `orientation/pitch`. Quantified the drift: pitch change during the stops +1.41° → −0.24° predicts −0.282 m/s² of gravity leakage vs −0.263 observed; the final −5.73 m/s needs only a −0.058 m/s² mean error (≈0.34°). Shell only for numpy over the exported CSV, never to decode the bag. |

What changed after the Claude run: `export_series` (its numpy needs aligned arrays, now for every format),
`add_script_series` (its corrected curve), `remove_curve`, `close_tab`, `get_values.at`.

## Product bugs found (outside the spike)

- **Tab title not refreshed** (fixed in #1481). `PluginPlotTabsController::createTabOfKind` re-creating a tab calls
  `PlotDocker::setName`, which stores the name but never updates the tab strip (`TabbedPlotWidget` has
  `setWidgetTabName` for widget tabs only). Toolbox plugins on `pj.plot_tabs.v1` hit it too. Fix: a plot-tab
  rename on `TabbedPlotWidget` that updates the frame label, called from the controller.

## Gaps the production design must close

- **Overlapping tools.** `list_catalog` / `list_topics` + `describe_topic`, `statistics` + `get_values` /
  `read_series` + `evaluate`, `add_script_series` / `create_derived_series` do the same jobs. Keep one of each
  after an agent trial shows which ones agents use; the spike keeps both for that trial.
- **Share the tool layer, don't copy it.** The spike compiles a snapshot of the plugin's code; production
  should move `tools.cpp` and `derived_recipes` where both the plugin and the app consume one copy.

- **Ownership scope.** The agent sees and edits only its own tabs, never the user's: safe, but it can't "add this
  to my plot". A product decision.
- **Long imports** past a client's tool timeout: keep hold-the-reply, add job handles for the long case.
- **Legend placement and other view settings** (agents asked for it).
- **Opt-in UI**: a Preferences toggle and a status-bar "agent connected" indicator; `--mcp` is the only switch
  now. A dialog opened by `ask_user` should say an agent asked for it.
- **Windows/macOS**: discovery folder paths, port binding, D3D11/Metal framebuffer grabs: untested.
- **Robustness found by review**: argument type errors threw out of a Qt slot (now caught and returned as a
  tool error, also in deferred steps); a stale discovery file was taken for a live instance when its port was reused (now the pid
  is checked first). Still open: `add_script_series` installs inputs that share no timestamps (the
  Assistant's `create_derived_series` refuses them); any non-200 reply makes the bridge delete a live
  instance's file; the bridge blocks forever on a hung GUI (`waitForReadyRead(-1)`); `unload_file` replies
  twice when the user must confirm; `screenshot` is annotated read-only but `tab` focuses one; the bridge's
  `tools/list` is its own static list, so a different PlotJuggler version on PATH than the running window
  advertises the wrong tools (forward `tools/list` to the live instance when one answers).
- **Windows plugin launch is unverified**: both MCP configs name the extension-less launcher; Codex is
  expected to pick its `.cmd` twin, Claude Code (libuv spawn) may not. If not: the Windows installer puts
  `bin/` on PATH and the configs name `plotjuggler4`.
- **Confirm vs act.** `unload_file` / `reload_file` duplicate the bodies behind `MainWindow`'s confirmation
  prompts; split them so the UI and the agent call one function.

## Recommendation for the production version

The spike answers the requirement questions; nothing found argues against shipping this shape.

1. **Keep the shape**: `plotjuggler4 --mcp-bridge` (stdio, the only thing agents configure) in front of a loopback
   HTTP endpoint in the app. Both MCP eras. Hand-written HTTP (QtHttpServer is GPL-3). Discovery under
   `AppLocalDataLocation`. Code in `pj_app/src/mcp/`: the server needs `MainWindow`, its views and the screenshot
   path; the protocol and bridge need only QtCore and could move to `pj_runtime` if a second host appears.
2. **Opt-in**: a Preferences toggle (off by default) next to `--mcp`, a status-bar indicator naming the connected
   agent, and an "an agent asked for this" line on dialogs opened by `ask_user`.
3. **Keep the tools** (30 now, fewer after the overlap trial), each a thin call into an existing service. Keep `add_filter` next to
   `add_script_series`: agents reach for it in the common case, and it runs through the same transform path.
4. **Settle three product decisions** before writing it: whether agents may touch the user's tabs (now: only their
   own); legend and other view settings; job handles for imports longer than a client's tool timeout.
5. **Fix outside the MCP work first**: the tab-title bug, and the confirm/act split.
6. **Ship the agent plugin** (`packaging/agent-plugin/`) from its own public repository so install is one
   `marketplace add` + one install per agent; the app records its path so the plugin finds AppImage, `.app`
   and Windows installs.
7. **Test on Windows and macOS** (discovery path, port binding, D3D11/Metal grabs) and add a protocol test that
   drives the bridge over stdio (the scripted session in this spike is the model).
