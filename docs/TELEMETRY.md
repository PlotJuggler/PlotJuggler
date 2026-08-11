# Anonymous usage statistics

PlotJuggler 4 sends **one small anonymous ping per launch** to
`https://app.plotjuggler.io/telemetry` to count daily users — the same
mechanism PlotJuggler 3 used, hardened for privacy.

## What is sent

| Field | Example | Purpose |
|---|---|---|
| `user_id` | `3f2c…` (SHA-256) | Salted hash of the OS machine id — counts unique machines; cannot be reversed or linked to other applications. |
| `os` / `os_version` | `ubuntu` / `24.04` | Platform-support decisions. |
| `version` | `4.0.1` | Adoption per release. |
| `installation` | `appimage` | Package-channel share (`source`, `appimage`, `deb`, `windows`). |
| `arch` | `x86_64` | Whether arm64 builds are worth shipping. |
| `display_server` | `wayland` | Linux only: Wayland vs X11 session share. |

**Never sent:** personal data, file names, paths, topic names, feature usage,
session timing, or any behavioral data. The full field list is enforced by a
unit test (`telemetry_ping_test`) — a payload change must update this
document and that test together.

## Opting out

Preferences → Appearance → **Anonymous usage statistics** (off = nothing is
ever sent). Headless `--screenshot` runs never send the ping.

The ping is also suppressed automatically when the `CI` environment variable
is set (GitHub Actions and virtually every CI system export it) or when
`PJ_DISABLE_TELEMETRY` is set (for non-CI automation such as docker builds or
test harnesses). CI runners have ephemeral machine ids, so without this guard
every automated launch would be counted as a new "user".

## Implementation

`pj_runtime`'s `TelemetryPing` (payload + transport); wired in
`pj_app/src/main.cpp` behind the `Preferences::send_anonymous_stats` setting. The packaging channel comes from
the `PJ_INSTALLATION` CMake cache variable (default `source`; release CI
stamps `appimage` / `deb` / `windows`).
