#!/usr/bin/env bash
# timeit.sh — run a command N times and report the spread, not just a number.
#
# The point is the spread. On a shared vCPU, identical work has measured 72 s,
# 86 s and 124 s in this project, so a single timing is worthless and even an
# average hides how untrustworthy it is. This reports min, median, max and the
# spread as a percentage of the median, so a reader can judge whether the machine
# was quiet enough for the comparison to mean anything.
#
# Usage:
#   ./timeit.sh <reps> [--label NAME] -- <command...>
#
# Prints one line per run plus a summary. Exits non-zero if the command fails.
set -u

REPS="${1:-3}"
shift || true
LABEL="run"
if [ "${1:-}" = "--label" ]; then LABEL="$2"; shift 2; fi
[ "${1:-}" = "--" ] && shift

if [ "$#" -eq 0 ]; then
  echo "usage: timeit.sh <reps> [--label NAME] -- <command...>" >&2
  exit 2
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
: > "$TMP/times"

i=0
while [ "$i" -lt "$REPS" ]; do
  start=$(date +%s%N)
  if ! "$@" >/dev/null 2>&1; then
    echo "FAIL: command failed on run $((i + 1))" >&2
    exit 1
  fi
  end=$(date +%s%N)
  ms=$(( (end - start) / 1000000 ))
  echo "$ms" >> "$TMP/times"
  printf '  %-14s run %d: %6d ms\n' "$LABEL" $((i + 1)) "$ms"
  i=$((i + 1))
done

sort -n "$TMP/times" -o "$TMP/sorted"
min=$(head -1 "$TMP/sorted")
max=$(tail -1 "$TMP/sorted")
n=$(wc -l < "$TMP/sorted")
if [ $((n % 2)) -eq 1 ]; then
  med=$(sed -n "$(( (n + 1) / 2 ))p" "$TMP/sorted")
else
  a=$(sed -n "$(( n / 2 ))p" "$TMP/sorted")
  b=$(sed -n "$(( n / 2 + 1 ))p" "$TMP/sorted")
  med=$(( (a + b) / 2 ))
fi
spread=$(( med > 0 ? (max - min) * 100 / med : 0 ))

printf '  %-14s min %d  median %d  max %d ms   spread %d%%\n' \
       "$LABEL" "$min" "$med" "$max" "$spread"

# A spread this large means the machine was too busy to compare anything.
if [ "$spread" -gt 20 ]; then
  printf '  %-14s NOTE: spread above 20%% — treat these numbers as unreliable\n' "$LABEL"
fi
