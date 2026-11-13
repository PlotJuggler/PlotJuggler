# Plan: agent tool calls as jobs (start, wait, kill)

Status: **retired** (2026-11-13). Built in this repository: the job table, `wait_s`, the job tools, kill for
evaluations, file loads, streams, exports and agent-opened dialogs, `notifications/cancelled` through the server and
the bridge, the user's view of running jobs, the MCP Tasks extension and `wait_for`. The design is in
[MCP_SERVER.md](../MCP_SERVER.md#jobs), which is the reference; this file is history.

## What was left to other repositories

- **The Assistant's Stop does not kill its turn's jobs** (other repo, `pj-official-plugins/toolbox_assistant_agent`).
  Stop cancels the backend process only, so a background job of that turn carries on. The smallest change: on Stop
  the plugin releases its endpoint (`pj.mcp_endpoint.v1` `release`), which revokes its token and so kills its jobs;
  the next turn's `acquire` gets a new one. That also kills jobs earlier turns left running: killing exactly one
  turn's jobs would need the plugin to track job ids.
- **The browser relay drops `notifications/cancelled` and does not speak MCP Tasks** (other repo,
  `plotjuggler-agent/relay/pj_mcp_relay.py`): it must forward notifications and `tasks/*` to the page, and advertise
  the extension in the `server/discover` reply it builds itself; the page side (`McpServer::handleRelayed`) already
  answers them.

## Decisions taken during implementation

- **D8 `job_kill` runs under a user's modal dialog**, like the read-only job tools: it only stops agent work. D6's
  dialog close targets only the dialog the agent itself opened.
- **D9 `job_wait`'s `timeout_s` defaults to 20 and is clamped to [0, 60]**, well under the bridge's 300 s and typical
  client timeouts; an agent that needs longer calls `job_wait` again.
- **D10 No `cancel` / `progress` hooks on `agent_tools::ToolResult`.** Destroying a continuation already releases its
  evaluation, and an evaluation reports no numeric progress, so the hooks would duplicate what exists.
- **D11 `create_markers` is not job-capable**: it never defers (it returns after `createMarkers`).
- **D12 `start_stream` answers when the stream runs, and its job lives as long as the stream** (D4 as built): the
  call is not held for the stream's life, nor for `wait_s` once the stream has started.
- **D13 Whether a job dies with its call is per tool**: an agent tool's and `unload_file`'s do; an import, a stream
  and an export carry on for their client to find.
- **D14 MCP Tasks is the 2026-07-28 extension** (`io.modelcontextprotocol/tasks`), not the 2025-11-25 core utility
  the plan first named: that one was moved out of the core and redesigned (no `tasks/result`, no `tasks/list`). No
  listed client implemented the extension when it was built here.
- **D15 `wait_for` is one agent tool**: one series, one comparison, a timeout; samples newer than the latest at the
  call.
- Scope: all three phases, each closed by the milestone review loop (time logged below).

## Open questions

- Whether a stream's job should report samples received as its progress.
- Whether the Assistant should kill only its current turn's jobs (see above).
- Whether `wait_for` should also wait for a topic that does not exist yet (today it fails at once, which catches a
  mistyped path).

## Time log

| Phase | Start | End | Wall time |
|---|---|---|---|
| 1 | 2026-11-13 19:35 | 2026-11-13 20:26 | 51 min (two Codex passes, one Claude review and a cleanup pass included) |
| 2 | 2026-11-13 20:26 | 2026-11-13 21:16 | 50 min (Codex approach check, Codex and Claude reviews, the before/after export measurement) |
| 3 | 2026-11-13 22:13 | 2026-11-13 22:38 | 25 min (Codex approach check, Codex and Claude reviews); the spec re-read came before, while Phase 2's reviews ran |

## Measurement kept from Phase 2

`export_series` of a 9.8-million-row single-series CSV, three runs per binary, probing `get_status` (answered on the
GUI thread) while the export ran: before (b9136ebd) the GUI was blocked for the whole export, 386-387 ms of a 0.39 s
call; after, the longest stall was 1 ms (17-18 probes, median 0.7 ms) and the export took 0.34-0.36 s.
