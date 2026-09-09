#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
# Post-link DWARF deduplication for a PJ4 tree built with PJ_DEBUG_INFO=full:
# dwz -m rewrites every executable so DIEs shared between them live once in
# build/pj_common.debug (DW_FORM_GNU_ref_alt / .gnu_debugaltlink). gdb and
# libdw (backward-cpp) follow the link as long as the .debug file stays next
# to the binaries. Idempotent: dwz refuses already-processed files (exit 1,
# ignored). Not needed for PJ_DEBUG_INFO=split trees (nothing to share).
set -euo pipefail
B="${1:?usage: dwz_dedup.sh <build-dir>}"
command -v dwz >/dev/null || { echo "dwz not installed; skipping"; exit 0; }
mapfile -d '' -t exes < <(find "$B" -type f -perm -u+x -not -path "$B/AppDir/*" -not -path "$B/plugins-built/*" -size +20M \
  -exec sh -c 'head -c4 "$1" | grep -q "^.ELF" && printf "%s\0" "$1"' _ {} \;)
[[ ${#exes[@]} -ge 2 ]] || { echo "fewer than two large executables; nothing to share"; exit 0; }
before=$(printf '%s\0' "${exes[@]}" | xargs -0 stat -c %s -- | awk '{s+=$1} END{print s}')
# -L/-M raise dwz's default DIE/file limits, which 400 MB inputs exceed.
# Ignore failures because dwz can refuse individual files, including reruns.
dwz -m "$B/pj_common.debug" -r -L 200000000 -M 200000000 "${exes[@]}" || true
after=$(printf '%s\0' "${exes[@]}" | xargs -0 stat -c %s -- | awk '{s+=$1} END{print s}')
common=$(stat -c %s -- "$B/pj_common.debug" 2>/dev/null || echo 0)
awk -v b="$before" -v a="$after" -v c="$common" 'BEGIN{printf "dwz: executables %.2f -> %.2f GB, shared file %.2f GB, net %.2f GB\n", b/1e9, a/1e9, c/1e9, (b-a-c)/1e9}'
