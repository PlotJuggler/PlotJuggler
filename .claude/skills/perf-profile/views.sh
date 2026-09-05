#!/usr/bin/env bash
# Standard views over a `perf script` dump (see SKILL.md Step 2). Text-only: no re-unwinding.
# Usage: views.sh <all.script> [start_sec end_sec]
set -euo pipefail
S="$1"; A="${2:-0}"; B="${3:-9999999999}"

echo "== per-second sample histogram =="
awk '!/^\t/ && NF {split($3,t,":"); c[int(t[1])]++} END {for (k in c) print k, c[k]}' "$S" | sort -n

echo "== per-thread samples (window $A-$B; ~997 samples = 1 CPU-second) =="
awk -v a="$A" -v b="$B" '!/^\t/ && NF {split($3,t,":"); s=t[1]+0; if (s>=a && s<b) c[$1" "$2]++}
     END {for (k in c) print c[k], k}' "$S" | sort -rn | head -15

echo "== flat self (leaf) hot functions, all threads merged (window) =="
awk -v a="$A" -v b="$B" -v RS= '{split($0,L,"\n"); split(L[1],h," "); s=h[3]+0
     if (s<a || s>=b || L[2]=="") next
     split(L[2],f," "); d=f[3]; gsub(/[()]/,"",d); n=split(d,p,"/")
     c[f[2]" ["p[n]"]"]++} END {for (k in c) print c[k], k}' "$S" | sort -rn | head -30

echo "== per-TID leaves: rerun with awk filter h[2]==<tid> for one thread =="
