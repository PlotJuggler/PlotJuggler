#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
# Move the browser build's pj-official-plugins pin (cmake/PjWasmDependencies.cmake) to <ref>
# (branch, tag or full commit sha; default main): resolve it to a full sha, download the codeload
# archive twice, require equal SHA-256 hashes, then rewrite the URL and URL_HASH lines of the
# pj_official_plugins FetchContent_Declare. Prints "<old sha> <new sha>", or "pin already at <sha>"
# when nothing moves. Any failure exits non-zero and leaves the file untouched.
# The archive must be requested by the FULL sha: a short sha yields a different archive and hash.
# Run by .github/workflows/wasm-plugins-pin.yml; needs bash, git, curl, sha256sum, awk.
set -euo pipefail

REPO=PlotJuggler/pj-official-plugins
ref="${1:-main}"
cmake="$(cd "$(dirname "$0")/.." && pwd)/cmake/PjWasmDependencies.cmake"

die() { echo "bump_wasm_plugins: $*" >&2; exit 1; }

case "${ref}" in -* | "") die "invalid ref '${ref}'" ;; esac

old="$(grep -oE "${REPO}/tar\.gz/[0-9a-f]{40}" "${cmake}" | cut -d/ -f4)"
[ "${#old}" -eq 40 ] || die "expected exactly one ${REPO} pin in ${cmake}"

if printf '%s' "${ref}" | grep -Eq '^[0-9a-f]{40}$'; then
  new="${ref}"
else
  # A tag's "^{}" line is the commit an annotated tag points at; a branch has only the plain line.
  new="$(GIT_TERMINAL_PROMPT=0 git ls-remote "https://github.com/${REPO}" \
    "refs/heads/${ref}" "refs/tags/${ref}" "refs/tags/${ref}^{}" |
    awk '$2 ~ /\^\{\}$/ { peeled = $1 } $2 !~ /\^\{\}$/ && !plain { plain = $1 }
         END { print (peeled != "" ? peeled : plain) }')"
fi
printf '%s' "${new}" | grep -Eq '^[0-9a-f]{40}$' || die "cannot resolve '${ref}' in ${REPO}"

if [ "${new}" = "${old}" ]; then
  echo "pin already at ${new}"
  exit 0
fi

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
url="https://codeload.github.com/${REPO}/tar.gz/${new}"
curl -fsSL --retry 3 -o "${work}/a.tar.gz" "${url}" || die "download of ${url} failed"
curl -fsSL --retry 3 -o "${work}/b.tar.gz" "${url}" || die "second download of ${url} failed"
hash="$(sha256sum "${work}/a.tar.gz" | cut -d' ' -f1)"
hash_b="$(sha256sum "${work}/b.tar.gz" | cut -d' ' -f1)"
[ "${hash}" = "${hash_b}" ] || die "two downloads of ${url} hash differently (${hash} vs ${hash_b})"

awk -v url="${url}" -v hash="${hash}" '
  /FetchContent_Declare\(pj_official_plugins/ { blk = 1 }
  blk && /^[[:space:]]*URL[[:space:]]/ { sub(/URL .*/, "URL " url) }
  blk && /^[[:space:]]*URL_HASH[[:space:]]/ { sub(/URL_HASH .*/, "URL_HASH SHA256=" hash) }
  blk && /\)/ { blk = 0 }
  { print }' "${cmake}" >"${work}/new.cmake"
[ "$(diff "${cmake}" "${work}/new.cmake" | grep -c '^[<>]' || true)" -eq 4 ] ||
  die "expected to rewrite exactly the URL and URL_HASH lines of pj_official_plugins"
grep -qF "tar.gz/${new}" "${work}/new.cmake" || die "rewrite did not produce the new URL"
grep -qF "SHA256=${hash}" "${work}/new.cmake" || die "rewrite did not produce the new hash"

cat "${work}/new.cmake" >"${cmake}"
echo "${old} ${new}"
