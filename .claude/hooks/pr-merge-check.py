#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""PreToolUse hook (Bash): warn, never refuse, when `gh pr merge` runs before the PR's checks have all passed.

`main` has no required status checks, so a merge made seconds after `gh pr create` lands before CI has started.
Warns the agent and the user while any check is failing, pending or cancelled, and while a PR younger than
NO_CHECKS_GRACE_S has no checks yet (they register a few seconds after the push). The merge always proceeds.
"""
import json
import re
import shlex
import subprocess
import sys
from datetime import datetime, timezone

NO_CHECKS_GRACE_S = 600
VALUE_FLAGS = {"-R", "--repo", "-t", "--subject", "-b", "--body", "-F", "--body-file", "--match-head-commit",
               "-A", "--author-email"}


def merge_target(command):
    """(pr, repo_args) of the first `gh pr merge` in a shell command, or None when there is none."""
    m = re.search(r"\bgh\s+pr\s+merge\b([^;&|\n]*)", command)
    if not m:
        return None
    try:
        tokens = shlex.split(m.group(1))
    except ValueError:
        tokens = m.group(1).split()
    pr, repo, skip = None, [], False
    for i, t in enumerate(tokens):
        if skip:
            skip = False
            continue
        if t in VALUE_FLAGS:
            skip = True
            if t in ("-R", "--repo") and i + 1 < len(tokens):
                repo = ["--repo", tokens[i + 1]]
        elif t.startswith("--repo="):
            repo = ["--repo", t.split("=", 1)[1]]
        elif not t.startswith("-") and pr is None:
            pr = t
    return (pr, repo)


def check_state(c):
    """pass / pending / fail for one statusCheckRollup entry (a CheckRun or a commit StatusContext)."""
    if c.get("__typename") == "StatusContext":
        return {"SUCCESS": "pass", "PENDING": "pending", "EXPECTED": "pending"}.get(c.get("state"), "fail")
    if c.get("status") != "COMPLETED":
        return "pending"
    return "pass" if c.get("conclusion") in ("SUCCESS", "NEUTRAL", "SKIPPED") else "fail"


def gh(args, cwd):
    return subprocess.run(["gh", *args], cwd=cwd, capture_output=True, text=True, timeout=60)


def warn(reason):
    print(json.dumps({"systemMessage": f"pr-merge-check: {reason}",
                      "hookSpecificOutput": {"hookEventName": "PreToolUse", "additionalContext": reason}}))
    sys.exit(0)


def main():
    event = json.load(sys.stdin)
    command = (event.get("tool_input") or {}).get("command", "")
    target = merge_target(command)
    if target is None:
        return
    pr, repo = target
    cwd = event.get("cwd")
    pr_args = ([pr] if pr else []) + repo
    view = gh(["pr", "view", *pr_args, "--json", "statusCheckRollup,createdAt"], cwd)
    try:
        pr_json = json.loads(view.stdout)
    except json.JSONDecodeError:
        warn(f"could not read the PR's checks ({view.stderr.strip()[:200]}).")
    states = {}
    for c in pr_json.get("statusCheckRollup") or []:
        # Job names repeat across workflows (Linux CI and macOS CI both have `build`).
        key = "/".join(filter(None, (c.get("workflowName"), c.get("name") or c.get("context")))) or "?"
        states.setdefault(key, set()).add(check_state(c))
    if not states:
        created = datetime.fromisoformat(pr_json["createdAt"].replace("Z", "+00:00"))
        age = (datetime.now(timezone.utc) - created).total_seconds()
        if age < NO_CHECKS_GRACE_S:
            warn(f"merging a PR with no CI checks yet: it was opened {int(age)} s ago and CI has not registered.")
        return
    # A check re-run after a failure or a cancellation appears twice: it counts as passed once one run passed.
    blocking = [f"{name} ({'/'.join(sorted(s))})" for name, s in states.items() if "pass" not in s]
    if blocking:
        warn(f"merging before every CI check has passed: {', '.join(blocking)}.")


def selftest():
    assert merge_target("ls") is None
    assert merge_target("gh pr merge 1596 --squash 2>&1 | tail -3") == ("1596", [])
    assert merge_target('cd x && gh pr merge 7 --repo O/r --squash --subject "a b (#7)"') == ("7", ["--repo", "O/r"])
    assert merge_target("gh pr merge --squash --body 'x 3' --repo=O/r") == (None, ["--repo", "O/r"])
    assert merge_target("gh pr merge -R O/r 12 --auto") == ("12", ["--repo", "O/r"])
    print("ok")


if __name__ == "__main__":
    selftest() if sys.argv[1:] == ["--selftest"] else main()
