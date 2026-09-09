#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
# Reports where the bytes in a PJ4 build tree are, by artifact class, in
# decimal GB. Read-only. Used by the build-size reduction plan's done_when
# checks and by anyone asking "why is build/ this big".
#
# Running ctest against a container-built tree (the only tree ctest may run on):
#   docker run --rm -v <worktree>:/work -w /work -e QT_QPA_PLATFORM=offscreen \
#     --entrypoint bash pj4-appimage-builder:jammy-qt<PJ_QT_VERSION>-x86_64-v2 -c \
#     'export LD_LIBRARY_PATH=$(find /root/.conan2/p -maxdepth 5 -type d -name lib | paste -sd:); \
#      ctest --test-dir build --output-on-failure --timeout 120'
#   Known environmental failures on this machine, not regressions: python_engine_test
#   (PYTHONHOME), DataProcessorTransformTest.MissingBackendDiagnosesNotDrops, RasterTextGlTest (no GPU).
set -euo pipefail
export LC_ALL=C
B="${1:?usage: build_tree_size.sh <build-dir>}"
[[ -d "$B" ]] || { echo "not a directory: $B" >&2; exit 2; }

gb() { awk -v s="${1:-0}" 'BEGIN{printf "%.2f", s/1e9}'; }
# Shell arithmetic needs integer byte counts, even when sums exceed 32 bits.
sum() { awk '{s+=$1} END{printf "%.0f\n", s+0}'; }

# Executables: ELF files with the exec bit, outside packaging output.
# NUL delimiters preserve paths containing whitespace and shell metacharacters.
mapfile -d '' -t exes < <(find "$B" -type f -perm -u+x \
  -not -path "$B/AppDir/*" -not -path "$B/plugins-built/*" \
  -exec sh -c 'head -c4 "$1" | grep -q "^.ELF" && printf "%s\0" "$1"' _ {} \;)
exe_bytes=0
if (( ${#exes[@]} )); then
  exe_bytes=$(printf '%s\0' "${exes[@]}" | xargs -0 -r stat -c %s -- | sum)
fi
# DWARF is a subset of EXE, not an additional contribution to TOTAL.
# The Size field is four fields after the section name in readelf -S -W.
exe_dwarf=0
for f in "${exes[@]}"; do
  d=$(readelf -S -W "$f" 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i ~ /^\.debug_/) {print $(i+4); break}}' \
      | while read -r h; do printf '%d\n' "0x$h"; done | sum)
  exe_dwarf=$((exe_dwarf + d))
done
obj=$(find "$B" -type f -name '*.o' -exec stat -c %s -- {} + | sum)
dwo=$(find "$B" -type f -name '*.dwo' -exec stat -c %s -- {} + | sum)
gen=$(find "$B" -type f \( -name 'qrc_*.cpp' -o -name 'moc_*.cpp' -o -name 'mocs_compilation*.cpp' -o -name 'ui_*.h' \) -exec stat -c %s -- {} + | sum)
ar=$(find "$B" -type f -name '*.a' -exec stat -c %s -- {} + | sum)
pkg=$( (du -sb -- "$B/AppDir" "$B/plugins-built" 2>/dev/null || true) | sum)
total=$(du -sb -- "$B" | awk '{print $1}')
other=$((total - exe_bytes - obj - dwo - gen - ar - pkg))

printf 'EXE        %s\n' "$(gb "$exe_bytes")"
printf 'EXE_DWARF  %s\n' "$(gb "$exe_dwarf")"
printf 'OBJ        %s\n' "$(gb "$obj")"
printf 'DWO        %s\n' "$(gb "$dwo")"
printf 'GEN_CPP    %s\n' "$(gb "$gen")"
printf 'ARCHIVE    %s\n' "$(gb "$ar")"
printf 'PACKAGING  %s\n' "$(gb "$pkg")"
printf 'OTHER      %s\n' "$(gb "$other")"
printf 'TOTAL      %s\n' "$(gb "$total")"
printf 'COUNT_EXE  %d\n' "${#exes[@]}"
