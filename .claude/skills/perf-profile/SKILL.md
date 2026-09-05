---
name: perf-profile
description: Use for ANY PJ4 performance question on Linux — sluggish plot zooming, dropped frames, GUI jank, a file-load or startup that "got slower", a suspected perf regression from a commit, "why is this slow" — before running `perf record`/`perf report` on plotjuggler4, or when a perf.data recording needs per-thread, flat, flamegraph or time-window views.
---

# CPU profiling PJ4 with perf

## Overview

Recorded failure modes: a 523 MB DWARF `perf.data` where **every** `perf report` took
minutes; the hybrid-CPU event split silently hiding half the samples behind `| head`;
DWARF user-stacks unwinding to nothing through stripped Qt while LBR resolved full
chains (`paintGL` → `QOpenGL2PaintEngineEx` → libgallium). The rule: **count first,
record cheap and single-view, unwind exactly once, never re-process.**

This machine: perf 7.0.12, i7-13700H hybrid (`cpu_core` = CPUs 0-11, `cpu_atom` = 12-19),
Arch LBR depth 32 on both PMUs, tracefs root-only, `sudo` prompts. Check
`cat /proc/sys/kernel/perf_event_paranoid` first: the recipes below assume ≤1; at ≥3
(the Debian/Ubuntu default) even user-space sampling of your own PID needs `sudo` or
`sudo sysctl kernel.perf_event_paranoid=1` for the session.

## Step -1 — Cheap wall-clock A/B before any capture

A suspected regression ("load went from 11 s to 38 s since PR #N") is settled by
a number, not a profile. Run the same headless load on both builds first; profile
only if the delta is real:

```bash
/usr/bin/time -v ./build/pj_app/plotjuggler4 --nosplash --layout <layout.pj4.xml> --exit-after-layout
#   -> "Elapsed (wall clock)" + "Maximum resident set size"; repeat on a worktree at the suspect base
```

## Step 0 — Environment (non-negotiable)

```bash
export DEBUGINFOD_URLS=          # Qt is stripped + absent from Ubuntu's server: leaving
                                 # this set re-queries the network every report (TTL 600s)
OUT=~/pjperf/$(date +%Y%m%d-%H%M%S); mkdir -p "$OUT"    # /home, NEVER /tmp (tmpfs=RAM)
```

## Step 0.5 — Count before you sample

```bash
PID=$(pgrep -x plotjuggler4)
perf top -p "$PID" -F 997                 # live triage, no file
perf stat -p "$PID" -I 1000               # this CPU emits TopdownL1 + IPC for free;
                                          # watch context-switches/s spike at the jank
```

## Step 1 — Attach and record (default: LBR)

```bash
taskset -acp 0-11 "$PID"   # pin to P-cores -> ONE PMU -> no hybrid split (unpin to re-verify)
perf record -F 997 --call-graph lbr -N --switch-events -p "$PID" -o "$OUT/rec.data"
```

- `-F 997` — prime; never 999/1000 (aliases with 60/120/144 Hz refresh → skewed samples).
- `lbr` — no frame pointers needed; unwinds through stripped Qt/Mesa; ~20× smaller files.
- `-N` — don't copy the ~380 MB binary into `~/.debug`; repeated runs fill the disk.
- `--switch-events` — free, unprivileged off-CPU timeline (gaps in hotspot's per-thread view).
- Need >32 frames or a `main()`-rooted flame graph → `--call-graph dwarf,8192 -F 499` instead.
- Ask for ONE tight interaction (~10-20 s); note wall-clock start/stop for windowing.
- Few samples? `perf record -vv` shows the configured freq/period.

## Step 2 — The ONE unwind pass

```bash
perf script -i "$OUT/rec.data" --header --no-inline \
    -F comm,tid,time,event,ip,sym,dso > "$OUT/all.script"
```
`--inline` is on by default and is a top analysis cost — always `--no-inline`.
Sanity-check the events present (two `cpu_*` lines = the hybrid split is live):
`grep -oE 'cpu_(core|atom)/[a-z]+/[A-Za-z]*|task-clock' "$OUT/all.script" | sort | uniq -c`

## Step 3 — Views (text processing only from here)

`views.sh` beside this file: per-second histogram, per-thread counts, flat leaves,
per-TID leaves, time-window cut. Interactive: `~/Apps/hotspot-v1.6.0-x86_64.AppImage
"$OUT/rec.data"` (per-thread timeline, drag a time range). Folded stacks without Perl:
`perf report -i "$OUT/rec.data" --no-inline --stdio -g folded,0` (`,0` — the default
0.5% threshold silently drops the tail). Caveat: LBR's 32-frame ceiling truncates
outer frames, so flame graphs render disconnected towers (hot sub-trees, still
readable); record DWARF when a rooted flame graph matters. Never pipe `perf report`
through `head` — each event section prints separately and you'll read only the first.

## Step 4 — Off-CPU when on-CPU explains nothing

Flat profile but frames still drop → the jank is a wait. `--switch-events` shows it as
gaps in hotspot's GUI-thread band. For the switch-out **stacks** without root
(`context-switches` is a software event): `perf record -e context-switches -c 1 -g
--call-graph lbr -p "$PID"`. Real wait-duration stacks need
`sudo offcputime-bpfcc -df -p "$PID" 30` — sudo prompts: hand it to the user.

## Step 5 — Interpretation checklist (Qt-specific), stop at first match

1. **Which thread?** PJ4 names none of its threads (`comm = plotjuggler4` for all);
   named ones are foreign: `<app>:gdrv0` = Mesa glthread, `QDBusConnection`, FFmpeg
   workers. Kernel frames print as raw `0xffffffff…` (kptr_restrict) — ignorable.
2. **GUI thread ≥ ~90% busy**, leaves in Qwt/QPainter/paint code → event-loop
   saturation. On the GL paint engine any **per-point QPainter call** is the classic
   cause: path stroking CPU-triangulates, pixmap blits convert+re-upload textures
   (fresh cacheKey per `QPixmap::toImage`). Batch the geometry instead.
3. **GUI thread idle but janky** → off-CPU (Step 4): swapBuffers/vsync, QMutex, I/O.
4. **Heavy `gdrv0`/libgallium** → driver-side; cross-check `./run.sh --apitrace`.
5. **Workers hot, GUI starved** → record with `--latency`, report `--latency`
   (parallelism-weighted overhead finds the serialized critical path).

## Do NOT try here (verified dead ends)

- `perf record -a`, tracepoints (`sched:*`, `--filter`), `perf sched`, `perf trace`,
  `perf probe` — all root-only on this box (perf_event_paranoid ≥1 + tracefs 0700).
- `perf record --off-cpu` — perf built without BPF skeletons; flag parses, then refuses.
- Hunting libunwind — DWARF unwinding here goes through libdw (`libunwind: OFF` is fine).
- Rebuilding Qt with frame pointers — LBR already unwinds through Qt; don't.

Performance claims need numbers (see `verify-live`): before/after sample share of the
named symbol, or replot rate ≤60 Hz — and say which views actually ran.
