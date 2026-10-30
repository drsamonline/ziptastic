#!/usr/bin/env bash
# silesia_gate.sh — the Phase 1 (and later) ratio gate against `xz -9`.
#
# For every file in the corpus it:
#   1. compresses with `xz -9`             (the baseline)
#   2. compresses with `ziptastic -9`      (the subject)
#   3. decompresses with ziptastic and cmp  (bit-exactness)
#   4. appends a row to a CSV, skipping files already done
#
# Because step 2 can take minutes on a 50 MB file, results are written
# incrementally: re-running the script resumes where it stopped.
#
# usage: silesia_gate.sh <corpus-dir> [out.csv]
#   env: ZIPTASTIC  path to the ziptastic binary (default: build/ziptastic)
#        XZ         path to an xz binary     (default: xz on PATH)
set -u

CORPUS="${1:?usage: silesia_gate.sh <corpus-dir> [out.csv]}"
OUT="${2:-silesia-gate.csv}"
ZIPTASTIC="${ZIPTASTIC:-build/ziptastic}"
XZ="${XZ:-xz}"
TMP="${TMPDIR:-/tmp}/chce-gate.$$"

[ -x "$ZIPTASTIC" ] || { echo "no ziptastic binary at $ZIPTASTIC" >&2; exit 2; }
command -v "$XZ" >/dev/null || { echo "no xz binary at $XZ" >&2; exit 2; }
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

[ -f "$OUT" ] || echo "file,raw,xz9,chce,delta_pct" > "$OUT"

fail=0
for path in "$CORPUS"/*; do
  [ -f "$path" ] || continue
  f=$(basename "$path")
  grep -q "^$f," "$OUT" && continue

  raw=$(stat -c%s "$path" 2>/dev/null || stat -f%z "$path")
  "$XZ" -9 -k -c "$path" > "$TMP/$f.xz"      || { echo "xz failed: $f" >&2; fail=1; continue; }
  "$ZIPTASIC" -9 -f -o "$TMP/$f.chce" "$path" || { echo "ziptastic failed: $f" >&2; fail=1; continue; }
  "$ZIPTASIC" -d -f -o "$TMP/$f.out" "$TMP/$f.chce" || { echo "decode failed: $f" >&2; fail=1; continue; }
  cmp -s "$path" "$TMP/$f.out" || { echo "ROUND-TRIP MISMATCH: $f" >&2; fail=1; continue; }

  x=$(stat -c%s "$TMP/$f.xz" 2>/dev/null || stat -f%z "$TMP/$f.xz")
  c=$(stat -c%s "$TMP/$f.chce" 2>/dev/null || stat -f%z "$TMP/$f.chce")
  d=$(awk -v c="$c" -v x="$x" 'BEGIN{printf "%+.4f", (c-x)/x*100}')
  echo "$f,$raw,$x,$c,$d" >> "$OUT"
  echo "  $f: raw=$raw xz=$x chce=$c delta=${d}%"
  rm -f "$TMP/$f.xz" "$TMP/$f.chce" "$TMP/$f.out"
done

echo
echo "--- totals ---"
awk -F, 'NR>1 {r+=$2; x+=$3; c+=$4; n++}
         END { if (n) printf "%d files  raw=%d  xz9=%d  chce=%d  delta=%+.4f%%\n",
                            n, r, x, c, (c-x)/x*100 }' "$OUT"

# The gate itself: no file may be larger than xz -9.
awk -F, 'NR>1 && $5+0 > 0 { printf "GATE FAIL: %s is %s%% larger than xz -9\n", $1, $5; bad=1 }
         END { exit bad?1:0 }' "$OUT" || fail=1

[ "$fail" -eq 0 ] && echo "GATE PASSED" || echo "GATE FAILED"
exit "$fail"
