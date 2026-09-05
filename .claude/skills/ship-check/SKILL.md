---
name: ship-check
description: Use when a PJ4 change is believed complete and it's time to prepare a commit or PR, when tests are red at finishing time and you must decide whether the failure is yours, or before any "done / ready to merge" claim.
---

# Finish-line checklist before commit / PR

## Overview

Recorded failure modes at the finish line: shipping with a stalled recompile, misreading a pre-existing red as a regression (or vice versa), hook-dirty commits after rebases, stale docs, and committing without approval. Run the gates **in order**; each exists because skipping it has cost real time.

**Gate 0 — approval is not yours to grant.** Never commit, push, or open a PR autonomously. The end state of this skill is a surfaced diff and a question, not a commit.

## Gate 1 — Build proof + full tests

```bash
./build.sh 2>&1 | tee /tmp/build.log        # then grep Building …cpp.o for your files
ctest --test-dir build --output-on-failure
```

## Gate 2 — Red-test triage (decision procedure, not judgment)

1. **Known-red?** `extension_manager_test.ConstructionReflectsStagedUpdateInOnePass` fails deterministically on clean checkouts (/tmp backup-rename). If your diff is outside `pj_marketplace`: note it, not a blocker.
2. **Pre-existing?** Reproduce on clean `origin/main` (primary checkout or fresh worktree). Red there too → pre-existing: **report it, don't fix it** unless asked, don't let it block.
3. **Yours.** Red only with your diff → fix before any "done" claim.
4. **Never** delete, weaken, `GTEST_SKIP`, or comment-out a failing test to get green. Surfacing a real failure is success; hiding it is the failure.

GL-test caveat: real-GL tests must self-skip below driver-reported GL 4.5 (`glGetString(GL_VERSION)`, not the requested format) — a Windows-CI-only shader failure is usually this, not your change.

## Gate 3 — Hooks and hygiene

```bash
pre-commit run --all-files
```

Mandatory after any rebase with manual conflict resolution (rebase skips hooks); fold fixes into the originating commit (`--fixup` + `--autosquash`).

## Gate 4 — Docs freshness

Behavior, public API, ABI structs, module ownership, or user-facing semantics changed? Then the owning module's `CLAUDE.md` / `docs/` must still be true — update in this same change or ask. **Do not commit known-stale docs.**

## Gate 5 — Diff self-review

Read the full diff once, checking exactly:

- [ ] No never-link-list violation (root CLAUDE.md → Dependency rules) in any `CMakeLists.txt` / `#include` change
- [ ] SPDX header on every new file
- [ ] No `.ui` `objectName` renamed
- [ ] No history comments ("previously", "changed from", "instead of", "NOT persisted")
- [ ] Host stayed domain-neutral (no plugin-specific terms in `pj_app`/`pj_runtime`/`conanfile.txt`)
- [ ] No leftover debug output, commented-out code, or scratch files

## Gate 6 — Surface and stop

Present: summary of the change, test results (including any known-red noted), the diff or `git diff --stat`, and the proposed commit message:

```
type(scope): subject          # feat|fix|chore|ci|docs|refactor|perf|test; scope = module/feature

<body: why + what; PR/issue refs>

Co-Authored-By: <model attribution>
```

Then **stop and ask**. Commit only on explicit confirmation in that turn.

## After approval — git execution traps

| Trap | Rule |
|---|---|
| `git mv` + edits | `git add` the new path again — "100% similarity" in output means you shipped a stale blob |
| Force-push | `--force-with-lease`, never bare `--force`; **reopen a closed PR before force-pushing** (after, reopen becomes impossible) |
| Squash-merge | Verify every intended commit landed: `git log --oneline <merge-commit>` |
| Merge from `origin/main` | Grep that feature entry points still have production callers — modify/delete conflicts drop wiring while CI stays green |
| `gh pr edit` GraphQL error | Fall back to `gh api repos/…/pulls/<n> -X PATCH -f …` |

## Reading CI

- **`gh run view --log-failed` usually prints nothing** (at run level and per job). Don't retry it — go straight to the job log:
  ```bash
  gh run view <run-id> --json jobs -q '.jobs[] | select(.conclusion=="failure") | "\(.databaseId) \(.name)"'
  gh api repos/PlotJuggler/PJ4/actions/jobs/<job-id>/logs | tr -d '\r' | grep -n -E "error|FAILED|Error:" | head -40
  ```
- **Windows CI is `continue-on-error`** even though Windows is a required target: read per-step status via `gh run view <id>`, never the run-level ✓.
- Windows red at `conan install` with "Cannot load recipe / conanfile.py not found" after a cache HIT = **poisoned ghcr Conan cache**, not your code → bump the `CACHE_VERSION` salt in `windows-ci.yml` (tag + GHA key + restore-keys).

## Rationalization table

| Excuse | Reality |
|---|---|
| "Only one test red, probably flaky" | Run the triage procedure; PJ4 has exactly one documented known-red |
| "Failure is in a module I didn't touch" | Still triage — step 2 takes two minutes and settles it |
| "I'll fix the docs in a follow-up PR" | Same change or an explicit question; stale docs don't ship |
| "Skip pre-commit, CI will catch it" | CI catching it costs a full round-trip; the hook costs seconds |
| "The user said run to completion" | Run-to-completion stops **at** the commit boundary, not through it |
| "Windows CI is green" | It's continue-on-error; green means nothing until you read the steps |
