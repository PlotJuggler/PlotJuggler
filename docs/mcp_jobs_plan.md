# Plan: agent tool calls as jobs (start, wait, kill)

Status: **in progress** (started 2026-11-13). Nothing below is built yet. The as-built MCP design is
[MCP_SERVER.md](./MCP_SERVER.md); read it first, together with `pj_app/CLAUDE.md` ("MCP server") and
`pj_runtime/CLAUDE.md` ("Agent tools"). When a phase lands, move its design into `MCP_SERVER.md` and trim it from this
plan; retire the plan to `archive/` when done.

## Why

A tool call is synchronous for the agent: it waits for the reply. Inside PlotJuggler a long call does not block the
GUI (a tool holds its reply: `ToolResult::deferred` for agent tools, a held `Reply` for app tools, resumed by
`McpServer::resumeDeferred` every 50 ms), but:

- **Nothing can be cancelled on request.** `notifications/cancelled` is ignored: the bridge `continue`s past every
  notification (`McpBridge.cpp`), the server answers every notification 202 (`McpServer.cpp`), and the bridge is
  strictly serial (it blocks on the HTTP exchange and cannot even read stdin meanwhile).
- **The only aborts are indirect.** A dropped connection drops the held reply at the next tick (an evaluation's
  continuation then releases its handle, which cancels the `EnginePool` job), but an import or stream carries on.
  Revocation (Preferences switch, plugin session teardown) drops replies and aborts sockets.
- **Long calls hit client timeouts** (the bridge's 300 s, often the client's own sooner) with no progress and no way
  to reattach to the outcome except polling `get_status`.
- **One call at a time per agent session** through the serial bridge, so an agent cannot evaluate loaded data
  while another file imports.
- **`export_series` runs on the GUI thread** (up to `kMaxExportRows` rows), the one sizeable synchronous freeze.

## Use cases this must serve

1. Open a multi-GB MCAP (minutes), analyze the partial data meanwhile (ingest is progressive), then the whole.
2. Wrong file or topics: kill the import, keeping or discarding what loaded.
3. A long Luau/Python evaluation: kill it beyond the budget chosen up front.
4. Start a stream, observe it for a while, stop it (the stream is the long-lived process).
5. Concurrency: two imports, or an evaluation while an import runs.
6. The user presses Stop in the Assistant: every job of that turn dies.
7. The agent's CLI crashes or times out and reconnects: it finds its jobs and their results.
8. The user sees what agents are running and stops any of it.

## Decisions (agreed)

- **D1 Auto-background, not an up-front flag.** Every job-capable tool takes `wait_s` (default 20, clamped to
  [0, 120]). Finished within `wait_s` → the ordinary reply. Still running → reply
  `{"job_id", "state", "progress"}` and the call becomes a background job. An agent never has to predict
  duration, and no call outlives a client timeout.
- **D2 Hang-up semantics.** A call still in the foreground dies with its connection (as today: its work is killed
  where a kill exists; an import keeps today's "carries on" behaviour, see D5). A background job survives a
  disconnect until it ends and its result expires. Revoking a token **kills** every job of that token (stricter than
  today's "drop the replies").
- **D3 Ownership.** A job belongs to its caller's owner: `agent-<client>` for the public token (so a reconnecting
  client of the same name sees its jobs again), the plugin's id for a plugin token. An agent lists, waits on and
  kills only its own jobs; the user sees and kills all.
- **D4 A stream is a job.** `start_stream` returns a job that stays `running` while the stream lives; killing it stops
  the stream. `stop_stream` stays as an alias for compatibility.
- **D5 Kill must stop real work**, per tool (table below). A foreground import whose client hangs up keeps running
  (existing documented behaviour, an agent can find the dataset via `get_status`); only an explicit kill or a
  revocation stops it.
- **D6 `job_kill` may close a dialog the agent opened** (titled "An AI agent asked you to choose: ..."), as if the user
  pressed Cancel. Never one the user opened.
- **D7 One job table, three doors**: explicit job tools (work with every client today), `notifications/cancelled`
  for foreground calls, and the MCP Tasks utility later when clients negotiate it.

## The model

```
call ──► running ──(ends within wait_s)──► ordinary reply                       (foreground; no job id surfaces)
            │
            └──(still running at wait_s)──► {job_id, state, progress}            (background)

states: running · waiting_for_user · completed · failed · killed
```

### Job tools

Implemented in `McpServer`, since they read its job table (see "Placement"), and served to **every token** (plugin
tokens too, so the Assistant can stop its own work). All read-only except `job_kill`, so they
run under a user's modal dialog (add them to `mcp::isReadOnlyTool`).

| Tool | Args | Returns |
|---|---|---|
| `jobs_list` | none | `[{job_id, tool, summary, state, elapsed_s, progress}]` of the caller's owner |
| `job_wait` | `job_id`, `timeout_s` (default 20, clamped below the bridge's call timeout) | the final tool result if it ends in time, else `{job_id, state, progress}` |
| `job_kill` | `job_id`, `keep_partial` (default true) | the final state once the kill took effect (`killed`, or `completed`/`failed` if it ended first) |

Results stay readable (repeatable `job_wait`) until expiry: 10 min after the end, at most the last 32 ended jobs per
owner. At most 8 running jobs per owner; the ninth call fails with a message naming `jobs_list`/`job_kill`. Job ids
are small per-server integers, never reused within a run; a foreign or unknown id reads "no such job" (do not leak
other owners' ids).

### Kill per tool

| Tool | Kill does | Existing entry point |
|---|---|---|
| `evaluate` (object form), `create_derived_object`, markers via `submitAndPoll` | release the evaluation handle → `EnginePool` job cancelled (Luau at its interrupt poll, Python at the next bytecode via `cancel_flags`) | `PendingEvaluation::release` → `OnDemandRuntimeHost::onRelease` |
| `open_file`, `reload_file` | `FileLoader::cancelLoad(ticket, keep_partial)` (queued: removed; active: stopped; idempotent); the held reply then sees `LoadOutcome::kCancelled` | `FileLoader::cancelLoad`, `McpServer::awaitLoad` |
| `start_stream` | stop that stream (the same path as `stop_stream`) | `McpServer::stopStream` / `StreamingSourceManager` |
| anything `waiting_for_user` (`ask_user` loader/stream dialogs, `unload_file`'s prompt) | reject the dialog the agent opened; the existing `stillAuthorized` / `still_allowed` checks already make a late answer a no-op | new: the job must hold a `QPointer` to the dialog it opened |
| `export_series` | set a cancel flag on a worker-thread export; a killed export deletes its partial file | new: move the write off the GUI thread first (Phase 2) |
| `screenshot`, quick reads, mutations that end in one step | never a job | — |

## Placement

- **`pj_runtime/include/pj_runtime/agent_tools/tool_context.hpp`** (MIT-licensed files: keep their SPDX and copyright
  lines): `ToolResult` gains two optional hooks, set only alongside `resume`:
  - `std::function<void(ToolContext&, bool keep_partial)> cancel` — stops the work; the continuation is then resumed
    once more (or destroyed) and must reach a terminal result.
  - `std::function<nlohmann::json()> progress` — cheap, GUI-thread, no nested event loop.

  `submitAndPoll` (`derived_tools.cpp`) fills both. Keep the contract: neither may spin a nested event loop.
- **`pj_app/src/mcp/McpServer.{h,cpp}`**: a job table replaces `deferred_`. Each entry: id, owner, `Caller`, tool,
  summary, state, start time, the agent continuation **or** an app-tool kill hook, the `Reply` of the waiting
  foreground call (if any), the stored result, expiry. Keep every invariant of today's `deferred_`:
  - resumed one at a time; a re-entrant `resumeDeferred` returns at once (`draining_`); rounds;
  - token checked before a continuation runs and before its follow-up is queued; continuation destroyed right after
    it ran;
  - `agent_sessions_` declared before the table (it must outlive it);
  - a mutating job waits (state `waiting_for_user`), not fails, while the user has a modal dialog open.

  App tools today hold a `Reply` directly (e.g. `awaitLoad` connects to `FileLoader::loadFinished`). Give them a
  small job handle instead: `startLoad` registers a job with a kill hook calling `FileLoader::cancelLoad(ticket, …)`
  and routes the eventual result into the job (which forwards it to the foreground `Reply` if still attached, or
  stores it).
- **Job tools** (`jobs_list`, `job_wait`, `job_kill`) are implemented in `McpServer` (they read the table) but listed in
  **both** tool sets (`mcp::ToolSet::kFull` and `kAgentOnly`): extend `McpProtocol`'s tool lists, not `pj_agent_tools`.
- **`pj_app/src/mcp/McpBridge.cpp`**: a stdin reader thread so a notification can arrive while a call is in flight;
  `notifications/cancelled{requestId}` for the in-flight call → POST it to the instance, which kills the foreground
  call carrying that id. The server needs the JSON-RPC id per foreground call (today it is not tracked per caller).
- **Browser relay** (`McpBrowserLink`): no change in principle (it reaches the same `dispatch`), but the "connection"
  for D2 is the relay link; confirm a relay message for `notifications/cancelled` routes like a request.
- **UI (Phase 2)**: the status-bar agent-activity label (`McpServer`'s client indicator) opens a popup listing running
  jobs with a Stop button each. Prefer a `.ui` file (root `CLAUDE.md`, UI conventions).
- **Assistant (Phase 2, other repo `pj-official-plugins/toolbox_assistant_agent`)**: its Stop calls `job_kill` on the
  turn's jobs, or a host-side "kill all of this token's jobs" if that proves simpler.

## Phases

### Phase 1 — job table, job tools, kill for evaluations and file loads

1. `ToolResult::cancel` / `progress` hooks; fill them in `submitAndPoll`. Tests in `pj_runtime/tests/agent_tools_*`
   (runner `pj_agent_tools_tests`): kill releases the handle; progress is callable each tick.
2. Job table in `McpServer` replacing `deferred_` (same invariants), `wait_s` on job-capable tools
   (`evaluate`, `create_derived_object`, `create_markers` where it defers, `open_file`, `reload_file`), job tools,
   per-owner limits and expiry, revocation kills.
3. `open_file` / `reload_file` kill via `FileLoader::cancelLoad`; progress from the load's ingest
   (find the ticket → `SessionManager` ingest token mapping; `wireIngestProgress()` shows where the UI gets it).
4. Bridge reader thread and `notifications/cancelled` for foreground calls.
5. Update `MCP_SERVER.md` (tool sets, deferred results → jobs, the bridge paragraph), `pj_app/CLAUDE.md` (MCP
   invariants), tool descriptions (`open_file` now says it may return a job).

Acceptance (`pj_app/tests/mcp_server_test.cpp`, `McpTestPeer` already drives `resumeDeferred`):
- a call that ends within `wait_s` replies exactly as today (no job id); one that does not returns a job id;
- `job_wait` returns the final result; again after a second `job_wait` (repeatable); "no such job" after expiry;
- `job_kill` on an evaluation cancels the pool job; on a queued and on an active load ends it `killed`, with and
  without `keep_partial`;
- another owner's job id reads "no such job"; a plugin token sees only its own jobs;
- revocation kills running jobs and applies nothing later (extend the existing "closing public access" cases);
- under a modal dialog the job tools still run, `job_kill` waits like any mutation (or decide it is allowed: see
  open questions);
- a foreground call whose socket closes is dropped as today; a background job survives it;
- the existing re-entrancy tests (`ADrainThatAReplyOrContinuationReentersRunsNothing`, the revoked-follow-up cases)
  still pass unchanged.

### Phase 2 — streams, export, dialogs, UI

- `start_stream` as a job (D4); kill = stop.
- `waiting_for_user` state and D6 for `ask_user` dialogs and `unload_file`'s prompt.
- `export_series` on a worker thread (stage the columns on the GUI thread, write off it, as the `pj_runtime` pool
  invariant does for derived runs), cancellable; measure the GUI freeze before/after and report the number.
- Jobs popup with Stop; Assistant Stop (other repo).

### Phase 3 — protocol and follow-ups

- MCP Tasks (experimental utility in spec 2025-11-25: task-augmented requests, `tasks/get`, `tasks/result`,
  `tasks/list`, `tasks/cancel`, per-tool task support) mapped onto the same table, when a client negotiates it.
  Re-read the current spec first; this summary was written from memory.
- "Wait-for" jobs: end when a condition holds (`/imu/acc_z > 20`) or after a timeout, for stream monitoring.

## Decisions taken at implementation start (2026-11-13)

- **D8 `job_kill` runs under a user's modal dialog**, like the read-only job tools: it only stops agent work. D6's
  dialog close targets only the dialog the agent itself opened.
- **D9 `job_wait`'s `timeout_s` defaults to 20 and is clamped to [0, 60]**, well under the bridge's 300 s and typical
  client timeouts; an agent that needs longer calls `job_wait` again.
- Scope: all three phases, each closed by the milestone review loop (time logged below).

## Open questions

- Whether `wait_s=0` (always background) should be offered to agents explicitly in tool descriptions.
- Progress units for loads (bytes vs rows vs fraction) depend on what the ingest producer reports.

## Time log

| Phase | Start | End | Wall time |
|---|---|---|---|
| 1 | | | |
| 2 | | | |
| 3 | | | |

## Pitfalls already known

- Never spin a nested event loop in a continuation, kill hook or progress hook (`ToolResult::resume` contract).
- Revocation can happen from inside a reply or continuation: kill and drop one job at a time, as `resumeDeferred`
  does now.
- A tool replies exactly once: a job that was foreground and is now background must not also answer the original
  `Reply` later.
- `FileLoader::cancelLoad` from a non-GUI thread degrades to best effort; call it on the GUI thread.
- Host code stays domain-neutral: no plugin-specific branches in the job table or kill hooks.
