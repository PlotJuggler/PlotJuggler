#!/usr/bin/env bash
# Create a PJ4 feature worktree under .worktrees/, wired for fast builds:
#   - branch off origin/main (override with --base) per the repo convention;
#   - .qt symlinked by ABSOLUTE path to the primary checkout's Qt install
#     (a relative symlink would resolve inside the worktree, where it is absent);
#   - submodules initialized by borrowing objects from the primary checkout
#     (local + offline, with a GitHub fallback for commits it lacks).
# Set up only by default (seconds); pass --build to compile too.
# Tear down with ./scripts/worktree-rm.sh once the PR is merged.
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: ./scripts/worktree-new.sh <branch> [dir] [--base <ref>] [--build]

  <branch>      new branch, created on the worktree (e.g. fix/foo)
  [dir]         dir under .worktrees/ (default: branch basename, so
                fix/foo -> .worktrees/foo)
  --base <ref>  base to branch from (default: origin/main)
  --build       run ./build.sh in the worktree after setup
  -h, --help    show this help
EOF
}

BRANCH=""
DIR=""
BASE="origin/main"
DO_BUILD=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h | --help) usage; exit 0 ;;
    --base) BASE="${2:?--base needs a ref}"; shift 2 ;;
    --base=*) BASE="${1#*=}"; shift ;;
    --build) DO_BUILD=1; shift ;;
    -*) echo "worktree-new: unknown flag: $1" >&2; usage >&2; exit 2 ;;
    *)
      if [[ -z "$BRANCH" ]]; then
        BRANCH="$1"
      elif [[ -z "$DIR" ]]; then
        DIR="$1"
      else
        echo "worktree-new: unexpected argument: $1" >&2; exit 2
      fi
      shift ;;
  esac
done

[[ -n "$BRANCH" ]] || { echo "worktree-new: missing <branch>" >&2; usage >&2; exit 2; }
[[ -n "$DIR" ]] || DIR="${BRANCH##*/}"

# The primary checkout holds .qt + the populated submodules. Resolve it whether
# we're invoked from it or from another worktree (both share one git dir).
MAIN_REPO="$(dirname "$(cd "$(git rev-parse --git-common-dir)" && pwd)")"
source "${MAIN_REPO}/versions.env"
WT="$MAIN_REPO/.worktrees/$DIR"

[[ ! -e "$WT" ]] || { echo "worktree-new: $WT already exists" >&2; exit 1; }
if git -C "$MAIN_REPO" show-ref --verify --quiet "refs/heads/$BRANCH"; then
  echo "worktree-new: branch '$BRANCH' already exists" >&2; exit 1
fi

QT_SRC="$(readlink -f "$MAIN_REPO/.qt")"
[[ -d "$QT_SRC/${PJ_QT_VERSION}/gcc_64" ]] ||
  echo "worktree-new: WARNING: $QT_SRC/${PJ_QT_VERSION}/gcc_64 missing — run ./scripts/install_qt6.sh in the primary checkout" >&2

echo "worktree-new: fetching origin..."
git -C "$MAIN_REPO" fetch origin --quiet

echo "worktree-new: creating $WT (branch '$BRANCH' off $BASE)..."
git -C "$MAIN_REPO" worktree add -b "$BRANCH" "$WT" "$BASE"

ln -s "$QT_SRC" "$WT/.qt"

echo "worktree-new: initializing submodules (borrowing objects from the primary checkout)..."
while read -r _key path; do
  ref="$MAIN_REPO/$path"
  if [[ -e "$ref/.git" ]]; then
    # --reference borrows the primary checkout's objects (fast, offline) but
    # keeps the real URL, so a commit it lacks is still fetched from GitHub.
    git -C "$WT" submodule update --init --reference "$ref" -- "$path" >/dev/null
  else
    git -C "$WT" submodule update --init -- "$path"
  fi
done < <(git -C "$WT" config -f "$WT/.gitmodules" --get-regexp '^submodule\..*\.path$')
# Pick up nested submodules (if any) and anything skipped above.
git -C "$WT" submodule update --init --recursive >/dev/null

if [[ $DO_BUILD == 1 ]]; then
  echo "worktree-new: building..."
  (cd "$WT" && ./build.sh)
fi

echo
echo "worktree-new: ready -> $WT"
echo "  build:  cd $WT && ./build.sh"
echo "  remove: ./scripts/worktree-rm.sh $DIR   (after the PR merges)"
