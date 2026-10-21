#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Computes the summary counts of docs/derived_objects/SCENARIOS.md from its rows.

    scripts/derived_objects_scenario_counts.py --check   # exit 1 and print a diff when stale
    scripts/derived_objects_scenario_counts.py --fix     # rewrite the counts in place
    scripts/derived_objects_scenario_counts.py --self-test

A slice edits only its own scenario and gap rows; this script owns the "N scenario rows
covering M TESTs; K gaps" sentence and the per-area status table (Rows, keep, keep*,
changes, removed, decided here, and the Total row). M is read from the Coverage check
("TESTs in scope: M"), the one place that fact is written by hand.
"""

import argparse
import collections
import difflib
import re
import sys
from pathlib import Path

DEFAULT_FILE = Path(__file__).resolve().parent.parent / "docs/derived_objects/SCENARIOS.md"
COLUMNS = ["keep", "keep*", "changes", "removed", "decided here"]
STATUS = [  # (status cell prefix, column)
    ("keep*", "keep*"), ("keep", "keep"), ("**changes", "changes"),
    ("**removed", "removed"), ("**decided here**", "decided here"),
]
ROW_RE = re.compile(r"^\| ([A-Z]+)-(\d+) \|")
TABLE_RE = re.compile(r"^(\| \[(\w+)\]\(#\w+\) [^|]*)\|.*$")
SENTENCE_RE = re.compile(r"\d+ scenario rows covering \d+ TESTs; \d+ \[gaps\]")


class RowError(Exception):
    pass


def count(lines):
    """Returns ({area: Counter(column)}, gap count) from the scenario and gap rows."""
    areas, gaps = collections.defaultdict(collections.Counter), 0
    for number, line in enumerate(lines, 1):
        match = ROW_RE.match(line)
        if not match:
            continue
        if match[1] == "GAP":
            gaps += 1
            continue
        # The status is the cell before the Notes cell; the Test(s) cell may be absent.
        cells = [cell.strip() for cell in line.strip()[1:-1].split(" | ")]
        status = cells[-2] if len(cells) >= 5 else ""
        column = next((col for prefix, col in STATUS if status.startswith(prefix)), None)
        if column is None:
            raise RowError(f"line {number}: {match[1]}-{match[2]}: unknown status {status[:40]!r}"
                           " (expected keep, keep*, **changes: X**, **removed: X** or **decided here**)")
        areas[match[1]]["rows"] += 1
        areas[match[1]][column] += 1
    return areas, gaps


def recompute(text):
    lines = text.split("\n")
    areas, gaps = count(lines)
    in_scope = re.search(r"TESTs in scope: (\d+)", text)
    if not in_scope:
        raise RowError("no 'TESTs in scope: N' line in the Coverage check")
    total, seen = collections.Counter(), set()
    for index, line in enumerate(lines):
        table = TABLE_RE.match(line)
        if table and table[2] in areas:
            seen.add(table[2])
            cells = [areas[table[2]][name] for name in ["rows"] + COLUMNS]
            lines[index] = f"{table[1]}| " + " | ".join(map(str, cells)) + " |"
            total.update(areas[table[2]])
    if missing := sorted(set(areas) - seen):
        raise RowError(f"area {', '.join(missing)} has rows but no table line "
                       "('| [XX](#xx) Name | 0 | 0 | 0 | 0 | 0 | 0 |')")
    bold = " | ".join(f"**{total[name]}**" for name in ["rows"] + COLUMNS)
    text = "\n".join(re.sub(r"^\| \*\*Total\*\* \|.*$", f"| **Total** | {bold} |", line) for line in lines)
    sentence = f"{total['rows']} scenario rows covering {in_scope[1]} TESTs; {gaps} [gaps]"
    return SENTENCE_RE.sub(sentence, text, count=1)


def self_test():
    rows = ("| REC-01 | s | e | t | keep | n |\n| REC-02 | s | e | **changes: X** | n |\n"
            "| REC-03 | s | e | t | **decided here** — why | n |\n| GAP-01 | d | m | t |\n")
    doc = ("1 scenario rows covering 9 TESTs; 0 [gaps]\n\n| [REC](#rec) Recipes | 0 | 0 | 0 | 0 | 0 | 0 |\n"
           "| **Total** | **0** | **0** | **0** | **0** | **0** | **0** |\n\n" + rows + "TESTs in scope: 9.\n")
    fixed = recompute(doc)
    assert "3 scenario rows covering 9 TESTs; 1 [gaps]" in fixed, fixed
    assert "| [REC](#rec) Recipes | 3 | 1 | 0 | 1 | 0 | 1 |" in fixed, fixed
    assert "| **Total** | **3** | **1** | **0** | **1** | **0** | **1** |" in fixed, fixed
    assert recompute(fixed) == fixed
    try:
        recompute(doc.replace("**decided here**", "bogus"))
    except RowError as error:
        assert "REC-03" in str(error)
    else:
        raise AssertionError("an unknown status must be an error")
    print("self-test ok")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("file", nargs="?", type=Path, default=DEFAULT_FILE)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--fix", action="store_true")
    mode.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    with open(args.file, encoding="utf-8", newline="") as handle:
        text = handle.read()
    try:
        fixed = recompute(text)
    except RowError as error:
        sys.exit(f"{args.file}: {error}")
    if fixed == text:
        return
    if args.fix:
        with open(args.file, "w", encoding="utf-8", newline="") as handle:
            handle.write(fixed)
        print(f"{args.file}: counts rewritten")
        return
    sys.stdout.writelines(difflib.unified_diff(text.splitlines(True), fixed.splitlines(True), "current", "computed", n=0))
    sys.exit(f"{args.file}: summary counts are stale; run {Path(__file__).name} --fix")


if __name__ == "__main__":
    main()
