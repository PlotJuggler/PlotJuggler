#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
# Copy the working tree and Git metadata into a stable build context.
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: packaging/snap/build.sh [--context DIR] [--clean]

  --context DIR   Stable generated context outside this checkout (default: user cache).
  --clean         Clean the cached plotjuggler part before building.
  -h, --help      Show this help.

The working tree and .git are copied, including staged, unstaged and untracked files.
The root build/ directory and packaging/snap/dist/ are excluded.
Existing context contents are retained; a non-empty context produces a warning.
Packages are written to packaging/snap/dist/. Builds use LXD.
EOF
}

fail() { echo "snap build: ${*}" >&2; exit 1; }

packaging=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
root=$(cd -- "${packaging}/../.." && pwd -P)
context=
clean=false

while ((${#})); do
  case "${1}" in
    --context)
      ((${#} >= 2)) && [[ -n "${2}" ]] || fail "${1} needs a value"
      context=${2}
      shift 2 ;;
    --clean) clean=true; shift ;;
    -h|--help) usage; exit 0 ;;
    *) fail "unknown argument: ${1} (see --help)" ;;
  esac
done

context=${context:-${HOME}/.cache/plotjuggler-snap}
case "${context}/" in "${root}/"*) fail "context must be outside checkout: ${context}" ;; esac
mkdir -p "${context}"
if [[ -n "$(find "${context}" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
  echo "snap build: warning: starting from a non-empty context: ${context}" >&2
fi
rsync -a --exclude=/build/ --exclude=/packaging/snap/dist/ --exclude=__pycache__/ \
  "${root}/" "${context}/"
cp -a "${context}/packaging/snap/." "${context}/snap/"
echo "Build context: ${context}"
mkdir -p "${packaging}/dist"
cd "${context}"
if [[ "${clean}" == true ]]; then
  snapcraft clean plotjuggler --use-lxd
fi
snapcraft pack --use-lxd
mv -- plotjuggler4_*.snap "${packaging}/dist/"
