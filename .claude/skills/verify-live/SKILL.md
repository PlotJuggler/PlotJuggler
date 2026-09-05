---
name: verify-live
description: Use when confirming a PJ4 change actually works in the running app — visual, widget, layout, or performance changes; before claiming anything is fixed or verified; or when an edit appears to have had no effect after rebuilding.
---

# Verifying a change in the live app

## Overview

The two recorded failure modes: claiming a fix works when the file **never recompiled**, and claiming visual verification that never happened ("I don't see any difference" round-trips). The rule: **prove the rebuild, verify in the live app, and calibrate the claim to what you actually checked.**

## Step 1 — Prove the rebuild (always, before any conclusion)

```bash
./build.sh 2>&1 | tee /tmp/build.log
grep "Building.*<YourFile>.cpp.o" /tmp/build.log   # must appear, or the edit is NOT live
```

- RCC/QSS-only rebuilds mask a stalled C++ recompile — a missing `.cpp.o` line means "no effect" is expected, not mysterious.
- **Never chain `pkill …; ./build.sh`** — the chain exits 144 and skips compiling. Kill the app in one command, build in a separate one.

If the edit "had no effect", suspect this step before suspecting the code.

## Step 2 — Launch with real data

```bash
./run.sh                              # loads only marketplace-installed extensions
./run.sh --plugin-dir ../pj-official-plugins/build/all/Release/bin   # locally-built plugins
```

- `run.sh` already unsets `QT_IM_MODULE` (IBus segfault) and pins the bundled Qt plugins — launch through it, not the raw binary.
- Test data: `example.mcap` at the repo root; more under module test assets.
- Bound the run: `timeout 20 ./run.sh …` for scripted checks so nothing hangs the session.

## Step 3 — Observe by change type

**Visual / layout / widget:**
- Screenshot when cheap: `pj_app`'s `--screenshot` flag, or the qt-widgeteer harness (`3rdparty/qt-widgeteer`, in-process `WidgeteerClient`).
- **GNOME Wayland blocks external window capture** (grim etc. return nothing) — that's the environment, not the app. Use the in-app paths above or XWayland.
- Sizing looks wrong / C++ sizing calls ignored: that's QSS overriding C++ (`QStyleSheetStyle`). Diagnose in the **live** app with an env-gated `qWarning` dump of size/min/max/iconSize + `style()->className()` — offscreen tests use a different style and cannot reproduce it. Fix in QSS, not C++.

**Performance:**
- Produce a **number**: repaint/replot rate (must stay ≤60 Hz), frame time, on-CPU samples. Adjectives are not verification.
- Tools: `./run.sh --apitrace` (GL trace), `./run.sh --heaptrack` (heap), `perf` on the live PID (see the `perf-profile` skill).
- Suspect hot-path reads: anything calling `latestAt` inside a per-tick gate decompresses cold chunks — use `indexAt`/`entryTimestamps` and re-measure.

**Behavioral (non-visual):**
- Prefer an automated reproduction (gtest in the module) over eyeballing; the live run then confirms wiring, not logic.

## Step 4 — Calibrate the claim

| What you actually did | What you may say |
|---|---|
| Built + tests pass, never launched | "Tests pass; not verified in the live app" |
| Launched, exercised the feature, watched it | "Verified live: <what you saw>" |
| Screenshot taken | Attach it; say what it shows |
| Measured before/after | State both numbers and the setup |
| Offscreen/unit test only, for a visual change | "Logic covered by test; visual appearance unverified" |

Never imply visual verification that didn't happen — the honest weaker claim costs one sentence; the false stronger claim costs a round-trip and trust.

## Red flags

- Concluding "the fix didn't work" without the `Building …cpp.o` grep
- `pkill` and `./build.sh` in one command
- "Verified" in the summary with no launch in the transcript
- A performance claim containing no number
- Debugging a sizing bug in offscreen tests
