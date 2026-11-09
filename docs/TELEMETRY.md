# Anonymous usage statistics

PlotJuggler 4 reports two kinds of anonymous statistics, each with its own
switch in Preferences → Appearance:

- **The launch ping** (*Anonymous usage statistics*): one small ping per launch
  to `https://app.plotjuggler.io/telemetry` to count daily users — the same
  mechanism PlotJuggler 3 used, hardened for privacy. Environment facts only.
- **The daily usage report** (*Anonymous feature usage*): counters of which
  features and plugins were used, summed per UTC day on the machine and uploaded
  at a later launch. See [Daily usage report](#daily-usage-report).

Both follow the same build and automation gates below.

**Unpackaged and browser builds never send either:**

- **A build release packaging did not stamp** — `installation` is `source`
  (the `PJ_INSTALLATION` default, matched trimmed and case-insensitively) or
  empty. Every self-built tree carries it: developers, CI, and any automation
  that launches the real app. They all report the in-development version, so
  counting them invents users on a version that was never released.
- **The WebAssembly build.** A browser tab is a page view rather than an
  installed user, and it cannot be recognised across reloads (see Opting out),
  so counting one would inflate the installed base rather than measure it.

This is a denylist, not an allowlist: our own releases stamp `appimage`, `deb`
or `windows`, but any *other* non-empty stamp is sent as-is, so a downstream
packager who sets their own channel name still shows up rather than silently
vanishing. The endpoint should treat an unrecognised channel as exactly that —
a third-party package — and never expect `source`.

## What is sent

| Field | Example | Purpose |
|---|---|---|
| `user_id` | `3f2c…` (SHA-256) | Salted hash of the OS machine id — counts unique machines; cannot be reversed or linked to other applications. `QSysInfo::machineUniqueId()` is empty on platforms Qt does not implement it for, and the `QSettings` fallback is only as durable as the settings backend: a build whose settings do not survive a restart mints a fresh id every launch and inflates the unique-user count. |
| `os` / `os_version` | `ubuntu` / `24.04` | Platform-support decisions. |
| `version` | `4.0.1` | Adoption per release. |
| `installation` | `appimage` | Package-channel share. Our releases send `appimage`, `deb` or `windows`; a downstream package sends whatever it stamped. A `source` or empty stamp suppresses the ping entirely. |
| `arch` | `x86_64` | Whether arm64 builds are worth shipping. |
| `display_server` | `wayland` | Linux only: Wayland vs X11 session share. |

**Never sent by the ping:** personal data, file names, paths, topic names,
feature usage, session timing, or any behavioral data (feature usage is the
daily report's job, under its own switch). The full field list is enforced by a
unit test (`telemetry_ping_test`) — a payload change must update this
document and that test together.

## Daily usage report

Counters, never an event stream: the app adds up uses per UTC day on the
machine and uploads each finished day once, so a report carries no timestamp
finer than the day and no order of actions. Days are uploaded at a later launch
(in one POST to `https://app.plotjuggler.io/telemetry/v2/usage`) and deleted
locally after the server accepts them. At most 7 finished days are kept: an
offline machine drops older ones unsent.

The body is `{"schema": 2, "reports": [...]}`, one report per finished day:

| Field | Example | Purpose |
|---|---|---|
| `report_id` | random UUID | Minted with the day, so a retried upload can be dropped as a duplicate. Linked to nothing else. |
| `day` | `2026-11-08` | The UTC day counted. |
| `period_id` | `9a1e…` (SHA-256) | Salted hash of the machine id **and the month**: counts distinct machines per month, changes every month, and uses a salt of its own so it cannot be matched with the launch ping's `user_id`. |
| `version` / `os` / `arch` / `installation` | `4.1.0` / `windows` / `x86_64` / `windows` | As in the ping; `version` is the release that did the counting. |
| `features` | `{"layout.load": 2}` | Uses of each counted feature (table below). |
| `plugins` | `[{"kind": "toolbox", "action": "open", "id": "…", "n": 3}]` | Uses of each plugin (actions below). |
| `installed` | `[{"kind": "parser", "id": "…", "v": "1.0.3", "n": 1}]` | The plugins loaded that day, with their version. |

**Which plugins are named.** A plugin's id (and version) is sent only when the
plugin is public: compiled into this build, shipped in its bundled set, or
listed in the official marketplace registry (fetched for that purpose; a
user-configured registry never counts, since a company's own registry lists
exactly the in-house ids that must stay private). Every other plugin — from a
custom folder, `--plugin-dir`, or a local archive — is folded per family into
the id `_private`, its version dropped. If the registry cannot be fetched,
nothing is sent and the days wait for the next launch.

| Plugin `kind` | `action` | Counted when |
|---|---|---|
| `data_source` | `file` | a file was loaded through the plugin |
| `data_source` | `stream` | the plugin started streaming (also from a layout) |
| `parser` | `decode` | the plugin won the decoding of one topic (one per topic, not per message) |
| `toolbox` | `open` | the toolbox was launched |
| `widget` | `create` | the plugin created a Custom Widget panel (also from a layout) |

| Feature key | Counted when |
|---|---|
| `layout.load` | the user opened a layout file (dialog or recent list) |
| `layout.save` | the user saved a layout file |
| `recording.start` | a session recording started |
| `panel.scene3d` / `panel.scene2d` | a Scene3D / Scene2D panel was created (also from a layout) |

**Never sent:** file names, paths, topic or series names, plugin configuration,
timestamps finer than the day, the order of actions, or a private plugin's id
or version. The key set and the `_private` fold are enforced by
`usage_reporter_test`; a new counter means a wire key in `UsageCounters.cpp`,
a row above, and the server's allowlist.

**Locally**, the counters live in `usage_counters.json` in the app data folder
(`QStandardPaths::AppDataLocation`), raw plugin ids included — the folding
happens when a report is built. Turning the switch off deletes that file at
once; turning it on takes effect at the next launch.

### Server contract

The endpoint is served by the telemetry Worker (`PlotJuggler/pj-telemetry`);
`packaging/web/worker.mjs` forwards every path under `/telemetry/` to it. The
client treats any 2xx as "accepted, forget these days", so the Worker must:

- **Deduplicate** on `report_id` (a reply lost after a commit makes the client
  resend), and answer 2xx for an already-seen id.
- **Validate before storing**: reject an unknown `schema`; drop feature keys
  and plugin `kind`/`action` values outside the tables above; accept a plugin
  id only when it is in the official registry or `_private` (anything else
  becomes `_private`, a second barrier against leaking in-house ids); clamp
  counts; refuse a `day` more than 8 days old or in the future.
- **Store aggregates, not reports**, and no IP address.

A schema for Cloudflare D1 that does this, with daily rows upserted per report
(`reports` = machine-days, `uses` = summed counts):

```sql
CREATE TABLE seen_reports (report_id TEXT PRIMARY KEY, received_day TEXT NOT NULL);  -- purge after 14 days
CREATE TABLE daily_feature (day TEXT, version TEXT, os TEXT, feature TEXT,
  reports INTEGER NOT NULL, uses INTEGER NOT NULL, PRIMARY KEY (day, version, os, feature));
CREATE TABLE daily_plugin (day TEXT, kind TEXT, action TEXT, plugin_id TEXT, os TEXT,
  reports INTEGER NOT NULL, uses INTEGER NOT NULL, PRIMARY KEY (day, kind, action, plugin_id, os));
CREATE TABLE daily_installed (day TEXT, kind TEXT, plugin_id TEXT, plugin_version TEXT,
  machines INTEGER NOT NULL, PRIMARY KEY (day, kind, plugin_id, plugin_version));
-- Distinct machines per month: rows live only until the month is rolled up (a scheduled
-- job early the next month fills monthly_uniques, then deletes the month's presence rows).
CREATE TABLE month_presence (month TEXT, subject TEXT, period_id TEXT, PRIMARY KEY (month, subject, period_id));
CREATE TABLE monthly_uniques (month TEXT, subject TEXT, machines INTEGER NOT NULL, PRIMARY KEY (month, subject));
```

`subject` is `feature:<key>`, `plugin:<kind>:<id>` or `installed:<id>`. With
that, "which plugins are used" is `daily_plugin` (volume) and
`monthly_uniques` (reach), and "installed but unused" is `daily_installed`
against `daily_plugin`. No identifier outlives the month it counts.

## Opting out

Preferences → Appearance → **Anonymous usage statistics** (off = the ping is
never sent) and **Anonymous feature usage** (off = nothing is counted or sent,
and what was counted is deleted). Headless `--screenshot` runs send neither.

The ping is suppressed outright for an unpackaged build, as above — which
already covers every developer and CI launch of a build from this tree, and
does so in `TelemetryPing::send()` rather than at a call site, so no caller can
reintroduce the leak. The remaining guards protect a *packaged* build that
automation happens to launch.

The ping is also suppressed automatically when the `CI` environment variable
is set (GitHub Actions and virtually every CI system export it) or when
`PJ_DISABLE_TELEMETRY` is set (for non-CI automation such as docker builds or
test harnesses). CI runners have ephemeral machine ids, so without this guard
every automated launch would be counted as a new "user".

Automation that launches the real GUI inside a container must set
`PJ_DISABLE_TELEMETRY` itself: `docker run` does not forward the host's `CI`.
`packaging/*/smoke_test.sh` and `packaging/appimage/run_in_docker.sh` do.

**Neither guard can work in a browser**, which is the second reason the
WebAssembly build does not ping at all: `getenv` under Emscripten reads a
synthetic table that never inherits the host process environment, so `CI` is
invisible to the running module. The browser suites in `tests/wasm/` boot the
application dozens of times per run on US-hosted CI runners; they would have
been indistinguishable from real users. `tests/wasm/support/browser.js` adds a
second, independent barrier: Chromium fails resolution of every non-localhost
name (`MAP * ~NOTFOUND`) — note that mapping to `0.0.0.0` would NOT do this,
since the kernel rewrites a connect to the unspecified address as a connect to
loopback, quietly serving a foreign origin from the fixture server — and
Firefox sends every non-localhost request to a dead proxy on port 9.

Because a browser has neither guard nor a durable machine id, the Preferences
switch is hidden in that build rather than offering control over nothing.

## Implementation

`pj_runtime`'s `TelemetryPing` (payload + transport); wired in
`pj_app/src/main.cpp` behind the `Preferences::send_anonymous_stats` setting
and a `#ifndef PJ_TARGET_WASM` guard. The usage report is `pj_runtime`'s
`UsageCounters` (the process-wide `usageCounters()`, a no-op until enabled) and
`UsageReporter` (report building and upload), started by
`MainWindow::startUsageReporting` behind `Preferences::send_usage_stats` and
the same gates; both senders share the machine identity and channel gate in
`pj_runtime/src/TelemetryIdentity.cpp`. The packaging channel comes from
the `PJ_INSTALLATION` CMake cache variable (default `source`; release CI
stamps `appimage` / `deb` / `windows`).

The channel gate lives in `TelemetryPing::send()` and `UsageReporter::send()`
(pinned by each test's `UnpackagedBuildsNeverReachTheEndpoint`);
the user-facing opt-out and the automation guards stay in the shell beside the
other startup gates.
