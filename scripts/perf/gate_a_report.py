#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Gate A table from a scripts/perf/gate_a.sh run directory.

    gate_a_report.py DIR [--self-test]

Reads DIR/raw/<round>/<config>__<case>.log, takes each metric's per-run value
(the benchmark's own median of its frames), and writes DIR/gate_a.md: the median
of the rounds with [min-max], the RHI GL / GL ratio and the verdict under the
Gate A rules (pj_scene3D/docs/TECHNICAL_NOTES.md §9):

  time (wall, GUI CPU):  RHI <= max(1.2 x GL, GL + 1 ms)
  memory (RSS, GPU):     RHI <= 1.10 x GL
  absolute budgets:      the row's own limit (a 16.7 ms streaming plot, ...)
  idle frames:           0

Verdicts are on RHI GL (Linux's native backend); RHI VK is for reference.
A row is "pass" only when every gated measurement exists in every round (any
config's round directory counts), from runs that exited cleanly (gate_a.sh
appends each run's exit status to its log): a missing or failed run makes it
"incomplete", unless a measurement that does exist already fails the row. The
idle row needs every case of IDLE_CASES in every round.
"""

import pathlib
import shlex
import statistics
import sys

CONFIGS = ("gl", "rhi_gl", "rhi_vk")


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


def time_ok(gl, rhi):
    return rhi <= max(1.2 * gl, gl + 1.0)


def mem_ok(gl, rhi):
    return rhi <= 1.10 * gl


# Each row: (label, [(case, key, rule)], note). rule: "time", "mem", ("max", limit),
# "zero", or None (reported, not gated). Plot rows: config gl is Qwt's GL canvas.
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


def verdict_of(runs, cells):
    """(ratio text, verdict) of one row, judged on RHI GL: pass, **fail** or incomplete."""
    ratios = []
    failed = False
    incomplete = False
    for case, key, rule in cells:
        gl = metric(runs, "gl", case, key)
        rhi = metric(runs, "rhi_gl", case, key)
        if rule is None:  # reported, not gated
            ratios.append(f"{rhi[0] / gl[0]:.2f}x" if rhi and gl and gl[0] else "–")
            continue
        if rhi is None:
            ratios.append("–")
            incomplete = True
            continue
        incomplete = incomplete or not complete(rhi)
        if isinstance(rule, tuple):
            ok = rhi[0] <= rule[1]
            ratios.append(f"limit {rule[1]:g}")
        elif gl is None or not gl[0]:
            ratios.append("–")
            incomplete = True
            continue
        else:
            incomplete = incomplete or not complete(gl)
            ok = time_ok(gl[0], rhi[0]) if rule == "time" else mem_ok(gl[0], rhi[0])
            text = f"{rhi[0] / gl[0]:.2f}x"
            if rule == "time" and not ok:
                text += f" (limit {max(1.2 * gl[0], gl[0] + 1.0):.1f})"
            ratios.append(text)
        failed = failed or not ok
    return " / ".join(ratios), "**fail**" if failed else "incomplete" if incomplete else "pass"


# The cases whose benchmark reports idle_frames (scene3d_frame_benchmark, the mixed layout).
IDLE_CASES = ("CloudStatic", "CloudStream", "CubesAndLinesOrbit", "MarkersPanFollow", "RobotTfAnimated",
              "Layers10", "Layers100", "Layers1000", "DockMovesCloud", "DockMovesRobot", "FirstShow", "mixed")


def idle_row(runs):
    """The worst idle_frames per config; RHI GL fails on any non-zero, and is
    incomplete unless every case of IDLE_CASES reported in every round."""
    rounds = rounds_of(runs)
    cells = {}
    incomplete = not rounds
    for config in CONFIGS:
        worst = None
        for case in IDLE_CASES:
            results = runs.get((config, case), {})
            for rnd in rounds:
                result = results.get(rnd)
                if result is None or "idle_frames" not in result:
                    incomplete = incomplete or config == "rhi_gl"
                    continue
                worst = max(worst or 0, int(result["idle_frames"]))
        cells[config] = "–" if worst is None else str(worst)
    if cells["rhi_gl"] not in ("0", "–"):
        return cells, "**fail**"
    return cells, "incomplete" if incomplete or cells["rhi_gl"] == "–" else "pass"


def report(out_dir):
    runs = load(out_dir)
    lines = [
        "| Row | GL build | RHI GL | Ratio (RHI GL) | Verdict | RHI VK |",
        "|---|---|---|---|---|---|",
    ]
    for label, cells in ROWS:
        if cells == "idle":
            values, verdict = idle_row(runs)
            lines.append(f"| {label} | {values['gl']} | {values['rhi_gl']} | – | {verdict} | {values['rhi_vk']} |")
            continue
        columns = {}
        for config in CONFIGS:
            columns[config] = " / ".join(
                fmt(metric(runs, config, case, key), spread=True) for case, key, _rule in cells)
        ratio, verdict = verdict_of(runs, cells)
        lines.append(
            f"| {label} | {columns['gl']} | {columns['rhi_gl']} | {ratio} | {verdict} | {columns['rhi_vk']} |")
    meta = out_dir / "meta.txt"
    header = meta.read_text() if meta.exists() else ""
    rounds = len(rounds_of(runs))
    text = (f"# Gate A\n\n```\n{header}```\n\nMedian of {rounds} rounds, [min–max] where the spread "
            "exceeds 10%. Plot rows: GL build = Qwt's GL canvas.\n\n" + "\n".join(lines) + "\n")
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

    runs = {("gl", "c"): by_round({"k": "1"}, {"k": "3"}, None), ("rhi_gl", "c"): by_round({"k": "4"})}
    assert metric(runs, "gl", "c", "k") == (2.0, 1.0, 3.0, 1)
    assert metric(runs, "rhi_gl", "c", "k") == (4.0, 4.0, 4.0, 2)  # rounds 2 and 3 never ran
    assert verdict_of(runs, [("c", "k", "time")])[1] == "**fail**"  # a real failure wins
    good = {("gl", "c"): by_round({"k": "10"}, {"k": "10"}), ("rhi_gl", "c"): by_round({"k": "11"}, {"k": "11"})}
    assert verdict_of(good, [("c", "k", "time")])[1] == "pass"
    assert verdict_of({}, [("c", "k", "time")])[1] == "incomplete"  # nothing ran
    no_rhi = {("gl", "c"): by_round({"k": "10"}), ("rhi_gl", "c"): by_round(None, None)}
    assert verdict_of(no_rhi, [("c", "k", "time")])[1] == "incomplete"  # every RHI run failed
    mixed = {("gl", "c"): by_round({"k": "10"}, {"k": "10"}), ("rhi_gl", "c"): by_round({"k": "10"}, None)}
    assert verdict_of(mixed, [("c", "k", "time")])[1] == "incomplete"  # a pass beside a failed run
    one_rhi_round = {("gl", "c"): by_round(*[{"k": "10"}] * 5), ("rhi_gl", "c"): by_round({"k": "10"})}
    assert verdict_of(one_rhi_round, [("c", "k", "time")])[1] == "incomplete"  # 4 RHI rounds missing
    one_gl_round = {("gl", "c"): by_round({"k": "10"}), ("rhi_gl", "c"): by_round(*[{"k": "10"}] * 5)}
    assert verdict_of(one_gl_round, [("c", "k", "time")])[1] == "incomplete"  # 4 baseline rounds missing
    no_base = {("rhi_gl", "c"): by_round({"k": "1"})}
    assert verdict_of(no_base, [("c", "k", "time")])[1] == "incomplete"  # no GL baseline
    assert verdict_of(no_base, [("c", "k", ("max", 2.0))])[1] == "pass"  # absolute limits need none
    assert verdict_of({("gl", "c"): by_round({"k": "1"})}, [("c", "k", None)])[1] == "pass"  # not gated
    idle = {("rhi_gl", case): by_round({"idle_frames": "0"}, {"idle_frames": "0"}) for case in IDLE_CASES}
    assert idle_row({})[1] == "incomplete"
    assert idle_row(idle)[1] == "pass"
    assert idle_row({("rhi_gl", "mixed"): by_round({"idle_frames": "0"})})[1] == "incomplete"  # other cases missing
    assert idle_row({**idle, ("rhi_gl", "mixed"): by_round({"idle_frames": "0"})})[1] == "incomplete"  # a round
    assert idle_row({**idle, ("rhi_gl", "mixed"): by_round({"idle_frames": "0"}, None)})[1] == "incomplete"
    assert idle_row({**idle, ("rhi_gl", "mixed"): by_round({"idle_frames": "2"}, None)})[1] == "**fail**"
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        raw = pathlib.Path(tmp) / "raw" / "1"
        raw.mkdir(parents=True)
        (raw / "rhi_gl__c.log").write_text("S3D_BENCH k=1\nGATE_RUN_EXIT=134\n")
        (raw / "gl__c.log").write_text("S3D_BENCH k=1\nGATE_RUN_EXIT=0\n")
        loaded = load(pathlib.Path(tmp))
        assert loaded[("rhi_gl", "c")] == {"1": None} and loaded[("gl", "c")] == {"1": {"k": "1"}}
    print("self-test ok")


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        self_test()
    else:
        report(pathlib.Path(sys.argv[1]))
