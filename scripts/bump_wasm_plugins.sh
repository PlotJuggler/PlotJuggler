#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
# Move the browser build's pj-official-plugins pin (cmake/PjWasmDependencies.cmake) to <ref>: a branch,
# a tag or a full commit sha (default main). The ref is resolved to a full sha, its codeload archive is
# downloaded and checked to be a gzip tar, and the URL and URL_HASH lines of the pj_official_plugins
# FetchContent_Declare are rewritten. Prints "<old sha> <new sha>" on stdout, the two equal when the pin
# already is at <ref> (nothing is downloaded or written then). Any failure exits non-zero with the file
# untouched. The archive must be requested by the FULL sha: a short sha yields a different archive and hash.
# Run by .github/workflows/wasm-plugins-pin.yml; needs bash, git, curl, tar, sha256sum, sed.
set -euo pipefail

REPO=PlotJuggler/pj-official-plugins
ref="${1:-main}"
cmake="$(cd "$(dirname "$0")/.." && pwd)/cmake/PjWasmDependencies.cmake"

die() { echo "bump_wasm_plugins: $*" >&2; exit 1; }

old="$(grep -oE "${REPO}/tar\.gz/[0-9a-f]{40}" "${cmake}" | cut -d/ -f4 || true)"
[[ ${old} =~ ^[0-9a-f]{40}$ ]] || die "expected exactly one ${REPO} pin in ${cmake}"

if [[ ${ref} =~ ^[0-9a-f]{40}$ ]]; then
  new="${ref}"
else
  # Only plain refnames: a glob character would make ls-remote match several refs.
  [[ ${ref} =~ ^[A-Za-z0-9_][A-Za-z0-9._/+-]*$ ]] || die "invalid ref '${ref}' (a branch, a tag or a 40-hex sha)"
  refs="$(GIT_TERMINAL_PROMPT=0 git ls-remote "https://github.com/${REPO}" \
    "refs/heads/${ref}" "refs/tags/${ref}" "refs/tags/${ref}^{}")" || die "git ls-remote ${REPO} failed"
  branch_sha="$(awk -v r="refs/heads/${ref}" '$2 == r { print $1 }' <<<"${refs}")"
  # An annotated tag's "^{}" line is the commit it points at.
  tag_sha="$(awk -v r="refs/tags/${ref}" -v p="refs/tags/${ref}^{}" \
    '$2 == r { plain = $1 } $2 == p { peeled = $1 } END { print (peeled != "" ? peeled : plain) }' <<<"${refs}")"
  if [[ -n ${branch_sha} && -n ${tag_sha} && ${branch_sha} != "${tag_sha}" ]]; then
    die "'${ref}' is both a branch (${branch_sha}) and a tag (${tag_sha}); pass the full sha"
  fi
  new="${branch_sha:-${tag_sha}}"
  [[ ${new} =~ ^[0-9a-f]{40}$ ]] || die "cannot resolve '${ref}' in ${REPO}"
fi

if [[ ${new} == "${old}" ]]; then
  echo "pin already at ${new}" >&2
  echo "${old} ${new}"
  exit 0
fi

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
url="https://codeload.github.com/${REPO}/tar.gz/${new}"
curl -fsSL --retry 3 --max-time 300 -o "${work}/pin.tar.gz" "${url}" || die "download of ${url} failed"
tar -tzf "${work}/pin.tar.gz" >/dev/null 2>&1 || die "${url} is not a gzip tar archive"
hash="$(sha256sum "${work}/pin.tar.gz" | cut -d' ' -f1)"

sed -e '/FetchContent_Declare(pj_official_plugins/,/)/{
  s#^\([[:space:]]*URL \).*#\1'"${url}"'#
  s#^\([[:space:]]*URL_HASH SHA256=\).*#\1'"${hash}"'#
}' "${cmake}" >"${work}/new.cmake"
[[ "$(diff "${cmake}" "${work}/new.cmake" | grep -c '^[<>]' || true)" -eq 4 ]] ||
  die "expected to rewrite exactly the URL and URL_HASH lines of pj_official_plugins"

cat "${work}/new.cmake" >"${cmake}"
echo "${old} ${new}"
