#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Gate A table from a scripts/perf/gate_a.sh run directory.

    gate_a_report.py DIR [--baseline RUN_DIR] [--self-test]

Reads DIR/raw/<round>/<config>__<case>.log, takes each metric's per-run value
(the benchmark's own median of its frames), and writes DIR/gate_a.md: the median
of the rounds with [min-max], the candidate / reference ratio and the verdict
under the Gate A rules (pj_scene3D/docs/TECHNICAL_NOTES.md §9):

  time (wall, GUI CPU):  RHI <= max(1.2 x REF, REF + 1 ms)
  memory (RSS, GPU):     RHI <= 1.10 x REF
  absolute budgets:      the row's own limit (a 16.7 ms streaming plot, ...)
  idle frames:           0

The candidate is config rhi_gl (Linux's native backend); rhi_vk is for
reference. The reference REF is RUN_DIR's rhi_gl runs (a frozen baseline run)
or, without --baseline, DIR's config base (a baseline tree measured in the same
rounds). A gated row is also "**regressed**" when, over at least 3 complete
rounds each, every candidate round is worse than every
baseline round (min > max) and the median by more than 2%: a reproducible
regression the broad ratios above would let through.

A row is "pass" only when every gated measurement exists in every round (any
config's round directory of the same run counts), from runs that exited cleanly
(gate_a.sh appends each run's exit status to its log): a missing or failed run
makes it "incomplete", unless a measurement that does exist already fails the
row. The idle row needs every case of IDLE_CASES in every round.
"""

import argparse
import pathlib
import shlex
import statistics
from typing import NamedTuple


class Reference(NamedTuple):
    """Where the reference column comes from: `runs` (a load()) and its config."""
    runs: dict
    config: str
    label: str


def parse_line(line):
    """key=value tokens of one benchmark result line ('device="a b"' allowed)."""
    values = {}
    for token in shlex.split(line):
        if "=" in token:
            key, value = token.split("=", 1)
            values[key] = value
    return values


def load(out_dir):
    """{(config, case): {round: values dict, None for a failed run}} from every raw log.

    A run failed when it printed no result line or exited non-zero (gate_a.sh's
    "GATE_RUN_EXIT=<status>" line; logs without it are judged by their result)."""
    runs = {}
    for log in sorted(out_dir.glob("raw/*/*.log")):
        config, case = log.stem.split("__", 1)
        result = None
        exit_status = 0
        for line in log.read_text(errors="replace").splitlines():
            if line.startswith(("S3D_BENCH ", "MIXED_BENCH ", "MIXED_TAB ", "canvas=")):
                result = parse_line(line)
            elif line.startswith("GATE_RUN_EXIT="):
                exit_status = int(line.split("=", 1)[1] or 1)
        runs.setdefault((config, case), {})[log.parent.name] = result if exit_status == 0 else None
    return runs


def rounds_of(runs):
    """Every round any run has: the coverage each (config, case) must have."""
    return {rnd for results in runs.values() for rnd in results}


def metric(runs, config, case, key):
    """(median, min, max, missing rounds) of `key` over the rounds; None when absent.
    A round is missing when its run failed, lacks `key`, or did not run."""
    values = []
    failed = 0
    results = runs.get((config, case), {})
    for rnd in rounds_of(runs):
        result = results.get(rnd)
        if result is None:
            failed += 1
            continue
        try:
            values.append(float(result[key]))
        except (KeyError, ValueError):
            failed += 1
    if not values:
        return None
    return statistics.median(values), min(values), max(values), failed


def fmt(m, spread=False, digits=1):
    if m is None:
        return "–"
    text = f"{m[0]:.{digits}f}"
    if spread and m[2] - m[1] > 0.1 * max(abs(m[0]), 1e-9):
        text += f" [{m[1]:.{digits}f}–{m[2]:.{digits}f}]"
    if m[3]:
        text += f" ({m[3]} missing)"
    return text


def time_ok(ref, rhi):
    return rhi <= max(1.2 * ref, ref + 1.0)


def mem_ok(ref, rhi):
    return rhi <= 1.10 * ref


# Each row: (label, [(case, key, rule)], note). rule: "time", "mem", ("max", limit),
# or None (reported, not gated); the idle row is "idle".
ROWS = [
    ("Cloud 10M static: wall / CPU ms", [("CloudStatic", "wall_ms", "time"), ("CloudStatic", "cpu_ms", "time")]),
    ("Cloud 10M static: RSS MiB", [("CloudStatic", "rss_mib", "mem")]),
    ("Cloud 10M GPU (DRM; DPR 2) MiB ≤ 550", [("CloudStatic", "gpu_drm_mib", None),
                                              ("CloudStaticDpr2", "gpu_drm_mib", ("max", 550.0))]),
    ("Cloud stream 300k/frame: wall ms", [("CloudStream", "wall_ms", "time")]),
    ("Cloud stream: CPU ms / RSS MiB", [("CloudStream", "cpu_ms", "time"), ("CloudStream", "rss_mib", "mem")]),
    ("Cloud stream: ingestion ms (not gated)", [("CloudStream", "ingest_ms", None)]),
    ("100k cubes + 1M line vertices, orbit: wall / CPU ms",
     [("CubesAndLinesOrbit", "wall_ms", "time"), ("CubesAndLinesOrbit", "cpu_ms", "time")]),
    ("Cubes + lines: RSS / peak MiB", [("CubesAndLinesOrbit", "rss_mib", "mem"),
                                       ("CubesAndLinesOrbit", "hwm_mib", None)]),
    ("10k markers, pan + follow: wall ms", [("MarkersPanFollow", "wall_ms", "time")]),
    ("Markers: GUI CPU ms", [("MarkersPanFollow", "cpu_ms", "time")]),
    ("50-link robot, TF each frame: wall / CPU ms",
     [("RobotTfAnimated", "wall_ms", "time"), ("RobotTfAnimated", "cpu_ms", "time")]),
    ("10 / 100 / 1000 layers: wall ms",
     [("Layers10", "wall_ms", "time"), ("Layers100", "wall_ms", "time"), ("Layers1000", "wall_ms", "time")]),
    ("1000 layers: RSS MiB, first show ms",
     [("Layers1000", "rss_mib", "mem"), ("Layers1000", "first_show_ms", "time")]),
    ("10M cloud dock: float / redock / hide-show ms",
     [("DockMovesCloud", "float_ms", "time"), ("DockMovesCloud", "redock_ms", "time"),
      ("DockMovesCloud", "tab_ms", "time")]),
    ("50-link robot dock: float / redock / hide-show ms",
     [("DockMovesRobot", "float_ms", "time"), ("DockMovesRobot", "redock_ms", "time"),
      ("DockMovesRobot", "tab_ms", "time")]),
    ("ADS dock-area tab switch, to 9.9M cloud / to plot: ms",
     [("tab_switch", "tab_ms", "time"), ("tab_switch", "plot_tab_ms", "time")]),
    ("First show, 10M cloud + robot, ms", [("FirstShow", "first_show_ms", "time")]),
    ("Idle frames when static, every case", "idle"),
    ("Plots: dashed 5 x 100k, wall ms", [("plot_dashed", "wall_ms", "time")]),
    ("Plots: 50 x 100k, wall ms", [("plot_many_curves", "wall_ms", "time")]),
    ("Plots: tracker only, 10 x 100k, wall ms", [("plot_tracker_only", "wall_ms", "time")]),
    ("Plot GPU memory, 1600x900, DPR 1 / 2 (MiB)",
     [("plot_static_1m", "gpu_drm_mib", "mem"), ("plot_static_1m_dpr2", "gpu_drm_mib", "mem")]),
    ("Streaming plot, 10 x 100k, ≤ 16.7 ms", [("plot_streaming", "wall_ms", ("max", 16.7))]),
    ("Mixed: 6 plots + Scene3D, wall / CPU ms", [("mixed", "wall_ms", "time"), ("mixed", "cpu_ms", "time")]),
    ("Mixed: RSS MiB", [("mixed", "rss_mib", "mem")]),
]


def complete(measured):
    """`measured` (a metric()) has a value in every round."""
    return measured is not None and measured[3] == 0


# A regression against a QRhi baseline must show in every one of at least this
# many rounds on both sides, and move the median by more than REGRESSION_MARGIN.
REGRESSION_ROUNDS = 3
REGRESSION_MARGIN = 0.02


def reproducibly_worse(ref, runs, base, rhi):
    """Every candidate round is worse than every baseline round (min > max), over
    complete runs of REGRESSION_ROUNDS or more, by more than the margin: outside
    measurement noise, where the broad Gate A ratios would still pass."""
    enough = min(len(rounds_of(ref.runs)), len(rounds_of(runs))) >= REGRESSION_ROUNDS
    return (enough and complete(base) and complete(rhi) and rhi[1] > base[2] and
            rhi[0] > (1.0 + REGRESSION_MARGIN) * base[0])


def verdict_of(ref, runs, cells):
    """(ratio text, verdict) of one row, judged on the candidate's rhi_gl against
    `ref`: pass, **fail**, **regressed** or incomplete."""
    ratios = []
    failed = False
    regressed = False
    incomplete = False
    for case, key, rule in cells:
        base = metric(ref.runs, ref.config, case, key)
        rhi = metric(runs, "rhi_gl", case, key)
        if rule is None:  # reported, not gated
            ratios.append(f"{rhi[0] / base[0]:.2f}x" if rhi and base and base[0] else "–")
            continue
        if rhi is None:
            ratios.append("–")
            incomplete = True
            continue
        incomplete = incomplete or not complete(rhi)
        if isinstance(rule, tuple):
            ok = rhi[0] <= rule[1]
            ratios.append(f"limit {rule[1]:g}")
        elif base is None or not base[0]:
            ratios.append("–")
            incomplete = True
            continue
        else:
            incomplete = incomplete or not complete(base)
            ok = time_ok(base[0], rhi[0]) if rule == "time" else mem_ok(base[0], rhi[0])
            text = f"{rhi[0] / base[0]:.2f}x"
            if rule == "time" and not ok:
                text += f" (limit {max(1.2 * base[0], base[0] + 1.0):.1f})"
            if reproducibly_worse(ref, runs, base, rhi):
                regressed = True
                text += " (every round worse)"
            ratios.append(text)
        failed = failed or not ok
    verdict = "**fail**" if failed else "**regressed**" if regressed else "incomplete" if incomplete else "pass"
    return " / ".join(ratios), verdict


# The cases whose benchmark reports idle_frames (scene3d_frame_benchmark, the mixed layout).
IDLE_CASES = ("CloudStatic", "CloudStream", "CubesAndLinesOrbit", "MarkersPanFollow", "RobotTfAnimated",
              "Layers10", "Layers100", "Layers1000", "DockMovesCloud", "DockMovesRobot", "FirstShow", "mixed")


def worst_idle(runs, config):
    """(worst idle_frames of `config` over IDLE_CASES and rounds or None, whether any is missing)."""
    worst = None
    missing = False
    for case in IDLE_CASES:
        results = runs.get((config, case), {})
        for rnd in rounds_of(runs):
            result = results.get(rnd)
            if result is None or "idle_frames" not in result:
                missing = True
                continue
            worst = max(worst or 0, int(result["idle_frames"]))
    return worst, missing


def columns(ref, runs):
    """The table's (runs, config) per column: the reference, the candidate on OpenGL, on Vulkan."""
    return {"ref": (ref.runs, ref.config), "rhi_gl": (runs, "rhi_gl"), "rhi_vk": (runs, "rhi_vk")}


def idle_row(ref, runs):
    """The worst idle_frames per column; rhi_gl fails on any non-zero, and is
    incomplete unless every case of IDLE_CASES reported in every round."""
    cells = {}
    for column, (column_runs, config) in columns(ref, runs).items():
        worst, missing = worst_idle(column_runs, config)
        cells[column] = "–" if worst is None else str(worst)
        if column == "rhi_gl":
            incomplete = missing or not rounds_of(runs)
    if cells["rhi_gl"] not in ("0", "–"):
        return cells, "**fail**"
    return cells, "incomplete" if incomplete or cells["rhi_gl"] == "–" else "pass"


def reference_of(runs, baseline_dir):
    """The reference column: a baseline run directory's rhi_gl, else this run's base."""
    if baseline_dir is not None:
        return Reference(load(baseline_dir), "rhi_gl", f"Baseline ({baseline_dir.name})")
    return Reference(runs, "base", "Baseline")


def report(out_dir, baseline_dir=None):
    runs = load(out_dir)
    ref = reference_of(runs, baseline_dir)
    lines = [
        f"| Row | {ref.label} | RHI GL | Ratio (RHI GL) | Verdict | RHI VK |",
        "|---|---|---|---|---|---|",
    ]
    for label, cells in ROWS:
        if cells == "idle":
            values, verdict = idle_row(ref, runs)
            lines.append(f"| {label} | {values['ref']} | {values['rhi_gl']} | – | {verdict} | {values['rhi_vk']} |")
            continue
        values = {
            column: " / ".join(fmt(metric(column_runs, config, case, key), spread=True) for case, key, _rule in cells)
            for column, (column_runs, config) in columns(ref, runs).items()
        }
        ratio, verdict = verdict_of(ref, runs, cells)
        lines.append(
            f"| {label} | {values['ref']} | {values['rhi_gl']} | {ratio} | {verdict} | {values['rhi_vk']} |")
    meta = out_dir / "meta.txt"
    header = meta.read_text() if meta.exists() else ""
    rounds = len(rounds_of(runs))
    text = (f"# Gate A\n\n```\n{header}```\n\nMedian of {rounds} rounds, [min–max] where the spread "
            f"exceeds 10%. Reference: {ref.label}, {len(rounds_of(ref.runs))} rounds.\n\n" + "\n".join(lines) + "\n")
    (out_dir / "gate_a.md").write_text(text)
    print(text)


def self_test():
    assert parse_line('S3D_BENCH case=x device="Mesa Intel(R) Xe" wall_ms=2.5') == {
        "case": "x", "device": "Mesa Intel(R) Xe", "wall_ms": "2.5"}
    assert time_ok(10.0, 12.0) and not time_ok(10.0, 12.1)
    assert time_ok(2.0, 3.0) and not time_ok(2.0, 3.1)
    assert mem_ok(100.0, 110.0) and not mem_ok(100.0, 110.5)
    def by_round(*results):  # {round: result}, rounds numbered from 1
        return {str(i + 1): result for i, result in enumerate(results)}

    def verdict(runs, cells):  # judged against this run's base
        return verdict_of(Reference(runs, "base", "base"), runs, cells)[1]

    runs = {("base", "c"): by_round({"k": "1"}, {"k": "3"}, None), ("rhi_gl", "c"): by_round({"k": "4"})}
    assert metric(runs, "base", "c", "k") == (2.0, 1.0, 3.0, 1)
    assert metric(runs, "rhi_gl", "c", "k") == (4.0, 4.0, 4.0, 2)  # rounds 2 and 3 never ran
    assert verdict(runs, [("c", "k", "time")]) == "**fail**"  # a real failure wins
    good = {("base", "c"): by_round({"k": "10"}, {"k": "10"}), ("rhi_gl", "c"): by_round({"k": "11"}, {"k": "11"})}
    assert verdict(good, [("c", "k", "time")]) == "pass"
    assert verdict({}, [("c", "k", "time")]) == "incomplete"  # nothing ran
    no_rhi = {("base", "c"): by_round({"k": "10"}), ("rhi_gl", "c"): by_round(None, None)}
    assert verdict(no_rhi, [("c", "k", "time")]) == "incomplete"  # every RHI run failed
    mixed = {("base", "c"): by_round({"k": "10"}, {"k": "10"}), ("rhi_gl", "c"): by_round({"k": "10"}, None)}
    assert verdict(mixed, [("c", "k", "time")]) == "incomplete"  # a pass beside a failed run
    one_rhi_round = {("base", "c"): by_round(*[{"k": "10"}] * 5), ("rhi_gl", "c"): by_round({"k": "10"})}
    assert verdict(one_rhi_round, [("c", "k", "time")]) == "incomplete"  # 4 RHI rounds missing
    one_base_round = {("base", "c"): by_round({"k": "10"}), ("rhi_gl", "c"): by_round(*[{"k": "10"}] * 5)}
    assert verdict(one_base_round, [("c", "k", "time")]) == "incomplete"  # 4 baseline rounds missing
    no_base = {("rhi_gl", "c"): by_round({"k": "1"})}
    assert verdict(no_base, [("c", "k", "time")]) == "incomplete"  # no reference
    assert verdict(no_base, [("c", "k", ("max", 2.0))]) == "pass"  # absolute limits need none
    assert verdict({("base", "c"): by_round({"k": "1"})}, [("c", "k", None)]) == "pass"  # not gated
    # Within the broad ratio but worse in every round: a regression.
    def rounds(*values):
        return by_round(*[{"k": str(v)} for v in values])

    base = {("base", "c"): rounds(10, 10.2, 10.1), ("rhi_gl", "c"): rounds(10.5, 11, 10.6)}
    assert verdict(base, [("c", "k", "time")]) == "**regressed**"
    noisy = {("base", "c"): rounds(10, 11, 10), ("rhi_gl", "c"): rounds(10.5, 11, 10.6)}
    assert verdict(noisy, [("c", "k", "time")]) == "pass"  # overlapping rounds: noise
    two = {("base", "c"): rounds(10, 10.2), ("rhi_gl", "c"): rounds(10.5, 11)}
    assert verdict(two, [("c", "k", "time")]) == "pass"  # too few rounds to tell
    tiny = {("base", "c"): rounds(245.7, 245.7, 245.7), ("rhi_gl", "c"): rounds(245.8, 245.8, 245.8)}
    assert verdict(tiny, [("c", "k", "mem")]) == "pass"  # within the margin
    idle = {("rhi_gl", case): by_round({"idle_frames": "0"}, {"idle_frames": "0"}) for case in IDLE_CASES}
    def idle_verdict(runs):
        return idle_row(Reference(runs, "base", "base"), runs)[1]
    assert idle_verdict({}) == "incomplete"
    assert idle_verdict(idle) == "pass"
    assert idle_verdict({("rhi_gl", "mixed"): by_round({"idle_frames": "0"})}) == "incomplete"  # other cases missing
    assert idle_verdict({**idle, ("rhi_gl", "mixed"): by_round({"idle_frames": "0"})}) == "incomplete"  # a round
    assert idle_verdict({**idle, ("rhi_gl", "mixed"): by_round({"idle_frames": "0"}, None)}) == "incomplete"
    assert idle_verdict({**idle, ("rhi_gl", "mixed"): by_round({"idle_frames": "2"}, None)}) == "**fail**"
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        raw = pathlib.Path(tmp) / "raw" / "1"
        raw.mkdir(parents=True)
        (raw / "rhi_gl__c.log").write_text("S3D_BENCH k=1\nGATE_RUN_EXIT=134\n")
        (raw / "base__c.log").write_text("S3D_BENCH k=1\nGATE_RUN_EXIT=0\n")
        loaded = load(pathlib.Path(tmp))
        assert loaded[("rhi_gl", "c")] == {"1": None} and loaded[("base", "c")] == {"1": {"k": "1"}}
        # A baseline run directory: its rhi_gl runs, over its own rounds.
        (raw.parent / "2").mkdir()
        (raw.parent / "2" / "rhi_gl__c.log").write_text("S3D_BENCH k=3\nGATE_RUN_EXIT=0\n")
        ref = reference_of({("rhi_gl", "c"): by_round({"k": "1"})}, pathlib.Path(tmp))
        assert ref.config == "rhi_gl" and metric(ref.runs, ref.config, "c", "k") == (3.0, 3.0, 3.0, 1)
        assert reference_of({("base", "c"): {}}, None).config == "base"
    print("self-test ok")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("dir", nargs="?", type=pathlib.Path)
    parser.add_argument("--baseline", type=pathlib.Path, help="a frozen gate_a.sh run whose rhi_gl is the reference")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    elif args.dir is None:
        parser.error("DIR is required")
    else:
        report(args.dir, args.baseline)
