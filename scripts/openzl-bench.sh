#!/usr/bin/env bash
# openzl-bench.sh — OpenZL (zli) against ziptastic on a corpus directory.
#
# This is the external check behind docs/OPENZL_MEASURED.md. It prints one row per file and a
# total, and it refuses to report a size from an archive it could not decode back byte for byte.
#
#   scripts/openzl-bench.sh <corpus-dir> [profile]
#
#   ZLI=~/openzl/build/cli/zli      path to the zli binary (required)
#   ZIPT_BIN=./build/ziptastic      optional: adds a ziptastic column and its time
#   PROFILE=serial                  default profile; use `sao`, `csv`, ... for format-aware runs
#
# Building zli without make is in the doc: Ninja, zig cc, and ml_selector patched out.
set -u

CORPUS="${1:?usage: openzl-bench.sh <corpus-dir> [profile]}"
PROFILE="${2:-${PROFILE:-serial}}"
ZLI="${ZLI:-zli}"
ZIPT_BIN="${ZIPT_BIN:-}"
LEVEL="${LEVEL:-9}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

command -v "$ZLI" >/dev/null 2>&1 || [ -x "$ZLI" ] || { echo "no zli at $ZLI" >&2; exit 2; }

printf "%-12s %12s %14s" file raw "openzl $PROFILE"
[ -n "$ZIPT_BIN" ] && printf " %12s %8s" ziptastic gain
printf " %8s\n" seconds

total_raw=0; total_z=0; total_zip=0; total_secs=0
for path in "$CORPUS"/*; do
  [ -f "$path" ] || continue
  name=$(basename "$path")
  raw=$(stat -c%s "$path")
  t0=$(date +%s.%N)
  "$ZLI" compress -p "$PROFILE" -l "$LEVEL" -o "$WORK/a.zl" -f "$path" >/dev/null 2>&1 \
    || { echo "$name: compress failed" >&2; exit 1; }
  t1=$(date +%s.%N)
  secs=$(python3 -c "print('%.3f' % ($t1 - $t0))")
  "$ZLI" decompress -o "$WORK/a.out" -f "$WORK/a.zl" >/dev/null 2>&1 \
    || { echo "$name: decompress failed" >&2; exit 1; }
  cmp -s "$path" "$WORK/a.out" || { echo "$name: ROUND-TRIP MISMATCH, refusing to report it" >&2; exit 1; }
  z=$(stat -c%s "$WORK/a.zl")
  total_raw=$((total_raw + raw)); total_z=$((total_z + z))
  total_secs=$(python3 -c "print('%.3f' % ($total_secs + $secs))")

  if [ -n "$ZIPT_BIN" ]; then
    "$ZIPT_BIN" -9 -f -o "$WORK/a.dr" "$path" >/dev/null 2>&1 || { echo "$name: ziptastic failed" >&2; exit 1; }
    "$ZIPT_BIN" -d -f -o "$WORK/a.dr.out" "$WORK/a.dr" >/dev/null 2>&1 || { echo "$name: ziptastic decode failed" >&2; exit 1; }
    cmp -s "$path" "$WORK/a.dr.out" || { echo "$name: ziptastic ROUND-TRIP MISMATCH" >&2; exit 1; }
    zip=$(stat -c%s "$WORK/a.dr"); total_zip=$((total_zip + zip))
    gain=$(python3 -c "print('%+.2f%%' % (100*($z-$zip)/$zip))")
    printf "%-12s %12d %14d %12d %8s %8s\n" "$name" "$raw" "$z" "$zip" "$gain" "$secs"
  else
    printf "%-12s %12d %14d %8s\n" "$name" "$raw" "$z" "$secs"
  fi
  rm -f "$WORK"/a.zl "$WORK"/a.out "$WORK"/a.dr "$WORK"/a.dr.out
done

python3 - "$total_raw" "$total_z" "$total_zip" "$total_secs" <<'PY'
import sys
raw, z, zip_, secs = (float(x) for x in sys.argv[1:5])
print("\ntotal: raw %d, openzl %d (%.2f%%), seconds %.1f" % (raw, z, 100*z/raw, secs))
if zip_:
    print("       ziptastic %d (%.2f%%), openzl vs ziptastic %+.2f%%" % (zip_, 100*zip_/raw, 100*(z-zip_)/zip_))
PY
