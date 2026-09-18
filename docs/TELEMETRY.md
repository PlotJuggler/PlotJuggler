# Anonymous usage statistics

PlotJuggler 4 sends **one small anonymous ping per launch** to
`https://app.plotjuggler.io/telemetry` to count daily users — the same
mechanism PlotJuggler 3 used, hardened for privacy.

**Unpackaged and browser builds never ping:**

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

**Never sent:** personal data, file names, paths, topic names, feature usage,
session timing, or any behavioral data. The full field list is enforced by a
unit test (`telemetry_ping_test`) — a payload change must update this
document and that test together.

## Opting out

Preferences → Appearance → **Anonymous usage statistics** (off = nothing is
ever sent). Headless `--screenshot` runs never send the ping.

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
been indistinguishable from real users. `tests/wasm/support/browser.js` forces
every non-localhost name to fail resolution (`MAP * ~NOTFOUND`) as a second,
independent barrier — note that mapping to `0.0.0.0` would NOT do this, since
the kernel rewrites a connect to the unspecified address as a connect to
loopback, quietly serving a foreign origin from the fixture server.

Because a browser has neither guard nor a durable machine id, the Preferences
switch is hidden in that build rather than offering control over nothing.

## Implementation

`pj_runtime`'s `TelemetryPing` (payload + transport); wired in
`pj_app/src/main.cpp` behind the `Preferences::send_anonymous_stats` setting
and a `#ifndef PJ_TARGET_WASM` guard. The packaging channel comes from
the `PJ_INSTALLATION` CMake cache variable (default `source`; release CI
stamps `appimage` / `deb` / `windows`).

The channel gate lives in `TelemetryPing::send()` (a single chokepoint,
pinned by `telemetry_ping_test`'s `UnpackagedBuildsNeverReachTheEndpoint`);
the user-facing opt-out and the automation guards stay in the shell beside the
other startup gates.
