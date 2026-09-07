#!/usr/bin/env bash
# Remove a PJ4 feature worktree created by ./scripts/worktree-new.sh and delete its
# branch. Refuses if the worktree has uncommitted TRACKED changes (so the
# force-remove below can't eat unpushed source); --force overrides that. The
# remove is always forced at the git level because the worktree carries
# submodules and a gitignored build/ that plain `git worktree remove` won't drop.
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: ./scripts/worktree-rm.sh <dir> [--keep-branch] [--force]

  <dir>          worktree dir under .worktrees/ (the name scripts/worktree-new.sh used)
  --keep-branch  remove the worktree but keep its local branch
  --force        skip the uncommitted-tracked-changes guard
  -h, --help     show this help

Tip: `git worktree list` shows the active worktrees and their dirs.
EOF
}

NAME=""
KEEP_BRANCH=0
FORCE=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h | --help) usage; exit 0 ;;
    --keep-branch) KEEP_BRANCH=1; shift ;;
    --force) FORCE=1; shift ;;
    -*) echo "worktree-rm: unknown flag: $1" >&2; usage >&2; exit 2 ;;
    *)
      if [[ -z "$NAME" ]]; then NAME="$1"; else echo "worktree-rm: unexpected argument: $1" >&2; exit 2; fi
      shift ;;
  esac
done

[[ -n "$NAME" ]] || { echo "worktree-rm: missing <dir>" >&2; usage >&2; exit 2; }

MAIN_REPO="$(dirname "$(cd "$(git rev-parse --git-common-dir)" && pwd)")"
WT="$MAIN_REPO/.worktrees/$NAME"

[[ -d "$WT" ]] || { echo "worktree-rm: no worktree at $WT (see: git worktree list)" >&2; exit 1; }

BRANCH="$(git -C "$WT" rev-parse --abbrev-ref HEAD 2>/dev/null || true)"

if [[ $FORCE == 0 && -n "$(git -C "$WT" status --porcelain --untracked-files=no)" ]]; then
  echo "worktree-rm: $WT has uncommitted tracked changes — commit/stash, or pass --force:" >&2
  git -C "$WT" status --short --untracked-files=no >&2
  exit 1
fi

echo "worktree-rm: removing $WT..."
git -C "$MAIN_REPO" worktree remove --force "$WT"
git -C "$MAIN_REPO" worktree prune

if [[ $KEEP_BRANCH == 0 && -n "$BRANCH" && "$BRANCH" != "HEAD" ]]; then
  if git -C "$MAIN_REPO" branch -D "$BRANCH"; then
    echo "worktree-rm: deleted branch $BRANCH"
  else
    echo "worktree-rm: kept branch $BRANCH (delete manually if you meant to)" >&2
  fi
fi

echo "worktree-rm: done."
